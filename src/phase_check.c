#include "phase_check.h"
#include "ds/hashmap.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

typedef struct {
    AstMode mode;
    LatVec  errors;      /* vec of char* */
    LatMap *scope_stack;  /* array of LatMap (string -> AstPhase) */
    size_t  scope_count;
    size_t  scope_cap;
    LatMap  struct_defs;  /* name -> StructDecl* */
    LatMap  fn_defs;      /* name -> FnDecl* */
} PhaseChecker;

static void pc_init(PhaseChecker *pc, AstMode mode) {
    pc->mode = mode;
    pc->errors = lat_vec_new(sizeof(char *));
    pc->scope_cap = 8;
    pc->scope_count = 1;
    pc->scope_stack = malloc(pc->scope_cap * sizeof(LatMap));
    pc->scope_stack[0] = lat_map_new(sizeof(AstPhase));
    pc->struct_defs = lat_map_new(sizeof(StructDecl *));
    pc->fn_defs = lat_map_new(sizeof(FnDecl *));
}

static void pc_free(PhaseChecker *pc) {
    for (size_t i = 0; i < pc->scope_count; i++)
        lat_map_free(&pc->scope_stack[i]);
    free(pc->scope_stack);
    lat_map_free(&pc->struct_defs);
    lat_map_free(&pc->fn_defs);
}

static void pc_push_scope(PhaseChecker *pc) {
    if (pc->scope_count >= pc->scope_cap) {
        pc->scope_cap *= 2;
        pc->scope_stack = realloc(pc->scope_stack, pc->scope_cap * sizeof(LatMap));
    }
    pc->scope_stack[pc->scope_count++] = lat_map_new(sizeof(AstPhase));
}

static void pc_pop_scope(PhaseChecker *pc) {
    if (pc->scope_count > 1) {
        pc->scope_count--;
        lat_map_free(&pc->scope_stack[pc->scope_count]);
    }
}

static void pc_define(PhaseChecker *pc, const char *name, AstPhase phase) {
    lat_map_set(&pc->scope_stack[pc->scope_count - 1], name, &phase);
}

static AstPhase pc_lookup(const PhaseChecker *pc, const char *name) {
    for (size_t i = pc->scope_count; i > 0; i--) {
        AstPhase *p = lat_map_get(&pc->scope_stack[i - 1], name);
        if (p) return *p;
    }
    return PHASE_UNSPECIFIED;
}

static void pc_error(PhaseChecker *pc, const char *fmt, ...) {
    char *msg = NULL;
    va_list args;
    va_start(args, fmt);
    (void)vasprintf(&msg, fmt, args);
    va_end(args);
    lat_vec_push(&pc->errors, &msg);
}

/* Forward declarations */
static AstPhase pc_check_expr(PhaseChecker *pc, const Expr *expr);
static void pc_check_stmt(PhaseChecker *pc, const Stmt *stmt);
static void pc_check_spawn_stmt(PhaseChecker *pc, const Stmt *stmt);
static void pc_check_spawn_expr(PhaseChecker *pc, const Expr *expr);

