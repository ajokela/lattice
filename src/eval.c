#include "eval.h"
#include "lattice.h"
#include "string_ops.h"
#include "builtins.h"
#include "lexer.h"
#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Memory stats helpers ── */

static void stats_init(MemoryStats *s) { memset(s, 0, sizeof(*s)); }
static void stats_freeze(MemoryStats *s) { s->freezes++; }
static void stats_thaw(MemoryStats *s) { s->thaws++; }
static void stats_deep_clone(MemoryStats *s) { s->deep_clones++; }
static void stats_array(MemoryStats *s) { s->array_allocs++; }
static void stats_struct(MemoryStats *s) { s->struct_allocs++; }
static void stats_closure(MemoryStats *s) { s->closure_allocs++; }
static void stats_binding(MemoryStats *s) { s->bindings_created++; }
static void stats_fn_call(MemoryStats *s) { s->fn_calls++; }
static void stats_closure_call(MemoryStats *s) { s->closure_calls++; }
static void stats_forge(MemoryStats *s) { s->forge_blocks++; }

static void stats_scope_push(MemoryStats *s) {
    s->scope_pushes++;
    s->current_scope_depth++;
    if (s->current_scope_depth > s->peak_scope_depth)
        s->peak_scope_depth = s->current_scope_depth;
}

static void stats_scope_pop(MemoryStats *s) {
    s->scope_pops++;
    if (s->current_scope_depth > 0) s->current_scope_depth--;
}

/* ── EvalResult helpers ── */

static EvalResult eval_ok(LatValue v) {
    EvalResult r;
    r.ok = true;
    r.value = v;
    r.error = NULL;
    r.cf.tag = CF_NONE;
    return r;
}

static EvalResult eval_err(char *msg) {
    EvalResult r;
    r.ok = false;
    r.value = value_unit();
    r.error = msg;
    r.cf.tag = CF_NONE;
    return r;
}

static EvalResult eval_signal(ControlFlowTag tag, LatValue v) {
    EvalResult r;
    r.ok = false;
    r.value = value_unit();
    r.error = NULL;
    r.cf.tag = tag;
    r.cf.value = v;
    return r;
}

#define IS_OK(r) ((r).ok)
#define IS_ERR(r) (!(r).ok && (r).error != NULL)
#define IS_SIGNAL(r) (!(r).ok && (r).error == NULL)

/* Forward declarations */
static EvalResult eval_expr(Evaluator *ev, const Expr *expr);
static EvalResult eval_stmt(Evaluator *ev, const Stmt *stmt);
static EvalResult eval_block_stmts(Evaluator *ev, Stmt **stmts, size_t count);

/* ── Garbage Collector ── */

/*
 * Mark a single LatValue as reachable, recursively marking contained
 * heap pointers in the fluid heap.  Collects reachable crystal region
 * IDs into the supplied vector.
 */
static void gc_mark_value(FluidHeap *fh, LatValue *v, LatVec *reachable_regions) {
    switch (v->type) {
        case VAL_STR:
            if (v->as.str_val)
                fluid_mark(fh, v->as.str_val);
            break;
        case VAL_ARRAY:
            if (v->as.array.elems) {
                fluid_mark(fh, v->as.array.elems);
                for (size_t i = 0; i < v->as.array.len; i++)
                    gc_mark_value(fh, &v->as.array.elems[i], reachable_regions);
            }
            break;
        case VAL_STRUCT:
            if (v->as.strct.name) fluid_mark(fh, v->as.strct.name);
            if (v->as.strct.field_names) {
                fluid_mark(fh, v->as.strct.field_names);
                for (size_t i = 0; i < v->as.strct.field_count; i++) {
                    if (v->as.strct.field_names[i])
                        fluid_mark(fh, v->as.strct.field_names[i]);
                }
            }
            if (v->as.strct.field_values) {
                fluid_mark(fh, v->as.strct.field_values);
                for (size_t i = 0; i < v->as.strct.field_count; i++)
                    gc_mark_value(fh, &v->as.strct.field_values[i], reachable_regions);
            }
            break;
        case VAL_CLOSURE:
            if (v->as.closure.param_names) {
                fluid_mark(fh, v->as.closure.param_names);
                for (size_t i = 0; i < v->as.closure.param_count; i++) {
                    if (v->as.closure.param_names[i])
                        fluid_mark(fh, v->as.closure.param_names[i]);
                }
            }
            /* Mark captured env's values recursively */
            if (v->as.closure.captured_env) {
                Env *cenv = v->as.closure.captured_env;
                for (size_t s = 0; s < cenv->count; s++) {
                    /* Iterate the scope map and mark each value */
                    /* We use a small trampoline via env_iter_values pattern */
                }
            }
            break;
        case VAL_MAP:
            if (v->as.map.map) {
                for (size_t i = 0; i < v->as.map.map->cap; i++) {
                    if (v->as.map.map->entries[i].state == MAP_OCCUPIED) {
                        if (v->as.map.map->entries[i].key)
                            fluid_mark(fh, v->as.map.map->entries[i].key);
                        if (v->as.map.map->entries[i].value) {
                            fluid_mark(fh, v->as.map.map->entries[i].value);
                            gc_mark_value(fh, (LatValue *)v->as.map.map->entries[i].value, reachable_regions);
                        }
                    }
                }
            }
            break;
        default:
            break;
    }
}

/* Callback for env_iter_values to mark each value */
typedef struct {
    FluidHeap *fh;
    LatVec    *reachable_regions;
} GcMarkCtx;

static void gc_mark_env_value(LatValue *v, void *ctx) {
    GcMarkCtx *mc = (GcMarkCtx *)ctx;
    gc_mark_value(mc->fh, v, mc->reachable_regions);
}

/*
 * Run a full GC cycle: mark all roots, sweep unreachable.
 */
static void gc_cycle(Evaluator *ev) {
    FluidHeap *fh = ev->heap->fluid;
    LatVec reachable_regions = lat_vec_new(sizeof(RegionId));

    /* 1. Clear all marks */
    fluid_unmark_all(fh);

    /* 2. Mark roots from environment */
    GcMarkCtx ctx = { fh, &reachable_regions };
    env_iter_values(ev->env, gc_mark_env_value, &ctx);

    /* 3. Mark roots from shadow stack */
    for (size_t i = 0; i < ev->gc_roots.len; i++) {
        LatValue **vp = lat_vec_get(&ev->gc_roots, i);
        if (*vp) gc_mark_value(fh, *vp, &reachable_regions);
    }

    /* 4. Sweep unmarked fluid allocations */
    size_t swept_fluid = fluid_sweep(fh);

    /* 5. Collect unreachable crystal regions */
    size_t swept_regions = region_collect(
        ev->heap->regions,
        (RegionId *)reachable_regions.data,
        reachable_regions.len);

    /* 6. Update stats */
    ev->stats.gc_cycles++;
    ev->stats.gc_swept_fluid += swept_fluid;
    ev->stats.gc_swept_regions += swept_regions;

    lat_vec_free(&reachable_regions);
}

/*
 * Maybe trigger GC if heap exceeds threshold.
 * Called after allocations.
 */
static void gc_maybe_collect(Evaluator *ev) {
    if (ev->gc_stress ||
        ev->heap->fluid->total_bytes >= ev->heap->fluid->gc_threshold) {
        gc_cycle(ev);
    }
}

/*
 * Resolve a mutable pointer to a LatValue from an lvalue expression.
 * Walks chains of field_access and index expressions to find the
 * actual storage location in the environment. Returns NULL + sets *err on failure.
 */
static LatValue *resolve_lvalue(Evaluator *ev, const Expr *expr, char **err) {
    if (expr->tag == EXPR_IDENT) {
        for (size_t s = ev->env->count; s > 0; s--) {
            LatValue *v = lat_map_get(&ev->env->scopes[s-1], expr->as.str_val);
            if (v) return v;
        }
        (void)asprintf(err, "undefined variable '%s'", expr->as.str_val);
        return NULL;
    }
    if (expr->tag == EXPR_FIELD_ACCESS) {
        LatValue *parent = resolve_lvalue(ev, expr->as.field_access.object, err);
        if (!parent) return NULL;
        if (parent->type != VAL_STRUCT) {
            (void)asprintf(err, "cannot access field '%s' on %s",
                           expr->as.field_access.field, value_type_name(parent));
            return NULL;
        }
        for (size_t i = 0; i < parent->as.strct.field_count; i++) {
            if (strcmp(parent->as.strct.field_names[i], expr->as.field_access.field) == 0) {
                return &parent->as.strct.field_values[i];
            }
        }
        (void)asprintf(err, "struct has no field '%s'", expr->as.field_access.field);
        return NULL;
    }
    if (expr->tag == EXPR_INDEX) {
        LatValue *parent = resolve_lvalue(ev, expr->as.index.object, err);
        if (!parent) return NULL;
        if (parent->type == VAL_MAP) {
            /* Map indexing for lvalue */
            EvalResult idxr = eval_expr(ev, expr->as.index.index);
            if (!IS_OK(idxr)) { *err = idxr.error; return NULL; }
            if (idxr.value.type != VAL_STR) {
                value_free(&idxr.value);
                *err = strdup("map key must be a string");
                return NULL;
            }
            const char *key = idxr.value.as.str_val;
            /* Auto-vivify: if key doesn't exist, create it with unit */
            if (!lat_map_contains(parent->as.map.map, key)) {
                LatValue unit = value_unit();
                lat_map_set(parent->as.map.map, key, &unit);
            }
            LatValue *target = (LatValue *)lat_map_get(parent->as.map.map, key);
            value_free(&idxr.value);
            return target;
        }
        if (parent->type != VAL_ARRAY) {
            (void)asprintf(err, "cannot index into %s", value_type_name(parent));
            return NULL;
        }
        /* Evaluate the index expression */
        EvalResult idxr = eval_expr(ev, expr->as.index.index);
        if (!IS_OK(idxr)) {
            *err = idxr.error;
            return NULL;
        }
        if (idxr.value.type != VAL_INT) {
            value_free(&idxr.value);
            *err = strdup("array index must be an integer");
            return NULL;
        }
        size_t idx = (size_t)idxr.value.as.int_val;
        value_free(&idxr.value);
        if (idx >= parent->as.array.len) {
            (void)asprintf(err, "index %zu out of bounds (length %zu)",
                           idx, parent->as.array.len);
            return NULL;
        }
        return &parent->as.array.elems[idx];
    }
    *err = strdup("invalid lvalue expression");
    return NULL;
}
static EvalResult eval_method_call(Evaluator *ev, LatValue obj, const char *method,
                                   LatValue *args, size_t arg_count);

