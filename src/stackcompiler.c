#include "stackcompiler.h"
#include "stackopcode.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Compiler state ── */

static Compiler *current = NULL;
static char *compile_error = NULL;

/* Set a compile error with line info in "line:1: message" format.
 * The line parameter is 1-based (from AST s->line). */
static void set_compile_error(int line, const char *msg) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%d:1: %s", line, msg);
    compile_error = strdup(buf);
}

/* Track declared enums so EXPR_ENUM_VARIANT can fall back to function call */
static char **known_enums = NULL;
static size_t known_enum_count = 0;
static size_t known_enum_cap = 0;

static void register_enum(const char *name) {
    if (known_enum_count >= known_enum_cap) {
        known_enum_cap = known_enum_cap ? known_enum_cap * 2 : 8;
        known_enums = realloc(known_enums, known_enum_cap * sizeof(char *));
    }
    known_enums[known_enum_count++] = strdup(name);
}

static bool is_known_enum(const char *name) {
    for (size_t i = 0; i < known_enum_count; i++)
        if (strcmp(known_enums[i], name) == 0) return true;
    return false;
}

static void free_known_enums(void) {
    for (size_t i = 0; i < known_enum_count; i++) free(known_enums[i]);
    free(known_enums);
    known_enums = NULL;
    known_enum_count = 0;
    known_enum_cap = 0;
}

static LatValue const_eval_expr(Expr *e);

static void compiler_init(Compiler *comp, Compiler *enclosing, FunctionType type) {
    comp->enclosing = enclosing;
    comp->chunk = chunk_new();
    comp->type = type;
    comp->func_name = NULL;
    comp->arity = 0;
    comp->local_count = 0;
    comp->local_cap = 256;
    comp->locals = malloc(comp->local_cap * sizeof(Local));
    if (!comp->locals) {
        compile_error = strdup("out of memory");
        return;
    }
    comp->upvalues = NULL;
    comp->upvalue_count = 0;
    comp->scope_depth = (type == FUNC_SCRIPT) ? 0 : 1;
    comp->break_jumps = NULL;
    comp->break_count = 0;
    comp->break_cap = 0;
    comp->loop_start = 0;
    comp->loop_depth = 0;
    comp->loop_break_local_count = 0;
    comp->loop_continue_local_count = 0;
    comp->contracts = NULL;
    comp->contract_count = 0;
    comp->return_type_name = NULL;

    /* Reserve slot 0 for the function itself (or empty for script) */
    if (type != FUNC_SCRIPT) {
        Local *local = &comp->locals[comp->local_count++];
        local->name = strdup("");
        local->depth = 0;
        local->is_captured = false;
    }

    current = comp;
}

static void compiler_cleanup(Compiler *comp) {
    for (size_t i = 0; i < comp->local_count; i++) free(comp->locals[i].name);
    free(comp->locals);
    free(comp->upvalues);
    free(comp->break_jumps);
    free(comp->func_name);
}

static Chunk *current_chunk(void) { return current->chunk; }

static void emit_byte(uint8_t byte, int line) { chunk_write(current_chunk(), byte, line); }

static void emit_bytes(uint8_t b1, uint8_t b2, int line) {
    emit_byte(b1, line);
    emit_byte(b2, line);
}

static void emit_constant_idx(uint8_t op, uint8_t op16, size_t idx, int line) {
    if (idx <= 255) {
        emit_bytes(op, (uint8_t)idx, line);
    } else if (idx <= 65535) {
        emit_byte(op16, line);
        emit_byte((uint8_t)((idx >> 8) & 0xff), line);
        emit_byte((uint8_t)(idx & 0xff), line);
    } else {
        compile_error = strdup("too many constants in one chunk (>65535)");
    }
}

static size_t emit_constant(LatValue val, int line) {
    size_t idx = chunk_add_constant(current_chunk(), val);
    emit_constant_idx(OP_CONSTANT, OP_CONSTANT_16, idx, line);
    return idx;
}

static size_t emit_jump(uint8_t op, int line) {
    emit_byte(op, line);
    emit_byte(0xff, line);
    emit_byte(0xff, line);
    return current_chunk()->code_len - 2;
}

static void patch_jump(size_t offset) {
    size_t jump = current_chunk()->code_len - offset - 2;
    if (jump > 65535) {
        compile_error = strdup("jump offset too large");
        return;
    }
    current_chunk()->code[offset] = (uint8_t)((jump >> 8) & 0xff);
    current_chunk()->code[offset + 1] = (uint8_t)(jump & 0xff);
}

static void emit_loop(size_t loop_start, int line) {
    emit_byte(OP_LOOP, line);
    size_t offset = current_chunk()->code_len - loop_start + 2;
    if (offset > 65535) {
        compile_error = strdup("loop body too large");
        return;
    }
    emit_byte((uint8_t)((offset >> 8) & 0xff), line);
    emit_byte((uint8_t)(offset & 0xff), line);
}

/* ── Scope management ── */

static void begin_scope(void) { current->scope_depth++; }

static void end_scope(int line) {
    /* Run any defers registered at the current scope depth before popping locals.
     * Push a dummy value because OP_DEFER_RUN saves/restores TOS for each defer. */
    emit_byte(OP_UNIT, line);
    emit_byte(OP_DEFER_RUN, line);
    emit_byte((uint8_t)current->scope_depth, line);
    emit_byte(OP_POP, line); /* pop the dummy unit */
    current->scope_depth--;
    while (current->local_count > 0 && current->locals[current->local_count - 1].depth > current->scope_depth) {
        if (current->locals[current->local_count - 1].is_captured) {
            emit_byte(OP_CLOSE_UPVALUE, line);
        } else {
            emit_byte(OP_POP, line);
        }
        free(current->locals[current->local_count - 1].name);
        current->local_count--;
    }
}

/* Like end_scope but preserves TOS (the expression result) across local pops.
 * Emits OP_SWAP before each OP_POP/OP_CLOSE_UPVALUE to sink the result
 * value past each local being removed. */
static void end_scope_preserve_tos(int line) {
    /* Run any defers registered at the current scope depth.
     * TOS is already the expression result; OP_DEFER_RUN saves/restores it. */
    emit_byte(OP_DEFER_RUN, line);
    emit_byte((uint8_t)current->scope_depth, line);
    current->scope_depth--;
    while (current->local_count > 0 && current->locals[current->local_count - 1].depth > current->scope_depth) {
        emit_byte(OP_SWAP, line);
        if (current->locals[current->local_count - 1].is_captured) {
            emit_byte(OP_CLOSE_UPVALUE, line);
        } else {
            emit_byte(OP_POP, line);
        }
        free(current->locals[current->local_count - 1].name);
        current->local_count--;
    }
}

static void add_local(const char *name) {
    if (current->local_count >= current->local_cap) {
        current->local_cap *= 2;
        current->locals = realloc(current->locals, current->local_cap * sizeof(Local));
    }
    size_t slot = current->local_count;
    Local *local = &current->locals[current->local_count++];
    local->name = strdup(name);
    local->depth = current->scope_depth;
    local->is_captured = false;
    /* Record name in chunk's debug table for runtime tracking support */
    chunk_set_local_name(current_chunk(), slot, name);
}

static int resolve_local(Compiler *comp, const char *name) {
    for (int i = (int)comp->local_count - 1; i >= 0; i--) {
        if (strcmp(comp->locals[i].name, name) == 0) return i;
    }
    return -1;
}

/* ── Upvalue resolution ── */

static int add_upvalue(Compiler *comp, uint8_t index, bool is_local) {
    /* Check if we already have this upvalue */
    for (size_t i = 0; i < comp->upvalue_count; i++) {
        if (comp->upvalues[i].index == index && comp->upvalues[i].is_local == is_local) return (int)i;
    }
    if (comp->upvalue_count >= 256) {
        compile_error = strdup("too many upvalues in one function");
        return -1;
    }
    comp->upvalues = realloc(comp->upvalues, (comp->upvalue_count + 1) * sizeof(CompilerUpvalue));
    comp->upvalues[comp->upvalue_count].index = index;
    comp->upvalues[comp->upvalue_count].is_local = is_local;
    return (int)comp->upvalue_count++;
}

static int resolve_upvalue(Compiler *comp, const char *name) {
    if (!comp->enclosing) return -1;

    int local = resolve_local(comp->enclosing, name);
    if (local != -1) {
        comp->enclosing->locals[local].is_captured = true;
        return add_upvalue(comp, (uint8_t)local, true);
    }

    int upvalue = resolve_upvalue(comp->enclosing, name);
    if (upvalue != -1) return add_upvalue(comp, (uint8_t)upvalue, false);

    return -1;
}

/* ── Break/continue helpers ── */

static void push_break_jump(size_t offset) {
    if (current->break_count >= current->break_cap) {
        current->break_cap = current->break_cap ? current->break_cap * 2 : 8;
        current->break_jumps = realloc(current->break_jumps, current->break_cap * sizeof(size_t));
    }
    current->break_jumps[current->break_count++] = offset;
}

/* ── Compile expressions ── */

static void compile_expr(const Expr *e, int line);
static void compile_stmt(const Stmt *s);

/* Compile a statement and emit OP_RESET_EPHEMERAL to reclaim temporaries. */
static void compile_stmt_reset(const Stmt *s) {
    compile_stmt(s);
    if (!compile_error) emit_byte(OP_RESET_EPHEMERAL, s->line);
}

static void compile_function_body(FunctionType type, const char *name, Param *params, size_t param_count, Stmt **body,
                                  size_t body_count, ContractClause *contracts, size_t contract_count,
                                  TypeExpr *return_type, int line);

/* Compile a list of statements into a standalone sub-chunk.
 * Saves/restores the current compiler so this can be called mid-compilation. */
static Chunk *compile_sub_body(Stmt **stmts, size_t count, int line) {
    Compiler *saved = current;
    Compiler sub;
    compiler_init(&sub, NULL, FUNC_SCRIPT);

    /* If last statement is an expression, use its value as the sub-body result */
    if (count > 0 && stmts[count - 1]->tag == STMT_EXPR) {
        for (size_t i = 0; i + 1 < count; i++) compile_stmt(stmts[i]);
        compile_expr(stmts[count - 1]->as.expr, line);
    } else {
        for (size_t i = 0; i < count; i++) compile_stmt(stmts[i]);
        emit_byte(OP_UNIT, line);
    }
    emit_byte(OP_RETURN, line);

    Chunk *ch = sub.chunk;
    compiler_cleanup(&sub);
    current = saved;
    return ch;
}

#ifndef __EMSCRIPTEN__
/* Compile a single expression into a standalone sub-chunk (evaluates and returns value). */
static Chunk *compile_sub_expr(const Expr *expr, int line) {
    Compiler *saved = current;
    Compiler sub;
    compiler_init(&sub, NULL, FUNC_SCRIPT);

    compile_expr(expr, line);
    emit_byte(OP_RETURN, line);

    Chunk *ch = sub.chunk;
    compiler_cleanup(&sub);
    current = saved;
    return ch;
}
#endif

/* Store a pre-compiled Chunk* as a VAL_CLOSURE constant in the current chunk. */
static size_t add_chunk_constant(Chunk *ch) {
    LatValue fn_val;
    memset(&fn_val, 0, sizeof(fn_val));
    fn_val.type = VAL_CLOSURE;
    fn_val.phase = VTAG_UNPHASED;
    fn_val.region_id = (size_t)-1;
    fn_val.as.closure.body = NULL;
    fn_val.as.closure.native_fn = ch;
    return chunk_add_constant(current_chunk(), fn_val);
}

/* ── Recursive constant folding ──
 * Attempts to evaluate an expression at compile time.
 * Returns true and sets *out if the entire expression tree is constant.
 * Returns false for non-constant expressions (fall through to normal compilation).
 * Skips AND/OR/NIL_COALESCE (short-circuit semantics) and div-by-zero (runtime error). */
static bool try_const_fold(const Expr *e, LatValue *out) {
    if (!e) return false;
    switch (e->tag) {
        case EXPR_INT_LIT: *out = value_int(e->as.int_val); return true;
        case EXPR_FLOAT_LIT: *out = value_float(e->as.float_val); return true;
        case EXPR_BOOL_LIT: *out = value_bool(e->as.bool_val); return true;
        case EXPR_STRING_LIT: *out = value_string(e->as.str_val); return true;
        case EXPR_NIL_LIT: *out = value_nil(); return true;

        case EXPR_UNARYOP: {
            LatValue operand;
            if (!try_const_fold(e->as.unaryop.operand, &operand)) return false;
            switch (e->as.unaryop.op) {
                case UNOP_NEG:
                    if (operand.type == VAL_INT) {
                        *out = value_int(-operand.as.int_val);
                        return true;
                    }
                    if (operand.type == VAL_FLOAT) {
                        *out = value_float(-operand.as.float_val);
                        return true;
                    }
                    break;
                case UNOP_NOT:
                    if (operand.type == VAL_BOOL) {
                        *out = value_bool(!operand.as.bool_val);
                        return true;
                    }
                    break;
                case UNOP_BIT_NOT:
                    if (operand.type == VAL_INT) {
                        *out = value_int(~operand.as.int_val);
                        return true;
                    }
                    break;
            }
            value_free(&operand);
            return false;
        }

        case EXPR_BINOP: {
            /* Skip short-circuit operators */
            if (e->as.binop.op == BINOP_AND || e->as.binop.op == BINOP_OR || e->as.binop.op == BINOP_NIL_COALESCE)
                return false;

            LatValue lv, rv;
            if (!try_const_fold(e->as.binop.left, &lv)) return false;
            if (!try_const_fold(e->as.binop.right, &rv)) {
                value_free(&lv);
                return false;
            }

            /* String concatenation */
            if (lv.type == VAL_STR && rv.type == VAL_STR && e->as.binop.op == BINOP_ADD) {
                size_t llen = strlen(lv.as.str_val), rlen = strlen(rv.as.str_val);
                char *buf = malloc(llen + rlen + 1);
                memcpy(buf, lv.as.str_val, llen);
                memcpy(buf + llen, rv.as.str_val, rlen);
                buf[llen + rlen] = '\0';
                *out = value_string(buf);
                free(buf);
                value_free(&lv);
                value_free(&rv);
                return true;
            }

            /* Numeric operations */
            if ((lv.type != VAL_INT && lv.type != VAL_FLOAT) || (rv.type != VAL_INT && rv.type != VAL_FLOAT)) {
                value_free(&lv);
                value_free(&rv);
                return false;
            }

            bool both_int = (lv.type == VAL_INT && rv.type == VAL_INT);
            int64_t li = lv.type == VAL_INT ? lv.as.int_val : 0;
            int64_t ri = rv.type == VAL_INT ? rv.as.int_val : 0;
            double lf = lv.type == VAL_FLOAT ? lv.as.float_val : (double)li;
            double rf = rv.type == VAL_FLOAT ? rv.as.float_val : (double)ri;

            switch (e->as.binop.op) {
                case BINOP_ADD: *out = both_int ? value_int(li + ri) : value_float(lf + rf); return true;
                case BINOP_SUB: *out = both_int ? value_int(li - ri) : value_float(lf - rf); return true;
                case BINOP_MUL: *out = both_int ? value_int(li * ri) : value_float(lf * rf); return true;
                case BINOP_DIV:
                    if (both_int) {
                        if (ri == 0) return false;
                        *out = value_int(li / ri);
                    } else {
                        if (rf == 0.0) return false;
                        *out = value_float(lf / rf);
                    }
                    return true;
                case BINOP_MOD:
                    if (!both_int || ri == 0) return false;
                    *out = value_int(li % ri);
                    return true;
                case BINOP_LT: *out = value_bool(both_int ? li < ri : lf < rf); return true;
                case BINOP_GT: *out = value_bool(both_int ? li > ri : lf > rf); return true;
                case BINOP_LTEQ: *out = value_bool(both_int ? li <= ri : lf <= rf); return true;
                case BINOP_GTEQ: *out = value_bool(both_int ? li >= ri : lf >= rf); return true;
                case BINOP_EQ: *out = value_bool(both_int ? li == ri : lf == rf); return true;
                case BINOP_NEQ: *out = value_bool(both_int ? li != ri : lf != rf); return true;
                /* Bitwise: integer only */
                case BINOP_BIT_AND:
                    if (!both_int) return false;
                    *out = value_int(li & ri);
                    return true;
                case BINOP_BIT_OR:
                    if (!both_int) return false;
                    *out = value_int(li | ri);
                    return true;
                case BINOP_BIT_XOR:
                    if (!both_int) return false;
                    *out = value_int(li ^ ri);
                    return true;
                case BINOP_LSHIFT:
                    if (!both_int || ri < 0 || ri >= 64) return false;
                    *out = value_int(li << ri);
                    return true;
                case BINOP_RSHIFT:
                    if (!both_int || ri < 0 || ri >= 64) return false;
                    *out = value_int(li >> ri);
                    return true;
                default: return false;
            }
        }
        default: return false;
    }
}