static AstPhase pc_check_expr(PhaseChecker *pc, const Expr *expr) {
    switch (expr->tag) {
        case EXPR_INT_LIT: case EXPR_FLOAT_LIT: case EXPR_STRING_LIT: case EXPR_BOOL_LIT:
        case EXPR_NIL_LIT:
            return PHASE_UNSPECIFIED;

        case EXPR_IDENT:
            return pc_lookup(pc, expr->as.str_val);

        case EXPR_BINOP:
            pc_check_expr(pc, expr->as.binop.left);
            pc_check_expr(pc, expr->as.binop.right);
            return PHASE_UNSPECIFIED;

        case EXPR_UNARYOP:
            pc_check_expr(pc, expr->as.unaryop.operand);
            return PHASE_UNSPECIFIED;

        case EXPR_CALL: {
            pc_check_expr(pc, expr->as.call.func);
            AstPhase arg_phases[expr->as.call.arg_count];
            for (size_t i = 0; i < expr->as.call.arg_count; i++)
                arg_phases[i] = pc_check_expr(pc, expr->as.call.args[i]);
            /* In strict mode, check argument phases against function parameter constraints */
            if (pc->mode == MODE_STRICT && expr->as.call.func->tag == EXPR_IDENT) {
                const char *fn_name = expr->as.call.func->as.str_val;
                FnDecl **fdp = lat_map_get(&pc->fn_defs, fn_name);
                if (fdp) {
                    FnDecl *fd = *fdp;
                    /* Check all overloads — error only if no overload matches */
                    bool any_match = false;
                    for (FnDecl *cand = fd; cand; cand = cand->next_overload) {
                        bool match = true;
                        size_t check = expr->as.call.arg_count < cand->param_count
                                       ? expr->as.call.arg_count : cand->param_count;
                        for (size_t i = 0; i < check; i++) {
                            if (cand->params[i].is_variadic) break;
                            AstPhase pp = cand->params[i].ty.phase;
                            AstPhase ap = arg_phases[i];
                            if (pp == PHASE_FLUID && ap == PHASE_CRYSTAL) { match = false; break; }
                            if (pp == PHASE_CRYSTAL && ap == PHASE_FLUID) { match = false; break; }
                        }
                        if (match) { any_match = true; break; }
                    }
                    if (!any_match)
                        pc_error(pc, "strict mode: no matching overload for '%s' with given argument phases", fn_name);
                }
            }
            return PHASE_UNSPECIFIED;
        }

        case EXPR_METHOD_CALL:
            pc_check_expr(pc, expr->as.method_call.object);
            for (size_t i = 0; i < expr->as.method_call.arg_count; i++)
                pc_check_expr(pc, expr->as.method_call.args[i]);
            return PHASE_UNSPECIFIED;

        case EXPR_FIELD_ACCESS: {
            AstPhase obj_phase = pc_check_expr(pc, expr->as.field_access.object);
            return (obj_phase == PHASE_CRYSTAL) ? PHASE_CRYSTAL : obj_phase;
        }

        case EXPR_INDEX:
            pc_check_expr(pc, expr->as.index.object);
            pc_check_expr(pc, expr->as.index.index);
            return PHASE_UNSPECIFIED;

        case EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.count; i++)
                pc_check_expr(pc, expr->as.array.elems[i]);
            return PHASE_UNSPECIFIED;

        case EXPR_SPREAD:
            pc_check_expr(pc, expr->as.spread_expr);
            return PHASE_UNSPECIFIED;

        case EXPR_TUPLE:
            for (size_t i = 0; i < expr->as.tuple.count; i++)
                pc_check_expr(pc, expr->as.tuple.elems[i]);
            return PHASE_CRYSTAL;

        case EXPR_STRUCT_LIT:
            for (size_t i = 0; i < expr->as.struct_lit.field_count; i++)
                pc_check_expr(pc, expr->as.struct_lit.fields[i].value);
            return PHASE_UNSPECIFIED;

        case EXPR_FREEZE: {
            AstPhase inner = pc_check_expr(pc, expr->as.freeze.expr);
            if (expr->as.freeze.contract)
                pc_check_expr(pc, expr->as.freeze.contract);
            if (pc->mode == MODE_STRICT && inner == PHASE_CRYSTAL)
                pc_error(pc, "strict mode: cannot freeze an already crystal value");
            return PHASE_CRYSTAL;
        }

        case EXPR_THAW: {
            AstPhase inner = pc_check_expr(pc, expr->as.freeze_expr);
            if (pc->mode == MODE_STRICT && inner == PHASE_FLUID)
                pc_error(pc, "strict mode: cannot thaw an already fluid value");
            return PHASE_FLUID;
        }

        case EXPR_CLONE:
            return pc_check_expr(pc, expr->as.freeze_expr);

        case EXPR_ANNEAL:
            pc_check_expr(pc, expr->as.anneal.expr);
            pc_check_expr(pc, expr->as.anneal.closure);
            return PHASE_CRYSTAL;

        case EXPR_FORGE:
            pc_push_scope(pc);
            for (size_t i = 0; i < expr->as.block.count; i++)
                pc_check_stmt(pc, expr->as.block.stmts[i]);
            pc_pop_scope(pc);
            return PHASE_CRYSTAL;

        case EXPR_CRYSTALLIZE:
            pc_check_expr(pc, expr->as.crystallize.expr);
            pc_push_scope(pc);
            for (size_t i = 0; i < expr->as.crystallize.body_count; i++)
                pc_check_stmt(pc, expr->as.crystallize.body[i]);
            pc_pop_scope(pc);
            return PHASE_UNSPECIFIED;

        case EXPR_SUBLIMATE:
            return pc_check_expr(pc, expr->as.freeze_expr);

        case EXPR_IF:
            pc_check_expr(pc, expr->as.if_expr.cond);
            pc_push_scope(pc);
            for (size_t i = 0; i < expr->as.if_expr.then_count; i++)
                pc_check_stmt(pc, expr->as.if_expr.then_stmts[i]);
            pc_pop_scope(pc);
            if (expr->as.if_expr.else_stmts) {
                pc_push_scope(pc);
                for (size_t i = 0; i < expr->as.if_expr.else_count; i++)
                    pc_check_stmt(pc, expr->as.if_expr.else_stmts[i]);
                pc_pop_scope(pc);
            }
            return PHASE_UNSPECIFIED;

        case EXPR_BLOCK:
            pc_push_scope(pc);
            for (size_t i = 0; i < expr->as.block.count; i++)
                pc_check_stmt(pc, expr->as.block.stmts[i]);
            pc_pop_scope(pc);
            return PHASE_UNSPECIFIED;

        case EXPR_CLOSURE:
            if (expr->as.closure.default_values) {
                for (size_t i = 0; i < expr->as.closure.param_count; i++)
                    if (expr->as.closure.default_values[i])
                        pc_check_expr(pc, expr->as.closure.default_values[i]);
            }
            return PHASE_UNSPECIFIED;

        case EXPR_RANGE:
            pc_check_expr(pc, expr->as.range.start);
            pc_check_expr(pc, expr->as.range.end);
            return PHASE_UNSPECIFIED;

        case EXPR_PRINT:
            for (size_t i = 0; i < expr->as.print.arg_count; i++)
                pc_check_expr(pc, expr->as.print.args[i]);
            return PHASE_UNSPECIFIED;

        case EXPR_TRY_CATCH:
            pc_push_scope(pc);
            for (size_t i = 0; i < expr->as.try_catch.try_count; i++)
                pc_check_stmt(pc, expr->as.try_catch.try_stmts[i]);
            pc_pop_scope(pc);
            pc_push_scope(pc);
            pc_define(pc, expr->as.try_catch.catch_var, PHASE_FLUID);
            for (size_t i = 0; i < expr->as.try_catch.catch_count; i++)
                pc_check_stmt(pc, expr->as.try_catch.catch_stmts[i]);
            pc_pop_scope(pc);
            return PHASE_UNSPECIFIED;

        case EXPR_SCOPE:
            pc_push_scope(pc);
            for (size_t i = 0; i < expr->as.block.count; i++)
                pc_check_stmt(pc, expr->as.block.stmts[i]);
            pc_pop_scope(pc);
            return PHASE_UNSPECIFIED;

        case EXPR_INTERP_STRING:
            for (size_t i = 0; i < expr->as.interp.count; i++)
                pc_check_expr(pc, expr->as.interp.exprs[i]);
            return PHASE_UNSPECIFIED;

        case EXPR_SPAWN:
            if (pc->mode == MODE_STRICT) {
                pc_push_scope(pc);
                for (size_t i = 0; i < expr->as.block.count; i++)
                    pc_check_spawn_stmt(pc, expr->as.block.stmts[i]);
                pc_pop_scope(pc);
            } else {
                pc_push_scope(pc);
                for (size_t i = 0; i < expr->as.block.count; i++)
                    pc_check_stmt(pc, expr->as.block.stmts[i]);
                pc_pop_scope(pc);
            }
            return PHASE_UNSPECIFIED;

        case EXPR_MATCH:
            pc_check_expr(pc, expr->as.match_expr.scrutinee);
            for (size_t i = 0; i < expr->as.match_expr.arm_count; i++) {
                MatchArm *arm = &expr->as.match_expr.arms[i];
                if (arm->guard) pc_check_expr(pc, arm->guard);
                for (size_t j = 0; j < arm->body_count; j++)
                    pc_check_stmt(pc, arm->body[j]);
            }
            return PHASE_UNSPECIFIED;

        case EXPR_ENUM_VARIANT:
            for (size_t i = 0; i < expr->as.enum_variant.arg_count; i++)
                pc_check_expr(pc, expr->as.enum_variant.args[i]);
            return PHASE_UNSPECIFIED;
    }
    return PHASE_UNSPECIFIED;
}