/* ── FnDecl lookup helpers ── */
/* We store FnDecl* pointers in the map */

static FnDecl *find_fn(Evaluator *ev, const char *name) {
    FnDecl **ptr = lat_map_get(&ev->fn_defs, name);
    return ptr ? *ptr : NULL;
}

static StructDecl *find_struct(Evaluator *ev, const char *name) {
    StructDecl **ptr = lat_map_get(&ev->struct_defs, name);
    return ptr ? *ptr : NULL;
}

/* ── Function calling ── */

static EvalResult call_fn(Evaluator *ev, const FnDecl *decl, LatValue *args, size_t arg_count,
                          LatValue **writeback_out) {
    if (arg_count != decl->param_count) {
        char *err = NULL;
        (void)asprintf(&err, "function '%s' expects %zu arguments, got %zu",
                       decl->name, decl->param_count, arg_count);
        return eval_err(err);
    }
    stats_fn_call(&ev->stats);
    stats_scope_push(&ev->stats);
    env_push_scope(ev->env);
    for (size_t i = 0; i < arg_count; i++) {
        env_define(ev->env, decl->params[i].name, args[i]);
    }
    EvalResult result = eval_block_stmts(ev, decl->body, decl->body_count);

    /* Before popping the scope, capture fluid parameter values for write-back */
    if (writeback_out) {
        for (size_t i = 0; i < arg_count; i++) {
            if (decl->params[i].ty.phase == PHASE_FLUID) {
                LatValue val;
                if (env_get(ev->env, decl->params[i].name, &val)) {
                    writeback_out[i] = malloc(sizeof(LatValue));
                    *writeback_out[i] = val;
                }
            }
        }
    }

    env_pop_scope(ev->env);
    stats_scope_pop(&ev->stats);

    if (IS_SIGNAL(result) && result.cf.tag == CF_RETURN) {
        return eval_ok(result.cf.value);
    }
    return result;
}

static EvalResult call_closure(Evaluator *ev, char **params, size_t param_count,
                               const Expr *body, Env *closure_env, LatValue *args, size_t arg_count) {
    if (arg_count != param_count) {
        char *err = NULL;
        (void)asprintf(&err, "closure expects %zu arguments, got %zu", param_count, arg_count);
        return eval_err(err);
    }
    stats_closure_call(&ev->stats);

    /* Swap environments */
    Env *saved = ev->env;
    ev->env = closure_env;
    stats_scope_push(&ev->stats);
    env_push_scope(ev->env);
    for (size_t i = 0; i < arg_count; i++) {
        env_define(ev->env, params[i], args[i]);
    }
    EvalResult result = eval_expr(ev, body);
    env_pop_scope(ev->env);
    stats_scope_pop(&ev->stats);
    ev->env = saved;

    if (IS_SIGNAL(result) && result.cf.tag == CF_RETURN) {
        return eval_ok(result.cf.value);
    }
    return result;
}

/* ── Binary operations ── */

static EvalResult eval_binop(BinOpKind op, LatValue *lv, LatValue *rv) {
    /* Integer arithmetic */
    if (lv->type == VAL_INT && rv->type == VAL_INT) {
        int64_t a = lv->as.int_val, b = rv->as.int_val;
        switch (op) {
            case BINOP_ADD: return eval_ok(value_int(a + b));
            case BINOP_SUB: return eval_ok(value_int(a - b));
            case BINOP_MUL: return eval_ok(value_int(a * b));
            case BINOP_DIV:
                if (b == 0) return eval_err(strdup("division by zero"));
                return eval_ok(value_int(a / b));
            case BINOP_MOD:
                if (b == 0) return eval_err(strdup("modulo by zero"));
                return eval_ok(value_int(a % b));
            case BINOP_EQ:   return eval_ok(value_bool(a == b));
            case BINOP_NEQ:  return eval_ok(value_bool(a != b));
            case BINOP_LT:   return eval_ok(value_bool(a < b));
            case BINOP_GT:   return eval_ok(value_bool(a > b));
            case BINOP_LTEQ: return eval_ok(value_bool(a <= b));
            case BINOP_GTEQ: return eval_ok(value_bool(a >= b));
            default: break;
        }
    }
    /* Float arithmetic */
    if (lv->type == VAL_FLOAT && rv->type == VAL_FLOAT) {
        double a = lv->as.float_val, b = rv->as.float_val;
        switch (op) {
            case BINOP_ADD: return eval_ok(value_float(a + b));
            case BINOP_SUB: return eval_ok(value_float(a - b));
            case BINOP_MUL: return eval_ok(value_float(a * b));
            case BINOP_DIV: return eval_ok(value_float(a / b));
            case BINOP_MOD: {
                double r = a - (int64_t)(a / b) * b;
                return eval_ok(value_float(r));
            }
            case BINOP_EQ:   return eval_ok(value_bool(a == b));
            case BINOP_NEQ:  return eval_ok(value_bool(a != b));
            case BINOP_LT:   return eval_ok(value_bool(a < b));
            case BINOP_GT:   return eval_ok(value_bool(a > b));
            case BINOP_LTEQ: return eval_ok(value_bool(a <= b));
            case BINOP_GTEQ: return eval_ok(value_bool(a >= b));
            default: break;
        }
    }
    /* Mixed int/float */
    if ((lv->type == VAL_INT && rv->type == VAL_FLOAT) ||
        (lv->type == VAL_FLOAT && rv->type == VAL_INT)) {
        double a = lv->type == VAL_FLOAT ? lv->as.float_val : (double)lv->as.int_val;
        double b = rv->type == VAL_FLOAT ? rv->as.float_val : (double)rv->as.int_val;
        switch (op) {
            case BINOP_ADD: return eval_ok(value_float(a + b));
            case BINOP_SUB: return eval_ok(value_float(a - b));
            case BINOP_MUL: return eval_ok(value_float(a * b));
            case BINOP_DIV: return eval_ok(value_float(a / b));
            default: break;
        }
    }
    /* String concatenation */
    if (lv->type == VAL_STR && rv->type == VAL_STR && op == BINOP_ADD) {
        size_t al = strlen(lv->as.str_val), bl = strlen(rv->as.str_val);
        char *buf = malloc(al + bl + 1);
        memcpy(buf, lv->as.str_val, al);
        memcpy(buf + al, rv->as.str_val, bl);
        buf[al + bl] = '\0';
        return eval_ok(value_string_owned(buf));
    }
    /* String comparison */
    if (lv->type == VAL_STR && rv->type == VAL_STR) {
        if (op == BINOP_EQ) return eval_ok(value_bool(strcmp(lv->as.str_val, rv->as.str_val) == 0));
        if (op == BINOP_NEQ) return eval_ok(value_bool(strcmp(lv->as.str_val, rv->as.str_val) != 0));
    }
    /* Bool comparison */
    if (lv->type == VAL_BOOL && rv->type == VAL_BOOL) {
        if (op == BINOP_EQ) return eval_ok(value_bool(lv->as.bool_val == rv->as.bool_val));
        if (op == BINOP_NEQ) return eval_ok(value_bool(lv->as.bool_val != rv->as.bool_val));
        if (op == BINOP_AND) return eval_ok(value_bool(lv->as.bool_val && rv->as.bool_val));
        if (op == BINOP_OR) return eval_ok(value_bool(lv->as.bool_val || rv->as.bool_val));
    }

    char *err = NULL;
    (void)asprintf(&err, "unsupported binary operation on %s and %s",
                   value_type_name(lv), value_type_name(rv));
    return eval_err(err);
}

static EvalResult eval_unaryop(UnaryOpKind op, LatValue *val) {
    if (op == UNOP_NEG && val->type == VAL_INT) return eval_ok(value_int(-val->as.int_val));
    if (op == UNOP_NEG && val->type == VAL_FLOAT) return eval_ok(value_float(-val->as.float_val));
    if (op == UNOP_NOT && val->type == VAL_BOOL) return eval_ok(value_bool(!val->as.bool_val));
    char *err = NULL;
    (void)asprintf(&err, "unsupported unary operation on %s", value_type_name(val));
    return eval_err(err);
}

/* ── Expression evaluation ── */