static void compile_expr(const Expr *e, int line) {
    if (compile_error) return;
    if (e->line > 0) line = e->line;

    switch (e->tag) {
        case EXPR_INT_LIT:
            if (e->as.int_val >= -128 && e->as.int_val <= 127) {
                emit_bytes(OP_LOAD_INT8, (uint8_t)(int8_t)e->as.int_val, line);
            } else {
                emit_constant(value_int(e->as.int_val), line);
            }
            break;

        case EXPR_FLOAT_LIT: emit_constant(value_float(e->as.float_val), line); break;

        case EXPR_BOOL_LIT: emit_byte(e->as.bool_val ? OP_TRUE : OP_FALSE, line); break;

        case EXPR_NIL_LIT: emit_byte(OP_NIL, line); break;

        case EXPR_STRING_LIT: emit_constant(value_string(e->as.str_val), line); break;

        case EXPR_IDENT: {
            int slot = resolve_local(current, e->as.str_val);
            if (slot >= 0) {
                emit_bytes(OP_GET_LOCAL, (uint8_t)slot, line);
            } else {
                int upvalue = resolve_upvalue(current, e->as.str_val);
                if (upvalue >= 0) {
                    emit_bytes(OP_GET_UPVALUE, (uint8_t)upvalue, line);
                } else {
                    size_t idx = chunk_add_constant(current_chunk(), value_string(e->as.str_val));
                    emit_constant_idx(OP_GET_GLOBAL, OP_GET_GLOBAL_16, idx, line);
                }
            }
            break;
        }

        case EXPR_BINOP: {
            /* Recursive constant folding: evaluate at compile time if entire tree is constant */
            {
                LatValue folded;
                if (try_const_fold(e, &folded)) {
                    if (folded.type == VAL_INT) {
                        int64_t v = folded.as.int_val;
                        if (v >= -128 && v <= 127) emit_bytes(OP_LOAD_INT8, (uint8_t)(int8_t)v, line);
                        else emit_constant(value_int(v), line);
                    } else if (folded.type == VAL_STR) {
                        emit_constant(folded, line);
                    } else {
                        emit_constant(folded, line);
                    }
                    break;
                }
            }

            /* Short-circuit AND/OR */
            if (e->as.binop.op == BINOP_AND) {
                compile_expr(e->as.binop.left, line);
                size_t end_jump = emit_jump(OP_JUMP_IF_FALSE, line);
                emit_byte(OP_POP, line);
                compile_expr(e->as.binop.right, line);
                patch_jump(end_jump);
                break;
            }
            if (e->as.binop.op == BINOP_OR) {
                compile_expr(e->as.binop.left, line);
                size_t end_jump = emit_jump(OP_JUMP_IF_TRUE, line);
                emit_byte(OP_POP, line);
                compile_expr(e->as.binop.right, line);
                patch_jump(end_jump);
                break;
            }
            /* Nil coalesce */
            if (e->as.binop.op == BINOP_NIL_COALESCE) {
                compile_expr(e->as.binop.left, line);
                size_t end_jump = emit_jump(OP_JUMP_IF_NOT_NIL, line);
                emit_byte(OP_POP, line);
                compile_expr(e->as.binop.right, line);
                patch_jump(end_jump);
                break;
            }
            /* Normal binary ops */
            compile_expr(e->as.binop.left, line);
            compile_expr(e->as.binop.right, line);
            switch (e->as.binop.op) {
                case BINOP_ADD: emit_byte(OP_ADD, line); break;
                case BINOP_SUB: emit_byte(OP_SUB, line); break;
                case BINOP_MUL: emit_byte(OP_MUL, line); break;
                case BINOP_DIV: emit_byte(OP_DIV, line); break;
                case BINOP_MOD: emit_byte(OP_MOD, line); break;
                case BINOP_EQ: emit_byte(OP_EQ, line); break;
                case BINOP_NEQ: emit_byte(OP_NEQ, line); break;
                case BINOP_LT: emit_byte(OP_LT, line); break;
                case BINOP_GT: emit_byte(OP_GT, line); break;
                case BINOP_LTEQ: emit_byte(OP_LTEQ, line); break;
                case BINOP_GTEQ: emit_byte(OP_GTEQ, line); break;
                case BINOP_BIT_AND: emit_byte(OP_BIT_AND, line); break;
                case BINOP_BIT_OR: emit_byte(OP_BIT_OR, line); break;
                case BINOP_BIT_XOR: emit_byte(OP_BIT_XOR, line); break;
                case BINOP_LSHIFT: emit_byte(OP_LSHIFT, line); break;
                case BINOP_RSHIFT: emit_byte(OP_RSHIFT, line); break;
                default: break; /* AND/OR/NIL_COALESCE handled above */
            }
            break;
        }

        case EXPR_UNARYOP: {
            /* Recursive constant folding for unary expressions */
            LatValue folded;
            if (try_const_fold(e, &folded)) {
                if (folded.type == VAL_INT) {
                    int64_t v = folded.as.int_val;
                    if (v >= -128 && v <= 127) emit_bytes(OP_LOAD_INT8, (uint8_t)(int8_t)v, line);
                    else emit_constant(value_int(v), line);
                } else {
                    emit_constant(folded, line);
                }
                break;
            }
            compile_expr(e->as.unaryop.operand, line);
            switch (e->as.unaryop.op) {
                case UNOP_NEG: emit_byte(OP_NEG, line); break;
                case UNOP_NOT: emit_byte(OP_NOT, line); break;
                case UNOP_BIT_NOT: emit_byte(OP_BIT_NOT, line); break;
            }
            break;
        }

        case EXPR_PRINT: {
            for (size_t i = 0; i < e->as.print.arg_count; i++) compile_expr(e->as.print.args[i], line);
            emit_bytes(OP_PRINT, (uint8_t)e->as.print.arg_count, line);
            break;
        }

        case EXPR_IF: {
            compile_expr(e->as.if_expr.cond, line);
            size_t then_jump = emit_jump(OP_JUMP_IF_FALSE, line);
            emit_byte(OP_POP, line); /* pop condition (then path) */

            /* Then branch (scoped so locals like 'flux j' are cleaned up) */
            begin_scope();
            /* If last stmt is STMT_EXPR, use its value as the branch result */
            if (e->as.if_expr.then_count > 0 &&
                e->as.if_expr.then_stmts[e->as.if_expr.then_count - 1]->tag == STMT_EXPR) {
                for (size_t i = 0; i + 1 < e->as.if_expr.then_count; i++)
                    compile_stmt_reset(e->as.if_expr.then_stmts[i]);
                /* Compile last expression WITHOUT pop — value stays on stack */
                compile_expr(e->as.if_expr.then_stmts[e->as.if_expr.then_count - 1]->as.expr, line);
                end_scope_preserve_tos(line);
            } else {
                for (size_t i = 0; i < e->as.if_expr.then_count; i++) compile_stmt_reset(e->as.if_expr.then_stmts[i]);
                emit_byte(OP_UNIT, line);
                end_scope(line);
            }

            size_t else_jump = emit_jump(OP_JUMP, line);
            patch_jump(then_jump);
            emit_byte(OP_POP, line); /* pop condition (else path) */

            /* Else branch */
            if (e->as.if_expr.else_stmts) {
                begin_scope();
                if (e->as.if_expr.else_count > 0 &&
                    e->as.if_expr.else_stmts[e->as.if_expr.else_count - 1]->tag == STMT_EXPR) {
                    for (size_t i = 0; i + 1 < e->as.if_expr.else_count; i++)
                        compile_stmt_reset(e->as.if_expr.else_stmts[i]);
                    compile_expr(e->as.if_expr.else_stmts[e->as.if_expr.else_count - 1]->as.expr, line);
                    end_scope_preserve_tos(line);
                } else {
                    for (size_t i = 0; i < e->as.if_expr.else_count; i++)
                        compile_stmt_reset(e->as.if_expr.else_stmts[i]);
                    emit_byte(OP_UNIT, line);
                    end_scope(line);
                }
            } else {
                emit_byte(OP_NIL, line);
            }
            patch_jump(else_jump);
            break;
        }

        case EXPR_BLOCK: {
            begin_scope();
            /* If last stmt is an expression, use its value as the block result */
            if (e->as.block.count > 0 && e->as.block.stmts[e->as.block.count - 1]->tag == STMT_EXPR) {
                for (size_t i = 0; i + 1 < e->as.block.count; i++) compile_stmt_reset(e->as.block.stmts[i]);
                compile_expr(e->as.block.stmts[e->as.block.count - 1]->as.expr, line);
                end_scope_preserve_tos(line);
            } else {
                for (size_t i = 0; i < e->as.block.count; i++) compile_stmt_reset(e->as.block.stmts[i]);
                end_scope(line);
                emit_byte(OP_UNIT, line);
            }
            break;
        }

        case EXPR_CALL: {
            /* Intercept phase system special forms */
            if (e->as.call.func->tag == EXPR_IDENT) {
                const char *fn = e->as.call.func->as.str_val;

                if (strcmp(fn, "react") == 0 && e->as.call.arg_count == 2 && e->as.call.args[0]->tag == EXPR_IDENT) {
                    const char *var_name = e->as.call.args[0]->as.str_val;
                    compile_expr(e->as.call.args[1], line);
                    size_t idx = chunk_add_constant(current_chunk(), value_string(var_name));
                    emit_bytes(OP_REACT, (uint8_t)idx, line);
                    break;
                }
                if (strcmp(fn, "unreact") == 0 && e->as.call.arg_count == 1 && e->as.call.args[0]->tag == EXPR_IDENT) {
                    const char *var_name = e->as.call.args[0]->as.str_val;
                    size_t idx = chunk_add_constant(current_chunk(), value_string(var_name));
                    emit_bytes(OP_UNREACT, (uint8_t)idx, line);
                    break;
                }
                if (strcmp(fn, "bond") == 0 && e->as.call.arg_count >= 2 && e->as.call.args[0]->tag == EXPR_IDENT) {
                    const char *target = e->as.call.args[0]->as.str_val;
                    size_t target_idx = chunk_add_constant(current_chunk(), value_string(target));
                    /* Check if last arg is a string literal (strategy) */
                    size_t dep_end = e->as.call.arg_count;
                    const char *strategy = "mirror";
                    Expr *last_arg = e->as.call.args[e->as.call.arg_count - 1];
                    if (last_arg->tag == EXPR_STRING_LIT) {
                        strategy = last_arg->as.str_val;
                        dep_end--;
                    }
                    for (size_t i = 1; i < dep_end; i++) {
                        const char *dep_name =
                            (e->as.call.args[i]->tag == EXPR_IDENT) ? e->as.call.args[i]->as.str_val : "";
                        size_t dep_idx = chunk_add_constant(current_chunk(), value_string(dep_name));
                        emit_bytes(OP_CONSTANT, (uint8_t)dep_idx, line);
                        size_t strat_idx = chunk_add_constant(current_chunk(), value_string(strategy));
                        emit_bytes(OP_CONSTANT, (uint8_t)strat_idx, line);
                        emit_bytes(OP_BOND, (uint8_t)target_idx, line);
                        if (i + 1 < dep_end) emit_byte(OP_POP, line);
                    }
                    break;
                }
                if (strcmp(fn, "unbond") == 0 && e->as.call.arg_count >= 2 && e->as.call.args[0]->tag == EXPR_IDENT) {
                    const char *target = e->as.call.args[0]->as.str_val;
                    size_t target_idx = chunk_add_constant(current_chunk(), value_string(target));
                    for (size_t i = 1; i < e->as.call.arg_count; i++) {
                        const char *dep_name =
                            (e->as.call.args[i]->tag == EXPR_IDENT) ? e->as.call.args[i]->as.str_val : "";
                        size_t dep_idx = chunk_add_constant(current_chunk(), value_string(dep_name));
                        emit_bytes(OP_CONSTANT, (uint8_t)dep_idx, line);
                        emit_bytes(OP_UNBOND, (uint8_t)target_idx, line);
                        if (i + 1 < e->as.call.arg_count) emit_byte(OP_POP, line);
                    }
                    break;
                }
                if (strcmp(fn, "seed") == 0 && e->as.call.arg_count == 2 && e->as.call.args[0]->tag == EXPR_IDENT) {
                    const char *var_name = e->as.call.args[0]->as.str_val;
                    compile_expr(e->as.call.args[1], line);
                    size_t idx = chunk_add_constant(current_chunk(), value_string(var_name));
                    emit_bytes(OP_SEED, (uint8_t)idx, line);
                    break;
                }
                if (strcmp(fn, "unseed") == 0 && e->as.call.arg_count == 1 && e->as.call.args[0]->tag == EXPR_IDENT) {
                    const char *var_name = e->as.call.args[0]->as.str_val;
                    size_t idx = chunk_add_constant(current_chunk(), value_string(var_name));
                    emit_bytes(OP_UNSEED, (uint8_t)idx, line);
                    break;
                }
                /* track(var) / history(var) / phases(var): pass var name as string */
                if ((strcmp(fn, "track") == 0 || strcmp(fn, "history") == 0 || strcmp(fn, "phases") == 0) &&
                    e->as.call.arg_count == 1 && e->as.call.args[0]->tag == EXPR_IDENT) {
                    compile_expr(e->as.call.func, line);
                    emit_constant(value_string(e->as.call.args[0]->as.str_val), line);
                    emit_bytes(OP_CALL, 1, line);
                    break;
                }
                /* rewind(var, n): pass var name as string, compile second arg normally */
                if (strcmp(fn, "rewind") == 0 && e->as.call.arg_count == 2 && e->as.call.args[0]->tag == EXPR_IDENT) {
                    compile_expr(e->as.call.func, line);
                    emit_constant(value_string(e->as.call.args[0]->as.str_val), line);
                    compile_expr(e->as.call.args[1], line);
                    emit_bytes(OP_CALL, 2, line);
                    break;
                }
                /* pressurize(var, mode) / depressurize(var): pass var name as string */
                if (strcmp(fn, "pressurize") == 0 && e->as.call.arg_count == 2 &&
                    e->as.call.args[0]->tag == EXPR_IDENT) {
                    compile_expr(e->as.call.func, line);
                    emit_constant(value_string(e->as.call.args[0]->as.str_val), line);
                    compile_expr(e->as.call.args[1], line);
                    emit_bytes(OP_CALL, 2, line);
                    break;
                }
                if (strcmp(fn, "depressurize") == 0 && e->as.call.arg_count == 1 &&
                    e->as.call.args[0]->tag == EXPR_IDENT) {
                    compile_expr(e->as.call.func, line);
                    emit_constant(value_string(e->as.call.args[0]->as.str_val), line);
                    emit_bytes(OP_CALL, 1, line);
                    break;
                }
            }
            compile_expr(e->as.call.func, line);
            for (size_t i = 0; i < e->as.call.arg_count; i++) compile_expr(e->as.call.args[i], line);
            emit_bytes(OP_CALL, (uint8_t)e->as.call.arg_count, line);
            break;
        }

        case EXPR_ARRAY: {
            bool has_spread = false;
            for (size_t i = 0; i < e->as.array.count; i++) {
                if (e->as.array.elems[i]->tag == EXPR_SPREAD) {
                    has_spread = true;
                    break;
                }
            }
            for (size_t i = 0; i < e->as.array.count; i++) compile_expr(e->as.array.elems[i], line);
            emit_bytes(OP_BUILD_ARRAY, (uint8_t)e->as.array.count, line);
            if (has_spread) emit_byte(OP_ARRAY_FLATTEN, line);
            break;
        }

        case EXPR_RANGE: {
            compile_expr(e->as.range.start, line);
            compile_expr(e->as.range.end, line);
            emit_byte(OP_BUILD_RANGE, line);
            break;
        }

        case EXPR_TUPLE: {
            for (size_t i = 0; i < e->as.tuple.count; i++) compile_expr(e->as.tuple.elems[i], line);
            emit_bytes(OP_BUILD_TUPLE, (uint8_t)e->as.tuple.count, line);
            break;
        }

        case EXPR_INDEX: {
            /* Fast path: if object is a local variable (non-optional), use OP_INDEX_LOCAL
             * to avoid cloning the entire container. */
            if (!e->as.index.optional && e->as.index.object->tag == EXPR_IDENT) {
                int slot = resolve_local(current, e->as.index.object->as.str_val);
                if (slot >= 0) {
                    compile_expr(e->as.index.index, line);
                    emit_bytes(OP_INDEX_LOCAL, (uint8_t)slot, line);
                    break;
                }
            }
            compile_expr(e->as.index.object, line);
            size_t end_jump = 0;
            if (e->as.index.optional) {
                size_t skip = emit_jump(OP_JUMP_IF_NOT_NIL, line);
                end_jump = emit_jump(OP_JUMP, line);
                patch_jump(skip);
            }
            compile_expr(e->as.index.index, line);
            emit_byte(OP_INDEX, line);
            if (e->as.index.optional) patch_jump(end_jump);
            break;
        }

        case EXPR_FIELD_ACCESS: {
            /* Fast path: local variable field access without cloning container */
            if (!e->as.field_access.optional && e->as.field_access.object->tag == EXPR_IDENT) {
                int slot = resolve_local(current, e->as.field_access.object->as.str_val);
                if (slot >= 0) {
                    size_t idx = chunk_add_constant(current_chunk(), value_string(e->as.field_access.field));
                    emit_byte(OP_GET_FIELD_LOCAL, line);
                    emit_byte((uint8_t)slot, line);
                    emit_byte((uint8_t)idx, line);
                    break;
                }
            }
            compile_expr(e->as.field_access.object, line);
            size_t end_jump = 0;
            if (e->as.field_access.optional) {
                size_t skip = emit_jump(OP_JUMP_IF_NOT_NIL, line);
                end_jump = emit_jump(OP_JUMP, line);
                patch_jump(skip);
            }
            size_t idx = chunk_add_constant(current_chunk(), value_string(e->as.field_access.field));
            emit_bytes(OP_GET_FIELD, (uint8_t)idx, line);
            if (e->as.field_access.optional) patch_jump(end_jump);
            break;
        }

        case EXPR_METHOD_CALL: {
            bool opt = e->as.method_call.optional;
            /* If object is a local variable, use OP_INVOKE_LOCAL to mutate in-place */
            if (e->as.method_call.object->tag == EXPR_IDENT) {
                int slot = resolve_local(current, e->as.method_call.object->as.str_val);
                if (slot >= 0) {
                    size_t end_jump = 0;
                    if (opt) {
                        /* Check the local for nil before invoking */
                        emit_bytes(OP_GET_LOCAL, (uint8_t)slot, line);
                        size_t skip = emit_jump(OP_JUMP_IF_NOT_NIL, line);
                        /* Is nil — pop the peeked value, push nil as result */
                        emit_byte(OP_POP, line);
                        emit_byte(OP_NIL, line);
                        end_jump = emit_jump(OP_JUMP, line);
                        patch_jump(skip);
                        emit_byte(OP_POP, line); /* pop the peeked non-nil value */
                    }
                    for (size_t i = 0; i < e->as.method_call.arg_count; i++)
                        compile_expr(e->as.method_call.args[i], line);
                    size_t idx = chunk_add_constant(current_chunk(), value_string(e->as.method_call.method));
                    if (idx > 255) {
                        emit_byte(OP_INVOKE_LOCAL_16, line);
                        emit_byte((uint8_t)slot, line);
                        emit_byte((uint8_t)((idx >> 8) & 0xff), line);
                        emit_byte((uint8_t)(idx & 0xff), line);
                        emit_byte((uint8_t)e->as.method_call.arg_count, line);
                    } else {
                        emit_byte(OP_INVOKE_LOCAL, line);
                        emit_byte((uint8_t)slot, line);
                        emit_byte((uint8_t)idx, line);
                        emit_byte((uint8_t)e->as.method_call.arg_count, line);
                    }
                    if (opt) patch_jump(end_jump);
                    break;
                }
                /* If not a local and not an upvalue, it's a global —
                 * use OP_INVOKE_GLOBAL for write-back of mutating builtins */
                int upvalue = resolve_upvalue(current, e->as.method_call.object->as.str_val);
                if (upvalue < 0) {
                    size_t end_jump = 0;
                    if (opt) {
                        size_t tmp_idx =
                            chunk_add_constant(current_chunk(), value_string(e->as.method_call.object->as.str_val));
                        emit_constant_idx(OP_GET_GLOBAL, OP_GET_GLOBAL_16, tmp_idx, line);
                        size_t skip = emit_jump(OP_JUMP_IF_NOT_NIL, line);
                        emit_byte(OP_POP, line);
                        emit_byte(OP_NIL, line);
                        end_jump = emit_jump(OP_JUMP, line);
                        patch_jump(skip);
                        emit_byte(OP_POP, line);
                    }
                    for (size_t i = 0; i < e->as.method_call.arg_count; i++)
                        compile_expr(e->as.method_call.args[i], line);
                    size_t name_idx =
                        chunk_add_constant(current_chunk(), value_string(e->as.method_call.object->as.str_val));
                    size_t method_idx = chunk_add_constant(current_chunk(), value_string(e->as.method_call.method));
                    if (name_idx > 255 || method_idx > 255) {
                        emit_byte(OP_INVOKE_GLOBAL_16, line);
                        emit_byte((uint8_t)((name_idx >> 8) & 0xff), line);
                        emit_byte((uint8_t)(name_idx & 0xff), line);
                        emit_byte((uint8_t)((method_idx >> 8) & 0xff), line);
                        emit_byte((uint8_t)(method_idx & 0xff), line);
                        emit_byte((uint8_t)e->as.method_call.arg_count, line);
                    } else {
                        emit_byte(OP_INVOKE_GLOBAL, line);
                        emit_byte((uint8_t)name_idx, line);
                        emit_byte((uint8_t)method_idx, line);
                        emit_byte((uint8_t)e->as.method_call.arg_count, line);
                    }
                    if (opt) patch_jump(end_jump);
                    break;
                }
            }
            compile_expr(e->as.method_call.object, line);
            size_t end_jump = 0;
            if (opt) {
                size_t skip = emit_jump(OP_JUMP_IF_NOT_NIL, line);
                end_jump = emit_jump(OP_JUMP, line);
                patch_jump(skip);
            }
            for (size_t i = 0; i < e->as.method_call.arg_count; i++) compile_expr(e->as.method_call.args[i], line);
            size_t idx = chunk_add_constant(current_chunk(), value_string(e->as.method_call.method));
            emit_byte(OP_INVOKE, line);
            emit_byte((uint8_t)idx, line);
            emit_byte((uint8_t)e->as.method_call.arg_count, line);
            if (opt) patch_jump(end_jump);
            break;
        }

        case EXPR_STRUCT_LIT: {
            /* Push field values in order */
            for (size_t i = 0; i < e->as.struct_lit.field_count; i++)
                compile_expr(e->as.struct_lit.fields[i].value, line);
            /* Struct name + field names must be consecutive in the constant pool */
            size_t name_idx = chunk_add_constant_nodupe(current_chunk(), value_string(e->as.struct_lit.name));
            emit_byte(OP_BUILD_STRUCT, line);
            emit_byte((uint8_t)name_idx, line);
            emit_byte((uint8_t)e->as.struct_lit.field_count, line);
            /* Also store field names in constants for the VM to use (consecutive) */
            for (size_t i = 0; i < e->as.struct_lit.field_count; i++)
                chunk_add_constant_nodupe(current_chunk(), value_string(e->as.struct_lit.fields[i].name));
            break;
        }

        case EXPR_CLOSURE: {
            /* Compile as anonymous function */
            Compiler func_comp;
            compiler_init(&func_comp, current, FUNC_CLOSURE);
            func_comp.arity = (int)e->as.closure.param_count;

            /* Add params as locals */
            for (size_t i = 0; i < e->as.closure.param_count; i++) add_local(e->as.closure.params[i]);

            /* Compile body - if the body is a block, compile statements directly
             * at the closure's scope level (no extra begin_scope/end_scope) so
             * that OP_RETURN handles cleanup and the result value isn't lost. */
            if (e->as.closure.body->tag == EXPR_BLOCK) {
                Expr *block = e->as.closure.body;
                if (block->as.block.count > 0 && block->as.block.stmts[block->as.block.count - 1]->tag == STMT_EXPR) {
                    for (size_t i = 0; i + 1 < block->as.block.count; i++) compile_stmt(block->as.block.stmts[i]);
                    compile_expr(block->as.block.stmts[block->as.block.count - 1]->as.expr, line);
                } else {
                    for (size_t i = 0; i < block->as.block.count; i++) compile_stmt(block->as.block.stmts[i]);
                    emit_byte(OP_UNIT, line);
                }
            } else {
                compile_expr(e->as.closure.body, line);
            }
            emit_byte(OP_RETURN, line);

            Chunk *fn_chunk = func_comp.chunk;
            size_t upvalue_count = func_comp.upvalue_count;
            CompilerUpvalue *upvalues = NULL;
            if (upvalue_count > 0) {
                upvalues = malloc(upvalue_count * sizeof(CompilerUpvalue));
                if (!upvalues) return;
                memcpy(upvalues, func_comp.upvalues, upvalue_count * sizeof(CompilerUpvalue));
            }

            /* Store default parameter values and variadic flag on the chunk */
            {
                int dc = 0;
                bool vd = e->as.closure.has_variadic;
                if (e->as.closure.default_values) {
                    for (size_t i = 0; i < e->as.closure.param_count; i++) {
                        if (e->as.closure.default_values[i]) dc++;
                    }
                }
                fn_chunk->default_count = dc;
                fn_chunk->fn_has_variadic = vd;
                if (dc > 0) {
                    fn_chunk->default_values = malloc(dc * sizeof(LatValue));
                    if (!fn_chunk->default_values) return;
                    int di = 0;
                    for (size_t i = 0; i < e->as.closure.param_count; i++) {
                        if (e->as.closure.default_values && e->as.closure.default_values[i]) {
                            fn_chunk->default_values[di++] = const_eval_expr(e->as.closure.default_values[i]);
                        }
                    }
                }
            }

            compiler_cleanup(&func_comp);
            current = func_comp.enclosing;

            /* Store the function's chunk as a constant in the enclosing chunk.
             * We use a closure value with body=NULL to indicate a compiled function. */
            LatValue fn_val;
            memset(&fn_val, 0, sizeof(fn_val));
            fn_val.type = VAL_CLOSURE;
            fn_val.phase = VTAG_UNPHASED;
            fn_val.region_id = (size_t)-1;
            if (e->as.closure.param_count > 0) {
                fn_val.as.closure.param_names = malloc(e->as.closure.param_count * sizeof(char *));
                if (!fn_val.as.closure.param_names) return;
                for (size_t i = 0; i < e->as.closure.param_count; i++)
                    fn_val.as.closure.param_names[i] = strdup(e->as.closure.params[i]);
            } else {
                fn_val.as.closure.param_names = NULL;
            }
            fn_val.as.closure.param_count = (size_t)func_comp.arity;
            fn_val.as.closure.body = NULL;
            fn_val.as.closure.captured_env = NULL;
            fn_val.as.closure.default_values = NULL;
            fn_val.as.closure.has_variadic = false;
            fn_val.as.closure.native_fn = fn_chunk;
            size_t fn_idx = chunk_add_constant(current_chunk(), fn_val);

            if (fn_idx <= 255) {
                emit_byte(OP_CLOSURE, line);
                emit_byte((uint8_t)fn_idx, line);
            } else {
                emit_byte(OP_CLOSURE_16, line);
                emit_byte((uint8_t)((fn_idx >> 8) & 0xff), line);
                emit_byte((uint8_t)(fn_idx & 0xff), line);
            }
            emit_byte((uint8_t)upvalue_count, line);
            for (size_t i = 0; i < upvalue_count; i++) {
                emit_byte(upvalues[i].is_local ? 1 : 0, line);
                emit_byte(upvalues[i].index, line);
            }
            free(upvalues);
            break;
        }

        case EXPR_MATCH: {
            compile_expr(e->as.match_expr.scrutinee, line);
            size_t *end_jumps = malloc(e->as.match_expr.arm_count * sizeof(size_t));
            if (!end_jumps) return;
            size_t end_jump_count = 0;

            /* Stack invariant: scrutinee S stays on stack across all arms.
             * Non-binding: DUP -> [S, S']. Check -> [S, S', bool].
             *   Match: pop bool+S'+S, body -> [result].
             *   No match: pop bool+S' -> [S], next arm.
             * Binding: DUP -> [S, S']. S' becomes local, guard references it.
             *   Match: body -> [S, n, result], swap+end_scope+swap+pop -> [result].
             *   No match: pop guard_bool, pop n -> [S], next arm. */

            for (size_t i = 0; i < e->as.match_expr.arm_count; i++) {
                MatchArm *arm = &e->as.match_expr.arms[i];

                if (arm->pattern->tag == PAT_BINDING) {
                    /* Binding: Track scrutinee with a dummy local so that the
                     * binding variable and body-locals get correct slot indices.
                     * Stack: [..., S]. After DUP: [..., S, S']. */
                    begin_scope();
                    add_local("");           /* dummy slot tracks scrutinee S */
                    emit_byte(OP_DUP, line); /* [S, S'] */
                    add_local(arm->pattern->as.binding_name);

                    /* Compile guard (or TRUE if none) */
                    if (arm->guard) compile_expr(arm->guard, line); /* [S, n, guard_bool] */
                    else emit_byte(OP_TRUE, line);                  /* [S, n, true] */

                    size_t next_arm = emit_jump(OP_JUMP_IF_FALSE, line);
                    emit_byte(OP_POP, line); /* pop bool: [S, n] */

                    /* Compile arm body in a nested scope so body-locals
                     * are cleaned up before the binding variable. */
                    begin_scope();
                    if (arm->body_count > 0 && arm->body[arm->body_count - 1]->tag == STMT_EXPR) {
                        for (size_t j = 0; j + 1 < arm->body_count; j++) compile_stmt_reset(arm->body[j]);
                        compile_expr(arm->body[arm->body_count - 1]->as.expr, line);
                        end_scope_preserve_tos(line);
                    } else {
                        for (size_t j = 0; j < arm->body_count; j++) compile_stmt_reset(arm->body[j]);
                        end_scope(line);
                        emit_byte(OP_UNIT, line);
                    }

                    /* Stack: [S(dummy), n(binding), result].
                     * Swap result past n, then pop n and S via end_scope. */
                    emit_byte(OP_SWAP, line); /* [S, result, n] */
                    emit_byte(OP_POP, line);  /* [S, result] — pop n manually */
                    emit_byte(OP_SWAP, line); /* [result, S] */
                    emit_byte(OP_POP, line);  /* [result] — pop S manually */
                    /* Remove locals from compiler without emitting more pops */
                    current->scope_depth--;
                    while (current->local_count > 0 &&
                           current->locals[current->local_count - 1].depth > current->scope_depth) {
                        free(current->locals[current->local_count - 1].name);
                        current->local_count--;
                    }

                    end_jumps[end_jump_count++] = emit_jump(OP_JUMP, line);

                    patch_jump(next_arm);
                    /* JUMP_IF_FALSE doesn't pop: [S, n, false] */
                    emit_byte(OP_POP, line); /* pop false: [S, n] */
                    emit_byte(OP_POP, line); /* pop n: [S] */
                } else if (arm->pattern->tag == PAT_ARRAY || arm->pattern->tag == PAT_STRUCT) {
                    /* Array/Struct destructuring patterns.
                     * Two-phase approach:
                     * Phase 1: DUP S -> [S, S']. Run structural checks (type, length, literal values)
                     *          producing a single bool on the stack: [S, S', bool].
                     *          All checks are chained via short-circuit AND.
                     * Phase 2: If bool is true, pop bool + S', then re-extract bindings into scope.
                     *          If bool is false, pop bool + S', continue to next arm. */
                    emit_byte(OP_DUP, line); /* [S, S'] */

                    /* Phase 1: check pattern — leave [S, S', bool] on stack */
                    if (arm->pattern->tag == PAT_ARRAY) {
                        ArrayPatElem *pelems = arm->pattern->as.array.elems;
                        size_t pat_count = arm->pattern->as.array.count;
                        int rest_idx = -1;
                        size_t fixed_count = 0;
                        for (size_t k = 0; k < pat_count; k++) {
                            if (pelems[k].is_rest) rest_idx = (int)k;
                            else fixed_count++;
                        }

                        /* typeof(S') == "Array" */
                        emit_byte(OP_DUP, line); /* [S, S', S''] */
                        emit_constant_idx(OP_GET_GLOBAL, OP_GET_GLOBAL_16,
                                          chunk_add_constant(current_chunk(), value_string("typeof")), line);
                        emit_byte(OP_SWAP, line);     /* [S, S', typeof, S''] */
                        emit_bytes(OP_CALL, 1, line); /* [S, S', type_str] */
                        emit_constant(value_string("Array"), line);
                        emit_byte(OP_EQ, line); /* [S, S', bool] */

                        /* Short-circuit: if false, skip length check */
                        size_t skip_len = emit_jump(OP_JUMP_IF_FALSE, line);
                        emit_byte(OP_POP, line); /* pop true */

                        /* Check length */
                        emit_byte(OP_DUP, line); /* [S, S', S''] */
                        {
                            size_t len_ci = chunk_add_constant(current_chunk(), value_string("len"));
                            emit_bytes(OP_INVOKE, (uint8_t)len_ci, line);
                            emit_byte(0, line); /* [S, S', arr_len] */
                        }
                        if (rest_idx >= 0) {
                            emit_constant(value_int((int64_t)fixed_count), line);
                            emit_byte(OP_GTEQ, line); /* [S, S', bool] */
                        } else {
                            emit_constant(value_int((int64_t)pat_count), line);
                            emit_byte(OP_EQ, line); /* [S, S', bool] */
                        }

                        /* Check literal elements if any */
                        for (size_t k = 0; k < pat_count; k++) {
                            Pattern *sub = pelems[k].pattern;
                            if (pelems[k].is_rest) continue;
                            if (sub->tag != PAT_LITERAL) continue;
                            /* Short-circuit: if already false, skip */
                            size_t skip_lit = emit_jump(OP_JUMP_IF_FALSE, line);
                            emit_byte(OP_POP, line); /* pop true */
                            /* Extract element and compare */
                            emit_byte(OP_DUP, line); /* [S, S', S''] */
                            emit_constant(value_int((int64_t)k), line);
                            emit_byte(OP_INDEX, line);           /* [S, S', elem] */
                            compile_expr(sub->as.literal, line); /* [S, S', elem, lit] */
                            emit_byte(OP_EQ, line);              /* [S, S', bool] */
                            size_t lit_done = emit_jump(OP_JUMP, line);
                            patch_jump(skip_lit);
                            patch_jump(lit_done);
                        }

                        size_t len_done = emit_jump(OP_JUMP, line);
                        patch_jump(skip_len);
                        patch_jump(len_done);
                        /* Stack: [S, S', bool] */
                    } else {
                        /* PAT_STRUCT: typeof(S') == "Struct" */
                        emit_byte(OP_DUP, line); /* [S, S', S''] */
                        emit_constant_idx(OP_GET_GLOBAL, OP_GET_GLOBAL_16,
                                          chunk_add_constant(current_chunk(), value_string("typeof")), line);
                        emit_byte(OP_SWAP, line);
                        emit_bytes(OP_CALL, 1, line);
                        emit_constant(value_string("Struct"), line);
                        emit_byte(OP_EQ, line); /* [S, S', bool] */

                        /* Check literal field values */
                        StructPatField *spfields = arm->pattern->as.strct.fields;
                        size_t spat_count = arm->pattern->as.strct.count;
                        for (size_t k = 0; k < spat_count; k++) {
                            if (spfields[k].value_pat == NULL) continue;
                            if (spfields[k].value_pat->tag != PAT_LITERAL) continue;
                            size_t skip_fld = emit_jump(OP_JUMP_IF_FALSE, line);
                            emit_byte(OP_POP, line);
                            emit_byte(OP_DUP, line); /* [S, S', S''] */
                            size_t fci = chunk_add_constant(current_chunk(), value_string(spfields[k].name));
                            emit_bytes(OP_GET_FIELD, (uint8_t)fci, line);
                            compile_expr(spfields[k].value_pat->as.literal, line);
                            emit_byte(OP_EQ, line);
                            size_t fld_done = emit_jump(OP_JUMP, line);
                            patch_jump(skip_fld);
                            patch_jump(fld_done);
                        }
                    }

                    /* Guard: chain with the bool */
                    if (arm->guard) {
                        size_t skip_guard = emit_jump(OP_JUMP_IF_FALSE, line);
                        emit_byte(OP_POP, line);
                        compile_expr(arm->guard, line);
                        size_t guard_done = emit_jump(OP_JUMP, line);
                        patch_jump(skip_guard);
                        patch_jump(guard_done);
                    }

                    /* Stack: [S, S', bool]. Branch on result. */
                    size_t next_arm = emit_jump(OP_JUMP_IF_FALSE, line);
                    emit_byte(OP_POP, line); /* pop true: [S, S'] */
                    emit_byte(OP_POP, line); /* pop S': [S] */

                    /* Phase 2: Extract bindings into scope and compile body */
                    begin_scope();
                    add_local(""); /* dummy for S */

                    if (arm->pattern->tag == PAT_ARRAY) {
                        ArrayPatElem *pelems = arm->pattern->as.array.elems;
                        size_t pat_count = arm->pattern->as.array.count;
                        int rest_idx = -1;
                        for (size_t k = 0; k < pat_count; k++) {
                            if (pelems[k].is_rest) rest_idx = (int)k;
                        }

                        /* Re-extract from S (on stack as local 'dummy') */
                        size_t s_slot = current->local_count - 1;

                        for (size_t k = 0; k < pat_count; k++) {
                            Pattern *sub = pelems[k].pattern;
                            if (pelems[k].is_rest) {
                                /* ...rest: invoke .slice(start, end) */
                                emit_byte(OP_GET_LOCAL, line);
                                emit_byte((uint8_t)s_slot, line);
                                emit_constant(value_int((int64_t)rest_idx), line);
                                /* end = arr_len - after_count */
                                emit_byte(OP_GET_LOCAL, line);
                                emit_byte((uint8_t)s_slot, line);
                                {
                                    size_t lci = chunk_add_constant(current_chunk(), value_string("len"));
                                    emit_bytes(OP_INVOKE, (uint8_t)lci, line);
                                    emit_byte(0, line);
                                }
                                size_t after = pat_count - 1 - (size_t)rest_idx;
                                if (after > 0) {
                                    emit_constant(value_int((int64_t)after), line);
                                    emit_byte(OP_SUB, line);
                                }
                                {
                                    size_t sci = chunk_add_constant(current_chunk(), value_string("slice"));
                                    emit_bytes(OP_INVOKE, (uint8_t)sci, line);
                                    emit_byte(2, line);
                                }
                                if (sub->tag == PAT_BINDING) add_local(sub->as.binding_name);
                                else add_local("");
                                continue;
                            }
                            if (rest_idx >= 0 && (int)k > rest_idx) {
                                emit_byte(OP_GET_LOCAL, line);
                                emit_byte((uint8_t)s_slot, line);
                                {
                                    size_t lci = chunk_add_constant(current_chunk(), value_string("len"));
                                    emit_bytes(OP_INVOKE, (uint8_t)lci, line);
                                    emit_byte(0, line);
                                }
                                emit_constant(value_int((int64_t)(pat_count - 1 - k)), line);
                                emit_byte(OP_SUB, line);
                                emit_byte(OP_GET_LOCAL, line);
                                emit_byte((uint8_t)s_slot, line);
                                emit_byte(OP_SWAP, line);
                                emit_byte(OP_INDEX, line);
                            } else {
                                emit_byte(OP_GET_LOCAL, line);
                                emit_byte((uint8_t)s_slot, line);
                                emit_constant(value_int((int64_t)k), line);
                                emit_byte(OP_INDEX, line);
                            }
                            if (sub->tag == PAT_BINDING) add_local(sub->as.binding_name);
                            else add_local("");
                        }
                    } else {
                        /* PAT_STRUCT: extract fields */
                        StructPatField *spfields = arm->pattern->as.strct.fields;
                        size_t spat_count = arm->pattern->as.strct.count;
                        size_t s_slot = current->local_count - 1;

                        for (size_t k = 0; k < spat_count; k++) {
                            emit_byte(OP_GET_LOCAL, line);
                            emit_byte((uint8_t)s_slot, line);
                            size_t fci = chunk_add_constant(current_chunk(), value_string(spfields[k].name));
                            emit_bytes(OP_GET_FIELD, (uint8_t)fci, line);

                            if (spfields[k].value_pat == NULL) add_local(spfields[k].name);
                            else if (spfields[k].value_pat->tag == PAT_BINDING)
                                add_local(spfields[k].value_pat->as.binding_name);
                            else add_local("");
                        }
                    }

                    /* Compile arm body in nested scope */
                    size_t bindings_pushed = current->local_count - (current->scope_depth > 0 ? 1 : 0);
                    (void)bindings_pushed;
                    begin_scope();
                    if (arm->body_count > 0 && arm->body[arm->body_count - 1]->tag == STMT_EXPR) {
                        for (size_t j = 0; j + 1 < arm->body_count; j++) compile_stmt_reset(arm->body[j]);
                        compile_expr(arm->body[arm->body_count - 1]->as.expr, line);
                        end_scope_preserve_tos(line);
                    } else {
                        for (size_t j = 0; j < arm->body_count; j++) compile_stmt_reset(arm->body[j]);
                        end_scope(line);
                        emit_byte(OP_UNIT, line);
                    }

                    /* Result on top: [..., S, bindings..., result].
                     * Swap result past all binding locals + S via end_scope. */
                    {
                        /* Count how many locals are in current scope (including dummy for S) */
                        size_t n_scope_locals = 0;
                        for (size_t li = current->local_count; li > 0; li--) {
                            if (current->locals[li - 1].depth < current->scope_depth) break;
                            n_scope_locals++;
                        }
                        for (size_t sl = 0; sl < n_scope_locals; sl++) {
                            emit_byte(OP_SWAP, line);
                            emit_byte(OP_POP, line);
                        }
                    }

                    /* Remove locals from compiler */
                    current->scope_depth--;
                    while (current->local_count > 0 &&
                           current->locals[current->local_count - 1].depth > current->scope_depth) {
                        free(current->locals[current->local_count - 1].name);
                        current->local_count--;
                    }

                    end_jumps[end_jump_count++] = emit_jump(OP_JUMP, line);

                    /* Fail path: [S, S', false] */
                    patch_jump(next_arm);
                    emit_byte(OP_POP, line); /* pop false: [S, S'] */
                    emit_byte(OP_POP, line); /* pop S': [S] */
                } else {
                    /* Non-binding: LITERAL, WILDCARD, RANGE */
                    emit_byte(OP_DUP, line); /* [S, S'] */

                    /* Pattern check — all leave [S, S', bool] */
                    switch (arm->pattern->tag) {
                        case PAT_LITERAL:
                            emit_byte(OP_DUP, line); /* [S, S', S''] */
                            compile_expr(arm->pattern->as.literal, line);
                            emit_byte(OP_EQ, line); /* [S, S', bool] */
                            break;
                        case PAT_WILDCARD:
                            if (arm->pattern->phase_qualifier == PHASE_FLUID) {
                                /* fluid _ => check phase is NOT crystal */
                                emit_byte(OP_DUP, line);        /* [S, S', S''] */
                                emit_byte(OP_IS_CRYSTAL, line); /* [S, S', is_crystal] */
                                emit_byte(OP_NOT, line);        /* [S, S', !is_crystal] */
                            } else if (arm->pattern->phase_qualifier == PHASE_CRYSTAL) {
                                /* crystal _ => check phase IS crystal */
                                emit_byte(OP_DUP, line);        /* [S, S', S''] */
                                emit_byte(OP_IS_CRYSTAL, line); /* [S, S', is_crystal] */
                            } else {
                                emit_byte(OP_TRUE, line); /* [S, S', true] */
                            }
                            break;
                        case PAT_RANGE:
                            emit_byte(OP_DUP, line);
                            compile_expr(arm->pattern->as.range.start, line);
                            emit_byte(OP_GTEQ, line);
                            {
                                size_t range_fail = emit_jump(OP_JUMP_IF_FALSE, line);
                                emit_byte(OP_POP, line);
                                emit_byte(OP_DUP, line);
                                compile_expr(arm->pattern->as.range.end, line);
                                emit_byte(OP_LTEQ, line);
                                size_t range_done = emit_jump(OP_JUMP, line);
                                patch_jump(range_fail);
                                patch_jump(range_done);
                            }
                            break;
                        default: break;
                    }

                    /* Guard */
                    if (arm->guard) {
                        size_t guard_skip = emit_jump(OP_JUMP_IF_FALSE, line);
                        emit_byte(OP_POP, line);
                        compile_expr(arm->guard, line);
                        size_t guard_done = emit_jump(OP_JUMP, line);
                        patch_jump(guard_skip);
                        patch_jump(guard_done);
                    }

                    size_t next_arm = emit_jump(OP_JUMP_IF_FALSE, line);
                    emit_byte(OP_POP, line); /* pop bool: [S, S'] */
                    emit_byte(OP_POP, line); /* pop S': [S] */
                    emit_byte(OP_POP, line); /* pop S: [] */

                    /* Compile arm body */
                    if (arm->body_count > 0 && arm->body[arm->body_count - 1]->tag == STMT_EXPR) {
                        for (size_t j = 0; j + 1 < arm->body_count; j++) compile_stmt_reset(arm->body[j]);
                        compile_expr(arm->body[arm->body_count - 1]->as.expr, line);
                    } else {
                        for (size_t j = 0; j < arm->body_count; j++) compile_stmt_reset(arm->body[j]);
                        emit_byte(OP_UNIT, line);
                    }

                    end_jumps[end_jump_count++] = emit_jump(OP_JUMP, line);

                    patch_jump(next_arm);
                    /* JUMP_IF_FALSE doesn't pop: [S, S', false] */
                    emit_byte(OP_POP, line); /* pop false: [S, S'] */
                    emit_byte(OP_POP, line); /* pop S': [S] */
                }
            }

            /* No arm matched - pop scrutinee, push nil */
            emit_byte(OP_POP, line);
            emit_byte(OP_NIL, line);

            for (size_t i = 0; i < end_jump_count; i++) patch_jump(end_jumps[i]);
            free(end_jumps);
            break;
        }

        case EXPR_TRY_CATCH: {
            size_t handler_jump = emit_jump(OP_PUSH_EXCEPTION_HANDLER, line);

            /* Save compiler state before try body so we can restore it
             * for the catch path (the error handler resets the stack). */
            size_t saved_local_count = current->local_count;
            int saved_scope_depth = current->scope_depth;

            /* Try body — if last stmt is an expression, use its value */
            if (e->as.try_catch.try_count > 0 &&
                e->as.try_catch.try_stmts[e->as.try_catch.try_count - 1]->tag == STMT_EXPR) {
                for (size_t i = 0; i + 1 < e->as.try_catch.try_count; i++) compile_stmt(e->as.try_catch.try_stmts[i]);
                compile_expr(e->as.try_catch.try_stmts[e->as.try_catch.try_count - 1]->as.expr, line);
            } else {
                for (size_t i = 0; i < e->as.try_catch.try_count; i++) compile_stmt(e->as.try_catch.try_stmts[i]);
                emit_byte(OP_UNIT, line);
            }

            emit_byte(OP_POP_EXCEPTION_HANDLER, line);
            size_t end_jump = emit_jump(OP_JUMP, line);

            /* Catch block — restore compiler state to match the stack
             * state after the error handler fires (stack reset + error pushed). */
            patch_jump(handler_jump);
            /* Free any locals created in the try body */
            while (current->local_count > saved_local_count) { free(current->locals[--current->local_count].name); }
            current->scope_depth = saved_scope_depth;

            /* Error value is on stack */
            if (e->as.try_catch.catch_var) {
                add_local(e->as.try_catch.catch_var);
                size_t catch_slot = current->local_count - 1;

                if (e->as.try_catch.catch_count > 0 &&
                    e->as.try_catch.catch_stmts[e->as.try_catch.catch_count - 1]->tag == STMT_EXPR) {
                    for (size_t i = 0; i + 1 < e->as.try_catch.catch_count; i++)
                        compile_stmt(e->as.try_catch.catch_stmts[i]);
                    compile_expr(e->as.try_catch.catch_stmts[e->as.try_catch.catch_count - 1]->as.expr, line);
                } else {
                    for (size_t i = 0; i < e->as.try_catch.catch_count; i++)
                        compile_stmt(e->as.try_catch.catch_stmts[i]);
                    emit_byte(OP_UNIT, line);
                }

                /* Result is on TOS, catch var is below. Overwrite catch var
                 * with the result, pop the extra copy, then remove the local. */
                emit_bytes(OP_SET_LOCAL, (uint8_t)catch_slot, line);
                emit_byte(OP_POP, line);
                free(current->locals[--current->local_count].name);
            } else {
                emit_byte(OP_POP, line);

                if (e->as.try_catch.catch_count > 0 &&
                    e->as.try_catch.catch_stmts[e->as.try_catch.catch_count - 1]->tag == STMT_EXPR) {
                    for (size_t i = 0; i + 1 < e->as.try_catch.catch_count; i++)
                        compile_stmt(e->as.try_catch.catch_stmts[i]);
                    compile_expr(e->as.try_catch.catch_stmts[e->as.try_catch.catch_count - 1]->as.expr, line);
                } else {
                    for (size_t i = 0; i < e->as.try_catch.catch_count; i++)
                        compile_stmt(e->as.try_catch.catch_stmts[i]);
                    emit_byte(OP_UNIT, line);
                }
            }

            patch_jump(end_jump);
            break;
        }

        case EXPR_TRY_PROPAGATE: {
            compile_expr(e->as.try_propagate_expr, line);
            emit_byte(OP_TRY_UNWRAP, line);
            break;
        }

        case EXPR_INTERP_STRING: {
            /* Build interpolated string: part0 + expr0 + part1 + expr1 + ... + partN */
            bool first = true;
            for (size_t i = 0; i < e->as.interp.count; i++) {
                /* String part before expression */
                if (e->as.interp.parts[i] && e->as.interp.parts[i][0] != '\0') {
                    emit_constant(value_string(e->as.interp.parts[i]), line);
                    if (!first) emit_byte(OP_ADD, line);
                    first = false;
                }
                /* Expression */
                compile_expr(e->as.interp.exprs[i], line);
                if (!first) emit_byte(OP_ADD, line);
                first = false;
            }
            /* Final string part */
            if (e->as.interp.parts[e->as.interp.count] && e->as.interp.parts[e->as.interp.count][0] != '\0') {
                emit_constant(value_string(e->as.interp.parts[e->as.interp.count]), line);
                if (!first) emit_byte(OP_ADD, line);
                first = false;
            }
            if (first) {
                /* Empty interpolated string */
                emit_constant(value_string(""), line);
            }
            break;
        }

        case EXPR_ENUM_VARIANT: {
            if (!is_known_enum(e->as.enum_variant.enum_name)) {
                /* Not a declared enum — fall back to global function call
                 * e.g. Map::new() calls the "Map::new" builtin */
                char key[256];
                snprintf(key, sizeof(key), "%s::%s", e->as.enum_variant.enum_name, e->as.enum_variant.variant_name);
                size_t fn_idx = chunk_add_constant(current_chunk(), value_string(key));
                emit_constant_idx(OP_GET_GLOBAL, OP_GET_GLOBAL_16, fn_idx, line);
                for (size_t i = 0; i < e->as.enum_variant.arg_count; i++)
                    compile_expr(e->as.enum_variant.args[i], line);
                emit_bytes(OP_CALL, (uint8_t)e->as.enum_variant.arg_count, line);
                break;
            }
            for (size_t i = 0; i < e->as.enum_variant.arg_count; i++) compile_expr(e->as.enum_variant.args[i], line);
            size_t enum_idx = chunk_add_constant(current_chunk(), value_string(e->as.enum_variant.enum_name));
            size_t var_idx = chunk_add_constant(current_chunk(), value_string(e->as.enum_variant.variant_name));
            emit_byte(OP_BUILD_ENUM, line);
            emit_byte((uint8_t)enum_idx, line);
            emit_byte((uint8_t)var_idx, line);
            emit_byte((uint8_t)e->as.enum_variant.arg_count, line);
            break;
        }

        case EXPR_FREEZE: {
            /* ── Partial freeze: freeze(s.field) for struct fields ── */
            if (e->as.freeze.expr->tag == EXPR_FIELD_ACCESS) {
                Expr *parent = e->as.freeze.expr->as.field_access.object;
                const char *field = e->as.freeze.expr->as.field_access.field;
                if (parent->tag == EXPR_IDENT) {
                    const char *pname = parent->as.str_val;
                    emit_constant(value_string(field), line);
                    size_t pname_idx = chunk_add_constant(current_chunk(), value_string(pname));
                    int slot = resolve_local(current, pname);
                    emit_byte(OP_FREEZE_FIELD, line);
                    emit_byte((uint8_t)pname_idx, line);
                    if (slot >= 0) {
                        emit_byte(0, line);
                        emit_byte((uint8_t)slot, line);
                    } else {
                        int upvalue = resolve_upvalue(current, pname);
                        if (upvalue >= 0) {
                            emit_byte(1, line);
                            emit_byte((uint8_t)upvalue, line);
                        } else {
                            emit_byte(2, line);
                            emit_byte(0, line);
                        }
                    }
                } else {
                    /* Non-ident parent: fall back to OP_FREEZE on field value */
                    compile_expr(e->as.freeze.expr, line);
                    emit_byte(OP_FREEZE, line);
                }
                break;
            }

            /* ── Partial freeze: freeze(m["key"]) for map keys ── */
            if (e->as.freeze.expr->tag == EXPR_INDEX) {
                Expr *parent = e->as.freeze.expr->as.index.object;
                if (parent->tag == EXPR_IDENT) {
                    const char *pname = parent->as.str_val;
                    compile_expr(e->as.freeze.expr->as.index.index, line);
                    size_t pname_idx = chunk_add_constant(current_chunk(), value_string(pname));
                    int slot = resolve_local(current, pname);
                    emit_byte(OP_FREEZE_FIELD, line);
                    emit_byte((uint8_t)pname_idx, line);
                    if (slot >= 0) {
                        emit_byte(0, line);
                        emit_byte((uint8_t)slot, line);
                    } else {
                        int upvalue = resolve_upvalue(current, pname);
                        if (upvalue >= 0) {
                            emit_byte(1, line);
                            emit_byte((uint8_t)upvalue, line);
                        } else {
                            emit_byte(2, line);
                            emit_byte(0, line);
                        }
                    }
                } else {
                    compile_expr(e->as.freeze.expr, line);
                    emit_byte(OP_FREEZE, line);
                }
                break;
            }

            /* ── Freeze-except: freeze(x) except ["field1", ...] ── */
            if (e->as.freeze.except_count > 0 && e->as.freeze.expr->tag == EXPR_IDENT) {
                const char *name = e->as.freeze.expr->as.str_val;
                size_t name_idx = chunk_add_constant(current_chunk(), value_string(name));
                for (size_t i = 0; i < e->as.freeze.except_count; i++)
                    compile_expr(e->as.freeze.except_fields[i], line);
                emit_byte(OP_FREEZE_EXCEPT, line);
                emit_byte((uint8_t)name_idx, line);
                int slot = resolve_local(current, name);
                if (slot >= 0) {
                    emit_byte(0, line);
                    emit_byte((uint8_t)slot, line);
                } else {
                    int upvalue = resolve_upvalue(current, name);
                    if (upvalue >= 0) {
                        emit_byte(1, line);
                        emit_byte((uint8_t)upvalue, line);
                    } else {
                        emit_byte(2, line);
                        emit_byte(0, line);
                    }
                }
                emit_byte((uint8_t)e->as.freeze.except_count, line);
                break;
            }

            /* ── Normal freeze (with optional contract) ── */
            compile_expr(e->as.freeze.expr, line);

            /* Freeze contract: freeze(x) where |v| { ... } */
            if (e->as.freeze.contract && e->as.freeze.expr->tag == EXPR_IDENT) {
                /* Stack: [value]. Handler saves this state. */
                size_t handler_jump = emit_jump(OP_PUSH_EXCEPTION_HANDLER, line);
                emit_byte(OP_DUP, line);
                compile_expr(e->as.freeze.contract, line);
                emit_byte(OP_SWAP, line);
                emit_byte(OP_CALL, line);
                emit_byte(1, line);
                emit_byte(OP_POP, line);
                emit_byte(OP_POP_EXCEPTION_HANDLER, line);
                size_t past_catch = emit_jump(OP_JUMP, line);
                /* Catch: stack restored to [value], then error pushed → [value, error] */
                patch_jump(handler_jump);
                {
                    size_t prefix_idx = chunk_add_constant(current_chunk(), value_string("freeze contract failed: "));
                    emit_byte(OP_CONSTANT, line);
                    emit_byte((uint8_t)prefix_idx, line);
                }
                emit_byte(OP_SWAP, line);
                emit_byte(OP_CONCAT, line);
                /* [value, "freeze contract failed: <msg>"] → throw */
                emit_byte(OP_SWAP, line);
                emit_byte(OP_POP, line); /* pop value */
                emit_byte(OP_THROW, line);
                patch_jump(past_catch);
                /* Stack: [value] — proceed with freeze */
            }

            if (e->as.freeze.expr->tag == EXPR_IDENT) {
                const char *name = e->as.freeze.expr->as.str_val;
                size_t name_idx = chunk_add_constant(current_chunk(), value_string(name));
                int slot = resolve_local(current, name);
                if (slot >= 0) {
                    emit_byte(OP_FREEZE_VAR, line);
                    emit_byte((uint8_t)name_idx, line);
                    emit_byte(0, line);
                    emit_byte((uint8_t)slot, line);
                } else {
                    int upvalue = resolve_upvalue(current, name);
                    if (upvalue >= 0) {
                        emit_byte(OP_FREEZE_VAR, line);
                        emit_byte((uint8_t)name_idx, line);
                        emit_byte(1, line);
                        emit_byte((uint8_t)upvalue, line);
                    } else {
                        emit_byte(OP_FREEZE_VAR, line);
                        emit_byte((uint8_t)name_idx, line);
                        emit_byte(2, line);
                        emit_byte(0, line);
                    }
                }
            } else {
                emit_byte(OP_FREEZE, line);
            }
            break;
        }

        case EXPR_THAW: {
            compile_expr(e->as.freeze_expr, line);
            if (e->as.freeze_expr->tag == EXPR_IDENT) {
                const char *name = e->as.freeze_expr->as.str_val;
                size_t name_idx = chunk_add_constant(current_chunk(), value_string(name));
                int slot = resolve_local(current, name);
                if (slot >= 0) {
                    emit_byte(OP_THAW_VAR, line);
                    emit_byte((uint8_t)name_idx, line);
                    emit_byte(0, line); /* loc_type = local */
                    emit_byte((uint8_t)slot, line);
                } else {
                    int upvalue = resolve_upvalue(current, name);
                    if (upvalue >= 0) {
                        emit_byte(OP_THAW_VAR, line);
                        emit_byte((uint8_t)name_idx, line);
                        emit_byte(1, line); /* loc_type = upvalue */
                        emit_byte((uint8_t)upvalue, line);
                    } else {
                        emit_byte(OP_THAW_VAR, line);
                        emit_byte((uint8_t)name_idx, line);
                        emit_byte(2, line); /* loc_type = global */
                        emit_byte(0, line);
                    }
                }
            } else {
                emit_byte(OP_THAW, line);
            }
            break;
        }

        case EXPR_CLONE: {
            compile_expr(e->as.freeze_expr, line);
            emit_byte(OP_CLONE, line);
            break;
        }

        case EXPR_FORGE: {
            begin_scope();
            /* If last stmt is an expression, use its value as the forge result */
            if (e->as.block.count > 0 && e->as.block.stmts[e->as.block.count - 1]->tag == STMT_EXPR) {
                for (size_t i = 0; i + 1 < e->as.block.count; i++) compile_stmt(e->as.block.stmts[i]);
                compile_expr(e->as.block.stmts[e->as.block.count - 1]->as.expr, line);
            } else {
                for (size_t i = 0; i < e->as.block.count; i++) compile_stmt(e->as.block.stmts[i]);
                emit_byte(OP_UNIT, line);
            }
            end_scope(line);
            emit_byte(OP_FREEZE, line); /* auto-freeze the result */
            break;
        }

        case EXPR_ANNEAL: {
            /* anneal(target, closure) - thaw, call closure, refreeze
             * Must check: target is crystal; wrap closure errors with "anneal failed:" */

            /* Phase check: target must be crystal */
            compile_expr(e->as.anneal.expr, line); /* push target value */
            emit_byte(OP_IS_CRYSTAL, line);        /* replace with bool */
            size_t anneal_ok = emit_jump(OP_JUMP_IF_FALSE, line);
            /* is_crystal=true → continue */
            emit_byte(OP_POP, line); /* pop true */
            size_t anneal_past_check = emit_jump(OP_JUMP, line);
            /* is_crystal=false → error */
            patch_jump(anneal_ok);
            emit_byte(OP_POP, line); /* pop false */
            {
                size_t err_idx = chunk_add_constant(current_chunk(), value_string("anneal requires a crystal value"));
                emit_bytes(OP_CONSTANT, (uint8_t)err_idx, line);
                emit_byte(OP_THROW, line);
            }
            patch_jump(anneal_past_check);

            /* Wrap in try/catch for error prefix */
            size_t anneal_handler = emit_jump(OP_PUSH_EXCEPTION_HANDLER, line);
            size_t saved_lc = current->local_count;
            int saved_sd = current->scope_depth;

            compile_expr(e->as.anneal.closure, line);
            compile_expr(e->as.anneal.expr, line);
            emit_byte(OP_THAW, line);
            emit_bytes(OP_CALL, 1, line);
            if (e->as.anneal.expr->tag == EXPR_IDENT) {
                const char *name = e->as.anneal.expr->as.str_val;
                size_t name_idx = chunk_add_constant(current_chunk(), value_string(name));
                int slot = resolve_local(current, name);
                if (slot >= 0) {
                    emit_byte(OP_FREEZE_VAR, line);
                    emit_byte((uint8_t)name_idx, line);
                    emit_byte(0, line);
                    emit_byte((uint8_t)slot, line);
                } else {
                    int upvalue = resolve_upvalue(current, name);
                    if (upvalue >= 0) {
                        emit_byte(OP_FREEZE_VAR, line);
                        emit_byte((uint8_t)name_idx, line);
                        emit_byte(1, line);
                        emit_byte((uint8_t)upvalue, line);
                    } else {
                        emit_byte(OP_FREEZE_VAR, line);
                        emit_byte((uint8_t)name_idx, line);
                        emit_byte(2, line);
                        emit_byte(0, line);
                    }
                }
            } else {
                emit_byte(OP_FREEZE, line);
            }

            /* End of try block — pop handler, jump past catch */
            emit_byte(OP_POP_EXCEPTION_HANDLER, line);
            size_t anneal_end = emit_jump(OP_JUMP, line);

            /* Catch block — wrap error with "anneal failed: " prefix */
            patch_jump(anneal_handler);
            while (current->local_count > saved_lc) { free(current->locals[--current->local_count].name); }
            current->scope_depth = saved_sd;
            /* Error string is on TOS. Concatenate with prefix. */
            {
                size_t prefix_idx = chunk_add_constant(current_chunk(), value_string("anneal failed: "));
                emit_bytes(OP_CONSTANT, (uint8_t)prefix_idx, line);
                emit_byte(OP_SWAP, line);
                emit_byte(OP_CONCAT, line);
                emit_byte(OP_THROW, line);
            }
            patch_jump(anneal_end);
            break;
        }

        case EXPR_CRYSTALLIZE: {
            /* crystallize(target) { body } — freeze target, run body, thaw target
             * If already crystal, just run body without thaw afterward. */
            if (e->as.crystallize.expr->tag == EXPR_IDENT) {
                const char *name = e->as.crystallize.expr->as.str_val;
                size_t name_idx = chunk_add_constant(current_chunk(), value_string(name));
                int loc_type = 2;
                uint8_t slot = 0;
                int local_slot = resolve_local(current, name);
                if (local_slot != -1) {
                    loc_type = 0;
                    slot = (uint8_t)local_slot;
                } else {
                    int up_idx = resolve_upvalue(current, name);
                    if (up_idx != -1) {
                        loc_type = 1;
                        slot = (uint8_t)up_idx;
                    }
                }
                /* Check if already crystal — save result as a local */
                begin_scope();
                compile_expr(e->as.crystallize.expr, line); /* push current value */
                emit_byte(OP_IS_CRYSTAL, line);             /* replace with bool */
                add_local("");                              /* track the was_crystal flag */
                int flag_slot = current->local_count - 1;
                /* Freeze the variable */
                compile_expr(e->as.crystallize.expr, line);
                emit_byte(OP_FREEZE_VAR, line);
                emit_byte((uint8_t)name_idx, line);
                emit_byte((uint8_t)loc_type, line);
                emit_byte(slot, line);
                emit_byte(OP_POP, line); /* discard freeze result */
                /* Execute body */
                for (size_t i = 0; i < e->as.crystallize.body_count; i++) compile_stmt(e->as.crystallize.body[i]);
                /* Conditional thaw: only if was NOT already crystal */
                emit_bytes(OP_GET_LOCAL, (uint8_t)flag_slot, line); /* push was_crystal */
                size_t skip_thaw = emit_jump(OP_JUMP_IF_FALSE, line);
                /* was_crystal=true → skip thaw */
                emit_byte(OP_POP, line); /* pop true */
                size_t past_thaw = emit_jump(OP_JUMP, line);
                /* was_crystal=false → do thaw */
                patch_jump(skip_thaw);
                emit_byte(OP_POP, line); /* pop false */
                compile_expr(e->as.crystallize.expr, line);
                emit_byte(OP_THAW_VAR, line);
                emit_byte((uint8_t)name_idx, line);
                emit_byte((uint8_t)loc_type, line);
                emit_byte(slot, line);
                emit_byte(OP_POP, line); /* discard thaw result */
                patch_jump(past_thaw);
                end_scope(line); /* clean up was_crystal flag */
                emit_byte(OP_UNIT, line);
            } else {
                /* Fallback: just freeze the expression */
                compile_expr(e->as.crystallize.expr, line);
                emit_byte(OP_FREEZE, line);
            }
            break;
        }

        case EXPR_BORROW: {
            /* borrow(target) { body } — thaw target, run body, freeze target
             * If already fluid, just run body without freeze afterward. */
            if (e->as.borrow.expr->tag == EXPR_IDENT) {
                const char *name = e->as.borrow.expr->as.str_val;
                size_t name_idx = chunk_add_constant(current_chunk(), value_string(name));
                int loc_type = 2;
                uint8_t slot = 0;
                int local_slot = resolve_local(current, name);
                if (local_slot != -1) {
                    loc_type = 0;
                    slot = (uint8_t)local_slot;
                } else {
                    int up_idx = resolve_upvalue(current, name);
                    if (up_idx != -1) {
                        loc_type = 1;
                        slot = (uint8_t)up_idx;
                    }
                }
                /* Check if already fluid — save result as a local */
                begin_scope();
                compile_expr(e->as.borrow.expr, line); /* push current value */
                emit_byte(OP_IS_FLUID, line);          /* replace with bool */
                add_local("");                         /* track the was_fluid flag */
                int flag_slot = current->local_count - 1;
                /* Thaw the variable */
                compile_expr(e->as.borrow.expr, line);
                emit_byte(OP_THAW_VAR, line);
                emit_byte((uint8_t)name_idx, line);
                emit_byte((uint8_t)loc_type, line);
                emit_byte(slot, line);
                emit_byte(OP_POP, line); /* discard thaw result */
                /* Execute body */
                for (size_t i = 0; i < e->as.borrow.body_count; i++) compile_stmt(e->as.borrow.body[i]);
                /* Conditional freeze: only if was NOT already fluid */
                emit_bytes(OP_GET_LOCAL, (uint8_t)flag_slot, line); /* push was_fluid */
                size_t skip_freeze = emit_jump(OP_JUMP_IF_FALSE, line);
                /* was_fluid=true → skip freeze */
                emit_byte(OP_POP, line); /* pop true */
                size_t past_freeze = emit_jump(OP_JUMP, line);
                /* was_fluid=false → do freeze */
                patch_jump(skip_freeze);
                emit_byte(OP_POP, line); /* pop false */
                compile_expr(e->as.borrow.expr, line);
                emit_byte(OP_FREEZE_VAR, line);
                emit_byte((uint8_t)name_idx, line);
                emit_byte((uint8_t)loc_type, line);
                emit_byte(slot, line);
                emit_byte(OP_POP, line); /* discard freeze result */
                patch_jump(past_freeze);
                end_scope(line); /* clean up was_fluid flag */
                emit_byte(OP_UNIT, line);
            } else {
                /* Fallback: just thaw the expression */
                compile_expr(e->as.borrow.expr, line);
                emit_byte(OP_THAW, line);
            }
            break;
        }

        case EXPR_SUBLIMATE: {
            compile_expr(e->as.freeze_expr, line);
            if (e->as.freeze_expr->tag == EXPR_IDENT) {
                const char *name = e->as.freeze_expr->as.str_val;
                size_t name_idx = chunk_add_constant(current_chunk(), value_string(name));
                int slot = resolve_local(current, name);
                if (slot >= 0) {
                    emit_byte(OP_SUBLIMATE_VAR, line);
                    emit_byte((uint8_t)name_idx, line);
                    emit_byte(0, line); /* loc_type = local */
                    emit_byte((uint8_t)slot, line);
                } else {
                    int upvalue = resolve_upvalue(current, name);
                    if (upvalue >= 0) {
                        emit_byte(OP_SUBLIMATE_VAR, line);
                        emit_byte((uint8_t)name_idx, line);
                        emit_byte(1, line); /* loc_type = upvalue */
                        emit_byte((uint8_t)upvalue, line);
                    } else {
                        emit_byte(OP_SUBLIMATE_VAR, line);
                        emit_byte((uint8_t)name_idx, line);
                        emit_byte(2, line); /* loc_type = global */
                        emit_byte(0, line);
                    }
                }
            } else {
                emit_byte(OP_SUBLIMATE, line);
            }
            break;
        }

        case EXPR_SPREAD: {
            compile_expr(e->as.spread_expr, line);
            break;
        }

        case EXPR_SPAWN: {
            /* Compile as OP_SCOPE with 0 spawns (sub-chunk so return works) */
            Chunk *spawn_ch = compile_sub_body(e->as.block.stmts, e->as.block.count, line);
            uint8_t body_idx = (uint8_t)add_chunk_constant(spawn_ch);
            emit_byte(OP_SCOPE, line);
            emit_byte(0, line);        /* spawn_count = 0 */
            emit_byte(body_idx, line); /* sync_idx */
            break;
        }

        case EXPR_SCOPE: {
#ifdef __EMSCRIPTEN__
            /* WASM fallback: run as synchronous block */
            for (size_t i = 0; i < e->as.block.count; i++) compile_stmt(e->as.block.stmts[i]);
            emit_byte(OP_UNIT, line);
#else
            size_t total = e->as.block.count;

            /* Count spawns and collect non-spawn stmts */
            size_t spawn_count = 0;
            for (size_t i = 0; i < total; i++) {
                Stmt *s = e->as.block.stmts[i];
                if (s->tag == STMT_EXPR && s->as.expr->tag == EXPR_SPAWN) spawn_count++;
            }
            size_t sync_count = total - spawn_count;

            /* Compile sync body (all non-spawn stmts together) */
            uint8_t sync_idx = 0xFF;
            if (sync_count > 0) {
                Stmt **sync_stmts = malloc(sync_count * sizeof(Stmt *));
                if (!sync_stmts) return;
                size_t si = 0;
                for (size_t i = 0; i < total; i++) {
                    Stmt *s = e->as.block.stmts[i];
                    if (!(s->tag == STMT_EXPR && s->as.expr->tag == EXPR_SPAWN)) sync_stmts[si++] = s;
                }
                Chunk *sync_chunk = compile_sub_body(sync_stmts, sync_count, line);
                sync_idx = (uint8_t)add_chunk_constant(sync_chunk);
                free(sync_stmts);
            }

            /* Compile each spawn body */
            uint8_t *spawn_indices = NULL;
            if (spawn_count > 0) {
                spawn_indices = malloc(spawn_count * sizeof(uint8_t));
                if (!spawn_indices) return;
            }
            size_t spi = 0;
            for (size_t i = 0; i < total; i++) {
                Stmt *s = e->as.block.stmts[i];
                if (s->tag == STMT_EXPR && s->as.expr->tag == EXPR_SPAWN) {
                    Expr *spawn_expr = s->as.expr;
                    Chunk *spawn_ch = compile_sub_body(spawn_expr->as.block.stmts, spawn_expr->as.block.count, line);
                    spawn_indices[spi++] = (uint8_t)add_chunk_constant(spawn_ch);
                }
            }

            /* Emit: OP_SCOPE spawn_count sync_idx [spawn_idx...] */
            emit_byte(OP_SCOPE, line);
            emit_byte((uint8_t)spawn_count, line);
            emit_byte(sync_idx, line);
            for (size_t i = 0; i < spawn_count; i++) emit_byte(spawn_indices[i], line);
            free(spawn_indices);
#endif
            break;
        }

        case EXPR_SELECT: {
#ifdef __EMSCRIPTEN__
            emit_byte(OP_NIL, line);
#else
            size_t arm_count = e->as.select_expr.arm_count;
            SelectArm *arms = e->as.select_expr.arms;

            emit_byte(OP_SELECT, line);
            emit_byte((uint8_t)arm_count, line);

            for (size_t i = 0; i < arm_count; i++) {
                uint8_t flags = 0;
                if (arms[i].is_default) flags |= 0x01;
                if (arms[i].is_timeout) flags |= 0x02;
                if (arms[i].binding_name) flags |= 0x04;
                emit_byte(flags, line);

                /* Channel or timeout expression chunk */
                if (arms[i].is_default) {
                    emit_byte(0xFF, line);
                } else if (arms[i].is_timeout) {
                    Chunk *to_ch = compile_sub_expr(arms[i].timeout_expr, line);
                    emit_byte((uint8_t)add_chunk_constant(to_ch), line);
                } else {
                    Chunk *ch_ch = compile_sub_expr(arms[i].channel_expr, line);
                    emit_byte((uint8_t)add_chunk_constant(ch_ch), line);
                }

                /* Body chunk */
                Chunk *body_ch = compile_sub_body(arms[i].body, arms[i].body_count, line);
                emit_byte((uint8_t)add_chunk_constant(body_ch), line);

                /* Binding name (string constant or 0xFF) */
                if (arms[i].binding_name) {
                    emit_byte((uint8_t)chunk_add_constant(current_chunk(), value_string(arms[i].binding_name)), line);
                } else {
                    emit_byte(0xFF, line);
                }
            }
#endif
            break;
        }

        default: emit_byte(OP_NIL, line); break;
    }
}