static void pc_check_stmt(PhaseChecker *pc, const Stmt *stmt) {
    switch (stmt->tag) {
        case STMT_BINDING: {
            AstPhase value_phase = pc_check_expr(pc, stmt->as.binding.value);
            if (pc->mode == MODE_STRICT) {
                switch (stmt->as.binding.phase) {
                    case PHASE_UNSPECIFIED:
                        pc_error(pc, "strict mode: use 'flux' or 'fix' instead of 'let' for binding '%s'",
                                 stmt->as.binding.name);
                        break;
                    case PHASE_FLUID:
                        if (value_phase == PHASE_CRYSTAL)
                            pc_error(pc, "cannot bind crystal value with flux for '%s'",
                                     stmt->as.binding.name);
                        pc_define(pc, stmt->as.binding.name, PHASE_FLUID);
                        break;
                    case PHASE_CRYSTAL:
                        pc_define(pc, stmt->as.binding.name, PHASE_CRYSTAL);
                        break;
                }
            } else {
                AstPhase eff = stmt->as.binding.phase;
                if (eff == PHASE_UNSPECIFIED) eff = value_phase;
                pc_define(pc, stmt->as.binding.name, eff);
            }
            break;
        }
        case STMT_ASSIGN:
            if (pc->mode == MODE_STRICT && stmt->as.assign.target->tag == EXPR_IDENT) {
                AstPhase p = pc_lookup(pc, stmt->as.assign.target->as.str_val);
                if (p == PHASE_CRYSTAL)
                    pc_error(pc, "strict mode: cannot assign to crystal binding '%s'",
                             stmt->as.assign.target->as.str_val);
            }
            pc_check_expr(pc, stmt->as.assign.target);
            pc_check_expr(pc, stmt->as.assign.value);
            break;
        case STMT_EXPR:
            pc_check_expr(pc, stmt->as.expr);
            break;
        case STMT_RETURN:
            if (stmt->as.return_expr) pc_check_expr(pc, stmt->as.return_expr);
            break;
        case STMT_FOR:
            pc_check_expr(pc, stmt->as.for_loop.iter);
            pc_push_scope(pc);
            pc_define(pc, stmt->as.for_loop.var, PHASE_UNSPECIFIED);
            for (size_t i = 0; i < stmt->as.for_loop.body_count; i++)
                pc_check_stmt(pc, stmt->as.for_loop.body[i]);
            pc_pop_scope(pc);
            break;
        case STMT_WHILE:
            pc_check_expr(pc, stmt->as.while_loop.cond);
            pc_push_scope(pc);
            for (size_t i = 0; i < stmt->as.while_loop.body_count; i++)
                pc_check_stmt(pc, stmt->as.while_loop.body[i]);
            pc_pop_scope(pc);
            break;
        case STMT_LOOP:
            pc_push_scope(pc);
            for (size_t i = 0; i < stmt->as.loop.body_count; i++)
                pc_check_stmt(pc, stmt->as.loop.body[i]);
            pc_pop_scope(pc);
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
            break;
        case STMT_DESTRUCTURE: {
            pc_check_expr(pc, stmt->as.destructure.value);
            AstPhase eff = stmt->as.destructure.phase;
            if (pc->mode == MODE_STRICT && eff == PHASE_UNSPECIFIED)
                pc_error(pc, "strict mode: destructure requires explicit phase (flux/fix)");
            for (size_t i = 0; i < stmt->as.destructure.name_count; i++)
                pc_define(pc, stmt->as.destructure.names[i], eff);
            if (stmt->as.destructure.rest_name)
                pc_define(pc, stmt->as.destructure.rest_name, eff);
            break;
        }
        case STMT_IMPORT:
            /* Import bindings are unphased */
            if (stmt->as.import.alias)
                pc_define(pc, stmt->as.import.alias, PHASE_UNSPECIFIED);
            if (stmt->as.import.selective_names) {
                for (size_t i = 0; i < stmt->as.import.selective_count; i++)
                    pc_define(pc, stmt->as.import.selective_names[i], PHASE_UNSPECIFIED);
            }
            break;
    }
}