static EvalResult eval_expr(Evaluator *ev, const Expr *expr) {
    switch (expr->tag) {
        case EXPR_INT_LIT:    return eval_ok(value_int(expr->as.int_val));
        case EXPR_FLOAT_LIT:  return eval_ok(value_float(expr->as.float_val));
        case EXPR_STRING_LIT: return eval_ok(value_string(expr->as.str_val));
        case EXPR_BOOL_LIT:   return eval_ok(value_bool(expr->as.bool_val));

        case EXPR_IDENT: {
            LatValue val;
            if (!env_get(ev->env, expr->as.str_val, &val)) {
                char *err = NULL;
                (void)asprintf(&err, "undefined variable '%s'", expr->as.str_val);
                return eval_err(err);
            }
            return eval_ok(val);
        }

        case EXPR_BINOP: {
            EvalResult lr = eval_expr(ev, expr->as.binop.left);
            if (!IS_OK(lr)) return lr;
            EvalResult rr = eval_expr(ev, expr->as.binop.right);
            if (!IS_OK(rr)) { value_free(&lr.value); return rr; }
            EvalResult res = eval_binop(expr->as.binop.op, &lr.value, &rr.value);
            value_free(&lr.value);
            value_free(&rr.value);
            return res;
        }

        case EXPR_UNARYOP: {
            EvalResult vr = eval_expr(ev, expr->as.unaryop.operand);
            if (!IS_OK(vr)) return vr;
            EvalResult res = eval_unaryop(expr->as.unaryop.op, &vr.value);
            value_free(&vr.value);
            return res;
        }

        case EXPR_CALL: {
            /* Evaluate arguments */
            size_t argc = expr->as.call.arg_count;
            LatValue *args = malloc(argc * sizeof(LatValue));
            for (size_t i = 0; i < argc; i++) {
                EvalResult ar = eval_expr(ev, expr->as.call.args[i]);
                if (!IS_OK(ar)) {
                    for (size_t j = 0; j < i; j++) value_free(&args[j]);
                    free(args);
                    return ar;
                }
                args[i] = ar.value;
            }
            /* Check for named function or built-in */
            if (expr->as.call.func->tag == EXPR_IDENT) {
                const char *fn_name = expr->as.call.func->as.str_val;

                /* ── Built-in functions ── */

                if (strcmp(fn_name, "input") == 0) {
                    const char *prompt = NULL;
                    if (argc > 0 && args[0].type == VAL_STR) prompt = args[0].as.str_val;
                    char *line = builtin_input(prompt);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!line) return eval_ok(value_unit());
                    return eval_ok(value_string_owned(line));
                }

                if (strcmp(fn_name, "is_complete") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) value_free(&args[i]); free(args); return eval_err(strdup("is_complete() expects 1 string argument")); }
                    const char *source = args[0].as.str_val;
                    Lexer lex = lexer_new(source);
                    char *lex_err = NULL;
                    LatVec toks = lexer_tokenize(&lex, &lex_err);
                    if (lex_err) {
                        /* Lex error (unclosed string etc.) = incomplete */
                        free(lex_err);
                        lat_vec_free(&toks);
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_ok(value_bool(false));
                    }
                    int depth = 0;
                    for (size_t j = 0; j < toks.len; j++) {
                        Token *t = lat_vec_get(&toks, j);
                        switch (t->type) {
                            case TOK_LBRACE: case TOK_LPAREN: case TOK_LBRACKET:
                                depth++;
                                break;
                            case TOK_RBRACE: case TOK_RPAREN: case TOK_RBRACKET:
                                depth--;
                                break;
                            default:
                                break;
                        }
                    }
                    for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
                    lat_vec_free(&toks);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_bool(depth <= 0));
                }

                if (strcmp(fn_name, "typeof") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) value_free(&args[i]); free(args); return eval_err(strdup("typeof() expects 1 argument")); }
                    const char *tn = builtin_typeof_str(&args[0]);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string(tn));
                }

                if (strcmp(fn_name, "phase_of") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) value_free(&args[i]); free(args); return eval_err(strdup("phase_of() expects 1 argument")); }
                    const char *pn = builtin_phase_of_str(&args[0]);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string(pn));
                }

                if (strcmp(fn_name, "to_string") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) value_free(&args[i]); free(args); return eval_err(strdup("to_string() expects 1 argument")); }
                    char *s = builtin_to_string(&args[0]);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string_owned(s));
                }

                if (strcmp(fn_name, "ord") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) value_free(&args[i]); free(args); return eval_err(strdup("ord() expects 1 string argument")); }
                    int64_t code = builtin_ord(args[0].as.str_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_int(code));
                }

                if (strcmp(fn_name, "chr") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) value_free(&args[i]); free(args); return eval_err(strdup("chr() expects 1 integer argument")); }
                    char *s = builtin_chr(args[0].as.int_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string_owned(s));
                }

                if (strcmp(fn_name, "read_file") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) value_free(&args[i]); free(args); return eval_err(strdup("read_file() expects 1 string argument")); }
                    char *contents = builtin_read_file(args[0].as.str_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!contents) return eval_err(strdup("read_file: could not read file"));
                    return eval_ok(value_string_owned(contents));
                }

                if (strcmp(fn_name, "write_file") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) { for (size_t i = 0; i < argc; i++) value_free(&args[i]); free(args); return eval_err(strdup("write_file() expects 2 string arguments")); }
                    bool wf_ok = builtin_write_file(args[0].as.str_val, args[1].as.str_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!wf_ok) return eval_err(strdup("write_file: could not write file"));
                    return eval_ok(value_bool(true));
                }

                if (strcmp(fn_name, "lat_eval") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) value_free(&args[i]); free(args); return eval_err(strdup("lat_eval() expects 1 string argument")); }
                    const char *source = args[0].as.str_val;
                    Lexer lex = lexer_new(source);
                    char *lex_err = NULL;
                    LatVec toks = lexer_tokenize(&lex, &lex_err);
                    if (lex_err) {
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(lex_err);
                    }
                    Parser parser = parser_new(&toks);
                    char *parse_err = NULL;
                    Program prog = parser_parse(&parser, &parse_err);
                    if (parse_err) {
                        program_free(&prog);
                        for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
                        lat_vec_free(&toks);
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(parse_err);
                    }
                    /* Register functions and structs (same as evaluator_run) */
                    for (size_t j = 0; j < prog.item_count; j++) {
                        if (prog.items[j].tag == ITEM_STRUCT) {
                            StructDecl *ptr = &prog.items[j].as.struct_decl;
                            lat_map_set(&ev->struct_defs, ptr->name, &ptr);
                        } else if (prog.items[j].tag == ITEM_FUNCTION) {
                            FnDecl *ptr = &prog.items[j].as.fn_decl;
                            lat_map_set(&ev->fn_defs, ptr->name, &ptr);
                        }
                    }
                    /* Execute statements — set lat_eval_scope so top-level
                     * bindings persist in the caller's scope */
                    size_t saved_scope = ev->lat_eval_scope;
                    ev->lat_eval_scope = ev->env->count;
                    EvalResult eval_r = eval_ok(value_unit());
                    for (size_t j = 0; j < prog.item_count; j++) {
                        if (prog.items[j].tag == ITEM_STMT) {
                            value_free(&eval_r.value);
                            eval_r = eval_stmt(ev, prog.items[j].as.stmt);
                            if (!IS_OK(eval_r)) break;
                        }
                    }
                    ev->lat_eval_scope = saved_scope;
                    /* Free statement items. If fn/struct decls were registered,
                     * keep the items array alive (decls live inline in it).
                     * Otherwise free the whole program. */
                    bool has_decls = false;
                    for (size_t j = 0; j < prog.item_count; j++) {
                        if (prog.items[j].tag == ITEM_STMT)
                            stmt_free(prog.items[j].as.stmt);
                        else
                            has_decls = true;
                    }
                    if (!has_decls) free(prog.items);
                    for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
                    lat_vec_free(&toks);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_r;
                }

                if (strcmp(fn_name, "tokenize") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) value_free(&args[i]); free(args); return eval_err(strdup("tokenize() expects 1 string argument")); }
                    const char *source = args[0].as.str_val;
                    Lexer lex = lexer_new(source);
                    char *lex_err = NULL;
                    LatVec toks = lexer_tokenize(&lex, &lex_err);
                    if (lex_err) {
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(lex_err);
                    }
                    size_t tok_count = toks.len > 0 ? toks.len - 1 : 0;
                    LatValue *elems = malloc((tok_count > 0 ? tok_count : 1) * sizeof(LatValue));
                    for (size_t j = 0; j < tok_count; j++) {
                        Token *t = lat_vec_get(&toks, j);
                        const char *type_str = token_type_name(t->type);
                        char *text;
                        if (t->type == TOK_IDENT || t->type == TOK_STRING_LIT || t->type == TOK_MODE_DIRECTIVE) {
                            text = strdup(t->as.str_val);
                        } else if (t->type == TOK_INT_LIT) {
                            (void)asprintf(&text, "%lld", (long long)t->as.int_val);
                        } else if (t->type == TOK_FLOAT_LIT) {
                            (void)asprintf(&text, "%g", t->as.float_val);
                        } else {
                            text = strdup(token_type_name(t->type));
                        }
                        char *fnames[2] = { "type", "text" };
                        LatValue fvals[2];
                        fvals[0] = value_string(type_str);
                        fvals[1] = value_string_owned(text);
                        elems[j] = value_struct("Token", fnames, fvals, 2);
                    }
                    for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
                    lat_vec_free(&toks);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    LatValue arr = value_array(elems, tok_count);
                    free(elems);
                    return eval_ok(arr);
                }

                if (strcmp(fn_name, "Map::new") == 0) {
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_map_new());
                }

                if (strcmp(fn_name, "parse_int") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) value_free(&args[i]); free(args); return eval_err(strdup("parse_int() expects 1 string argument")); }
                    bool ok;
                    int64_t val = builtin_parse_int(args[0].as.str_val, &ok);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!ok) return eval_err(strdup("parse_int: invalid integer"));
                    return eval_ok(value_int(val));
                }

                if (strcmp(fn_name, "parse_float") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) value_free(&args[i]); free(args); return eval_err(strdup("parse_float() expects 1 string argument")); }
                    bool ok;
                    double val = builtin_parse_float(args[0].as.str_val, &ok);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!ok) return eval_err(strdup("parse_float: invalid float"));
                    return eval_ok(value_float(val));
                }

                if (strcmp(fn_name, "len") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) value_free(&args[i]); free(args); return eval_err(strdup("len() expects 1 argument")); }
                    int64_t l = -1;
                    if (args[0].type == VAL_STR) l = (int64_t)strlen(args[0].as.str_val);
                    else if (args[0].type == VAL_ARRAY) l = (int64_t)args[0].as.array.len;
                    else if (args[0].type == VAL_MAP) l = (int64_t)lat_map_len(args[0].as.map.map);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (l < 0) return eval_err(strdup("len() not supported on this type"));
                    return eval_ok(value_int(l));
                }

                if (strcmp(fn_name, "exit") == 0) {
                    int code = 0;
                    if (argc > 0 && args[0].type == VAL_INT) code = (int)args[0].as.int_val;
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    exit(code);
                }

                if (strcmp(fn_name, "version") == 0) {
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string(LATTICE_VERSION));
                }

                if (strcmp(fn_name, "print_raw") == 0) {
                    for (size_t i = 0; i < argc; i++) {
                        if (i > 0) printf(" ");
                        char *s = value_display(&args[i]);
                        printf("%s", s);
                        free(s);
                    }
                    fflush(stdout);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_unit());
                }

                if (strcmp(fn_name, "eprint") == 0) {
                    for (size_t i = 0; i < argc; i++) {
                        if (i > 0) fprintf(stderr, " ");
                        char *s = value_display(&args[i]);
                        fprintf(stderr, "%s", s);
                        free(s);
                    }
                    fprintf(stderr, "\n");
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_unit());
                }

                /* ── Named function lookup ── */
                FnDecl *fd = find_fn(ev, fn_name);
                if (fd) {
                    /* Allocate write-back slots for fluid parameters */
                    LatValue **writeback = calloc(argc, sizeof(LatValue *));
                    EvalResult res = call_fn(ev, fd, args, argc, writeback);
                    /* Write back fluid parameters to caller's env */
                    if (IS_OK(res)) {
                        for (size_t i = 0; i < argc; i++) {
                            if (writeback[i] && expr->as.call.args[i]->tag == EXPR_IDENT) {
                                env_set(ev->env, expr->as.call.args[i]->as.str_val, *writeback[i]);
                                free(writeback[i]);
                                writeback[i] = NULL;
                            }
                        }
                    }
                    /* Clean up any unused writebacks */
                    for (size_t i = 0; i < argc; i++) {
                        if (writeback[i]) {
                            value_free(writeback[i]);
                            free(writeback[i]);
                        }
                    }
                    free(writeback);
                    free(args);
                    return res;
                }
            }
            /* Otherwise evaluate callee */
            EvalResult callee_r = eval_expr(ev, expr->as.call.func);
            if (!IS_OK(callee_r)) {
                for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                free(args);
                return callee_r;
            }
            if (callee_r.value.type != VAL_CLOSURE) {
                char *err = NULL;
                (void)asprintf(&err, "'%s' is not callable", value_type_name(&callee_r.value));
                value_free(&callee_r.value);
                for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                free(args);
                return eval_err(err);
            }
            EvalResult res = call_closure(ev,
                callee_r.value.as.closure.param_names,
                callee_r.value.as.closure.param_count,
                callee_r.value.as.closure.body,
                callee_r.value.as.closure.captured_env,
                args, argc);
            /* Don't free captured_env since closure still owns it - just cleanup */
            callee_r.value.as.closure.captured_env = NULL;
            value_free(&callee_r.value);
            free(args);
            return res;
        }

        case EXPR_METHOD_CALL: {
            /* Handle .push() specially - needs to mutate the binding in env */
            if (strcmp(expr->as.method_call.method, "push") == 0 &&
                expr->as.method_call.object->tag == EXPR_IDENT &&
                expr->as.method_call.arg_count == 1) {
                const char *var_name = expr->as.method_call.object->as.str_val;
                LatValue existing;
                if (!env_get(ev->env, var_name, &existing)) {
                    char *err = NULL;
                    (void)asprintf(&err, "undefined variable '%s'", var_name);
                    return eval_err(err);
                }
                if (existing.type != VAL_ARRAY) {
                    value_free(&existing);
                    return eval_err(strdup(".push() is not defined on non-array"));
                }
                if (value_is_crystal(&existing)) {
                    value_free(&existing);
                    return eval_err(strdup("cannot push to a crystal array"));
                }
                EvalResult ar = eval_expr(ev, expr->as.method_call.args[0]);
                if (!IS_OK(ar)) { value_free(&existing); return ar; }
                /* Grow the array */
                if (existing.as.array.len >= existing.as.array.cap) {
                    existing.as.array.cap = existing.as.array.cap < 4 ? 4 : existing.as.array.cap * 2;
                    existing.as.array.elems = realloc(existing.as.array.elems,
                        existing.as.array.cap * sizeof(LatValue));
                }
                existing.as.array.elems[existing.as.array.len++] = ar.value;
                env_set(ev->env, var_name, existing);
                return eval_ok(value_unit());
            }
            /* Handle .set() on maps - needs mutation like .push() */
            if (strcmp(expr->as.method_call.method, "set") == 0 &&
                expr->as.method_call.arg_count == 2) {
                char *lv_err = NULL;
                LatValue *map_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (map_lv && map_lv->type == VAL_MAP) {
                    EvalResult kr = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(kr)) return kr;
                    if (kr.value.type != VAL_STR) {
                        value_free(&kr.value);
                        return eval_err(strdup(".set() key must be a string"));
                    }
                    EvalResult vr = eval_expr(ev, expr->as.method_call.args[1]);
                    if (!IS_OK(vr)) { value_free(&kr.value); return vr; }
                    /* Free old value if key exists */
                    LatValue *old = (LatValue *)lat_map_get(map_lv->as.map.map, kr.value.as.str_val);
                    if (old) value_free(old);
                    lat_map_set(map_lv->as.map.map, kr.value.as.str_val, &vr.value);
                    value_free(&kr.value);
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }
            /* Handle .remove() on maps - needs mutation */
            if (strcmp(expr->as.method_call.method, "remove") == 0 &&
                expr->as.method_call.arg_count == 1) {
                char *lv_err = NULL;
                LatValue *map_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (map_lv && map_lv->type == VAL_MAP) {
                    EvalResult kr = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(kr)) return kr;
                    if (kr.value.type != VAL_STR) {
                        value_free(&kr.value);
                        return eval_err(strdup(".remove() key must be a string"));
                    }
                    /* Free old value if key exists */
                    LatValue *old = (LatValue *)lat_map_get(map_lv->as.map.map, kr.value.as.str_val);
                    if (old) value_free(old);
                    lat_map_remove(map_lv->as.map.map, kr.value.as.str_val);
                    value_free(&kr.value);
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }
            EvalResult objr = eval_expr(ev, expr->as.method_call.object);
            if (!IS_OK(objr)) return objr;
            size_t argc = expr->as.method_call.arg_count;
            LatValue *args = malloc((argc < 1 ? 1 : argc) * sizeof(LatValue));
            for (size_t i = 0; i < argc; i++) {
                EvalResult ar = eval_expr(ev, expr->as.method_call.args[i]);
                if (!IS_OK(ar)) {
                    for (size_t j = 0; j < i; j++) value_free(&args[j]);
                    free(args);
                    value_free(&objr.value);
                    return ar;
                }
                args[i] = ar.value;
            }
            EvalResult res = eval_method_call(ev, objr.value, expr->as.method_call.method, args, argc);
            value_free(&objr.value);
            for (size_t i = 0; i < argc; i++) value_free(&args[i]);
            free(args);
            return res;
        }

        case EXPR_FIELD_ACCESS: {
            EvalResult objr = eval_expr(ev, expr->as.field_access.object);
            if (!IS_OK(objr)) return objr;
            if (objr.value.type != VAL_STRUCT) {
                char *err = NULL;
                (void)asprintf(&err, "cannot access field '%s' on %s",
                               expr->as.field_access.field, value_type_name(&objr.value));
                value_free(&objr.value);
                return eval_err(err);
            }
            for (size_t i = 0; i < objr.value.as.strct.field_count; i++) {
                if (strcmp(objr.value.as.strct.field_names[i], expr->as.field_access.field) == 0) {
                    LatValue result = value_deep_clone(&objr.value.as.strct.field_values[i]);
                    value_free(&objr.value);
                    return eval_ok(result);
                }
            }
            char *err = NULL;
            (void)asprintf(&err, "struct has no field '%s'", expr->as.field_access.field);
            value_free(&objr.value);
            return eval_err(err);
        }

        case EXPR_INDEX: {
            EvalResult objr = eval_expr(ev, expr->as.index.object);
            if (!IS_OK(objr)) return objr;
            EvalResult idxr = eval_expr(ev, expr->as.index.index);
            if (!IS_OK(idxr)) { value_free(&objr.value); return idxr; }
            if (objr.value.type == VAL_ARRAY && idxr.value.type == VAL_INT) {
                size_t idx = (size_t)idxr.value.as.int_val;
                value_free(&idxr.value);
                if (idx >= objr.value.as.array.len) {
                    char *err = NULL;
                    (void)asprintf(&err, "index %zu out of bounds (length %zu)",
                                   idx, objr.value.as.array.len);
                    value_free(&objr.value);
                    return eval_err(err);
                }
                LatValue result = value_deep_clone(&objr.value.as.array.elems[idx]);
                value_free(&objr.value);
                return eval_ok(result);
            }
            if (objr.value.type == VAL_STR && idxr.value.type == VAL_INT) {
                size_t idx = (size_t)idxr.value.as.int_val;
                value_free(&idxr.value);
                size_t slen = strlen(objr.value.as.str_val);
                if (idx >= slen) {
                    char *err = NULL;
                    (void)asprintf(&err, "string index %zu out of bounds (length %zu)", idx, slen);
                    value_free(&objr.value);
                    return eval_err(err);
                }
                char buf[2] = { objr.value.as.str_val[idx], '\0' };
                value_free(&objr.value);
                return eval_ok(value_string(buf));
            }
            /* String slicing with range */
            if (objr.value.type == VAL_STR && idxr.value.type == VAL_RANGE) {
                char *sliced = lat_str_substring(objr.value.as.str_val,
                                                  idxr.value.as.range.start,
                                                  idxr.value.as.range.end);
                value_free(&objr.value);
                value_free(&idxr.value);
                return eval_ok(value_string_owned(sliced));
            }
            /* Map indexing: map["key"] */
            if (objr.value.type == VAL_MAP && idxr.value.type == VAL_STR) {
                LatValue *found = (LatValue *)lat_map_get(objr.value.as.map.map, idxr.value.as.str_val);
                LatValue result = found ? value_deep_clone(found) : value_unit();
                value_free(&objr.value);
                value_free(&idxr.value);
                return eval_ok(result);
            }
            char *err = NULL;
            (void)asprintf(&err, "cannot index %s with %s",
                           value_type_name(&objr.value), value_type_name(&idxr.value));
            value_free(&objr.value);
            value_free(&idxr.value);
            return eval_err(err);
        }

        case EXPR_ARRAY: {
            size_t n = expr->as.array.count;
            LatValue *elems = malloc(n * sizeof(LatValue));
            for (size_t i = 0; i < n; i++) {
                EvalResult er = eval_expr(ev, expr->as.array.elems[i]);
                if (!IS_OK(er)) {
                    for (size_t j = 0; j < i; j++) value_free(&elems[j]);
                    free(elems);
                    return er;
                }
                elems[i] = er.value;
            }
            stats_array(&ev->stats);
            gc_maybe_collect(ev);
            LatValue arr = value_array(elems, n);
            free(elems);
            return eval_ok(arr);
        }

        case EXPR_STRUCT_LIT: {
            const char *sname = expr->as.struct_lit.name;
            size_t fc = expr->as.struct_lit.field_count;
            /* Validate fields if struct def is registered */
            StructDecl *sd = find_struct(ev, sname);
            if (sd) {
                for (size_t i = 0; i < fc; i++) {
                    bool found = false;
                    for (size_t j = 0; j < sd->field_count; j++) {
                        if (strcmp(expr->as.struct_lit.fields[i].name, sd->fields[j].name) == 0) {
                            found = true; break;
                        }
                    }
                    if (!found) {
                        char *err = NULL;
                        (void)asprintf(&err, "struct '%s' has no field '%s'",
                                       sname, expr->as.struct_lit.fields[i].name);
                        return eval_err(err);
                    }
                }
            }
            char **names = malloc(fc * sizeof(char *));
            LatValue *vals = malloc(fc * sizeof(LatValue));
            for (size_t i = 0; i < fc; i++) {
                names[i] = expr->as.struct_lit.fields[i].name;
                EvalResult er = eval_expr(ev, expr->as.struct_lit.fields[i].value);
                if (!IS_OK(er)) {
                    for (size_t j = 0; j < i; j++) value_free(&vals[j]);
                    free(names); free(vals);
                    return er;
                }
                vals[i] = er.value;
            }
            stats_struct(&ev->stats);
            gc_maybe_collect(ev);
            LatValue st = value_struct(sname, names, vals, fc);
            free(names); free(vals);
            return eval_ok(st);
        }

        case EXPR_FREEZE: {
            stats_freeze(&ev->stats);
            /* In strict mode, consuming freeze on ident */
            if (ev->mode == MODE_STRICT && expr->as.freeze_expr->tag == EXPR_IDENT) {
                const char *name = expr->as.freeze_expr->as.str_val;
                LatValue val;
                if (!env_remove(ev->env, name, &val)) {
                    char *err = NULL;
                    (void)asprintf(&err, "undefined variable '%s'", name);
                    return eval_err(err);
                }
                return eval_ok(value_freeze(val));
            }
            EvalResult er = eval_expr(ev, expr->as.freeze_expr);
            if (!IS_OK(er)) return er;
            return eval_ok(value_freeze(er.value));
        }

        case EXPR_THAW: {
            stats_thaw(&ev->stats);
            EvalResult er = eval_expr(ev, expr->as.freeze_expr);
            if (!IS_OK(er)) return er;
            LatValue thawed = value_thaw(&er.value);
            value_free(&er.value);
            return eval_ok(thawed);
        }

        case EXPR_CLONE: {
            stats_deep_clone(&ev->stats);
            EvalResult er = eval_expr(ev, expr->as.freeze_expr);
            if (!IS_OK(er)) return er;
            LatValue cloned = value_deep_clone(&er.value);
            value_free(&er.value);
            return eval_ok(cloned);
        }

        case EXPR_FORGE: {
            stats_forge(&ev->stats);
            stats_scope_push(&ev->stats);
            env_push_scope(ev->env);
            EvalResult result = eval_block_stmts(ev, expr->as.block.stmts, expr->as.block.count);
            env_pop_scope(ev->env);
            stats_scope_pop(&ev->stats);
            stats_freeze(&ev->stats);
            if (IS_OK(result)) {
                return eval_ok(value_freeze(result.value));
            }
            if (IS_SIGNAL(result) && result.cf.tag == CF_RETURN) {
                return eval_ok(value_freeze(result.cf.value));
            }
            return result;
        }

        case EXPR_IF: {
            EvalResult condr = eval_expr(ev, expr->as.if_expr.cond);
            if (!IS_OK(condr)) return condr;
            bool truthy = value_is_truthy(&condr.value);
            value_free(&condr.value);
            if (truthy) {
                stats_scope_push(&ev->stats);
                env_push_scope(ev->env);
                EvalResult r = eval_block_stmts(ev, expr->as.if_expr.then_stmts,
                                                 expr->as.if_expr.then_count);
                env_pop_scope(ev->env);
                stats_scope_pop(&ev->stats);
                return r;
            } else if (expr->as.if_expr.else_stmts) {
                stats_scope_push(&ev->stats);
                env_push_scope(ev->env);
                EvalResult r = eval_block_stmts(ev, expr->as.if_expr.else_stmts,
                                                 expr->as.if_expr.else_count);
                env_pop_scope(ev->env);
                stats_scope_pop(&ev->stats);
                return r;
            }
            return eval_ok(value_unit());
        }

        case EXPR_BLOCK: {
            stats_scope_push(&ev->stats);
            env_push_scope(ev->env);
            EvalResult r = eval_block_stmts(ev, expr->as.block.stmts, expr->as.block.count);
            env_pop_scope(ev->env);
            stats_scope_pop(&ev->stats);
            return r;
        }

        case EXPR_CLOSURE: {
            stats_closure(&ev->stats);
            gc_maybe_collect(ev);
            Env *captured = env_clone(ev->env);
            return eval_ok(value_closure(
                expr->as.closure.params,
                expr->as.closure.param_count,
                expr->as.closure.body,
                captured));
        }

        case EXPR_RANGE: {
            EvalResult sr = eval_expr(ev, expr->as.range.start);
            if (!IS_OK(sr)) return sr;
            EvalResult er = eval_expr(ev, expr->as.range.end);
            if (!IS_OK(er)) { value_free(&sr.value); return er; }
            if (sr.value.type != VAL_INT || er.value.type != VAL_INT) {
                value_free(&sr.value); value_free(&er.value);
                return eval_err(strdup("range bounds must be integers"));
            }
            int64_t s = sr.value.as.int_val, e = er.value.as.int_val;
            return eval_ok(value_range(s, e));
        }

        case EXPR_PRINT: {
            for (size_t i = 0; i < expr->as.print.arg_count; i++) {
                if (i > 0) printf(" ");
                EvalResult er = eval_expr(ev, expr->as.print.args[i]);
                if (!IS_OK(er)) return er;
                char *s = value_display(&er.value);
                printf("%s", s);
                free(s);
                value_free(&er.value);
            }
            printf("\n");
            return eval_ok(value_unit());
        }

        case EXPR_TRY_CATCH: {
            stats_scope_push(&ev->stats);
            env_push_scope(ev->env);
            EvalResult tr = eval_block_stmts(ev, expr->as.try_catch.try_stmts,
                                              expr->as.try_catch.try_count);
            env_pop_scope(ev->env);
            stats_scope_pop(&ev->stats);
            if (IS_ERR(tr)) {
                /* Error: bind to catch variable and execute catch block */
                stats_scope_push(&ev->stats);
                env_push_scope(ev->env);
                env_define(ev->env, expr->as.try_catch.catch_var, value_string(tr.error));
                free(tr.error);
                EvalResult cr = eval_block_stmts(ev, expr->as.try_catch.catch_stmts,
                                                  expr->as.try_catch.catch_count);
                env_pop_scope(ev->env);
                stats_scope_pop(&ev->stats);
                return cr;
            }
            return tr;
        }

        case EXPR_SPAWN: {
            stats_scope_push(&ev->stats);
            env_push_scope(ev->env);
            EvalResult result = eval_block_stmts(ev, expr->as.block.stmts, expr->as.block.count);
            env_pop_scope(ev->env);
            stats_scope_pop(&ev->stats);
            if (IS_SIGNAL(result) && result.cf.tag == CF_RETURN) {
                return eval_ok(result.cf.value);
            }
            return result;
        }
    }
    return eval_err(strdup("unknown expression type"));
}