/* ── Emit ensure contract checks (postconditions) ── */
/* Expects the return value on TOS. Leaves it on TOS unchanged. */
static void emit_ensure_checks(int line) {
    if (!current->contracts || current->contract_count == 0) return;
    for (size_t i = 0; i < current->contract_count; i++) {
        if (!current->contracts[i].is_ensure) continue;
        /* Stack: [..., return_val]
         * Need to call closure(return_val) and check result.
         * OP_CALL 1 expects: [..., callee, arg0] */
        emit_byte(OP_DUP, line);                             /* [..., ret, ret_copy] */
        compile_expr(current->contracts[i].condition, line); /* [..., ret, ret_copy, closure] */
        emit_byte(OP_SWAP, line);                            /* [..., ret, closure, ret_copy] */
        emit_bytes(OP_CALL, 1, line);                        /* [..., ret, result] */
        size_t ok_jump = emit_jump(OP_JUMP_IF_TRUE, line);
        emit_byte(OP_POP, line); /* pop false */
        emit_byte(OP_POP, line); /* pop return value */
        const char *user_msg = current->contracts[i].message ? current->contracts[i].message : "condition not met";
        char full_msg[512];
        snprintf(full_msg, sizeof(full_msg), "ensure failed in '%s': %s",
                 current->func_name ? current->func_name : "<anonymous>", user_msg);
        emit_constant(value_string(full_msg), line);
        emit_byte(OP_THROW, line);
        patch_jump(ok_jump);
        emit_byte(OP_POP, line); /* pop true */
    }
}