static void pc_check_fn(PhaseChecker *pc, const FnDecl *f) {
    pc_push_scope(pc);
    for (size_t i = 0; i < f->param_count; i++)
        pc_define(pc, f->params[i].name, f->params[i].ty.phase);
    for (size_t i = 0; i < f->body_count; i++)
        pc_check_stmt(pc, f->body[i]);
    pc_pop_scope(pc);
}

/* Spawn-specific checking */

static void pc_check_spawn_expr(PhaseChecker *pc, const Expr *expr) {
    switch (expr->tag) {
        case EXPR_IDENT:
            if (pc_lookup(pc, expr->as.str_val) == PHASE_FLUID)
                pc_error(pc, "strict mode: cannot use fluid binding '%s' across thread boundary in spawn",
                         expr->as.str_val);
            break;
        case EXPR_BINOP:
            pc_check_spawn_expr(pc, expr->as.binop.left);
            pc_check_spawn_expr(pc, expr->as.binop.right);
            break;
        case EXPR_UNARYOP:
            pc_check_spawn_expr(pc, expr->as.unaryop.operand);
            break;
        case EXPR_CALL:
            pc_check_spawn_expr(pc, expr->as.call.func);
            for (size_t i = 0; i < expr->as.call.arg_count; i++)
                pc_check_spawn_expr(pc, expr->as.call.args[i]);
            break;
        case EXPR_METHOD_CALL:
            pc_check_spawn_expr(pc, expr->as.method_call.object);
            for (size_t i = 0; i < expr->as.method_call.arg_count; i++)
                pc_check_spawn_expr(pc, expr->as.method_call.args[i]);
            break;
        case EXPR_FIELD_ACCESS:
            pc_check_spawn_expr(pc, expr->as.field_access.object);
            break;
        case EXPR_INDEX:
            pc_check_spawn_expr(pc, expr->as.index.object);
            pc_check_spawn_expr(pc, expr->as.index.index);
            break;
        case EXPR_FREEZE:
            pc_check_spawn_expr(pc, expr->as.freeze.expr);
            if (expr->as.freeze.contract)
                pc_check_spawn_expr(pc, expr->as.freeze.contract);
            break;
        case EXPR_THAW: case EXPR_CLONE:
            pc_check_spawn_expr(pc, expr->as.freeze_expr);
            break;
        case EXPR_ANNEAL:
            pc_check_spawn_expr(pc, expr->as.anneal.expr);
            pc_check_spawn_expr(pc, expr->as.anneal.closure);
            break;
        case EXPR_PRINT:
            for (size_t i = 0; i < expr->as.print.arg_count; i++)
                pc_check_spawn_expr(pc, expr->as.print.args[i]);
            break;
        case EXPR_INTERP_STRING:
            for (size_t i = 0; i < expr->as.interp.count; i++)
                pc_check_spawn_expr(pc, expr->as.interp.exprs[i]);
            break;
        case EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.count; i++)
                pc_check_spawn_expr(pc, expr->as.array.elems[i]);
            break;
        case EXPR_SPREAD:
            pc_check_spawn_expr(pc, expr->as.spread_expr);
            break;
        case EXPR_TUPLE:
            for (size_t i = 0; i < expr->as.tuple.count; i++)
                pc_check_spawn_expr(pc, expr->as.tuple.elems[i]);
            break;
        case EXPR_STRUCT_LIT:
            for (size_t i = 0; i < expr->as.struct_lit.field_count; i++)
                pc_check_spawn_expr(pc, expr->as.struct_lit.fields[i].value);
            break;
        default:
            pc_check_expr(pc, expr);
            break;
    }
}