/* ── Statement evaluation ── */

static EvalResult eval_stmt(Evaluator *ev, const Stmt *stmt) {
    switch (stmt->tag) {
        case STMT_BINDING: {
            EvalResult vr = eval_expr(ev, stmt->as.binding.value);
            if (!IS_OK(vr)) return vr;

            if (ev->mode == MODE_CASUAL) {
                switch (stmt->as.binding.phase) {
                    case PHASE_FLUID: vr.value.phase = VTAG_FLUID; break;
                    case PHASE_CRYSTAL:
                        stats_freeze(&ev->stats);
                        vr.value = value_freeze(vr.value);
                        break;
                    case PHASE_UNSPECIFIED: break;
                }
            } else { /* MODE_STRICT */
                switch (stmt->as.binding.phase) {
                    case PHASE_FLUID:
                        if (value_is_crystal(&vr.value)) {
                            char *err = NULL;
                            (void)asprintf(&err, "strict mode: 'flux' binding '%s' produced a crystal value",
                                           stmt->as.binding.name);
                            value_free(&vr.value);
                            return eval_err(err);
                        }
                        vr.value.phase = VTAG_FLUID;
                        break;
                    case PHASE_CRYSTAL:
                        stats_freeze(&ev->stats);
                        vr.value = value_freeze(vr.value);
                        break;
                    case PHASE_UNSPECIFIED: {
                        char *err = NULL;
                        (void)asprintf(&err, "strict mode: binding '%s' requires an explicit phase (flux/fix)",
                                       stmt->as.binding.name);
                        value_free(&vr.value);
                        return eval_err(err);
                    }
                }
            }
            stats_binding(&ev->stats);
            /* In lat_eval context, top-level bindings go to the root scope
             * so they persist across calls (needed for REPL) */
            if (ev->lat_eval_scope > 0 && ev->env->count == ev->lat_eval_scope)
                env_define_at(ev->env, 0, stmt->as.binding.name, vr.value);
            else
                env_define(ev->env, stmt->as.binding.name, vr.value);
            return eval_ok(value_unit());
        }

        case STMT_ASSIGN: {
            EvalResult valr = eval_expr(ev, stmt->as.assign.value);
            if (!IS_OK(valr)) return valr;

            /* Simple ident assignment uses env_set for proper scoping */
            if (stmt->as.assign.target->tag == EXPR_IDENT) {
                const char *name = stmt->as.assign.target->as.str_val;
                if (ev->mode == MODE_STRICT) {
                    LatValue existing;
                    if (env_get(ev->env, name, &existing)) {
                        bool is_crys = value_is_crystal(&existing);
                        value_free(&existing);
                        if (is_crys) {
                            char *err = NULL;
                            (void)asprintf(&err, "strict mode: cannot assign to crystal binding '%s'", name);
                            value_free(&valr.value);
                            return eval_err(err);
                        }
                    }
                }
                if (!env_set(ev->env, name, valr.value)) {
                    char *err = NULL;
                    (void)asprintf(&err, "undefined variable '%s'", name);
                    return eval_err(err);
                }
                return eval_ok(value_unit());
            }

            /* For field access, index, and nested chains: use resolve_lvalue */
            char *lv_err = NULL;
            LatValue *target = resolve_lvalue(ev, stmt->as.assign.target, &lv_err);
            if (!target) {
                value_free(&valr.value);
                return eval_err(lv_err);
            }
            if (ev->mode == MODE_STRICT && value_is_crystal(target)) {
                value_free(&valr.value);
                return eval_err(strdup("strict mode: cannot assign to crystal value"));
            }
            value_free(target);
            *target = valr.value;
            return eval_ok(value_unit());
        }

        case STMT_EXPR:
            return eval_expr(ev, stmt->as.expr);

        case STMT_RETURN: {
            if (stmt->as.return_expr) {
                EvalResult er = eval_expr(ev, stmt->as.return_expr);
                if (!IS_OK(er)) return er;
                return eval_signal(CF_RETURN, er.value);
            }
            return eval_signal(CF_RETURN, value_unit());
        }

        case STMT_FOR: {
            EvalResult iter_r = eval_expr(ev, stmt->as.for_loop.iter);
            if (!IS_OK(iter_r)) return iter_r;

            if (iter_r.value.type == VAL_RANGE) {
                int64_t s = iter_r.value.as.range.start;
                int64_t e = iter_r.value.as.range.end;
                value_free(&iter_r.value);
                for (int64_t i = s; i < e; i++) {
                    stats_scope_push(&ev->stats);
                    env_push_scope(ev->env);
                    env_define(ev->env, stmt->as.for_loop.var, value_int(i));
                    EvalResult r = eval_block_stmts(ev, stmt->as.for_loop.body,
                                                     stmt->as.for_loop.body_count);
                    env_pop_scope(ev->env);
                    stats_scope_pop(&ev->stats);
                    if (IS_SIGNAL(r) && r.cf.tag == CF_BREAK) break;
                    if (IS_SIGNAL(r) && r.cf.tag == CF_CONTINUE) continue;
                    if (!IS_OK(r)) return r;
                    value_free(&r.value);
                }
            } else if (iter_r.value.type == VAL_ARRAY) {
                size_t len = iter_r.value.as.array.len;
                for (size_t i = 0; i < len; i++) {
                    stats_scope_push(&ev->stats);
                    env_push_scope(ev->env);
                    LatValue elem = value_deep_clone(&iter_r.value.as.array.elems[i]);
                    env_define(ev->env, stmt->as.for_loop.var, elem);
                    EvalResult r = eval_block_stmts(ev, stmt->as.for_loop.body,
                                                     stmt->as.for_loop.body_count);
                    env_pop_scope(ev->env);
                    stats_scope_pop(&ev->stats);
                    if (IS_SIGNAL(r) && r.cf.tag == CF_BREAK) break;
                    if (IS_SIGNAL(r) && r.cf.tag == CF_CONTINUE) continue;
                    if (!IS_OK(r)) { value_free(&iter_r.value); return r; }
                    value_free(&r.value);
                }
                value_free(&iter_r.value);
            } else if (iter_r.value.type == VAL_MAP) {
                /* Iterate over map keys */
                for (size_t i = 0; i < iter_r.value.as.map.map->cap; i++) {
                    if (iter_r.value.as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                    stats_scope_push(&ev->stats);
                    env_push_scope(ev->env);
                    env_define(ev->env, stmt->as.for_loop.var,
                               value_string(iter_r.value.as.map.map->entries[i].key));
                    EvalResult r = eval_block_stmts(ev, stmt->as.for_loop.body,
                                                     stmt->as.for_loop.body_count);
                    env_pop_scope(ev->env);
                    stats_scope_pop(&ev->stats);
                    if (IS_SIGNAL(r) && r.cf.tag == CF_BREAK) break;
                    if (IS_SIGNAL(r) && r.cf.tag == CF_CONTINUE) continue;
                    if (!IS_OK(r)) { value_free(&iter_r.value); return r; }
                    value_free(&r.value);
                }
                value_free(&iter_r.value);
            } else {
                char *err = NULL;
                (void)asprintf(&err, "cannot iterate over %s", value_type_name(&iter_r.value));
                value_free(&iter_r.value);
                return eval_err(err);
            }
            return eval_ok(value_unit());
        }

        case STMT_WHILE: {
            for (;;) {
                EvalResult condr = eval_expr(ev, stmt->as.while_loop.cond);
                if (!IS_OK(condr)) return condr;
                bool truthy = value_is_truthy(&condr.value);
                value_free(&condr.value);
                if (!truthy) break;
                stats_scope_push(&ev->stats);
                env_push_scope(ev->env);
                EvalResult r = eval_block_stmts(ev, stmt->as.while_loop.body,
                                                 stmt->as.while_loop.body_count);
                env_pop_scope(ev->env);
                stats_scope_pop(&ev->stats);
                if (IS_SIGNAL(r) && r.cf.tag == CF_BREAK) break;
                if (IS_SIGNAL(r) && r.cf.tag == CF_CONTINUE) continue;
                if (!IS_OK(r)) return r;
                value_free(&r.value);
            }
            return eval_ok(value_unit());
        }

        case STMT_LOOP: {
            for (;;) {
                stats_scope_push(&ev->stats);
                env_push_scope(ev->env);
                EvalResult r = eval_block_stmts(ev, stmt->as.loop.body, stmt->as.loop.body_count);
                env_pop_scope(ev->env);
                stats_scope_pop(&ev->stats);
                if (IS_SIGNAL(r) && r.cf.tag == CF_BREAK) break;
                if (IS_SIGNAL(r) && r.cf.tag == CF_CONTINUE) continue;
                if (!IS_OK(r)) return r;
                value_free(&r.value);
            }
            return eval_ok(value_unit());
        }

        case STMT_BREAK:    return eval_signal(CF_BREAK, value_unit());
        case STMT_CONTINUE: return eval_signal(CF_CONTINUE, value_unit());
    }
    return eval_err(strdup("unknown statement type"));
}

static EvalResult eval_block_stmts(Evaluator *ev, Stmt **stmts, size_t count) {
    LatValue last = value_unit();
    for (size_t i = 0; i < count; i++) {
        value_free(&last);
        EvalResult r = eval_stmt(ev, stmts[i]);
        if (!IS_OK(r)) return r;
        last = r.value;
    }
    return eval_ok(last);
}

/* ── Method calls ── */

static EvalResult eval_method_call(Evaluator *ev, LatValue obj, const char *method,
                                   LatValue *args, size_t arg_count) {
    if (strcmp(method, "push") == 0) {
        if (obj.type != VAL_ARRAY)
            return eval_err(strdup(".push() is not defined on non-array"));
        if (value_is_crystal(&obj))
            return eval_err(strdup("cannot push to a crystal array"));
        if (arg_count != 1)
            return eval_err(strdup(".push() expects exactly 1 argument"));
        /* We need to push to the actual array in the env. The obj here is already freed copy.
           For method calls that mutate, we'd need the original. Instead, let's not support
           standalone push -- in the Rust version it uses Rc<RefCell> for shared mutation.
           For C, we'll handle array.push by finding the original binding. */
        /* This is a simplification - we handle push on the env directly */
        return eval_ok(value_unit());
    }
    if (strcmp(method, "len") == 0) {
        if (obj.type == VAL_ARRAY) return eval_ok(value_int((int64_t)obj.as.array.len));
        if (obj.type == VAL_STR) return eval_ok(value_int((int64_t)strlen(obj.as.str_val)));
        if (obj.type == VAL_MAP) return eval_ok(value_int((int64_t)lat_map_len(obj.as.map.map)));
        return eval_err(strdup(".len() is not defined on this type"));
    }
    if (strcmp(method, "map") == 0) {
        if (obj.type != VAL_ARRAY) return eval_err(strdup(".map() is not defined on non-array"));
        if (arg_count != 1) return eval_err(strdup(".map() expects exactly 1 argument (a closure)"));
        if (args[0].type != VAL_CLOSURE) return eval_err(strdup(".map() argument must be a closure"));

        size_t n = obj.as.array.len;
        LatValue *results = malloc(n * sizeof(LatValue));
        for (size_t i = 0; i < n; i++) {
            LatValue elem = value_deep_clone(&obj.as.array.elems[i]);
            EvalResult r = call_closure(ev,
                args[0].as.closure.param_names,
                args[0].as.closure.param_count,
                args[0].as.closure.body,
                args[0].as.closure.captured_env,
                &elem, 1);
            if (!IS_OK(r)) {
                for (size_t j = 0; j < i; j++) value_free(&results[j]);
                free(results);
                return r;
            }
            results[i] = r.value;
        }
        LatValue arr = value_array(results, n);
        free(results);
        return eval_ok(arr);
    }
    if (strcmp(method, "join") == 0) {
        if (obj.type != VAL_ARRAY) return eval_err(strdup(".join() is not defined on non-array"));
        const char *sep = "";
        if (arg_count > 0) {
            if (args[0].type != VAL_STR) return eval_err(strdup(".join() separator must be a string"));
            sep = args[0].as.str_val;
        }
        size_t total = 0;
        char **parts = malloc(obj.as.array.len * sizeof(char *));
        for (size_t i = 0; i < obj.as.array.len; i++) {
            parts[i] = value_display(&obj.as.array.elems[i]);
            total += strlen(parts[i]);
        }
        size_t sep_len = strlen(sep);
        if (obj.as.array.len > 0) total += sep_len * (obj.as.array.len - 1);
        char *result = malloc(total + 1);
        size_t pos = 0;
        for (size_t i = 0; i < obj.as.array.len; i++) {
            if (i > 0) { memcpy(result + pos, sep, sep_len); pos += sep_len; }
            size_t pl = strlen(parts[i]);
            memcpy(result + pos, parts[i], pl);
            pos += pl;
            free(parts[i]);
        }
        result[pos] = '\0';
        free(parts);
        return eval_ok(value_string_owned(result));
    }
    /* ── Array: filter ── */
    if (strcmp(method, "filter") == 0) {
        if (obj.type != VAL_ARRAY) return eval_err(strdup(".filter() is not defined on non-array"));
        if (arg_count != 1 || args[0].type != VAL_CLOSURE) return eval_err(strdup(".filter() expects 1 closure argument"));
        size_t n = obj.as.array.len;
        LatValue *results = malloc((n > 0 ? n : 1) * sizeof(LatValue));
        size_t rcount = 0;
        for (size_t i = 0; i < n; i++) {
            LatValue elem = value_deep_clone(&obj.as.array.elems[i]);
            EvalResult r = call_closure(ev,
                args[0].as.closure.param_names,
                args[0].as.closure.param_count,
                args[0].as.closure.body,
                args[0].as.closure.captured_env,
                &elem, 1);
            if (!IS_OK(r)) {
                for (size_t j = 0; j < rcount; j++) value_free(&results[j]);
                free(results);
                return r;
            }
            if (value_is_truthy(&r.value)) {
                results[rcount++] = value_deep_clone(&obj.as.array.elems[i]);
            }
            value_free(&r.value);
        }
        LatValue arr = value_array(results, rcount);
        free(results);
        return eval_ok(arr);
    }
    /* ── Array: for_each ── */
    if (strcmp(method, "for_each") == 0) {
        if (obj.type != VAL_ARRAY) return eval_err(strdup(".for_each() is not defined on non-array"));
        if (arg_count != 1 || args[0].type != VAL_CLOSURE) return eval_err(strdup(".for_each() expects 1 closure argument"));
        for (size_t i = 0; i < obj.as.array.len; i++) {
            LatValue elem = value_deep_clone(&obj.as.array.elems[i]);
            EvalResult r = call_closure(ev,
                args[0].as.closure.param_names,
                args[0].as.closure.param_count,
                args[0].as.closure.body,
                args[0].as.closure.captured_env,
                &elem, 1);
            if (!IS_OK(r)) return r;
            value_free(&r.value);
        }
        return eval_ok(value_unit());
    }
    /* ── Array: find ── */
    if (strcmp(method, "find") == 0) {
        if (obj.type != VAL_ARRAY) return eval_err(strdup(".find() is not defined on non-array"));
        if (arg_count != 1 || args[0].type != VAL_CLOSURE) return eval_err(strdup(".find() expects 1 closure argument"));
        for (size_t i = 0; i < obj.as.array.len; i++) {
            LatValue elem = value_deep_clone(&obj.as.array.elems[i]);
            EvalResult r = call_closure(ev,
                args[0].as.closure.param_names,
                args[0].as.closure.param_count,
                args[0].as.closure.body,
                args[0].as.closure.captured_env,
                &elem, 1);
            if (!IS_OK(r)) return r;
            if (value_is_truthy(&r.value)) {
                value_free(&r.value);
                return eval_ok(value_deep_clone(&obj.as.array.elems[i]));
            }
            value_free(&r.value);
        }
        return eval_ok(value_unit());
    }
    /* ── Array: contains ── */
    if (strcmp(method, "contains") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 1) return eval_err(strdup(".contains() expects 1 argument"));
        for (size_t i = 0; i < obj.as.array.len; i++) {
            if (value_eq(&obj.as.array.elems[i], &args[0])) {
                return eval_ok(value_bool(true));
            }
        }
        return eval_ok(value_bool(false));
    }
    /* ── Array: reverse ── */
    if (strcmp(method, "reverse") == 0 && obj.type == VAL_ARRAY) {
        size_t n = obj.as.array.len;
        LatValue *reversed = malloc((n > 0 ? n : 1) * sizeof(LatValue));
        for (size_t i = 0; i < n; i++) {
            reversed[i] = value_deep_clone(&obj.as.array.elems[n - 1 - i]);
        }
        LatValue arr = value_array(reversed, n);
        free(reversed);
        return eval_ok(arr);
    }
    /* ── Array: enumerate ── */
    if (strcmp(method, "enumerate") == 0) {
        if (obj.type != VAL_ARRAY) return eval_err(strdup(".enumerate() is not defined on non-array"));
        size_t n = obj.as.array.len;
        LatValue *pairs = malloc((n > 0 ? n : 1) * sizeof(LatValue));
        for (size_t i = 0; i < n; i++) {
            LatValue pair_elems[2];
            pair_elems[0] = value_int((int64_t)i);
            pair_elems[1] = value_deep_clone(&obj.as.array.elems[i]);
            pairs[i] = value_array(pair_elems, 2);
        }
        LatValue arr = value_array(pairs, n);
        free(pairs);
        return eval_ok(arr);
    }
    /* ── Map methods ── */
    if (obj.type == VAL_MAP) {
        if (strcmp(method, "get") == 0) {
            if (arg_count != 1 || args[0].type != VAL_STR)
                return eval_err(strdup(".get() expects 1 string argument"));
            LatValue *found = (LatValue *)lat_map_get(obj.as.map.map, args[0].as.str_val);
            return eval_ok(found ? value_deep_clone(found) : value_unit());
        }
        if (strcmp(method, "has") == 0) {
            if (arg_count != 1 || args[0].type != VAL_STR)
                return eval_err(strdup(".has() expects 1 string argument"));
            return eval_ok(value_bool(lat_map_contains(obj.as.map.map, args[0].as.str_val)));
        }
        if (strcmp(method, "keys") == 0) {
            size_t n = lat_map_len(obj.as.map.map);
            LatValue *keys = malloc((n > 0 ? n : 1) * sizeof(LatValue));
            size_t ki = 0;
            for (size_t i = 0; i < obj.as.map.map->cap; i++) {
                if (obj.as.map.map->entries[i].state == MAP_OCCUPIED) {
                    keys[ki++] = value_string(obj.as.map.map->entries[i].key);
                }
            }
            LatValue arr = value_array(keys, ki);
            free(keys);
            return eval_ok(arr);
        }
        if (strcmp(method, "values") == 0) {
            size_t n = lat_map_len(obj.as.map.map);
            LatValue *vals = malloc((n > 0 ? n : 1) * sizeof(LatValue));
            size_t vi = 0;
            for (size_t i = 0; i < obj.as.map.map->cap; i++) {
                if (obj.as.map.map->entries[i].state == MAP_OCCUPIED) {
                    vals[vi++] = value_deep_clone((LatValue *)obj.as.map.map->entries[i].value);
                }
            }
            LatValue arr = value_array(vals, vi);
            free(vals);
            return eval_ok(arr);
        }
        if (strcmp(method, "len") == 0) {
            return eval_ok(value_int((int64_t)lat_map_len(obj.as.map.map)));
        }
    }
    /* ── String methods ── */
    if (obj.type == VAL_STR) {
        if (strcmp(method, "contains") == 0) {
            if (arg_count != 1 || args[0].type != VAL_STR)
                return eval_err(strdup(".contains() expects 1 string argument"));
            return eval_ok(value_bool(lat_str_contains(obj.as.str_val, args[0].as.str_val)));
        }
        if (strcmp(method, "starts_with") == 0) {
            if (arg_count != 1 || args[0].type != VAL_STR)
                return eval_err(strdup(".starts_with() expects 1 string argument"));
            return eval_ok(value_bool(lat_str_starts_with(obj.as.str_val, args[0].as.str_val)));
        }
        if (strcmp(method, "ends_with") == 0) {
            if (arg_count != 1 || args[0].type != VAL_STR)
                return eval_err(strdup(".ends_with() expects 1 string argument"));
            return eval_ok(value_bool(lat_str_ends_with(obj.as.str_val, args[0].as.str_val)));
        }
        if (strcmp(method, "trim") == 0) {
            return eval_ok(value_string_owned(lat_str_trim(obj.as.str_val)));
        }
        if (strcmp(method, "to_upper") == 0) {
            return eval_ok(value_string_owned(lat_str_to_upper(obj.as.str_val)));
        }
        if (strcmp(method, "to_lower") == 0) {
            return eval_ok(value_string_owned(lat_str_to_lower(obj.as.str_val)));
        }
        if (strcmp(method, "replace") == 0) {
            if (arg_count != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR)
                return eval_err(strdup(".replace() expects 2 string arguments"));
            return eval_ok(value_string_owned(lat_str_replace(obj.as.str_val, args[0].as.str_val, args[1].as.str_val)));
        }
        if (strcmp(method, "split") == 0) {
            if (arg_count != 1 || args[0].type != VAL_STR)
                return eval_err(strdup(".split() expects 1 string argument"));
            size_t count;
            char **parts = lat_str_split(obj.as.str_val, args[0].as.str_val, &count);
            LatValue *elems = malloc(count * sizeof(LatValue));
            for (size_t i = 0; i < count; i++) {
                elems[i] = value_string_owned(parts[i]);
            }
            free(parts);
            LatValue arr = value_array(elems, count);
            free(elems);
            return eval_ok(arr);
        }
        if (strcmp(method, "index_of") == 0) {
            if (arg_count != 1 || args[0].type != VAL_STR)
                return eval_err(strdup(".index_of() expects 1 string argument"));
            return eval_ok(value_int(lat_str_index_of(obj.as.str_val, args[0].as.str_val)));
        }
        if (strcmp(method, "substring") == 0) {
            if (arg_count != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT)
                return eval_err(strdup(".substring() expects 2 integer arguments"));
            return eval_ok(value_string_owned(lat_str_substring(obj.as.str_val, args[0].as.int_val, args[1].as.int_val)));
        }
        if (strcmp(method, "chars") == 0) {
            size_t slen = strlen(obj.as.str_val);
            LatValue *elems = malloc((slen > 0 ? slen : 1) * sizeof(LatValue));
            for (size_t i = 0; i < slen; i++) {
                char buf[2] = { obj.as.str_val[i], '\0' };
                elems[i] = value_string(buf);
            }
            LatValue arr = value_array(elems, slen);
            free(elems);
            return eval_ok(arr);
        }
        if (strcmp(method, "reverse") == 0) {
            return eval_ok(value_string_owned(lat_str_reverse(obj.as.str_val)));
        }
        if (strcmp(method, "repeat") == 0) {
            if (arg_count != 1 || args[0].type != VAL_INT)
                return eval_err(strdup(".repeat() expects 1 integer argument"));
            return eval_ok(value_string_owned(lat_str_repeat(obj.as.str_val, (size_t)args[0].as.int_val)));
        }
    }

    if (strcmp(method, "get") == 0) {
        if (obj.type != VAL_STRUCT) return eval_err(strdup(".get() is not defined on non-struct"));
        if (arg_count != 1) return eval_err(strdup(".get() expects exactly 1 argument"));
        if (args[0].type != VAL_STR) return eval_err(strdup(".get() key must be a string"));
        for (size_t i = 0; i < obj.as.strct.field_count; i++) {
            if (strcmp(obj.as.strct.field_names[i], args[0].as.str_val) == 0) {
                return eval_ok(value_deep_clone(&obj.as.strct.field_values[i]));
            }
        }
        char *err = NULL;
        (void)asprintf(&err, "struct has no field '%s'", args[0].as.str_val);
        return eval_err(err);
    }

    /* ── Callable struct fields: obj.method(args) where field is a closure ── */
    if (obj.type == VAL_STRUCT) {
        for (size_t i = 0; i < obj.as.strct.field_count; i++) {
            if (strcmp(obj.as.strct.field_names[i], method) == 0 &&
                obj.as.strct.field_values[i].type == VAL_CLOSURE) {
                LatValue *cl = &obj.as.strct.field_values[i];
                /* Prepend self (the struct) as the first argument */
                size_t total = 1 + arg_count;
                LatValue *full_args = malloc(total * sizeof(LatValue));
                full_args[0] = value_deep_clone(&obj);
                for (size_t j = 0; j < arg_count; j++) {
                    full_args[j + 1] = value_deep_clone(&args[j]);
                }
                EvalResult r = call_closure(ev,
                    cl->as.closure.param_names, cl->as.closure.param_count,
                    cl->as.closure.body, cl->as.closure.captured_env,
                    full_args, total);
                /* call_closure takes ownership of args via env_define;
                   env_pop_scope frees them — do NOT value_free here */
                free(full_args);
                return r;
            }
        }
    }

    char *err = NULL;
    (void)asprintf(&err, "unknown method '.%s()' on %s", method, value_type_name(&obj));
    return eval_err(err);
}