/* ── Emit return type check (if return_type_name is set) ── */
/* Expects the return value on TOS. Leaves it on TOS unchanged. */
static void emit_return_type_check(int line) {
    if (!current->return_type_name) return;
    if (strcmp(current->return_type_name, "Any") == 0 || strcmp(current->return_type_name, "any") == 0) return;
    char err_msg[512];
    snprintf(err_msg, sizeof(err_msg), "function '%s' return type expects %s, got %%s",
             current->func_name ? current->func_name : "<anonymous>", current->return_type_name);
    size_t type_idx = chunk_add_constant(current_chunk(), value_string(current->return_type_name));
    size_t err_idx = chunk_add_constant(current_chunk(), value_string(err_msg));
    emit_byte(OP_CHECK_RETURN_TYPE, line);
    emit_byte((uint8_t)type_idx, line);
    emit_byte((uint8_t)err_idx, line);
}

/* Emit write-back chain for nested index assignment.
 * After OP_SET_INDEX leaves a modified intermediate on the stack,
 * walk up through parent EXPR_INDEX nodes to the root variable. */
static void emit_index_write_back(Expr *node, int line) {
    if (node->as.index.object->tag == EXPR_IDENT) {
        const char *name = node->as.index.object->as.str_val;
        int slot = resolve_local(current, name);
        if (slot >= 0) {
            compile_expr(node->as.index.index, line);
            emit_bytes(OP_SET_INDEX_LOCAL, (uint8_t)slot, line);
        } else {
            /* Global/upvalue: get parent, set index, write back */
            compile_expr(node->as.index.object, line);
            compile_expr(node->as.index.index, line);
            emit_byte(OP_SET_INDEX, line);
            int upvalue = resolve_upvalue(current, name);
            if (upvalue >= 0) {
                emit_bytes(OP_SET_UPVALUE, (uint8_t)upvalue, line);
            } else {
                size_t gidx = chunk_add_constant(current_chunk(), value_string(name));
                emit_constant_idx(OP_SET_GLOBAL, OP_SET_GLOBAL_16, gidx, line);
            }
        }
    } else if (node->as.index.object->tag == EXPR_INDEX) {
        /* Intermediate level: compile parent, set index, recurse up */
        compile_expr(node->as.index.object, line);
        compile_expr(node->as.index.index, line);
        emit_byte(OP_SET_INDEX, line);
        emit_index_write_back(node->as.index.object, line);
    }
}