static void pc_check_spawn_stmt(PhaseChecker *pc, const Stmt *stmt) {
    switch (stmt->tag) {
        case STMT_EXPR:
            pc_check_spawn_expr(pc, stmt->as.expr);
            break;
        case STMT_BINDING:
            pc_check_spawn_expr(pc, stmt->as.binding.value);
            if (pc->mode == MODE_STRICT && stmt->as.binding.phase == PHASE_UNSPECIFIED)
                pc_error(pc, "strict mode: use 'flux' or 'fix' instead of 'let' for binding '%s'",
                         stmt->as.binding.name);
            pc_define(pc, stmt->as.binding.name, stmt->as.binding.phase);
            break;
        case STMT_ASSIGN:
            pc_check_spawn_expr(pc, stmt->as.assign.target);
            pc_check_spawn_expr(pc, stmt->as.assign.value);
            break;
        default:
            pc_check_stmt(pc, stmt);
            break;
    }
}

/* ── Phase-dispatch registration for phase checker ── */

static bool pc_phase_signatures_match(const FnDecl *a, const FnDecl *b) {
    if (a->param_count != b->param_count) return false;
    for (size_t i = 0; i < a->param_count; i++) {
        if (a->params[i].ty.phase != b->params[i].ty.phase) return false;
    }
    return true;
}