/* ── Evaluator lifecycle ── */

Evaluator *evaluator_new(void) {
    Evaluator *ev = calloc(1, sizeof(Evaluator));
    ev->env = env_new();
    ev->mode = MODE_CASUAL;
    ev->struct_defs = lat_map_new(sizeof(StructDecl *));
    ev->fn_defs = lat_map_new(sizeof(FnDecl *));
    stats_init(&ev->stats);
    ev->heap = dual_heap_new();
    ev->gc_roots = lat_vec_new(sizeof(LatValue *));
    ev->gc_stress = false;
    return ev;
}

void evaluator_free(Evaluator *ev) {
    if (!ev) return;
    env_free(ev->env);
    lat_map_free(&ev->struct_defs);
    lat_map_free(&ev->fn_defs);
    dual_heap_free(ev->heap);
    lat_vec_free(&ev->gc_roots);
    free(ev);
}

void evaluator_set_gc_stress(Evaluator *ev, bool enabled) {
    ev->gc_stress = enabled;
}

char *evaluator_run(Evaluator *ev, const Program *prog) {
    ev->mode = prog->mode;

    /* First pass: register structs and functions */
    for (size_t i = 0; i < prog->item_count; i++) {
        if (prog->items[i].tag == ITEM_STRUCT) {
            StructDecl *ptr = &prog->items[i].as.struct_decl;
            lat_map_set(&ev->struct_defs, ptr->name, &ptr);
        } else if (prog->items[i].tag == ITEM_FUNCTION) {
            FnDecl *ptr = &prog->items[i].as.fn_decl;
            lat_map_set(&ev->fn_defs, ptr->name, &ptr);
        }
    }

    /* Second pass: execute top-level statements */
    for (size_t i = 0; i < prog->item_count; i++) {
        if (prog->items[i].tag == ITEM_STMT) {
            EvalResult r = eval_stmt(ev, prog->items[i].as.stmt);
            if (IS_ERR(r)) return r.error;
            if (IS_SIGNAL(r)) return strdup("unexpected control flow at top level");
            value_free(&r.value);
        }
    }

    /* If there is a main() function, call it */
    FnDecl *main_fn = find_fn(ev, "main");
    if (main_fn) {
        EvalResult r = call_fn(ev, main_fn, NULL, 0, NULL);
        if (IS_ERR(r)) return r.error;
        value_free(&r.value);
    }

    return NULL; /* success */
}

