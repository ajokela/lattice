#include "match_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ── Internal state ── */

typedef struct {
    const Program *prog;
    int warning_count;
} MatchChecker;

/* ── Enum lookup ── */

static const EnumDecl *find_enum_decl(const MatchChecker *mc, const char *name) {
    for (size_t i = 0; i < mc->prog->item_count; i++) {
        if (mc->prog->items[i].tag == ITEM_ENUM && strcmp(mc->prog->items[i].as.enum_decl.name, name) == 0) {
            return &mc->prog->items[i].as.enum_decl;
        }
    }
    return NULL;
}

/* ── Pattern analysis helpers ── */

/* Check if any arm has a wildcard or binding pattern (catch-all) */
static bool has_catch_all(const MatchArm *arms, size_t arm_count) {
    for (size_t i = 0; i < arm_count; i++) {
        const Pattern *p = arms[i].pattern;
        /* A guarded wildcard/binding doesn't count as a true catch-all
         * because the guard might fail */
        if (arms[i].guard != NULL) continue;
        /* Phase-qualified wildcards/bindings don't cover all phases */
        if (p->phase_qualifier != PHASE_UNSPECIFIED) continue;
        if (p->tag == PAT_WILDCARD || p->tag == PAT_BINDING) return true;
    }
    return false;
}

/* Check if a pattern is a literal enum variant expression */
static bool is_enum_variant_literal(const Pattern *p, const char **out_enum, const char **out_variant) {
    if (p->tag != PAT_LITERAL) return false;
    const Expr *lit = p->as.literal;
    if (lit->tag != EXPR_ENUM_VARIANT) return false;
    if (out_enum) *out_enum = lit->as.enum_variant.enum_name;
    if (out_variant) *out_variant = lit->as.enum_variant.variant_name;
    return true;
}

/* Check if a pattern is a bool literal with the given value */
static bool is_bool_literal(const Pattern *p, bool val) {
    if (p->tag != PAT_LITERAL) return false;
    const Expr *lit = p->as.literal;
    return lit->tag == EXPR_BOOL_LIT && lit->as.bool_val == val;
}

/* ── Exhaustiveness check for a single match expression ── */