/* ── Compile statements ── */

static void compile_stmt(const Stmt *s) {
    if (compile_error) return;

    int line = s->line;
    switch (s->tag) {
        case STMT_EXPR:
            compile_expr(s->as.expr, line);
            emit_byte(OP_POP, line);
            break;

        case STMT_BINDING: {
            if (s->as.binding.value) compile_expr(s->as.binding.value, line);
            else emit_byte(OP_NIL, line);

            /* Apply phase tag for flux/fix declarations */
            if (s->as.binding.phase == PHASE_FLUID) emit_byte(OP_MARK_FLUID, line);
            else if (s->as.binding.phase == PHASE_CRYSTAL) emit_byte(OP_FREEZE, line);

            if (current->scope_depth > 0) {
                add_local(s->as.binding.name);
            } else {
                size_t idx = chunk_add_constant(current_chunk(), value_string(s->as.binding.name));
                emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, idx, line);
            }
            break;
        }

        case STMT_ASSIGN: {
            /* Detect i += 1 / i -= 1 → OP_INC_LOCAL / OP_DEC_LOCAL
             * The parser desugars i += 1 into STMT_ASSIGN(IDENT("i"), BINOP(ADD, IDENT("i"), INT(1)))
             */
            if (s->as.assign.target->tag == EXPR_IDENT && s->as.assign.value->tag == EXPR_BINOP) {
                const char *name = s->as.assign.target->as.str_val;
                Expr *val = s->as.assign.value;
                if (val->as.binop.left->tag == EXPR_IDENT && strcmp(val->as.binop.left->as.str_val, name) == 0 &&
                    val->as.binop.right->tag == EXPR_INT_LIT && val->as.binop.right->as.int_val == 1) {
                    int slot = resolve_local(current, name);
                    if (slot >= 0) {
                        if (val->as.binop.op == BINOP_ADD) {
                            emit_bytes(OP_INC_LOCAL, (uint8_t)slot, line);
                            break; /* INC_LOCAL doesn't push; skip OP_POP at end */
                        }
                        if (val->as.binop.op == BINOP_SUB) {
                            emit_bytes(OP_DEC_LOCAL, (uint8_t)slot, line);
                            break;
                        }
                    }
                }

                /* Detect s += expr → OP_APPEND_STR_LOCAL (string append fast path)
                 * Pattern: STMT_ASSIGN(IDENT(name), BINOP(ADD, IDENT(name), expr))
                 * At runtime, falls back to generic ADD if types aren't strings. */
                if (val->as.binop.op == BINOP_ADD && val->as.binop.left->tag == EXPR_IDENT &&
                    strcmp(val->as.binop.left->as.str_val, name) == 0) {
                    int slot = resolve_local(current, name);
                    if (slot >= 0) {
                        compile_expr(val->as.binop.right, line);
                        emit_bytes(OP_APPEND_STR_LOCAL, (uint8_t)slot, line);
                        break;
                    }
                }
            }

            bool skip_pop = false;
            compile_expr(s->as.assign.value, line);
            if (s->as.assign.target->tag == EXPR_IDENT) {
                const char *name = s->as.assign.target->as.str_val;
                int slot = resolve_local(current, name);
                if (slot >= 0) {
                    emit_bytes(OP_SET_LOCAL_POP, (uint8_t)slot, line);
                    skip_pop = true;
                } else {
                    int upvalue = resolve_upvalue(current, name);
                    if (upvalue >= 0) {
                        emit_bytes(OP_SET_UPVALUE, (uint8_t)upvalue, line);
                    } else {
                        size_t idx = chunk_add_constant(current_chunk(), value_string(name));
                        emit_constant_idx(OP_SET_GLOBAL, OP_SET_GLOBAL_16, idx, line);
                    }
                }
            } else if (s->as.assign.target->tag == EXPR_FIELD_ACCESS) {
                compile_expr(s->as.assign.target->as.field_access.object, line);
                size_t idx =
                    chunk_add_constant(current_chunk(), value_string(s->as.assign.target->as.field_access.field));
                emit_bytes(OP_SET_FIELD, (uint8_t)idx, line);
                /* Write back the modified object to the variable */
                Expr *obj_expr = s->as.assign.target->as.field_access.object;
                if (obj_expr->tag == EXPR_IDENT) {
                    const char *name = obj_expr->as.str_val;
                    int slot = resolve_local(current, name);
                    if (slot >= 0) {
                        emit_bytes(OP_SET_LOCAL, (uint8_t)slot, line);
                    } else {
                        int upvalue = resolve_upvalue(current, name);
                        if (upvalue >= 0) {
                            emit_bytes(OP_SET_UPVALUE, (uint8_t)upvalue, line);
                        } else {
                            size_t gidx = chunk_add_constant(current_chunk(), value_string(name));
                            emit_constant_idx(OP_SET_GLOBAL, OP_SET_GLOBAL_16, gidx, line);
                        }
                    }
                }
            } else if (s->as.assign.target->tag == EXPR_INDEX &&
                       s->as.assign.target->as.index.index->tag == EXPR_RANGE) {
                /* Slice assignment: arr[start..end] = rhs_array */
                Expr *target = s->as.assign.target;
                Expr *range_expr = target->as.index.index;
                /* Stack has: [val] from compile_expr(value) above */

                /* Check if object is a local variable (fast path) */
                if (target->as.index.object->tag == EXPR_IDENT) {
                    int slot = resolve_local(current, target->as.index.object->as.str_val);
                    if (slot >= 0) {
                        compile_expr(range_expr->as.range.start, line);
                        compile_expr(range_expr->as.range.end, line);
                        /* Stack: [val, start, end] */
                        emit_bytes(OP_SET_SLICE_LOCAL, (uint8_t)slot, line);
                        break; /* SET_SLICE_LOCAL pushes nothing, skip OP_POP */
                    }
                }
                /* Non-local: compile obj, start, end then use OP_SET_SLICE */
                /* Stack: [val] */
                compile_expr(target->as.index.object, line);
                compile_expr(range_expr->as.range.start, line);
                compile_expr(range_expr->as.range.end, line);
                /* Stack: [val, obj, start, end] */
                emit_byte(OP_SET_SLICE, line);
                /* Stack: [modified_obj] — write back */
                if (target->as.index.object->tag == EXPR_IDENT) {
                    const char *name = target->as.index.object->as.str_val;
                    int upvalue = resolve_upvalue(current, name);
                    if (upvalue >= 0) {
                        emit_bytes(OP_SET_UPVALUE, (uint8_t)upvalue, line);
                    } else {
                        size_t gidx = chunk_add_constant(current_chunk(), value_string(name));
                        emit_constant_idx(OP_SET_GLOBAL, OP_SET_GLOBAL_16, gidx, line);
                    }
                }
            } else if (s->as.assign.target->tag == EXPR_INDEX) {
                Expr *target = s->as.assign.target;
                /* If object is a local, use OP_SET_INDEX_LOCAL to mutate in-place */
                if (target->as.index.object->tag == EXPR_IDENT) {
                    int slot = resolve_local(current, target->as.index.object->as.str_val);
                    if (slot >= 0) {
                        compile_expr(target->as.index.index, line);
                        emit_bytes(OP_SET_INDEX_LOCAL, (uint8_t)slot, line);
                        break; /* OP_SET_INDEX_LOCAL pushes nothing, skip OP_POP */
                    }
                }
                /* Nested index (e.g. m[i][j] = val): compile intermediate,
                 * SET_INDEX, then write-back chain to root variable */
                if (target->as.index.object->tag == EXPR_INDEX) {
                    compile_expr(target->as.index.object, line);
                    compile_expr(target->as.index.index, line);
                    emit_byte(OP_SET_INDEX, line);
                    emit_index_write_back(target->as.index.object, line);
                    break; /* write-back handles everything, skip OP_POP */
                }
                /* Fallback: non-local single-level index (global/upvalue) */
                compile_expr(target->as.index.object, line);
                compile_expr(target->as.index.index, line);
                emit_byte(OP_SET_INDEX, line);
                /* Write back the modified object to the variable */
                if (target->as.index.object->tag == EXPR_IDENT) {
                    const char *name = target->as.index.object->as.str_val;
                    int upvalue = resolve_upvalue(current, name);
                    if (upvalue >= 0) {
                        emit_bytes(OP_SET_UPVALUE, (uint8_t)upvalue, line);
                    } else {
                        size_t gidx = chunk_add_constant(current_chunk(), value_string(name));
                        emit_constant_idx(OP_SET_GLOBAL, OP_SET_GLOBAL_16, gidx, line);
                    }
                }
            }
            if (!skip_pop) emit_byte(OP_POP, line);
            break;
        }

        case STMT_RETURN:
            if (s->as.return_expr) compile_expr(s->as.return_expr, line);
            else emit_byte(OP_UNIT, line);
            emit_return_type_check(line);
            emit_ensure_checks(line);
            emit_byte(OP_DEFER_RUN, line);
            emit_byte(0, line); /* scope_depth 0 = run all defers */
            emit_byte(OP_RETURN, line);
            break;

        case STMT_WHILE: {
            size_t saved_break_count = current->break_count;
            size_t saved_loop_start = current->loop_start;
            int saved_loop_depth = current->loop_depth;
            size_t saved_break_lc = current->loop_break_local_count;
            size_t saved_continue_lc = current->loop_continue_local_count;

            current->loop_break_local_count = current->local_count;
            current->loop_continue_local_count = current->local_count;
            current->loop_start = current_chunk()->code_len;
            current->loop_depth++;

            compile_expr(s->as.while_loop.cond, line);
            size_t exit_jump = emit_jump(OP_JUMP_IF_FALSE, line);
            emit_byte(OP_POP, line);

            begin_scope();
            for (size_t i = 0; i < s->as.while_loop.body_count; i++) compile_stmt_reset(s->as.while_loop.body[i]);
            end_scope(0);

            emit_loop(current->loop_start, line);
            patch_jump(exit_jump);
            emit_byte(OP_POP, line);

            /* Patch break jumps */
            for (size_t i = saved_break_count; i < current->break_count; i++) patch_jump(current->break_jumps[i]);
            current->break_count = saved_break_count;
            current->loop_start = saved_loop_start;
            current->loop_depth = saved_loop_depth;
            current->loop_break_local_count = saved_break_lc;
            current->loop_continue_local_count = saved_continue_lc;
            break;
        }

        case STMT_LOOP: {
            size_t saved_break_count = current->break_count;
            size_t saved_loop_start = current->loop_start;
            int saved_loop_depth = current->loop_depth;
            size_t saved_break_lc = current->loop_break_local_count;
            size_t saved_continue_lc = current->loop_continue_local_count;

            current->loop_break_local_count = current->local_count;
            current->loop_continue_local_count = current->local_count;
            current->loop_start = current_chunk()->code_len;
            current->loop_depth++;

            begin_scope();
            for (size_t i = 0; i < s->as.loop.body_count; i++) compile_stmt_reset(s->as.loop.body[i]);
            end_scope(0);

            emit_loop(current->loop_start, line);

            /* Patch break jumps */
            for (size_t i = saved_break_count; i < current->break_count; i++) patch_jump(current->break_jumps[i]);
            current->break_count = saved_break_count;
            current->loop_start = saved_loop_start;
            current->loop_depth = saved_loop_depth;
            current->loop_break_local_count = saved_break_lc;
            current->loop_continue_local_count = saved_continue_lc;
            break;
        }

        case STMT_FOR: {
            size_t saved_break_count = current->break_count;
            size_t saved_loop_start = current->loop_start;
            int saved_loop_depth = current->loop_depth;
            size_t saved_break_lc = current->loop_break_local_count;
            size_t saved_continue_lc = current->loop_continue_local_count;

            current->loop_break_local_count = current->local_count;

            begin_scope();
            /* Compile iterator expression and init */
            compile_expr(s->as.for_loop.iter, line);
            emit_byte(OP_ITER_INIT, line);

            /* Track the iterator state (collection + index) as anonymous locals
             * so subsequent local slot numbers are correct. */
            add_local(""); /* collection */
            add_local(""); /* index */

            /* continue should preserve iterator state but pop loop var + body locals */
            current->loop_continue_local_count = current->local_count;

            current->loop_start = current_chunk()->code_len;
            current->loop_depth++;

            /* OP_ITER_NEXT pushes next value or jumps to end */
            size_t exit_jump = emit_jump(OP_ITER_NEXT, line);

            /* Bind loop variable */
            add_local(s->as.for_loop.var);

            /* Compile body in inner scope */
            begin_scope();
            for (size_t i = 0; i < s->as.for_loop.body_count; i++) compile_stmt_reset(s->as.for_loop.body[i]);
            end_scope(0);

            /* Pop loop variable */
            emit_byte(OP_POP, line);
            free(current->locals[current->local_count - 1].name);
            current->local_count--;

            emit_loop(current->loop_start, line);

            patch_jump(exit_jump);
            /* Pop iterator state (two values: index + collection) */
            emit_byte(OP_POP, line);
            emit_byte(OP_POP, line);
            /* Remove iterator placeholder locals */
            free(current->locals[current->local_count - 1].name);
            current->local_count--;
            free(current->locals[current->local_count - 1].name);
            current->local_count--;

            end_scope(0);

            /* Patch break jumps */
            for (size_t i = saved_break_count; i < current->break_count; i++) patch_jump(current->break_jumps[i]);
            current->break_count = saved_break_count;
            current->loop_start = saved_loop_start;
            current->loop_depth = saved_loop_depth;
            current->loop_break_local_count = saved_break_lc;
            current->loop_continue_local_count = saved_continue_lc;
            break;
        }

        case STMT_BREAK: {
            if (current->loop_depth == 0) {
                set_compile_error(s->line, "break outside of loop");
                return;
            }
            /* Pop locals declared inside the loop before jumping out */
            for (size_t i = current->local_count; i > current->loop_break_local_count; i--) {
                if (current->locals[i - 1].is_captured) {
                    emit_byte(OP_CLOSE_UPVALUE, line);
                } else {
                    emit_byte(OP_POP, line);
                }
            }
            size_t jump = emit_jump(OP_JUMP, line);
            push_break_jump(jump);
            break;
        }

        case STMT_CONTINUE: {
            if (current->loop_depth == 0) {
                set_compile_error(s->line, "continue outside of loop");
                return;
            }
            /* Pop locals declared inside the loop before jumping back */
            for (size_t i = current->local_count; i > current->loop_continue_local_count; i--) {
                if (current->locals[i - 1].is_captured) {
                    emit_byte(OP_CLOSE_UPVALUE, line);
                } else {
                    emit_byte(OP_POP, line);
                }
            }
            emit_loop(current->loop_start, line);
            break;
        }

        case STMT_DESTRUCTURE: {
            compile_expr(s->as.destructure.value, line);
            if (current->scope_depth > 0) {
                /* Store source as hidden local so each extraction can reference it */
                size_t src_slot = current->local_count;
                add_local(""); /* hidden local */
                if (s->as.destructure.kind == DESTRUCT_ARRAY) {
                    for (size_t i = 0; i < s->as.destructure.name_count; i++) {
                        emit_bytes(OP_GET_LOCAL, (uint8_t)src_slot, line);
                        emit_constant(value_int((int64_t)i), line);
                        emit_byte(OP_INDEX, line);
                        add_local(s->as.destructure.names[i]);
                    }
                    if (s->as.destructure.rest_name) {
                        /* ...rest: slice array from name_count to end */
                        emit_bytes(OP_GET_LOCAL, (uint8_t)src_slot, line);
                        int64_t start = (int64_t)s->as.destructure.name_count;
                        emit_constant(value_int(start), line);
                        /* Use native "len" to get end index */
                        emit_bytes(OP_GET_LOCAL, (uint8_t)src_slot, line);
                        size_t len_idx = chunk_add_constant(current_chunk(), value_string("len"));
                        emit_bytes(OP_INVOKE, (uint8_t)len_idx, line);
                        emit_byte(0, line); /* 0 args to .len() */
                        emit_byte(OP_BUILD_RANGE, line);
                        emit_byte(OP_INDEX, line);
                        add_local(s->as.destructure.rest_name);
                    }
                } else {
                    for (size_t i = 0; i < s->as.destructure.name_count; i++) {
                        emit_bytes(OP_GET_LOCAL, (uint8_t)src_slot, line);
                        size_t fidx = chunk_add_constant(current_chunk(), value_string(s->as.destructure.names[i]));
                        emit_bytes(OP_GET_FIELD, (uint8_t)fidx, line);
                        add_local(s->as.destructure.names[i]);
                    }
                }
                /* Hidden local cleaned up when enclosing scope ends */
            } else {
                /* Global path: OP_DUP works because DEFINE_GLOBAL pops each value */
                if (s->as.destructure.kind == DESTRUCT_ARRAY) {
                    for (size_t i = 0; i < s->as.destructure.name_count; i++) {
                        emit_byte(OP_DUP, line);
                        emit_constant(value_int((int64_t)i), line);
                        emit_byte(OP_INDEX, line);
                        size_t idx = chunk_add_constant(current_chunk(), value_string(s->as.destructure.names[i]));
                        emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, idx, line);
                    }
                    if (s->as.destructure.rest_name) {
                        /* Stack: [source]
                         * Need: source[name_count..source.len()] as rest */
                        int64_t start = (int64_t)s->as.destructure.name_count;
                        emit_byte(OP_DUP, line); /* [source, source] */
                        emit_byte(OP_DUP, line); /* [source, source, source] */
                        size_t len_idx2 = chunk_add_constant(current_chunk(), value_string("len"));
                        emit_bytes(OP_INVOKE, (uint8_t)len_idx2, line);
                        emit_byte(0, line); /* [source, source, len] — invoke consumes TOS source, pushes len */
                        emit_constant(value_int(start), line); /* [source, source, len, start] */
                        emit_byte(OP_SWAP, line);              /* [source, source, start, len] */
                        emit_byte(OP_BUILD_RANGE, line);       /* [source, source, Range(start..len)] */
                        emit_byte(OP_INDEX, line);             /* [source, rest_slice] */
                        size_t rest_idx =
                            chunk_add_constant(current_chunk(), value_string(s->as.destructure.rest_name));
                        emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, rest_idx, line);
                    }
                } else {
                    for (size_t i = 0; i < s->as.destructure.name_count; i++) {
                        emit_byte(OP_DUP, line);
                        size_t fidx = chunk_add_constant(current_chunk(), value_string(s->as.destructure.names[i]));
                        emit_bytes(OP_GET_FIELD, (uint8_t)fidx, line);
                        size_t nidx = chunk_add_constant(current_chunk(), value_string(s->as.destructure.names[i]));
                        emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, nidx, line);
                    }
                }
                emit_byte(OP_POP, line); /* pop the original value */
            }
            break;
        }

        case STMT_DEFER: {
            /* Emit OP_DEFER_PUSH with scope_depth + offset past the defer body.
             * Format: OP_DEFER_PUSH, scope_depth(1), offset(2).
             * The scope_depth byte lets OP_DEFER_RUN run only defers
             * registered at a given scope level. */
            emit_byte(OP_DEFER_PUSH, line);
            emit_byte((uint8_t)current->scope_depth, line);
            /* Emit 2-byte jump offset placeholder */
            emit_byte(0xff, line);
            emit_byte(0xff, line);
            size_t defer_jump = current_chunk()->code_len - 2;
            for (size_t i = 0; i < s->as.defer.body_count; i++) compile_stmt(s->as.defer.body[i]);
            emit_byte(OP_UNIT, line);   /* implicit return value for defer block */
            emit_byte(OP_RETURN, line); /* return from defer block */
            patch_jump(defer_jump);
            break;
        }

        case STMT_IMPORT: {
            size_t path_idx = chunk_add_constant(current_chunk(), value_string(s->as.import.module_path));
            emit_bytes(OP_IMPORT, (uint8_t)path_idx, line);
            if (s->as.import.alias) {
                if (current->scope_depth > 0) {
                    add_local(s->as.import.alias);
                } else {
                    size_t name_idx = chunk_add_constant(current_chunk(), value_string(s->as.import.alias));
                    emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, name_idx, line);
                }
            } else if (s->as.import.selective_names && s->as.import.selective_count > 0) {
                /* Selective import: import { x, y } from "path" */
                for (size_t i = 0; i < s->as.import.selective_count; i++) {
                    emit_byte(OP_DUP, line); /* duplicate module map */
                    size_t field_idx =
                        chunk_add_constant(current_chunk(), value_string(s->as.import.selective_names[i]));
                    emit_bytes(OP_GET_FIELD, (uint8_t)field_idx, line);
                    /* Check that the export exists (not nil) */
                    emit_byte(OP_DUP, line);
                    size_t import_ok = emit_jump(OP_JUMP_IF_NOT_NIL, line);
                    /* nil → error: module does not export this name */
                    emit_byte(OP_POP, line); /* pop nil dup */
                    emit_byte(OP_POP, line); /* pop nil val */
                    emit_byte(OP_POP, line); /* pop module map */
                    {
                        char err_msg[512];
                        snprintf(err_msg, sizeof(err_msg), "module '%s' does not export '%s'", s->as.import.module_path,
                                 s->as.import.selective_names[i]);
                        size_t err_idx = chunk_add_constant(current_chunk(), value_string(err_msg));
                        emit_bytes(OP_CONSTANT, (uint8_t)err_idx, line);
                        emit_byte(OP_THROW, line);
                    }
                    patch_jump(import_ok);
                    emit_byte(OP_POP, line); /* pop non-nil dup */
                    if (current->scope_depth > 0) {
                        add_local(s->as.import.selective_names[i]);
                    } else {
                        size_t name_idx =
                            chunk_add_constant(current_chunk(), value_string(s->as.import.selective_names[i]));
                        emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, name_idx, line);
                    }
                }
                emit_byte(OP_POP, line); /* pop original module map */
            } else {
                emit_byte(OP_POP, line);
            }
            break;
        }

        default: break;
    }
}