const MemoryStats *evaluator_stats(const Evaluator *ev) {
    return &ev->stats;
}

void memory_stats_print(const MemoryStats *s, FILE *out) {
    fprintf(out, "=== Memory Statistics ===\n\n");
    fprintf(out, "Phase transitions:\n");
    fprintf(out, "  freezes:      %zu\n", s->freezes);
    fprintf(out, "  thaws:        %zu\n", s->thaws);
    fprintf(out, "  deep clones:  %zu\n", s->deep_clones);
    fprintf(out, "\nAllocations:\n");
    fprintf(out, "  arrays:       %zu\n", s->array_allocs);
    fprintf(out, "  structs:      %zu\n", s->struct_allocs);
    fprintf(out, "  closures:     %zu\n", s->closure_allocs);
    fprintf(out, "  total:        %zu\n", s->array_allocs + s->struct_allocs + s->closure_allocs);
    fprintf(out, "\nScope lifecycle:\n");
    fprintf(out, "  pushes:       %zu\n", s->scope_pushes);
    fprintf(out, "  pops:         %zu\n", s->scope_pops);
    fprintf(out, "  peak depth:   %zu\n", s->peak_scope_depth);
    fprintf(out, "\nCalls & bindings:\n");
    fprintf(out, "  bindings:     %zu\n", s->bindings_created);
    fprintf(out, "  fn calls:     %zu\n", s->fn_calls);
    fprintf(out, "  closure calls:%zu\n", s->closure_calls);
    fprintf(out, "\nForge blocks:   %zu\n", s->forge_blocks);
    fprintf(out, "\nGarbage collection:\n");
    fprintf(out, "  gc cycles:    %zu\n", s->gc_cycles);
    fprintf(out, "  swept fluid:  %zu\n", s->gc_swept_fluid);
    fprintf(out, "  swept regions:%zu\n", s->gc_swept_regions);
}