static void check_match(MatchChecker *mc, const Expr *match_expr) {
    const MatchArm *arms = match_expr->as.match_expr.arms;
    size_t arm_count = match_expr->as.match_expr.arm_count;
    int line = match_expr->line;

    if (arm_count == 0) {
        fprintf(stderr, "warning: match expression on line %d has no arms\n", line);
        mc->warning_count++;
        return;
    }

    /* If there's a catch-all (unguarded wildcard or binding), it's exhaustive */
    if (has_catch_all(arms, arm_count)) return;

    /* Determine the type of the match from the patterns:
     * 1. If any arm matches an enum variant, we can infer the enum type
     * 2. If arms match true/false, it's a boolean match
     * 3. Otherwise (int, string, float), require a catch-all */

    /* Check for enum variant patterns */
    const char *enum_name = NULL;
    for (size_t i = 0; i < arm_count; i++) {
        const char *ename = NULL;
        if (is_enum_variant_literal(arms[i].pattern, &ename, NULL)) {
            enum_name = ename;
            break;
        }
    }

    if (enum_name) {
        /* Enum match: check all variants are covered */
        const EnumDecl *ed = find_enum_decl(mc, enum_name);
        if (!ed) {
            /* Unknown enum — can't check, skip */
            return;
        }

        /* Build a coverage array for each variant */
        bool *covered = calloc(ed->variant_count, sizeof(bool));
        if (!covered) return;

        for (size_t i = 0; i < arm_count; i++) {
            const char *vname = NULL;
            const char *ename = NULL;
            if (is_enum_variant_literal(arms[i].pattern, &ename, &vname)) {
                if (strcmp(ename, enum_name) != 0) continue;
                /* A guarded enum pattern still counts for variant coverage
                 * because the user explicitly listed the variant, even if
                 * the guard might fail.  A truly robust check would be more
                 * conservative, but for pragmatic warnings this is useful. */
                for (size_t j = 0; j < ed->variant_count; j++) {
                    if (strcmp(ed->variants[j].name, vname) == 0) {
                        covered[j] = true;
                        break;
                    }
                }
            }
            /* Wildcards/bindings with guards already skipped by has_catch_all */
        }

        /* Report missing variants */
        size_t missing_count = 0;
        for (size_t j = 0; j < ed->variant_count; j++) {
            if (!covered[j]) missing_count++;
        }

        if (missing_count > 0) {
            fprintf(stderr, "warning: non-exhaustive match on line %d: ", line);
            fprintf(stderr, "missing %s variant", enum_name);
            if (missing_count == 1) {
                fprintf(stderr, " ");
            } else {
                fprintf(stderr, "s ");
            }
            bool first = true;
            for (size_t j = 0; j < ed->variant_count; j++) {
                if (!covered[j]) {
                    if (!first) fprintf(stderr, ", ");
                    fprintf(stderr, "`%s::%s`", enum_name, ed->variants[j].name);
                    first = false;
                }
            }
            fprintf(stderr, "\n");
            mc->warning_count++;
        }

        free(covered);
        return;
    }

    /* Check for boolean match */
    bool has_true = false, has_false = false;
    bool all_bool = true;
    for (size_t i = 0; i < arm_count; i++) {
        const Pattern *p = arms[i].pattern;
        if (is_bool_literal(p, true)) {
            has_true = true;
        } else if (is_bool_literal(p, false)) {
            has_false = true;
        } else if (p->tag == PAT_LITERAL && p->as.literal->tag == EXPR_BOOL_LIT) {
            /* Already covered by above */
        } else {
            all_bool = false;
        }
    }

    if (all_bool && (has_true || has_false)) {
        /* This is a boolean match */
        if (!has_true || !has_false) {
            fprintf(stderr, "warning: non-exhaustive match on line %d: ", line);
            if (!has_true) fprintf(stderr, "missing `true` case");
            else fprintf(stderr, "missing `false` case");
            fprintf(stderr, "\n");
            mc->warning_count++;
        }
        return;
    }

    /* For int, string, float, or mixed patterns without a catch-all:
     * warn that a wildcard/binding is needed */
    fprintf(stderr,
            "warning: non-exhaustive match on line %d: "
            "consider adding a wildcard `_` arm\n",
            line);
    mc->warning_count++;
}

/* ── AST walker ── */

static void walk_expr(MatchChecker *mc, const Expr *e);
static void walk_stmt(MatchChecker *mc, const Stmt *s);
static void walk_stmts(MatchChecker *mc, Stmt **stmts, size_t count);