/* ── Constant-evaluate a default parameter expression ── */

static LatValue const_eval_expr(Expr *e) {
    if (!e) return value_nil();
    LatValue result;
    if (try_const_fold(e, &result)) return result;
    return value_nil();
}

/* ── Compile function body (for ITEM_FUNCTION and closures) ── */

static void compile_function_body(FunctionType type, const char *name, Param *params, size_t param_count, Stmt **body,
                                  size_t body_count, ContractClause *contracts, size_t contract_count,
                                  TypeExpr *return_type, int line) {
    Compiler func_comp;
    compiler_init(&func_comp, current, type);
    func_comp.func_name = name ? strdup(name) : NULL;
    func_comp.chunk->name = name ? strdup(name) : NULL;

    /* For impl methods, self occupies slot 0 (the reserved slot).
     * Rename it from "" to "self" and skip it in the param loop. */
    size_t first_param = 0;
    if (param_count > 0 && strcmp(params[0].name, "self") == 0) {
        free(func_comp.locals[0].name);
        func_comp.locals[0].name = strdup("self");
        first_param = 1;
    }
    func_comp.arity = (int)(param_count - first_param);
    func_comp.contracts = contracts;
    func_comp.contract_count = contract_count;
    func_comp.return_type_name = (return_type && return_type->name) ? return_type->name : NULL;

    /* Add remaining params as locals */
    for (size_t i = first_param; i < param_count; i++) add_local(params[i].name);

    /* Emit runtime parameter type checks */
    for (size_t i = first_param; i < param_count; i++) {
        if (params[i].is_variadic) break;
        if (!params[i].ty.name || strcmp(params[i].ty.name, "Any") == 0 || strcmp(params[i].ty.name, "any") == 0)
            continue;
        /* Resolve slot: slot 0 is reserved, params start at slot 1 (or first_param-adjusted) */
        int slot = resolve_local(current, params[i].name);
        if (slot < 0) continue;
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "function '%s' parameter '%s' expects type %s, got %%s",
                 name ? name : "<anonymous>", params[i].name, params[i].ty.name);
        size_t type_idx = chunk_add_constant(current_chunk(), value_string(params[i].ty.name));
        size_t err_idx = chunk_add_constant(current_chunk(), value_string(err_msg));
        emit_byte(OP_CHECK_TYPE, line);
        emit_byte((uint8_t)slot, line);
        emit_byte((uint8_t)type_idx, line);
        emit_byte((uint8_t)err_idx, line);
    }

    /* Compile require contracts (preconditions) */
    for (size_t i = 0; i < contract_count; i++) {
        if (contracts[i].is_ensure) continue;
        compile_expr(contracts[i].condition, line);
        size_t ok_jump = emit_jump(OP_JUMP_IF_TRUE, line);
        emit_byte(OP_POP, line);
        /* Format message to match tree-walker: "require failed in '<name>': <msg>" */
        const char *user_msg = contracts[i].message ? contracts[i].message : "condition not met";
        char full_msg[512];
        snprintf(full_msg, sizeof(full_msg), "require failed in '%s': %s", name ? name : "<anonymous>", user_msg);
        emit_constant(value_string(full_msg), line);
        emit_byte(OP_THROW, line);
        patch_jump(ok_jump);
        emit_byte(OP_POP, line); /* pop the true */
    }

    /* Compile body statements.
     * If the last statement is a bare expression, use its value as the
     * implicit return value instead of Unit. */
    if (body_count > 0 && body[body_count - 1]->tag == STMT_EXPR) {
        for (size_t i = 0; i + 1 < body_count; i++) compile_stmt_reset(body[i]);
        compile_expr(body[body_count - 1]->as.expr, line);
        emit_byte(OP_RESET_EPHEMERAL, line);
    } else {
        for (size_t i = 0; i < body_count; i++) compile_stmt_reset(body[i]);
        emit_byte(OP_UNIT, line);
    }
    emit_return_type_check(line);
    emit_ensure_checks(line);
    emit_byte(OP_DEFER_RUN, line);
    emit_byte(0, line); /* scope_depth 0 = run all defers */
    emit_byte(OP_RETURN, line);

    Chunk *fn_chunk = func_comp.chunk;
    size_t upvalue_count = func_comp.upvalue_count;
    CompilerUpvalue *upvalues = NULL;
    if (upvalue_count > 0) {
        upvalues = malloc(upvalue_count * sizeof(CompilerUpvalue));
        if (!upvalues) return;
        memcpy(upvalues, func_comp.upvalues, upvalue_count * sizeof(CompilerUpvalue));
    }

    /* Store default parameter values and variadic flag on the chunk */
    int default_count = 0;
    bool has_variadic = false;
    for (size_t i = first_param; i < param_count; i++) {
        if (params[i].default_value) default_count++;
        if (params[i].is_variadic) has_variadic = true;
    }
    fn_chunk->default_count = default_count;
    fn_chunk->fn_has_variadic = has_variadic;
    if (default_count > 0) {
        fn_chunk->default_values = malloc(default_count * sizeof(LatValue));
        if (!fn_chunk->default_values) return;
        int di = 0;
        for (size_t i = first_param; i < param_count; i++) {
            if (params[i].default_value) { fn_chunk->default_values[di++] = const_eval_expr(params[i].default_value); }
        }
    }

    /* Store per-parameter phase constraints (for phase dispatch/checking) */
    {
        bool has_phase_constraints = false;
        for (size_t i = first_param; i < param_count; i++) {
            if (params[i].is_variadic) break;
            if (params[i].ty.phase != PHASE_UNSPECIFIED) {
                has_phase_constraints = true;
                break;
            }
        }
        if (has_phase_constraints) {
            int pc = (int)(param_count - first_param);
            fn_chunk->param_phases = calloc(pc, sizeof(uint8_t));
            if (!fn_chunk->param_phases) return;
            fn_chunk->param_phase_count = pc;
            for (size_t i = first_param; i < param_count; i++) {
                if (params[i].is_variadic) break;
                fn_chunk->param_phases[i - first_param] = (uint8_t)params[i].ty.phase;
            }
        }
    }

    compiler_cleanup(&func_comp);
    current = func_comp.enclosing;

    /* Store the function's chunk as a constant. */
    LatValue fn_val;
    memset(&fn_val, 0, sizeof(fn_val));
    fn_val.type = VAL_CLOSURE;
    fn_val.phase = VTAG_UNPHASED;
    fn_val.region_id = (size_t)-1;
    /* Set param_names for display (e.g. value_repr shows <closure|name|>) */
    if (param_count > 0) {
        fn_val.as.closure.param_names = malloc(param_count * sizeof(char *));
        if (!fn_val.as.closure.param_names) return;
        for (size_t i = 0; i < param_count; i++) fn_val.as.closure.param_names[i] = strdup(params[i].name);
    } else {
        fn_val.as.closure.param_names = NULL;
    }
    fn_val.as.closure.param_count = param_count;
    fn_val.as.closure.body = NULL;
    fn_val.as.closure.captured_env = NULL;
    fn_val.as.closure.default_values = NULL;
    fn_val.as.closure.has_variadic = false;
    fn_val.as.closure.native_fn = fn_chunk;
    size_t fn_idx = chunk_add_constant(current_chunk(), fn_val);

    if (fn_idx <= 255) {
        emit_byte(OP_CLOSURE, line);
        emit_byte((uint8_t)fn_idx, line);
    } else {
        emit_byte(OP_CLOSURE_16, line);
        emit_byte((uint8_t)((fn_idx >> 8) & 0xff), line);
        emit_byte((uint8_t)(fn_idx & 0xff), line);
    }
    emit_byte((uint8_t)upvalue_count, line);
    for (size_t i = 0; i < upvalue_count; i++) {
        emit_byte(upvalues[i].is_local ? 1 : 0, line);
        emit_byte(upvalues[i].index, line);
    }
    free(upvalues);
}