static void pc_register_fn(LatMap *fn_defs, FnDecl *new_fn) {
    FnDecl **existing = lat_map_get(fn_defs, new_fn->name);
    if (!existing) {
        lat_map_set(fn_defs, new_fn->name, &new_fn);
        return;
    }
    FnDecl *head = *existing;
    if (pc_phase_signatures_match(head, new_fn)) {
        new_fn->next_overload = head->next_overload;
        lat_map_set(fn_defs, new_fn->name, &new_fn);
        return;
    }
    for (FnDecl *prev = head; prev->next_overload; prev = prev->next_overload) {
        if (pc_phase_signatures_match(prev->next_overload, new_fn)) {
            new_fn->next_overload = prev->next_overload->next_overload;
            prev->next_overload = new_fn;
            return;
        }
    }
    new_fn->next_overload = head;
    lat_map_set(fn_defs, new_fn->name, &new_fn);
}

/* ── Public API ── */

LatVec phase_check(const Program *prog) {
    PhaseChecker pc;
    pc_init(&pc, prog->mode);

    /* Register structs and functions */
    for (size_t i = 0; i < prog->item_count; i++) {
        if (prog->items[i].tag == ITEM_STRUCT) {
            StructDecl *ptr = &prog->items[i].as.struct_decl;
            lat_map_set(&pc.struct_defs, ptr->name, &ptr);
        } else if (prog->items[i].tag == ITEM_FUNCTION) {
            FnDecl *ptr = &prog->items[i].as.fn_decl;
            pc_register_fn(&pc.fn_defs, ptr);
        }
    }

    /* Check all items */
    for (size_t i = 0; i < prog->item_count; i++) {
        switch (prog->items[i].tag) {
            case ITEM_FUNCTION:
                pc_check_fn(&pc, &prog->items[i].as.fn_decl);
                break;
            case ITEM_STRUCT:
                break;
            case ITEM_STMT:
                pc_check_stmt(&pc, prog->items[i].as.stmt);
                break;
            case ITEM_TEST:
                for (size_t j = 0; j < prog->items[i].as.test_decl.body_count; j++)
                    pc_check_stmt(&pc, prog->items[i].as.test_decl.body[j]);
                break;
            case ITEM_ENUM:
                break;
        }
    }

    LatVec errors = pc.errors;
    pc.errors = lat_vec_new(sizeof(char *));  /* prevent double-free */
    pc_free(&pc);
    return errors;
}