static void walk_expr(MatchChecker *mc, const Expr *e) {
    if (!e) return;

    switch (e->tag) {
        case EXPR_MATCH:
            /* Check this match expression */
            check_match(mc, e);
            /* Also walk the scrutinee and arm bodies */
            walk_expr(mc, e->as.match_expr.scrutinee);
            for (size_t i = 0; i < e->as.match_expr.arm_count; i++) {
                const MatchArm *arm = &e->as.match_expr.arms[i];
                if (arm->guard) walk_expr(mc, arm->guard);
                walk_stmts(mc, arm->body, arm->body_count);
                /* Walk pattern literals for nested match expressions */
                if (arm->pattern->tag == PAT_LITERAL) walk_expr(mc, arm->pattern->as.literal);
                else if (arm->pattern->tag == PAT_RANGE) {
                    walk_expr(mc, arm->pattern->as.range.start);
                    walk_expr(mc, arm->pattern->as.range.end);
                }
            }
            break;

        case EXPR_BINOP:
            walk_expr(mc, e->as.binop.left);
            walk_expr(mc, e->as.binop.right);
            break;
        case EXPR_UNARYOP: walk_expr(mc, e->as.unaryop.operand); break;
        case EXPR_CALL:
            walk_expr(mc, e->as.call.func);
            for (size_t i = 0; i < e->as.call.arg_count; i++) walk_expr(mc, e->as.call.args[i]);
            break;
        case EXPR_METHOD_CALL:
            walk_expr(mc, e->as.method_call.object);
            for (size_t i = 0; i < e->as.method_call.arg_count; i++) walk_expr(mc, e->as.method_call.args[i]);
            break;
        case EXPR_FIELD_ACCESS: walk_expr(mc, e->as.field_access.object); break;
        case EXPR_INDEX:
            walk_expr(mc, e->as.index.object);
            walk_expr(mc, e->as.index.index);
            break;
        case EXPR_ARRAY:
            for (size_t i = 0; i < e->as.array.count; i++) walk_expr(mc, e->as.array.elems[i]);
            break;
        case EXPR_STRUCT_LIT:
            for (size_t i = 0; i < e->as.struct_lit.field_count; i++) walk_expr(mc, e->as.struct_lit.fields[i].value);
            break;
        case EXPR_IF:
            walk_expr(mc, e->as.if_expr.cond);
            walk_stmts(mc, e->as.if_expr.then_stmts, e->as.if_expr.then_count);
            walk_stmts(mc, e->as.if_expr.else_stmts, e->as.if_expr.else_count);
            break;
        case EXPR_BLOCK:
        case EXPR_SPAWN:
        case EXPR_SCOPE: walk_stmts(mc, e->as.block.stmts, e->as.block.count); break;
        case EXPR_FORGE: walk_stmts(mc, e->as.block.stmts, e->as.block.count); break;
        case EXPR_CLOSURE: walk_expr(mc, e->as.closure.body); break;
        case EXPR_RANGE:
            walk_expr(mc, e->as.range.start);
            walk_expr(mc, e->as.range.end);
            break;
        case EXPR_PRINT:
            for (size_t i = 0; i < e->as.print.arg_count; i++) walk_expr(mc, e->as.print.args[i]);
            break;
        case EXPR_TRY_CATCH:
            walk_stmts(mc, e->as.try_catch.try_stmts, e->as.try_catch.try_count);
            walk_stmts(mc, e->as.try_catch.catch_stmts, e->as.try_catch.catch_count);
            break;
        case EXPR_INTERP_STRING:
            for (size_t i = 0; i < e->as.interp.count; i++) walk_expr(mc, e->as.interp.exprs[i]);
            break;
        case EXPR_ENUM_VARIANT:
            for (size_t i = 0; i < e->as.enum_variant.arg_count; i++) walk_expr(mc, e->as.enum_variant.args[i]);
            break;
        case EXPR_FREEZE:
            walk_expr(mc, e->as.freeze.expr);
            if (e->as.freeze.contract) walk_expr(mc, e->as.freeze.contract);
            break;
        case EXPR_THAW:
        case EXPR_CLONE: walk_expr(mc, e->as.freeze_expr); break;
        case EXPR_ANNEAL:
            walk_expr(mc, e->as.anneal.expr);
            walk_expr(mc, e->as.anneal.closure);
            break;
        case EXPR_SPREAD: walk_expr(mc, e->as.spread_expr); break;
        case EXPR_TUPLE:
            for (size_t i = 0; i < e->as.tuple.count; i++) walk_expr(mc, e->as.tuple.elems[i]);
            break;
        case EXPR_CRYSTALLIZE:
            walk_expr(mc, e->as.crystallize.expr);
            walk_stmts(mc, e->as.crystallize.body, e->as.crystallize.body_count);
            break;
        case EXPR_BORROW:
            walk_expr(mc, e->as.borrow.expr);
            walk_stmts(mc, e->as.borrow.body, e->as.borrow.body_count);
            break;
        case EXPR_SUBLIMATE: walk_expr(mc, e->as.freeze_expr); break;
        case EXPR_TRY_PROPAGATE: walk_expr(mc, e->as.try_propagate_expr); break;
        case EXPR_SELECT:
            for (size_t i = 0; i < e->as.select_expr.arm_count; i++) {
                SelectArm *sa = &e->as.select_expr.arms[i];
                if (sa->channel_expr) walk_expr(mc, sa->channel_expr);
                if (sa->timeout_expr) walk_expr(mc, sa->timeout_expr);
                walk_stmts(mc, sa->body, sa->body_count);
            }
            break;

        /* Leaf nodes — nothing to walk */
        case EXPR_INT_LIT:
        case EXPR_FLOAT_LIT:
        case EXPR_STRING_LIT:
        case EXPR_BOOL_LIT:
        case EXPR_NIL_LIT:
        case EXPR_IDENT: break;
    }
}