/* ── Public API ── */

Chunk *stack_compile(const Program *prog, char **error) {
    Compiler top;
    compiler_init(&top, NULL, FUNC_SCRIPT);
    *error = NULL;

    /* Compile top-level items */
    int last_line = 0;
    for (size_t i = 0; i < prog->item_count; i++) {
        switch (prog->items[i].tag) {
            case ITEM_STMT: {
                Stmt *s = prog->items[i].as.stmt;
                if (s && s->line > 0) last_line = s->line;
                compile_stmt_reset(s);
                break;
            }

            case ITEM_FUNCTION: {
                FnDecl *fn = &prog->items[i].as.fn_decl;
                int fn_line = fn->line;
                compile_function_body(FUNC_FUNCTION, fn->name, fn->params, fn->param_count, fn->body, fn->body_count,
                                      fn->contracts, fn->contract_count, fn->return_type, fn_line);
                /* Define the function as a global */
                size_t name_idx = chunk_add_constant(current_chunk(), value_string(fn->name));
                emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, name_idx, fn_line);
                last_line = fn_line;
                break;
            }

            case ITEM_STRUCT: {
                /* Store struct metadata as a constant for OP_BUILD_STRUCT to use.
                 * We emit field names as an array constant. */
                StructDecl *sd = &prog->items[i].as.struct_decl;
                LatValue *field_names = malloc(sd->field_count * sizeof(LatValue));
                if (!field_names) break;
                for (size_t j = 0; j < sd->field_count; j++) field_names[j] = value_string(sd->fields[j].name);
                LatValue arr = value_array(field_names, sd->field_count);
                free(field_names);
                /* Store as global "__struct_<name>" */
                char meta_name[256];
                snprintf(meta_name, sizeof(meta_name), "__struct_%s", sd->name);
                size_t arr_idx = chunk_add_constant(current_chunk(), arr);
                emit_bytes(OP_CONSTANT, (uint8_t)arr_idx, 0);
                size_t name_idx = chunk_add_constant(current_chunk(), value_string(meta_name));
                emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, name_idx, 0);
                /* Alloy: emit per-field phase metadata if any field has a phase annotation */
                {
                    bool has_phase = false;
                    for (size_t j = 0; j < sd->field_count; j++) {
                        if (sd->fields[j].ty.phase != PHASE_UNSPECIFIED) {
                            has_phase = true;
                            break;
                        }
                    }
                    if (has_phase) {
                        LatValue *phases = malloc(sd->field_count * sizeof(LatValue));
                        if (!phases) break;
                        for (size_t j = 0; j < sd->field_count; j++)
                            phases[j] = value_int((int64_t)sd->fields[j].ty.phase);
                        LatValue phase_arr = value_array(phases, sd->field_count);
                        free(phases);
                        char phase_meta[256];
                        snprintf(phase_meta, sizeof(phase_meta), "__struct_phases_%s", sd->name);
                        size_t pi = chunk_add_constant(current_chunk(), phase_arr);
                        emit_bytes(OP_CONSTANT, (uint8_t)pi, 0);
                        size_t pn = chunk_add_constant(current_chunk(), value_string(phase_meta));
                        emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, pn, 0);
                    }
                }
                break;
            }

            case ITEM_ENUM: {
                /* Store enum metadata */
                EnumDecl *ed = &prog->items[i].as.enum_decl;
                register_enum(ed->name);
                char meta_name[256];
                snprintf(meta_name, sizeof(meta_name), "__enum_%s", ed->name);
                emit_byte(OP_TRUE, 0);
                size_t name_idx = chunk_add_constant(current_chunk(), value_string(meta_name));
                emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, name_idx, 0);
                break;
            }

            case ITEM_IMPL: {
                /* Compile impl methods and register them */
                ImplBlock *ib = &prog->items[i].as.impl_block;
                for (size_t j = 0; j < ib->method_count; j++) {
                    FnDecl *method = &ib->methods[j];
                    int m_line = method->line;
                    compile_function_body(FUNC_FUNCTION, method->name, method->params, method->param_count,
                                          method->body, method->body_count, method->contracts, method->contract_count,
                                          method->return_type, m_line);
                    /* Register as "TypeName::method" global */
                    char key[256];
                    snprintf(key, sizeof(key), "%s::%s", ib->type_name, method->name);
                    size_t key_idx = chunk_add_constant(current_chunk(), value_string(key));
                    emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, key_idx, m_line);
                    last_line = m_line;
                }
                break;
            }

            case ITEM_TRAIT:
            case ITEM_TEST:
                /* Traits are metadata only, tests are skipped in normal compilation */
                break;
        }

        if (compile_error) {
            *error = compile_error;
            compile_error = NULL;
            chunk_free(top.chunk);
            compiler_cleanup(&top);
            current = NULL;
            free_known_enums();
            return NULL;
        }
    }

    /* If a main() function was defined, auto-call it */
    bool has_main = false;
    for (size_t i = 0; i < prog->item_count; i++) {
        if (prog->items[i].tag == ITEM_FUNCTION && strcmp(prog->items[i].as.fn_decl.name, "main") == 0) {
            has_main = true;
            break;
        }
    }
    if (has_main) {
        size_t main_idx = chunk_add_constant(current_chunk(), value_string("main"));
        emit_constant_idx(OP_GET_GLOBAL, OP_GET_GLOBAL_16, main_idx, last_line);
        emit_bytes(OP_CALL, 0, last_line);
        emit_byte(OP_POP, last_line);
    }

    emit_byte(OP_UNIT, last_line);
    emit_byte(OP_RETURN, last_line);

    Chunk *result = top.chunk;
    compiler_cleanup(&top);
    current = NULL;
    free_known_enums();
    return result;
}

Chunk *stack_compile_module(const Program *prog, char **error) {
    Compiler top;
    compiler_init(&top, NULL, FUNC_SCRIPT);
    *error = NULL;

    for (size_t i = 0; i < prog->item_count; i++) {
        switch (prog->items[i].tag) {
            case ITEM_STMT: compile_stmt_reset(prog->items[i].as.stmt); break;
            case ITEM_FUNCTION: {
                FnDecl *fn = &prog->items[i].as.fn_decl;
                int fn_line = fn->line;
                compile_function_body(FUNC_FUNCTION, fn->name, fn->params, fn->param_count, fn->body, fn->body_count,
                                      fn->contracts, fn->contract_count, fn->return_type, fn_line);
                size_t name_idx = chunk_add_constant(current_chunk(), value_string(fn->name));
                emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, name_idx, fn_line);
                break;
            }
            case ITEM_STRUCT: {
                StructDecl *sd = &prog->items[i].as.struct_decl;
                LatValue *field_names = malloc(sd->field_count * sizeof(LatValue));
                if (!field_names) break;
                for (size_t j = 0; j < sd->field_count; j++) field_names[j] = value_string(sd->fields[j].name);
                LatValue arr = value_array(field_names, sd->field_count);
                free(field_names);
                char meta_name[256];
                snprintf(meta_name, sizeof(meta_name), "__struct_%s", sd->name);
                size_t arr_idx = chunk_add_constant(current_chunk(), arr);
                emit_bytes(OP_CONSTANT, (uint8_t)arr_idx, 0);
                size_t name_idx = chunk_add_constant(current_chunk(), value_string(meta_name));
                emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, name_idx, 0);
                /* Alloy: emit per-field phase metadata if any field has a phase annotation */
                {
                    bool has_phase = false;
                    for (size_t j = 0; j < sd->field_count; j++) {
                        if (sd->fields[j].ty.phase != PHASE_UNSPECIFIED) {
                            has_phase = true;
                            break;
                        }
                    }
                    if (has_phase) {
                        LatValue *phases = malloc(sd->field_count * sizeof(LatValue));
                        if (!phases) break;
                        for (size_t j = 0; j < sd->field_count; j++)
                            phases[j] = value_int((int64_t)sd->fields[j].ty.phase);
                        LatValue phase_arr = value_array(phases, sd->field_count);
                        free(phases);
                        char phase_meta[256];
                        snprintf(phase_meta, sizeof(phase_meta), "__struct_phases_%s", sd->name);
                        size_t pi = chunk_add_constant(current_chunk(), phase_arr);
                        emit_bytes(OP_CONSTANT, (uint8_t)pi, 0);
                        size_t pn = chunk_add_constant(current_chunk(), value_string(phase_meta));
                        emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, pn, 0);
                    }
                }
                break;
            }
            case ITEM_ENUM: {
                EnumDecl *ed = &prog->items[i].as.enum_decl;
                register_enum(ed->name);
                char meta_name[256];
                snprintf(meta_name, sizeof(meta_name), "__enum_%s", ed->name);
                emit_byte(OP_TRUE, 0);
                size_t name_idx = chunk_add_constant(current_chunk(), value_string(meta_name));
                emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, name_idx, 0);
                break;
            }
            case ITEM_IMPL: {
                ImplBlock *ib = &prog->items[i].as.impl_block;
                for (size_t j = 0; j < ib->method_count; j++) {
                    FnDecl *method = &ib->methods[j];
                    int m_line = method->line;
                    compile_function_body(FUNC_FUNCTION, method->name, method->params, method->param_count,
                                          method->body, method->body_count, method->contracts, method->contract_count,
                                          method->return_type, m_line);
                    char key[256];
                    snprintf(key, sizeof(key), "%s::%s", ib->type_name, method->name);
                    size_t key_idx = chunk_add_constant(current_chunk(), value_string(key));
                    emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, key_idx, m_line);
                }
                break;
            }
            case ITEM_TRAIT:
            case ITEM_TEST: break;
        }

        if (compile_error) {
            *error = compile_error;
            compile_error = NULL;
            chunk_free(top.chunk);
            compiler_cleanup(&top);
            current = NULL;
            free_known_enums();
            return NULL;
        }
    }

    /* No auto-call of main() for modules */
    emit_byte(OP_UNIT, 0);
    emit_byte(OP_RETURN, 0);

    Chunk *result = top.chunk;

    /* Copy export metadata from Program to Chunk */
    if (prog->has_exports) {
        result->has_exports = true;
        result->export_count = prog->export_count;
        result->export_names = malloc(prog->export_count * sizeof(char *));
        if (!result->export_names) {
            result->has_exports = false;
            result->export_count = 0;
        } else
            for (size_t i = 0; i < prog->export_count; i++) result->export_names[i] = strdup(prog->export_names[i]);
    }

    compiler_cleanup(&top);
    current = NULL;
    free_known_enums();
    return result;
}

Chunk *stack_compile_repl(const Program *prog, char **error) {
    Compiler top;
    compiler_init(&top, NULL, FUNC_SCRIPT);
    *error = NULL;

    for (size_t i = 0; i < prog->item_count; i++) {
        switch (prog->items[i].tag) {
            case ITEM_STMT: {
                bool is_last = (i == prog->item_count - 1);
                Stmt *s = prog->items[i].as.stmt;
                /* Last item is a bare expression: keep value on stack */
                if (is_last && s->tag == STMT_EXPR) {
                    compile_expr(s->as.expr, 0);
                    /* Skip OP_POP — value stays as return */
                } else {
                    compile_stmt_reset(s);
                }
                break;
            }
            case ITEM_FUNCTION: {
                FnDecl *fn = &prog->items[i].as.fn_decl;
                int fn_line = fn->line;
                compile_function_body(FUNC_FUNCTION, fn->name, fn->params, fn->param_count, fn->body, fn->body_count,
                                      fn->contracts, fn->contract_count, fn->return_type, fn_line);
                size_t name_idx = chunk_add_constant(current_chunk(), value_string(fn->name));
                emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, name_idx, fn_line);
                break;
            }
            case ITEM_STRUCT: {
                StructDecl *sd = &prog->items[i].as.struct_decl;
                LatValue *field_names = malloc(sd->field_count * sizeof(LatValue));
                if (!field_names) break;
                for (size_t j = 0; j < sd->field_count; j++) field_names[j] = value_string(sd->fields[j].name);
                LatValue arr = value_array(field_names, sd->field_count);
                free(field_names);
                char meta_name[256];
                snprintf(meta_name, sizeof(meta_name), "__struct_%s", sd->name);
                size_t arr_idx = chunk_add_constant(current_chunk(), arr);
                emit_bytes(OP_CONSTANT, (uint8_t)arr_idx, 0);
                size_t name_idx = chunk_add_constant(current_chunk(), value_string(meta_name));
                emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, name_idx, 0);
                {
                    bool has_phase = false;
                    for (size_t j = 0; j < sd->field_count; j++) {
                        if (sd->fields[j].ty.phase != PHASE_UNSPECIFIED) {
                            has_phase = true;
                            break;
                        }
                    }
                    if (has_phase) {
                        LatValue *phases = malloc(sd->field_count * sizeof(LatValue));
                        if (!phases) break;
                        for (size_t j = 0; j < sd->field_count; j++)
                            phases[j] = value_int((int64_t)sd->fields[j].ty.phase);
                        LatValue phase_arr = value_array(phases, sd->field_count);
                        free(phases);
                        char phase_meta[256];
                        snprintf(phase_meta, sizeof(phase_meta), "__struct_phases_%s", sd->name);
                        size_t pi = chunk_add_constant(current_chunk(), phase_arr);
                        emit_bytes(OP_CONSTANT, (uint8_t)pi, 0);
                        size_t pn = chunk_add_constant(current_chunk(), value_string(phase_meta));
                        emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, pn, 0);
                    }
                }
                break;
            }
            case ITEM_ENUM: {
                EnumDecl *ed = &prog->items[i].as.enum_decl;
                register_enum(ed->name);
                char meta_name[256];
                snprintf(meta_name, sizeof(meta_name), "__enum_%s", ed->name);
                emit_byte(OP_TRUE, 0);
                size_t name_idx = chunk_add_constant(current_chunk(), value_string(meta_name));
                emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, name_idx, 0);
                break;
            }
            case ITEM_IMPL: {
                ImplBlock *ib = &prog->items[i].as.impl_block;
                for (size_t j = 0; j < ib->method_count; j++) {
                    FnDecl *method = &ib->methods[j];
                    int m_line = method->line;
                    compile_function_body(FUNC_FUNCTION, method->name, method->params, method->param_count,
                                          method->body, method->body_count, method->contracts, method->contract_count,
                                          method->return_type, m_line);
                    char key[256];
                    snprintf(key, sizeof(key), "%s::%s", ib->type_name, method->name);
                    size_t key_idx = chunk_add_constant(current_chunk(), value_string(key));
                    emit_constant_idx(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_16, key_idx, m_line);
                }
                break;
            }
            case ITEM_TRAIT:
            case ITEM_TEST: break;
        }

        if (compile_error) {
            *error = compile_error;
            compile_error = NULL;
            chunk_free(top.chunk);
            compiler_cleanup(&top);
            current = NULL;
            /* Don't free known enums — they persist across REPL iterations */
            return NULL;
        }
    }

    /* Check if last item was a bare expression (value already on stack) */
    bool last_is_expr = (prog->item_count > 0 && prog->items[prog->item_count - 1].tag == ITEM_STMT &&
                         prog->items[prog->item_count - 1].as.stmt->tag == STMT_EXPR);

    if (last_is_expr) {
        emit_byte(OP_RETURN, 0);
    } else {
        emit_byte(OP_UNIT, 0);
        emit_byte(OP_RETURN, 0);
    }

    Chunk *result = top.chunk;
    compiler_cleanup(&top);
    current = NULL;
    /* Don't free known enums — they persist across REPL iterations */
    return result;
}

void stack_compiler_free_known_enums(void) { free_known_enums(); }