static void walk_stmt(MatchChecker *mc, const Stmt *s) {
    if (!s) return;
    switch (s->tag) {
        case STMT_BINDING: walk_expr(mc, s->as.binding.value); break;
        case STMT_ASSIGN:
            walk_expr(mc, s->as.assign.target);
            walk_expr(mc, s->as.assign.value);
            break;
        case STMT_EXPR: walk_expr(mc, s->as.expr); break;
        case STMT_RETURN: walk_expr(mc, s->as.return_expr); break;
        case STMT_FOR:
            walk_expr(mc, s->as.for_loop.iter);
            walk_stmts(mc, s->as.for_loop.body, s->as.for_loop.body_count);
            break;
        case STMT_WHILE:
            walk_expr(mc, s->as.while_loop.cond);
            walk_stmts(mc, s->as.while_loop.body, s->as.while_loop.body_count);
            break;
        case STMT_LOOP: walk_stmts(mc, s->as.loop.body, s->as.loop.body_count); break;
        case STMT_DESTRUCTURE: walk_expr(mc, s->as.destructure.value); break;
        case STMT_DEFER: walk_stmts(mc, s->as.defer.body, s->as.defer.body_count); break;
        case STMT_BREAK:
        case STMT_CONTINUE:
        case STMT_IMPORT: break;
    }
}

static void walk_stmts(MatchChecker *mc, Stmt **stmts, size_t count) {
    if (!stmts) return;
    for (size_t i = 0; i < count; i++) walk_stmt(mc, stmts[i]);
}

static void walk_fn_decl(MatchChecker *mc, const FnDecl *fn) {
    if (!fn) return;
    walk_stmts(mc, fn->body, fn->body_count);
    /* Walk contracts */
    for (size_t i = 0; i < fn->contract_count; i++) { walk_expr(mc, fn->contracts[i].condition); }
    /* Walk default parameter values */
    for (size_t i = 0; i < fn->param_count; i++) {
        if (fn->params[i].default_value) walk_expr(mc, fn->params[i].default_value);
    }
    /* Walk overloads */
    if (fn->next_overload) walk_fn_decl(mc, fn->next_overload);
}

/* ── Public API ── */

int check_match_exhaustiveness(const Program *prog) {
    if (!prog) return 0;

    MatchChecker mc;
    mc.prog = prog;
    mc.warning_count = 0;

    for (size_t i = 0; i < prog->item_count; i++) {
        const Item *item = &prog->items[i];
        switch (item->tag) {
            case ITEM_FUNCTION: walk_fn_decl(&mc, &item->as.fn_decl); break;
            case ITEM_STMT: walk_stmt(&mc, item->as.stmt); break;
            case ITEM_TEST: walk_stmts(&mc, item->as.test_decl.body, item->as.test_decl.body_count); break;
            case ITEM_IMPL:
                for (size_t j = 0; j < item->as.impl_block.method_count; j++)
                    walk_fn_decl(&mc, &item->as.impl_block.methods[j]);
                break;
            case ITEM_STRUCT:
            case ITEM_ENUM:
            case ITEM_TRAIT:
                /* No expressions to check */
                break;
        }
    }

    return mc.warning_count;
}
