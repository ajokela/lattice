#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

Parser parser_new(LatVec *tokens) {
    Parser p;
    p.tokens = (Token *)tokens->data;
    p.count = tokens->len;
    p.pos = 0;
    return p;
}

/* ── Helpers ── */

static Token *peek(Parser *p) {
    size_t idx = p->pos < p->count ? p->pos : p->count - 1;
    return &p->tokens[idx];
}

static TokenType peek_type(Parser *p) {
    return peek(p)->type;
}

static TokenType peek_ahead_type(Parser *p, size_t offset) {
    size_t idx = p->pos + offset;
    if (idx >= p->count) idx = p->count - 1;
    return p->tokens[idx].type;
}

static Token *advance(Parser *p) {
    size_t idx = p->pos < p->count ? p->pos : p->count - 1;
    if (p->pos < p->count) p->pos++;
    return &p->tokens[idx];
}

static bool at_eof(Parser *p) {
    return peek_type(p) == TOK_EOF;
}

static char *parser_error_fmt(Parser *p, const char *fmt, ...) {
    Token *t = peek(p);
    char *inner = NULL;
    va_list args;
    va_start(args, fmt);
    (void)vasprintf(&inner, fmt, args);
    va_end(args);
    char *err = NULL;
    (void)asprintf(&err, "%zu:%zu: parse error: %s", t->line, t->col, inner);
    free(inner);
    return err;
}

static bool expect(Parser *p, TokenType expected, char **err) {
    if (peek_type(p) == expected) {
        advance(p);
        return true;
    }
    *err = parser_error_fmt(p, "expected '%s', got '%s'",
                            token_type_name(expected),
                            token_type_name(peek_type(p)));
    return false;
}

static char *expect_ident(Parser *p, char **err) {
    if (peek_type(p) == TOK_IDENT) {
        Token *t = advance(p);
        return strdup(t->as.str_val);
    }
    *err = parser_error_fmt(p, "expected identifier, got '%s'",
                            token_type_name(peek_type(p)));
    return NULL;
}

static void eat_semicolon(Parser *p) {
    if (peek_type(p) == TOK_SEMICOLON) advance(p);
}

/* Forward declarations */
static Expr *parse_expr(Parser *p, char **err);
static Stmt *parse_stmt(Parser *p, char **err);

/* ── Block stmts ── */

static Stmt **parse_block_stmts(Parser *p, size_t *count, char **err) {
    size_t cap = 8;
    size_t n = 0;
    Stmt **stmts = malloc(cap * sizeof(Stmt *));

    while (peek_type(p) != TOK_RBRACE && !at_eof(p)) {
        Stmt *s = parse_stmt(p, err);
        if (!s) { free(stmts); return NULL; }
        if (n >= cap) {
            cap *= 2;
            stmts = realloc(stmts, cap * sizeof(Stmt *));
        }
        stmts[n++] = s;
    }
    *count = n;
    return stmts;
}

/* ── Types ── */

static TypeExpr *parse_type_expr(Parser *p, char **err) {
    TypeExpr *te = calloc(1, sizeof(TypeExpr));
    if (peek_type(p) == TOK_TILDE || peek_type(p) == TOK_FLUX) {
        advance(p); te->phase = PHASE_FLUID;
    } else if (peek_type(p) == TOK_STAR || peek_type(p) == TOK_FIX) {
        advance(p); te->phase = PHASE_CRYSTAL;
    } else {
        te->phase = PHASE_UNSPECIFIED;
    }

    if (peek_type(p) == TOK_LBRACKET) {
        advance(p);
        te->kind = TYPE_ARRAY;
        te->inner = parse_type_expr(p, err);
        if (!te->inner) { free(te); return NULL; }
        if (!expect(p, TOK_RBRACKET, err)) {
            type_expr_free(te); free(te); return NULL;
        }
    } else {
        te->kind = TYPE_NAMED;
        te->name = expect_ident(p, err);
        if (!te->name) { free(te); return NULL; }
    }
    return te;
}

/* ── Params ── */

static Param *parse_params(Parser *p, size_t *count, char **err) {
    size_t cap = 4;
    size_t n = 0;
    Param *params = malloc(cap * sizeof(Param));
    bool seen_default = false;
    bool seen_variadic = false;

    while (peek_type(p) != TOK_RPAREN && !at_eof(p)) {
        if (n >= cap) { cap *= 2; params = realloc(params, cap * sizeof(Param)); }
        if (seen_variadic) {
            *err = strdup("variadic parameter must be last");
            for (size_t i = 0; i < n; i++) { free(params[i].name); if (params[i].default_value) expr_free(params[i].default_value); }
            free(params);
            return NULL;
        }
        /* Check for variadic: ...name */
        bool is_variadic = false;
        if (peek_type(p) == TOK_DOTDOTDOT) {
            advance(p);
            is_variadic = true;
            seen_variadic = true;
        }
        params[n].name = expect_ident(p, err);
        if (!params[n].name) {
            for (size_t i = 0; i < n; i++) { free(params[i].name); if (params[i].default_value) expr_free(params[i].default_value); }
            free(params);
            return NULL;
        }
        if (!expect(p, TOK_COLON, err)) {
            free(params[n].name);
            for (size_t i = 0; i < n; i++) { free(params[i].name); if (params[i].default_value) expr_free(params[i].default_value); }
            free(params);
            return NULL;
        }
        TypeExpr *te = parse_type_expr(p, err);
        if (!te) {
            free(params[n].name);
            for (size_t i = 0; i < n; i++) { free(params[i].name); if (params[i].default_value) expr_free(params[i].default_value); }
            free(params);
            return NULL;
        }
        params[n].ty = *te;
        free(te);
        params[n].is_variadic = is_variadic;
        params[n].default_value = NULL;
        /* Check for default value: = expr */
        if (!is_variadic && peek_type(p) == TOK_EQ) {
            advance(p);
            seen_default = true;
            Expr *def = parse_expr(p, err);
            if (!def) {
                free(params[n].name);
                for (size_t i = 0; i < n; i++) { free(params[i].name); if (params[i].default_value) expr_free(params[i].default_value); }
                free(params);
                return NULL;
            }
            params[n].default_value = def;
        } else if (seen_default && !is_variadic) {
            *err = strdup("required parameter cannot follow a parameter with a default value");
            free(params[n].name);
            for (size_t i = 0; i < n; i++) { free(params[i].name); if (params[i].default_value) expr_free(params[i].default_value); }
            free(params);
            return NULL;
        }
        n++;
        if (peek_type(p) != TOK_RPAREN) {
            if (!expect(p, TOK_COMMA, err)) {
                for (size_t i = 0; i < n; i++) { free(params[i].name); if (params[i].default_value) expr_free(params[i].default_value); }
                free(params);
                return NULL;
            }
        }
    }
    *count = n;
    return params;
}

/* ── Arguments ── */

static Expr **parse_args(Parser *p, size_t *count, char **err) {
    size_t cap = 4;
    size_t n = 0;
    Expr **args = malloc(cap * sizeof(Expr *));

    while (peek_type(p) != TOK_RPAREN && !at_eof(p)) {
        Expr *e = parse_expr(p, err);
        if (!e) { free(args); return NULL; }
        if (n >= cap) { cap *= 2; args = realloc(args, cap * sizeof(Expr *)); }
        args[n++] = e;
        if (peek_type(p) != TOK_RPAREN) {
            if (!expect(p, TOK_COMMA, err)) {
                for (size_t i = 0; i < n; i++) expr_free(args[i]);
                free(args);
                return NULL;
            }
        }
    }
    *count = n;
    return args;
}

/* ── Expressions (precedence climbing) ── */

static Expr *parse_primary(Parser *p, char **err);
static Expr *parse_postfix(Parser *p, char **err);
static Expr *parse_unary(Parser *p, char **err);
static Expr *parse_multiplication(Parser *p, char **err);
static Expr *parse_addition(Parser *p, char **err);
static Expr *parse_shift(Parser *p, char **err);
static Expr *parse_range_expr(Parser *p, char **err);
static Expr *parse_comparison(Parser *p, char **err);
static Expr *parse_equality(Parser *p, char **err);
static Expr *parse_bitwise_and(Parser *p, char **err);
static Expr *parse_bitwise_xor(Parser *p, char **err);
static Expr *parse_bitwise_or(Parser *p, char **err);
static Expr *parse_and_expr(Parser *p, char **err);
static Expr *parse_or(Parser *p, char **err);

static Expr *parse_nil_coalesce(Parser *p, char **err);

static Expr *parse_expr(Parser *p, char **err) {
    return parse_nil_coalesce(p, err);
}

static Expr *parse_nil_coalesce(Parser *p, char **err) {
    Expr *left = parse_or(p, err);
    if (!left) return NULL;
    while (peek_type(p) == TOK_QUESTION_QUESTION) {
        advance(p);
        Expr *right = parse_or(p, err);
        if (!right) { expr_free(left); return NULL; }
        left = expr_binop(BINOP_NIL_COALESCE, left, right);
    }
    return left;
}

static Expr *parse_or(Parser *p, char **err) {
    Expr *left = parse_and_expr(p, err);
    if (!left) return NULL;
    while (peek_type(p) == TOK_OR) {
        advance(p);
        Expr *right = parse_and_expr(p, err);
        if (!right) { expr_free(left); return NULL; }
        left = expr_binop(BINOP_OR, left, right);
    }
    return left;
}

static Expr *parse_and_expr(Parser *p, char **err) {
    Expr *left = parse_bitwise_or(p, err);
    if (!left) return NULL;
    while (peek_type(p) == TOK_AND) {
        advance(p);
        Expr *right = parse_bitwise_or(p, err);
        if (!right) { expr_free(left); return NULL; }
        left = expr_binop(BINOP_AND, left, right);
    }
    return left;
}

static Expr *parse_bitwise_or(Parser *p, char **err) {
    Expr *left = parse_bitwise_xor(p, err);
    if (!left) return NULL;
    while (peek_type(p) == TOK_PIPE) {
        advance(p);
        Expr *right = parse_bitwise_xor(p, err);
        if (!right) { expr_free(left); return NULL; }
        left = expr_binop(BINOP_BIT_OR, left, right);
    }
    return left;
}

static Expr *parse_bitwise_xor(Parser *p, char **err) {
    Expr *left = parse_bitwise_and(p, err);
    if (!left) return NULL;
    while (peek_type(p) == TOK_CARET) {
        advance(p);
        Expr *right = parse_bitwise_and(p, err);
        if (!right) { expr_free(left); return NULL; }
        left = expr_binop(BINOP_BIT_XOR, left, right);
    }
    return left;
}

static Expr *parse_bitwise_and(Parser *p, char **err) {
    Expr *left = parse_equality(p, err);
    if (!left) return NULL;
    while (peek_type(p) == TOK_AMPERSAND) {
        advance(p);
        Expr *right = parse_equality(p, err);
        if (!right) { expr_free(left); return NULL; }
        left = expr_binop(BINOP_BIT_AND, left, right);
    }
    return left;
}

static Expr *parse_equality(Parser *p, char **err) {
    Expr *left = parse_comparison(p, err);
    if (!left) return NULL;
    for (;;) {
        BinOpKind op;
        if (peek_type(p) == TOK_EQEQ) op = BINOP_EQ;
        else if (peek_type(p) == TOK_BANGEQ) op = BINOP_NEQ;
        else break;
        advance(p);
        Expr *right = parse_comparison(p, err);
        if (!right) { expr_free(left); return NULL; }
        left = expr_binop(op, left, right);
    }
    return left;
}

static Expr *parse_comparison(Parser *p, char **err) {
    Expr *left = parse_shift(p, err);
    if (!left) return NULL;
    for (;;) {
        BinOpKind op;
        if (peek_type(p) == TOK_LT) op = BINOP_LT;
        else if (peek_type(p) == TOK_GT) op = BINOP_GT;
        else if (peek_type(p) == TOK_LTEQ) op = BINOP_LTEQ;
        else if (peek_type(p) == TOK_GTEQ) op = BINOP_GTEQ;
        else break;
        advance(p);
        Expr *right = parse_shift(p, err);
        if (!right) { expr_free(left); return NULL; }
        left = expr_binop(op, left, right);
    }
    return left;
}

static Expr *parse_shift(Parser *p, char **err) {
    Expr *left = parse_range_expr(p, err);
    if (!left) return NULL;
    for (;;) {
        BinOpKind op;
        if (peek_type(p) == TOK_LSHIFT) op = BINOP_LSHIFT;
        else if (peek_type(p) == TOK_RSHIFT) op = BINOP_RSHIFT;
        else break;
        advance(p);
        Expr *right = parse_range_expr(p, err);
        if (!right) { expr_free(left); return NULL; }
        left = expr_binop(op, left, right);
    }
    return left;
}

static Expr *parse_range_expr(Parser *p, char **err) {
    Expr *left = parse_addition(p, err);
    if (!left) return NULL;
    if (peek_type(p) == TOK_DOTDOT) {
        advance(p);
        Expr *right = parse_addition(p, err);
        if (!right) { expr_free(left); return NULL; }
        return expr_range(left, right);
    }
    return left;
}

static Expr *parse_addition(Parser *p, char **err) {
    Expr *left = parse_multiplication(p, err);
    if (!left) return NULL;
    for (;;) {
        BinOpKind op;
        if (peek_type(p) == TOK_PLUS) op = BINOP_ADD;
        else if (peek_type(p) == TOK_MINUS) op = BINOP_SUB;
        else break;
        advance(p);
        Expr *right = parse_multiplication(p, err);
        if (!right) { expr_free(left); return NULL; }
        left = expr_binop(op, left, right);
    }
    return left;
}

static Expr *parse_multiplication(Parser *p, char **err) {
    Expr *left = parse_unary(p, err);
    if (!left) return NULL;
    for (;;) {
        BinOpKind op;
        if (peek_type(p) == TOK_STAR) op = BINOP_MUL;
        else if (peek_type(p) == TOK_SLASH) op = BINOP_DIV;
        else if (peek_type(p) == TOK_PERCENT) op = BINOP_MOD;
        else break;
        advance(p);
        Expr *right = parse_unary(p, err);
        if (!right) { expr_free(left); return NULL; }
        left = expr_binop(op, left, right);
    }
    return left;
}

static Expr *parse_unary(Parser *p, char **err) {
    if (peek_type(p) == TOK_MINUS) {
        advance(p);
        Expr *e = parse_unary(p, err);
        if (!e) return NULL;
        return expr_unaryop(UNOP_NEG, e);
    }
    if (peek_type(p) == TOK_BANG) {
        advance(p);
        Expr *e = parse_unary(p, err);
        if (!e) return NULL;
        return expr_unaryop(UNOP_NOT, e);
    }
    if (peek_type(p) == TOK_TILDE) {
        advance(p);
        Expr *e = parse_unary(p, err);
        if (!e) return NULL;
        return expr_unaryop(UNOP_BIT_NOT, e);
    }
    return parse_postfix(p, err);
}

static bool is_struct_literal_ahead(Parser *p) {
    /* After '{', if we see ident ':' it's a struct literal */
    if (peek_ahead_type(p, 1) == TOK_RBRACE) return true; /* empty struct */
    return peek_ahead_type(p, 1) == TOK_IDENT && peek_ahead_type(p, 2) == TOK_COLON;
}

static Expr *parse_postfix(Parser *p, char **err) {
    Expr *e = parse_primary(p, err);
    if (!e) return NULL;
    for (;;) {
        if (peek_type(p) == TOK_DOT || peek_type(p) == TOK_QUESTION_DOT) {
            bool optional = (peek_type(p) == TOK_QUESTION_DOT);
            advance(p);
            /* Accept identifier or integer literal for tuple field access (e.g. t.0) */
            char *field = NULL;
            if (peek_type(p) == TOK_INT_LIT) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%lld", (long long)peek(p)->as.int_val);
                field = strdup(buf);
                advance(p);
            } else {
                field = expect_ident(p, err);
            }
            if (!field) { expr_free(e); return NULL; }
            if (peek_type(p) == TOK_LPAREN) {
                advance(p);
                size_t arg_count;
                Expr **args = parse_args(p, &arg_count, err);
                if (!args && *err) { free(field); expr_free(e); return NULL; }
                if (!expect(p, TOK_RPAREN, err)) {
                    free(field); expr_free(e);
                    for (size_t i = 0; i < arg_count; i++) expr_free(args[i]);
                    free(args);
                    return NULL;
                }
                Expr *mc = expr_method_call(e, field, args, arg_count);
                mc->as.method_call.optional = optional;
                e = mc;
            } else {
                Expr *fa = expr_field_access(e, field);
                fa->as.field_access.optional = optional;
                e = fa;
            }
        } else if (peek_type(p) == TOK_LBRACKET || peek_type(p) == TOK_QUESTION_LBRACKET) {
            bool optional = (peek_type(p) == TOK_QUESTION_LBRACKET);
            advance(p);
            Expr *idx = parse_expr(p, err);
            if (!idx) { expr_free(e); return NULL; }
            if (!expect(p, TOK_RBRACKET, err)) { expr_free(e); expr_free(idx); return NULL; }
            Expr *ix = expr_index(e, idx);
            ix->as.index.optional = optional;
            e = ix;
        } else if (peek_type(p) == TOK_LPAREN && e->tag == EXPR_IDENT) {
            advance(p);
            size_t arg_count;
            Expr **args = parse_args(p, &arg_count, err);
            if (!args && *err) { expr_free(e); return NULL; }
            if (!expect(p, TOK_RPAREN, err)) {
                expr_free(e);
                for (size_t i = 0; i < arg_count; i++) expr_free(args[i]);
                free(args);
                return NULL;
            }
            e = expr_call(e, args, arg_count);
        } else if (peek_type(p) == TOK_QUESTION) {
            /* Result ? operator — postfix try/propagate */
            advance(p);
            e = expr_try_propagate(e);
        } else {
            break;
        }
    }
    return e;
}

static Expr *parse_primary(Parser *p, char **err) {
    TokenType tt = peek_type(p);

    if (tt == TOK_INT_LIT) {
        Token *t = advance(p);
        return expr_int_lit(t->as.int_val);
    }
    if (tt == TOK_FLOAT_LIT) {
        Token *t = advance(p);
        return expr_float_lit(t->as.float_val);
    }
    if (tt == TOK_STRING_LIT) {
        Token *t = advance(p);
        return expr_string_lit(strdup(t->as.str_val));
    }
    if (tt == TOK_INTERP_START) {
        /* Interpolated string: INTERP_START expr (INTERP_MID expr)* INTERP_END */
        size_t cap = 4;
        size_t count = 0;
        char **parts = malloc((cap + 1) * sizeof(char *));
        Expr **exprs = malloc(cap * sizeof(Expr *));

        Token *t = advance(p); /* consume INTERP_START */
        parts[0] = strdup(t->as.str_val);

        for (;;) {
            /* Parse the interpolated expression */
            Expr *e = parse_expr(p, err);
            if (!e) goto interp_fail;
            if (count >= cap) {
                cap *= 2;
                parts = realloc(parts, (cap + 1) * sizeof(char *));
                exprs = realloc(exprs, cap * sizeof(Expr *));
            }
            exprs[count] = e;
            count++;

            if (peek_type(p) == TOK_INTERP_MID) {
                Token *mid = advance(p);
                parts[count] = strdup(mid->as.str_val);
            } else if (peek_type(p) == TOK_INTERP_END) {
                Token *end = advance(p);
                parts[count] = strdup(end->as.str_val);
                break;
            } else {
                *err = parser_error_fmt(p, "expected interpolation continuation or end, got '%s'",
                                        token_type_name(peek_type(p)));
                goto interp_fail;
            }
        }
        return expr_interp_string(parts, exprs, count);

    interp_fail:
        for (size_t i = 0; i <= count; i++) free(parts[i]);
        free(parts);
        for (size_t i = 0; i < count; i++) expr_free(exprs[i]);
        free(exprs);
        return NULL;
    }
    if (tt == TOK_TRUE) { advance(p); return expr_bool_lit(true); }
    if (tt == TOK_FALSE) { advance(p); return expr_bool_lit(false); }
    if (tt == TOK_NIL) { advance(p); return expr_nil_lit(); }

    if (tt == TOK_FREEZE) {
        advance(p);
        if (!expect(p, TOK_LPAREN, err)) return NULL;
        Expr *e = parse_expr(p, err);
        if (!e) return NULL;
        if (!expect(p, TOK_RPAREN, err)) { expr_free(e); return NULL; }
        /* Optional 'where' clause: freeze(x) where |v| { ... } */
        Expr *contract = NULL;
        if (peek_type(p) == TOK_IDENT) {
            Token *maybe_where = peek(p);
            if (maybe_where->as.str_val && strcmp(maybe_where->as.str_val, "where") == 0) {
                advance(p);  /* consume 'where' */
                contract = parse_expr(p, err);
                if (!contract) { expr_free(e); return NULL; }
            }
        }
        /* Optional 'except' clause: freeze(x) except ["field1", "field2"] */
        if (peek_type(p) == TOK_IDENT) {
            Token *maybe_except = peek(p);
            if (maybe_except->as.str_val && strcmp(maybe_except->as.str_val, "except") == 0) {
                advance(p);  /* consume 'except' */
                if (!expect(p, TOK_LBRACKET, err)) { expr_free(e); if (contract) expr_free(contract); return NULL; }
                size_t ecap = 4, en = 0;
                Expr **except_fields = malloc(ecap * sizeof(Expr *));
                while (peek_type(p) != TOK_RBRACKET && !at_eof(p)) {
                    Expr *ef = parse_expr(p, err);
                    if (!ef) { for (size_t i = 0; i < en; i++) expr_free(except_fields[i]); free(except_fields); expr_free(e); if (contract) expr_free(contract); return NULL; }
                    if (en >= ecap) { ecap *= 2; except_fields = realloc(except_fields, ecap * sizeof(Expr *)); }
                    except_fields[en++] = ef;
                    if (peek_type(p) != TOK_RBRACKET) {
                        if (!expect(p, TOK_COMMA, err)) { for (size_t i = 0; i < en; i++) expr_free(except_fields[i]); free(except_fields); expr_free(e); if (contract) expr_free(contract); return NULL; }
                    }
                }
                if (!expect(p, TOK_RBRACKET, err)) { for (size_t i = 0; i < en; i++) expr_free(except_fields[i]); free(except_fields); expr_free(e); if (contract) expr_free(contract); return NULL; }
                return expr_freeze_except(e, contract, except_fields, en);
            }
        }
        return expr_freeze(e, contract);
    }
    if (tt == TOK_THAW) {
        advance(p);
        if (!expect(p, TOK_LPAREN, err)) return NULL;
        Expr *e = parse_expr(p, err);
        if (!e) return NULL;
        if (!expect(p, TOK_RPAREN, err)) { expr_free(e); return NULL; }
        return expr_thaw(e);
    }
    if (tt == TOK_CLONE) {
        advance(p);
        if (!expect(p, TOK_LPAREN, err)) return NULL;
        Expr *e = parse_expr(p, err);
        if (!e) return NULL;
        if (!expect(p, TOK_RPAREN, err)) { expr_free(e); return NULL; }
        return expr_clone(e);
    }
    if (tt == TOK_ANNEAL) {
        advance(p);
        if (!expect(p, TOK_LPAREN, err)) return NULL;
        Expr *target = parse_expr(p, err);
        if (!target) return NULL;
        if (!expect(p, TOK_RPAREN, err)) { expr_free(target); return NULL; }
        /* Expect closure: |params| { body } */
        if (peek_type(p) != TOK_PIPE) {
            *err = strdup("anneal requires a closure: anneal(val) |v| { ... }");
            expr_free(target);
            return NULL;
        }
        Expr *closure = parse_expr(p, err);
        if (!closure) { expr_free(target); return NULL; }
        return expr_anneal(target, closure);
    }
    if (tt == TOK_CRYSTALLIZE) {
        advance(p);
        if (!expect(p, TOK_LPAREN, err)) return NULL;
        Expr *e = parse_expr(p, err);
        if (!e) return NULL;
        if (!expect(p, TOK_RPAREN, err)) { expr_free(e); return NULL; }
        if (!expect(p, TOK_LBRACE, err)) { expr_free(e); return NULL; }
        size_t count;
        Stmt **stmts = parse_block_stmts(p, &count, err);
        if (!stmts && *err) { expr_free(e); return NULL; }
        if (!expect(p, TOK_RBRACE, err)) {
            expr_free(e);
            for (size_t i = 0; i < count; i++) stmt_free(stmts[i]);
            free(stmts);
            return NULL;
        }
        return expr_crystallize(e, stmts, count);
    }
    if (tt == TOK_SUBLIMATE) {
        advance(p);
        if (!expect(p, TOK_LPAREN, err)) return NULL;
        Expr *e = parse_expr(p, err);
        if (!e) return NULL;
        if (!expect(p, TOK_RPAREN, err)) { expr_free(e); return NULL; }
        return expr_sublimate(e);
    }
    if (tt == TOK_PRINT) {
        advance(p);
        if (!expect(p, TOK_LPAREN, err)) return NULL;
        size_t arg_count;
        Expr **args = parse_args(p, &arg_count, err);
        if (!args && *err) return NULL;
        if (!expect(p, TOK_RPAREN, err)) {
            for (size_t i = 0; i < arg_count; i++) expr_free(args[i]);
            free(args);
            return NULL;
        }
        return expr_print(args, arg_count);
    }
    if (tt == TOK_FORGE) {
        advance(p);
        if (!expect(p, TOK_LBRACE, err)) return NULL;
        size_t count;
        Stmt **stmts = parse_block_stmts(p, &count, err);
        if (!stmts && *err) return NULL;
        if (!expect(p, TOK_RBRACE, err)) {
            for (size_t i = 0; i < count; i++) stmt_free(stmts[i]);
            free(stmts);
            return NULL;
        }
        return expr_forge(stmts, count);
    }
    if (tt == TOK_SPAWN) {
        advance(p);
        if (!expect(p, TOK_LBRACE, err)) return NULL;
        size_t count;
        Stmt **stmts = parse_block_stmts(p, &count, err);
        if (!stmts && *err) return NULL;
        if (!expect(p, TOK_RBRACE, err)) {
            for (size_t i = 0; i < count; i++) stmt_free(stmts[i]);
            free(stmts);
            return NULL;
        }
        return expr_spawn(stmts, count);
    }
    if (tt == TOK_SCOPE) {
        advance(p);
        if (!expect(p, TOK_LBRACE, err)) return NULL;
        size_t count;
        Stmt **stmts = parse_block_stmts(p, &count, err);
        if (!stmts && *err) return NULL;
        if (!expect(p, TOK_RBRACE, err)) {
            for (size_t i = 0; i < count; i++) stmt_free(stmts[i]);
            free(stmts);
            return NULL;
        }
        return expr_scope(stmts, count);
    }
    if (tt == TOK_TRY) {
        advance(p);
        if (!expect(p, TOK_LBRACE, err)) return NULL;
        size_t try_count;
        Stmt **try_stmts = parse_block_stmts(p, &try_count, err);
        if (!try_stmts && *err) return NULL;
        if (!expect(p, TOK_RBRACE, err)) {
            for (size_t i = 0; i < try_count; i++) stmt_free(try_stmts[i]);
            free(try_stmts);
            return NULL;
        }
        if (!expect(p, TOK_CATCH, err)) {
            for (size_t i = 0; i < try_count; i++) stmt_free(try_stmts[i]);
            free(try_stmts);
            return NULL;
        }
        char *catch_var = expect_ident(p, err);
        if (!catch_var) {
            for (size_t i = 0; i < try_count; i++) stmt_free(try_stmts[i]);
            free(try_stmts);
            return NULL;
        }
        if (!expect(p, TOK_LBRACE, err)) {
            for (size_t i = 0; i < try_count; i++) stmt_free(try_stmts[i]);
            free(try_stmts); free(catch_var);
            return NULL;
        }
        size_t catch_count;
        Stmt **catch_stmts = parse_block_stmts(p, &catch_count, err);
        if (!catch_stmts && *err) {
            for (size_t i = 0; i < try_count; i++) stmt_free(try_stmts[i]);
            free(try_stmts); free(catch_var);
            return NULL;
        }
        if (!expect(p, TOK_RBRACE, err)) {
            for (size_t i = 0; i < try_count; i++) stmt_free(try_stmts[i]);
            free(try_stmts); free(catch_var);
            for (size_t i = 0; i < catch_count; i++) stmt_free(catch_stmts[i]);
            free(catch_stmts);
            return NULL;
        }
        return expr_try_catch(try_stmts, try_count, catch_var, catch_stmts, catch_count);
    }
    if (tt == TOK_IDENT && peek(p)->as.str_val &&
        strcmp(peek(p)->as.str_val, "select") == 0 &&
        p->pos + 1 < p->count &&
        p->tokens[p->pos + 1].type == TOK_LBRACE) {
        advance(p);
        if (!expect(p, TOK_LBRACE, err)) return NULL;
        size_t cap = 4, n = 0;
        SelectArm *arms = malloc(cap * sizeof(SelectArm));
        while (peek_type(p) != TOK_RBRACE && !at_eof(p)) {
            if (n >= cap) { cap *= 2; arms = realloc(arms, cap * sizeof(SelectArm)); }
            memset(&arms[n], 0, sizeof(SelectArm));
            /* Check for 'default' arm */
            if (peek_type(p) == TOK_IDENT && peek(p)->as.str_val &&
                strcmp(peek(p)->as.str_val, "default") == 0) {
                advance(p);
                arms[n].is_default = true;
                if (!expect(p, TOK_FATARROW, err)) goto select_fail;
                if (!expect(p, TOK_LBRACE, err)) goto select_fail;
                arms[n].body = parse_block_stmts(p, &arms[n].body_count, err);
                if (!arms[n].body && *err) goto select_fail;
                if (!expect(p, TOK_RBRACE, err)) goto select_fail;
                n++;
                if (peek_type(p) == TOK_COMMA) advance(p);
                continue;
            }
            /* Check for 'timeout(expr)' arm */
            if (peek_type(p) == TOK_IDENT && peek(p)->as.str_val &&
                strcmp(peek(p)->as.str_val, "timeout") == 0) {
                advance(p);
                arms[n].is_timeout = true;
                if (!expect(p, TOK_LPAREN, err)) goto select_fail;
                arms[n].timeout_expr = parse_expr(p, err);
                if (!arms[n].timeout_expr) goto select_fail;
                if (!expect(p, TOK_RPAREN, err)) goto select_fail;
                if (!expect(p, TOK_FATARROW, err)) goto select_fail;
                if (!expect(p, TOK_LBRACE, err)) goto select_fail;
                arms[n].body = parse_block_stmts(p, &arms[n].body_count, err);
                if (!arms[n].body && *err) goto select_fail;
                if (!expect(p, TOK_RBRACE, err)) goto select_fail;
                n++;
                if (peek_type(p) == TOK_COMMA) advance(p);
                continue;
            }
            /* Normal arm: binding from channel_expr => { body } */
            arms[n].binding_name = expect_ident(p, err);
            if (!arms[n].binding_name) goto select_fail;
            if (!expect(p, TOK_FROM, err)) goto select_fail;
            arms[n].channel_expr = parse_expr(p, err);
            if (!arms[n].channel_expr) goto select_fail;
            if (!expect(p, TOK_FATARROW, err)) goto select_fail;
            if (!expect(p, TOK_LBRACE, err)) goto select_fail;
            arms[n].body = parse_block_stmts(p, &arms[n].body_count, err);
            if (!arms[n].body && *err) goto select_fail;
            if (!expect(p, TOK_RBRACE, err)) goto select_fail;
            n++;
            if (peek_type(p) == TOK_COMMA) advance(p);
        }
        if (!expect(p, TOK_RBRACE, err)) {
            select_fail:
            for (size_t i = 0; i < n; i++) {
                free(arms[i].binding_name);
                if (arms[i].channel_expr) expr_free(arms[i].channel_expr);
                if (arms[i].timeout_expr) expr_free(arms[i].timeout_expr);
                for (size_t j = 0; j < arms[i].body_count; j++) stmt_free(arms[i].body[j]);
                free(arms[i].body);
            }
            free(arms);
            return NULL;
        }
        return expr_select(arms, n);
    }
    if (tt == TOK_MATCH) {
        advance(p);
        Expr *scrutinee = parse_expr(p, err);
        if (!scrutinee) return NULL;
        if (!expect(p, TOK_LBRACE, err)) { expr_free(scrutinee); return NULL; }

        size_t cap = 4, n = 0;
        MatchArm *arms = malloc(cap * sizeof(MatchArm));

        while (peek_type(p) != TOK_RBRACE && !at_eof(p)) {
            if (n >= cap) { cap *= 2; arms = realloc(arms, cap * sizeof(MatchArm)); }

            /* Parse optional phase qualifier: fluid/crystal */
            AstPhase phase_qual = PHASE_UNSPECIFIED;
            if (peek_type(p) == TOK_IDENT) {
                Token *maybe_phase = peek(p);
                if (maybe_phase->as.str_val &&
                    (strcmp(maybe_phase->as.str_val, "fluid") == 0 ||
                     strcmp(maybe_phase->as.str_val, "crystal") == 0)) {
                    TokenType next = peek_ahead_type(p, 1);
                    if (next == TOK_IDENT || next == TOK_INT_LIT || next == TOK_FLOAT_LIT ||
                        next == TOK_STRING_LIT || next == TOK_TRUE || next == TOK_FALSE ||
                        next == TOK_NIL || next == TOK_MINUS) {
                        phase_qual = strcmp(maybe_phase->as.str_val, "fluid") == 0
                            ? PHASE_FLUID : PHASE_CRYSTAL;
                        advance(p);
                    }
                }
            }

            /* Parse pattern */
            Pattern *pat = NULL;
            TokenType pt = peek_type(p);
            if (pt == TOK_INT_LIT) {
                Token *t = advance(p);
                /* Check for range pattern: int..int */
                if (peek_type(p) == TOK_DOTDOT) {
                    advance(p);
                    Expr *start = expr_int_lit(t->as.int_val);
                    Expr *end = parse_expr(p, err);
                    if (!end) { expr_free(start); goto match_fail; }
                    pat = pattern_range(start, end);
                } else {
                    pat = pattern_literal(expr_int_lit(t->as.int_val));
                }
            } else if (pt == TOK_FLOAT_LIT) {
                Token *t = advance(p);
                pat = pattern_literal(expr_float_lit(t->as.float_val));
            } else if (pt == TOK_STRING_LIT) {
                Token *t = advance(p);
                pat = pattern_literal(expr_string_lit(strdup(t->as.str_val)));
            } else if (pt == TOK_TRUE) {
                advance(p);
                pat = pattern_literal(expr_bool_lit(true));
            } else if (pt == TOK_FALSE) {
                advance(p);
                pat = pattern_literal(expr_bool_lit(false));
            } else if (pt == TOK_NIL) {
                advance(p);
                pat = pattern_literal(expr_nil_lit());
            } else if (pt == TOK_IDENT) {
                Token *t = advance(p);
                if (strcmp(t->as.str_val, "_") == 0) {
                    pat = pattern_wildcard();
                } else {
                    /* Check for range: ident..expr */
                    if (peek_type(p) == TOK_DOTDOT) {
                        advance(p);
                        Expr *start = expr_ident(strdup(t->as.str_val));
                        Expr *end = parse_expr(p, err);
                        if (!end) { expr_free(start); goto match_fail; }
                        pat = pattern_range(start, end);
                    } else {
                        pat = pattern_binding(strdup(t->as.str_val));
                    }
                }
            } else if (pt == TOK_MINUS) {
                /* Negative literal: -1, -3.14 */
                advance(p);
                if (peek_type(p) == TOK_INT_LIT) {
                    Token *t = advance(p);
                    pat = pattern_literal(expr_int_lit(-t->as.int_val));
                } else if (peek_type(p) == TOK_FLOAT_LIT) {
                    Token *t = advance(p);
                    pat = pattern_literal(expr_float_lit(-t->as.float_val));
                } else {
                    *err = strdup("expected number after '-' in pattern");
                    goto match_fail;
                }
            } else {
                *err = strdup("expected pattern in match arm");
                goto match_fail;
            }

            /* Apply phase qualifier to pattern */
            if (pat) pat->phase_qualifier = phase_qual;

            /* Optional guard: if expr */
            Expr *guard = NULL;
            if (peek_type(p) == TOK_IF) {
                advance(p);
                guard = parse_expr(p, err);
                if (!guard) { pattern_free(pat); goto match_fail; }
            }

            /* => */
            if (!expect(p, TOK_FATARROW, err)) {
                pattern_free(pat);
                if (guard) expr_free(guard);
                goto match_fail;
            }

            /* Arm body: either { stmts } block or single expression */
            Stmt **body = NULL;
            size_t body_count = 0;
            if (peek_type(p) == TOK_LBRACE) {
                advance(p);
                body = parse_block_stmts(p, &body_count, err);
                if (!body && *err) {
                    pattern_free(pat);
                    if (guard) expr_free(guard);
                    goto match_fail;
                }
                if (!expect(p, TOK_RBRACE, err)) {
                    pattern_free(pat);
                    if (guard) expr_free(guard);
                    for (size_t i = 0; i < body_count; i++) stmt_free(body[i]);
                    free(body);
                    goto match_fail;
                }
            } else {
                Expr *arm_expr = parse_expr(p, err);
                if (!arm_expr) {
                    pattern_free(pat);
                    if (guard) expr_free(guard);
                    goto match_fail;
                }
                body = malloc(sizeof(Stmt *));
                body[0] = stmt_expr(arm_expr);
                body_count = 1;
            }

            arms[n].pattern = pat;
            arms[n].guard = guard;
            arms[n].body = body;
            arms[n].body_count = body_count;
            n++;

            /* Optional comma between arms */
            if (peek_type(p) == TOK_COMMA) advance(p);
        }

        if (!expect(p, TOK_RBRACE, err)) {
            match_fail:
            for (size_t i = 0; i < n; i++) {
                pattern_free(arms[i].pattern);
                if (arms[i].guard) expr_free(arms[i].guard);
                for (size_t j = 0; j < arms[i].body_count; j++) stmt_free(arms[i].body[j]);
                free(arms[i].body);
            }
            free(arms);
            expr_free(scrutinee);
            return NULL;
        }
        return expr_match(scrutinee, arms, n);
    }
    if (tt == TOK_IF) {
        advance(p);
        Expr *cond = parse_expr(p, err);
        if (!cond) return NULL;
        if (!expect(p, TOK_LBRACE, err)) { expr_free(cond); return NULL; }
        size_t then_count;
        Stmt **then_stmts = parse_block_stmts(p, &then_count, err);
        if (!then_stmts && *err) { expr_free(cond); return NULL; }
        if (!expect(p, TOK_RBRACE, err)) {
            expr_free(cond);
            for (size_t i = 0; i < then_count; i++) stmt_free(then_stmts[i]);
            free(then_stmts);
            return NULL;
        }
        Stmt **else_stmts = NULL;
        size_t else_count = 0;
        if (peek_type(p) == TOK_ELSE) {
            advance(p);
            if (!expect(p, TOK_LBRACE, err)) {
                expr_free(cond);
                for (size_t i = 0; i < then_count; i++) stmt_free(then_stmts[i]);
                free(then_stmts);
                return NULL;
            }
            else_stmts = parse_block_stmts(p, &else_count, err);
            if (!else_stmts && *err) {
                expr_free(cond);
                for (size_t i = 0; i < then_count; i++) stmt_free(then_stmts[i]);
                free(then_stmts);
                return NULL;
            }
            if (!expect(p, TOK_RBRACE, err)) {
                expr_free(cond);
                for (size_t i = 0; i < then_count; i++) stmt_free(then_stmts[i]);
                free(then_stmts);
                for (size_t i = 0; i < else_count; i++) stmt_free(else_stmts[i]);
                free(else_stmts);
                return NULL;
            }
        }
        return expr_if(cond, then_stmts, then_count, else_stmts, else_count);
    }
    if (tt == TOK_LBRACKET) {
        advance(p);
        size_t cap = 4;
        size_t n = 0;
        Expr **elems = malloc(cap * sizeof(Expr *));
        while (peek_type(p) != TOK_RBRACKET && !at_eof(p)) {
            bool is_spread = false;
            if (peek_type(p) == TOK_DOTDOTDOT) {
                advance(p);
                is_spread = true;
            }
            Expr *e = parse_expr(p, err);
            if (!e) {
                for (size_t i = 0; i < n; i++) expr_free(elems[i]);
                free(elems);
                return NULL;
            }
            if (is_spread) e = expr_spread(e);
            if (n >= cap) { cap *= 2; elems = realloc(elems, cap * sizeof(Expr *)); }
            elems[n++] = e;
            if (peek_type(p) != TOK_RBRACKET) {
                if (!expect(p, TOK_COMMA, err)) {
                    for (size_t i = 0; i < n; i++) expr_free(elems[i]);
                    free(elems);
                    return NULL;
                }
            }
        }
        if (!expect(p, TOK_RBRACKET, err)) {
            for (size_t i = 0; i < n; i++) expr_free(elems[i]);
            free(elems);
            return NULL;
        }
        return expr_array(elems, n);
    }
    if (tt == TOK_LPAREN) {
        advance(p);
        Expr *first = parse_expr(p, err);
        if (!first) return NULL;
        /* Check for comma: (expr, ...) is a tuple, (expr) is grouping */
        if (peek_type(p) == TOK_COMMA) {
            advance(p);
            size_t cap = 4, n = 1;
            Expr **elems = malloc(cap * sizeof(Expr *));
            elems[0] = first;
            /* (expr,) is a single-element tuple */
            while (peek_type(p) != TOK_RPAREN && !at_eof(p)) {
                Expr *e = parse_expr(p, err);
                if (!e) {
                    for (size_t i = 0; i < n; i++) expr_free(elems[i]);
                    free(elems);
                    return NULL;
                }
                if (n >= cap) { cap *= 2; elems = realloc(elems, cap * sizeof(Expr *)); }
                elems[n++] = e;
                if (peek_type(p) != TOK_RPAREN) {
                    if (!expect(p, TOK_COMMA, err)) {
                        for (size_t i = 0; i < n; i++) expr_free(elems[i]);
                        free(elems);
                        return NULL;
                    }
                }
            }
            if (!expect(p, TOK_RPAREN, err)) {
                for (size_t i = 0; i < n; i++) expr_free(elems[i]);
                free(elems);
                return NULL;
            }
            return expr_tuple(elems, n);
        }
        if (!expect(p, TOK_RPAREN, err)) { expr_free(first); return NULL; }
        return first;
    }
    if (tt == TOK_LBRACE) {
        advance(p);
        size_t count;
        Stmt **stmts = parse_block_stmts(p, &count, err);
        if (!stmts && *err) return NULL;
        if (!expect(p, TOK_RBRACE, err)) {
            for (size_t i = 0; i < count; i++) stmt_free(stmts[i]);
            free(stmts);
            return NULL;
        }
        return expr_block(stmts, count);
    }
    if (tt == TOK_PIPE) {
        /* Closure: |params| expr  or  |a, b = 1, ...rest| expr */
        advance(p);
        size_t cap = 4;
        size_t n = 0;
        char **params = malloc(cap * sizeof(char *));
        Expr **defaults = calloc(cap, sizeof(Expr *));
        bool has_variadic = false;
        bool seen_default = false;
        while (peek_type(p) != TOK_PIPE && !at_eof(p)) {
            if (has_variadic) {
                *err = strdup("variadic parameter must be last");
                for (size_t i = 0; i < n; i++) { free(params[i]); if (defaults[i]) expr_free(defaults[i]); }
                free(params); free(defaults);
                return NULL;
            }
            /* Check for variadic: ...name */
            bool is_variadic = false;
            if (peek_type(p) == TOK_DOTDOTDOT) {
                advance(p);
                is_variadic = true;
                has_variadic = true;
            }
            char *name = expect_ident(p, err);
            if (!name) {
                for (size_t i = 0; i < n; i++) { free(params[i]); if (defaults[i]) expr_free(defaults[i]); }
                free(params); free(defaults);
                return NULL;
            }
            if (n >= cap) {
                cap *= 2;
                params = realloc(params, cap * sizeof(char *));
                defaults = realloc(defaults, cap * sizeof(Expr *));
                for (size_t i = n; i < cap; i++) defaults[i] = NULL;
            }
            params[n] = name;
            defaults[n] = NULL;
            /* Check for default value: = expr (not for variadic params)
             * Use parse_bitwise_xor to avoid consuming | as bitwise OR,
             * since | delimits closure parameters. */
            if (!is_variadic && peek_type(p) == TOK_EQ) {
                advance(p);
                seen_default = true;
                Expr *def = parse_bitwise_xor(p, err);
                if (!def) {
                    for (size_t i = 0; i <= n; i++) { free(params[i]); if (defaults[i]) expr_free(defaults[i]); }
                    free(params); free(defaults);
                    return NULL;
                }
                defaults[n] = def;
            } else if (seen_default && !is_variadic) {
                *err = strdup("required parameter cannot follow a parameter with a default value");
                free(name);
                for (size_t i = 0; i < n; i++) { free(params[i]); if (defaults[i]) expr_free(defaults[i]); }
                free(params); free(defaults);
                return NULL;
            }
            n++;
            if (peek_type(p) != TOK_PIPE) {
                if (!expect(p, TOK_COMMA, err)) {
                    for (size_t i = 0; i < n; i++) { free(params[i]); if (defaults[i]) expr_free(defaults[i]); }
                    free(params); free(defaults);
                    return NULL;
                }
            }
        }
        if (!expect(p, TOK_PIPE, err)) {
            for (size_t i = 0; i < n; i++) { free(params[i]); if (defaults[i]) expr_free(defaults[i]); }
            free(params); free(defaults);
            return NULL;
        }
        Expr *body = parse_expr(p, err);
        if (!body) {
            for (size_t i = 0; i < n; i++) { free(params[i]); if (defaults[i]) expr_free(defaults[i]); }
            free(params); free(defaults);
            return NULL;
        }
        /* Check if any defaults were actually used */
        bool any_defaults = false;
        for (size_t i = 0; i < n; i++) { if (defaults[i]) { any_defaults = true; break; } }
        if (!any_defaults && !has_variadic) { free(defaults); defaults = NULL; }
        return expr_closure(params, n, body, defaults, has_variadic);
    }
    if (tt == TOK_IDENT) {
        Token *t = advance(p);
        char *name = strdup(t->as.str_val);
        /* Struct literal: Name { field: value, ... } */
        if (peek_type(p) == TOK_LBRACE && isupper((unsigned char)name[0]) && is_struct_literal_ahead(p)) {
            advance(p); /* { */
            size_t cap = 4;
            size_t n = 0;
            FieldInit *fields = malloc(cap * sizeof(FieldInit));
            while (peek_type(p) != TOK_RBRACE && !at_eof(p)) {
                if (n >= cap) { cap *= 2; fields = realloc(fields, cap * sizeof(FieldInit)); }
                fields[n].name = expect_ident(p, err);
                if (!fields[n].name) {
                    for (size_t i = 0; i < n; i++) { free(fields[i].name); expr_free(fields[i].value); }
                    free(fields); free(name);
                    return NULL;
                }
                if (!expect(p, TOK_COLON, err)) {
                    free(fields[n].name);
                    for (size_t i = 0; i < n; i++) { free(fields[i].name); expr_free(fields[i].value); }
                    free(fields); free(name);
                    return NULL;
                }
                fields[n].value = parse_expr(p, err);
                if (!fields[n].value) {
                    free(fields[n].name);
                    for (size_t i = 0; i < n; i++) { free(fields[i].name); expr_free(fields[i].value); }
                    free(fields); free(name);
                    return NULL;
                }
                n++;
                if (peek_type(p) != TOK_RBRACE) {
                    if (!expect(p, TOK_COMMA, err)) {
                        for (size_t i = 0; i < n; i++) { free(fields[i].name); expr_free(fields[i].value); }
                        free(fields); free(name);
                        return NULL;
                    }
                }
            }
            if (!expect(p, TOK_RBRACE, err)) {
                for (size_t i = 0; i < n; i++) { free(fields[i].name); expr_free(fields[i].value); }
                free(fields); free(name);
                return NULL;
            }
            return expr_struct_lit(name, fields, n);
        }
        /* Name::Variant or Name::method() */
        if (peek_type(p) == TOK_COLONCOLON) {
            advance(p);
            /* Accept contextual keywords (e.g. 'from' in Set::from) as identifiers */
            char *rhs = NULL;
            if (peek_type(p) == TOK_FROM) {
                advance(p);
                rhs = strdup("from");
            } else {
                rhs = expect_ident(p, err);
            }
            if (!rhs) { free(name); return NULL; }

            if (peek_type(p) == TOK_LPAREN) {
                /* Could be enum variant with args or static method call.
                 * We parse as enum variant; the evaluator falls back to
                 * static call if no enum matches. */
                advance(p);
                size_t arg_count;
                Expr **args = parse_args(p, &arg_count, err);
                if (!args && *err) { free(name); free(rhs); return NULL; }
                if (!expect(p, TOK_RPAREN, err)) {
                    free(name); free(rhs);
                    for (size_t i = 0; i < arg_count; i++) expr_free(args[i]);
                    free(args);
                    return NULL;
                }
                return expr_enum_variant(name, rhs, args, arg_count);
            }
            /* Unit variant (or identifier) */
            return expr_enum_variant(name, rhs, NULL, 0);
        }
        return expr_ident(name);
    }

    *err = parser_error_fmt(p, "unexpected token '%s' in expression",
                            token_type_name(peek_type(p)));
    return NULL;
}

/* ── Statements ── */

static Stmt *parse_binding(Parser *p, AstPhase phase, char **err) {
    advance(p); /* consume flux/fix/let */

    /* Array destructuring: let [a, b, ...rest] = expr */
    if (peek_type(p) == TOK_LBRACKET) {
        advance(p);
        size_t cap = 4, n = 0;
        char **names = malloc(cap * sizeof(char *));
        char *rest_name = NULL;
        while (peek_type(p) != TOK_RBRACKET && !at_eof(p)) {
            if (n >= cap) { cap *= 2; names = realloc(names, cap * sizeof(char *)); }
            if (peek_type(p) == TOK_DOTDOTDOT) {
                advance(p);
                rest_name = expect_ident(p, err);
                if (!rest_name) { for (size_t i = 0; i < n; i++) free(names[i]); free(names); return NULL; }
                break;
            }
            names[n] = expect_ident(p, err);
            if (!names[n]) { for (size_t i = 0; i < n; i++) free(names[i]); free(names); return NULL; }
            n++;
            if (peek_type(p) != TOK_RBRACKET && peek_type(p) != TOK_DOTDOTDOT) {
                if (!expect(p, TOK_COMMA, err)) { for (size_t i = 0; i < n; i++) free(names[i]); free(names); free(rest_name); return NULL; }
            }
        }
        if (!expect(p, TOK_RBRACKET, err)) { for (size_t i = 0; i < n; i++) free(names[i]); free(names); free(rest_name); return NULL; }
        if (!expect(p, TOK_EQ, err)) { for (size_t i = 0; i < n; i++) free(names[i]); free(names); free(rest_name); return NULL; }
        Expr *value = parse_expr(p, err);
        if (!value) { for (size_t i = 0; i < n; i++) free(names[i]); free(names); free(rest_name); return NULL; }
        eat_semicolon(p);
        return stmt_destructure(phase, DESTRUCT_ARRAY, names, n, rest_name, value);
    }

    /* Struct destructuring: let { x, y } = expr */
    if (peek_type(p) == TOK_LBRACE) {
        advance(p);
        size_t cap = 4, n = 0;
        char **names = malloc(cap * sizeof(char *));
        while (peek_type(p) != TOK_RBRACE && !at_eof(p)) {
            if (n >= cap) { cap *= 2; names = realloc(names, cap * sizeof(char *)); }
            names[n] = expect_ident(p, err);
            if (!names[n]) { for (size_t i = 0; i < n; i++) free(names[i]); free(names); return NULL; }
            n++;
            if (peek_type(p) != TOK_RBRACE) {
                if (!expect(p, TOK_COMMA, err)) { for (size_t i = 0; i < n; i++) free(names[i]); free(names); return NULL; }
            }
        }
        if (!expect(p, TOK_RBRACE, err)) { for (size_t i = 0; i < n; i++) free(names[i]); free(names); return NULL; }
        if (!expect(p, TOK_EQ, err)) { for (size_t i = 0; i < n; i++) free(names[i]); free(names); return NULL; }
        Expr *value = parse_expr(p, err);
        if (!value) { for (size_t i = 0; i < n; i++) free(names[i]); free(names); return NULL; }
        eat_semicolon(p);
        return stmt_destructure(phase, DESTRUCT_STRUCT, names, n, NULL, value);
    }

    /* Normal binding: let name = expr */
    char *name = expect_ident(p, err);
    if (!name) return NULL;

    TypeExpr *ty = NULL;
    if (peek_type(p) == TOK_COLON) {
        advance(p);
        ty = parse_type_expr(p, err);
        if (!ty) { free(name); return NULL; }
    }

    if (!expect(p, TOK_EQ, err)) { free(name); if (ty) { type_expr_free(ty); free(ty); } return NULL; }
    Expr *value = parse_expr(p, err);
    if (!value) { free(name); if (ty) { type_expr_free(ty); free(ty); } return NULL; }
    eat_semicolon(p);
    return stmt_binding(phase, name, ty, value);
}

static Stmt *parse_for_stmt(Parser *p, char **err) {
    if (!expect(p, TOK_FOR, err)) return NULL;
    char *var = expect_ident(p, err);
    if (!var) return NULL;
    if (!expect(p, TOK_IN, err)) { free(var); return NULL; }
    Expr *iter = parse_expr(p, err);
    if (!iter) { free(var); return NULL; }
    if (!expect(p, TOK_LBRACE, err)) { free(var); expr_free(iter); return NULL; }
    size_t count;
    Stmt **body = parse_block_stmts(p, &count, err);
    if (!body && *err) { free(var); expr_free(iter); return NULL; }
    if (!expect(p, TOK_RBRACE, err)) {
        free(var); expr_free(iter);
        for (size_t i = 0; i < count; i++) stmt_free(body[i]);
        free(body);
        return NULL;
    }
    return stmt_for(var, iter, body, count);
}

static Stmt *parse_while_stmt(Parser *p, char **err) {
    if (!expect(p, TOK_WHILE, err)) return NULL;
    Expr *cond = parse_expr(p, err);
    if (!cond) return NULL;
    if (!expect(p, TOK_LBRACE, err)) { expr_free(cond); return NULL; }
    size_t count;
    Stmt **body = parse_block_stmts(p, &count, err);
    if (!body && *err) { expr_free(cond); return NULL; }
    if (!expect(p, TOK_RBRACE, err)) {
        expr_free(cond);
        for (size_t i = 0; i < count; i++) stmt_free(body[i]);
        free(body);
        return NULL;
    }
    return stmt_while(cond, body, count);
}

static Stmt *parse_loop_stmt(Parser *p, char **err) {
    if (!expect(p, TOK_LOOP, err)) return NULL;
    if (!expect(p, TOK_LBRACE, err)) return NULL;
    size_t count;
    Stmt **body = parse_block_stmts(p, &count, err);
    if (!body && *err) return NULL;
    if (!expect(p, TOK_RBRACE, err)) {
        for (size_t i = 0; i < count; i++) stmt_free(body[i]);
        free(body);
        return NULL;
    }
    return stmt_loop(body, count);
}

/* Parse: import "path" as name
 * Parse: import { x, y } from "path" */
static Stmt *parse_import_stmt(Parser *p, char **err) {
    advance(p); /* consume 'import' */

    /* Selective import: import { name1, name2 } from "path" */
    if (peek_type(p) == TOK_LBRACE) {
        advance(p);
        size_t cap = 4, count = 0;
        char **names = malloc(cap * sizeof(char *));

        while (peek_type(p) != TOK_RBRACE && !at_eof(p)) {
            if (count >= cap) { cap *= 2; names = realloc(names, cap * sizeof(char *)); }
            names[count] = expect_ident(p, err);
            if (!names[count]) {
                for (size_t i = 0; i < count; i++) free(names[i]);
                free(names);
                return NULL;
            }
            count++;
            if (peek_type(p) == TOK_COMMA) advance(p);
        }

        if (!expect(p, TOK_RBRACE, err)) {
            for (size_t i = 0; i < count; i++) free(names[i]);
            free(names);
            return NULL;
        }
        if (!expect(p, TOK_FROM, err)) {
            for (size_t i = 0; i < count; i++) free(names[i]);
            free(names);
            return NULL;
        }

        if (peek_type(p) != TOK_STRING_LIT) {
            *err = parser_error_fmt(p, "expected string literal after 'from'");
            for (size_t i = 0; i < count; i++) free(names[i]);
            free(names);
            return NULL;
        }
        char *path = strdup(advance(p)->as.str_val);
        eat_semicolon(p);
        return stmt_import(path, NULL, names, count);
    }

    /* Full import: import "path" as name */
    if (peek_type(p) != TOK_STRING_LIT) {
        *err = parser_error_fmt(p, "expected string literal or '{' after 'import'");
        return NULL;
    }
    char *path = strdup(advance(p)->as.str_val);

    char *alias = NULL;
    if (peek_type(p) == TOK_AS) {
        advance(p);
        alias = expect_ident(p, err);
        if (!alias) { free(path); return NULL; }
    }

    eat_semicolon(p);
    return stmt_import(path, alias, NULL, 0);
}

static Stmt *parse_stmt(Parser *p, char **err) {
    TokenType tt = peek_type(p);

    if (tt == TOK_FLUX)  return parse_binding(p, PHASE_FLUID, err);
    if (tt == TOK_FIX)   return parse_binding(p, PHASE_CRYSTAL, err);
    if (tt == TOK_LET)   return parse_binding(p, PHASE_UNSPECIFIED, err);

    if (tt == TOK_IMPORT) return parse_import_stmt(p, err);

    if (tt == TOK_DEFER) {
        advance(p);
        if (!expect(p, TOK_LBRACE, err)) return NULL;
        size_t count;
        Stmt **body = parse_block_stmts(p, &count, err);
        if (!body && *err) return NULL;
        if (!expect(p, TOK_RBRACE, err)) {
            for (size_t i = 0; i < count; i++) stmt_free(body[i]);
            free(body);
            return NULL;
        }
        return stmt_defer(body, count);
    }

    if (tt == TOK_RETURN) {
        advance(p);
        if (peek_type(p) == TOK_RBRACE || peek_type(p) == TOK_SEMICOLON || at_eof(p)) {
            eat_semicolon(p);
            return stmt_return(NULL);
        }
        Expr *e = parse_expr(p, err);
        if (!e) return NULL;
        eat_semicolon(p);
        return stmt_return(e);
    }

    if (tt == TOK_FOR) return parse_for_stmt(p, err);
    if (tt == TOK_WHILE) return parse_while_stmt(p, err);
    if (tt == TOK_LOOP) return parse_loop_stmt(p, err);
    if (tt == TOK_BREAK) { advance(p); eat_semicolon(p); return stmt_break(); }
    if (tt == TOK_CONTINUE) { advance(p); eat_semicolon(p); return stmt_continue(); }

    /* Expression statement or assignment */
    Expr *e = parse_expr(p, err);
    if (!e) return NULL;
    if (peek_type(p) == TOK_EQ) {
        advance(p);
        Expr *value = parse_expr(p, err);
        if (!value) { expr_free(e); return NULL; }
        eat_semicolon(p);
        return stmt_assign(e, value);
    }
    /* Compound assignment: +=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>= */
    {
        BinOpKind cop;
        bool is_compound = true;
        switch (peek_type(p)) {
            case TOK_PLUS_EQ:    cop = BINOP_ADD; break;
            case TOK_MINUS_EQ:   cop = BINOP_SUB; break;
            case TOK_STAR_EQ:    cop = BINOP_MUL; break;
            case TOK_SLASH_EQ:   cop = BINOP_DIV; break;
            case TOK_PERCENT_EQ: cop = BINOP_MOD; break;
            case TOK_AMP_EQ:     cop = BINOP_BIT_AND; break;
            case TOK_PIPE_EQ:    cop = BINOP_BIT_OR; break;
            case TOK_CARET_EQ:   cop = BINOP_BIT_XOR; break;
            case TOK_LSHIFT_EQ:  cop = BINOP_LSHIFT; break;
            case TOK_RSHIFT_EQ:  cop = BINOP_RSHIFT; break;
            default: is_compound = false; break;
        }
        if (is_compound) {
            advance(p);
            Expr *rhs = parse_expr(p, err);
            if (!rhs) { expr_free(e); return NULL; }
            /* Desugar: target op= rhs  =>  target = target op rhs */
            Expr *target_clone = expr_clone_ast(e);
            if (!target_clone) { expr_free(rhs); expr_free(e); *err = strdup("invalid compound assignment target"); return NULL; }
            Expr *binop = expr_binop(cop, target_clone, rhs);
            eat_semicolon(p);
            return stmt_assign(e, binop);
        }
    }
    eat_semicolon(p);
    return stmt_expr(e);
}

/* ── Items ── */

static bool parse_fn_decl(Parser *p, FnDecl *out, char **err) {
    out->next_overload = NULL;
    out->contracts = NULL;
    out->contract_count = 0;
    if (!expect(p, TOK_FN, err)) return false;
    out->name = expect_ident(p, err);
    if (!out->name) return false;
    if (!expect(p, TOK_LPAREN, err)) { free(out->name); return false; }
    out->params = parse_params(p, &out->param_count, err);
    if (!out->params && *err) { free(out->name); return false; }
    if (!expect(p, TOK_RPAREN, err)) { free(out->name); free(out->params); return false; }

    out->return_type = NULL;
    if (peek_type(p) == TOK_ARROW) {
        advance(p);
        out->return_type = parse_type_expr(p, err);
        if (!out->return_type) { free(out->name); free(out->params); return false; }
    }

    /* Parse require/ensure contracts (contextual identifiers before '{') */
    {
        size_t ccap = 4, cn = 0;
        ContractClause *contracts = NULL;
        while (peek_type(p) == TOK_IDENT && peek(p)->as.str_val &&
               (strcmp(peek(p)->as.str_val, "require") == 0 ||
                strcmp(peek(p)->as.str_val, "ensure") == 0)) {
            if (!contracts) contracts = malloc(ccap * sizeof(ContractClause));
            if (cn >= ccap) { ccap *= 2; contracts = realloc(contracts, ccap * sizeof(ContractClause)); }
            bool is_ensure = (strcmp(peek(p)->as.str_val, "ensure") == 0);
            advance(p);
            Expr *cond = parse_expr(p, err);
            if (!cond) {
                for (size_t i = 0; i < cn; i++) { expr_free(contracts[i].condition); free(contracts[i].message); }
                free(contracts);
                free(out->name); free(out->params);
                if (out->return_type) { type_expr_free(out->return_type); free(out->return_type); }
                return false;
            }
            char *msg = NULL;
            if (peek_type(p) == TOK_COMMA) {
                advance(p);
                if (peek_type(p) == TOK_STRING_LIT) {
                    msg = strdup(advance(p)->as.str_val);
                }
            }
            contracts[cn].condition = cond;
            contracts[cn].message = msg;
            contracts[cn].is_ensure = is_ensure;
            cn++;
        }
        out->contracts = contracts;
        out->contract_count = cn;
    }

    if (!expect(p, TOK_LBRACE, err)) {
        free(out->name); free(out->params);
        if (out->return_type) { type_expr_free(out->return_type); free(out->return_type); }
        if (out->contracts) {
            for (size_t i = 0; i < out->contract_count; i++) { expr_free(out->contracts[i].condition); free(out->contracts[i].message); }
            free(out->contracts);
        }
        return false;
    }
    out->body = parse_block_stmts(p, &out->body_count, err);
    if (!out->body && *err) {
        free(out->name); free(out->params);
        if (out->return_type) { type_expr_free(out->return_type); free(out->return_type); }
        if (out->contracts) {
            for (size_t i = 0; i < out->contract_count; i++) { expr_free(out->contracts[i].condition); free(out->contracts[i].message); }
            free(out->contracts);
        }
        return false;
    }
    if (!expect(p, TOK_RBRACE, err)) {
        fn_decl_free(out);
        return false;
    }
    return true;
}

static bool parse_struct_decl(Parser *p, StructDecl *out, char **err) {
    if (!expect(p, TOK_STRUCT, err)) return false;
    out->name = expect_ident(p, err);
    if (!out->name) return false;
    if (!expect(p, TOK_LBRACE, err)) { free(out->name); return false; }

    size_t cap = 4;
    size_t n = 0;
    out->fields = malloc(cap * sizeof(FieldDecl));

    while (peek_type(p) != TOK_RBRACE && !at_eof(p)) {
        if (n >= cap) { cap *= 2; out->fields = realloc(out->fields, cap * sizeof(FieldDecl)); }
        out->fields[n].name = expect_ident(p, err);
        if (!out->fields[n].name) { free(out->name); free(out->fields); return false; }
        if (!expect(p, TOK_COLON, err)) {
            free(out->fields[n].name); free(out->name); free(out->fields);
            return false;
        }
        TypeExpr *te = parse_type_expr(p, err);
        if (!te) { free(out->fields[n].name); free(out->name); free(out->fields); return false; }
        out->fields[n].ty = *te;
        free(te);
        n++;
        if (peek_type(p) != TOK_RBRACE) {
            if (!expect(p, TOK_COMMA, err)) { free(out->name); free(out->fields); return false; }
        }
    }
    out->field_count = n;
    if (!expect(p, TOK_RBRACE, err)) { free(out->name); free(out->fields); return false; }
    return true;
}

/* ── Test declaration ── */

static bool parse_test_decl(Parser *p, TestDecl *out, char **err) {
    if (!expect(p, TOK_TEST, err)) return false;
    if (peek_type(p) != TOK_STRING_LIT) {
        *err = strdup("expected string literal for test name");
        return false;
    }
    Token *name_tok = advance(p);
    out->name = strdup(name_tok->as.str_val);
    if (!expect(p, TOK_LBRACE, err)) { free(out->name); return false; }
    out->body = parse_block_stmts(p, &out->body_count, err);
    if (!out->body && *err) { free(out->name); return false; }
    if (!expect(p, TOK_RBRACE, err)) {
        free(out->name);
        for (size_t i = 0; i < out->body_count; i++) stmt_free(out->body[i]);
        free(out->body);
        return false;
    }
    return true;
}

/* ── Enum declaration ── */

static bool parse_enum_decl(Parser *p, EnumDecl *out, char **err) {
    if (!expect(p, TOK_ENUM, err)) return false;
    out->name = expect_ident(p, err);
    if (!out->name) return false;
    if (!expect(p, TOK_LBRACE, err)) { free(out->name); return false; }

    size_t cap = 4;
    size_t n = 0;
    out->variants = malloc(cap * sizeof(VariantDecl));

    while (peek_type(p) != TOK_RBRACE && !at_eof(p)) {
        if (n >= cap) { cap *= 2; out->variants = realloc(out->variants, cap * sizeof(VariantDecl)); }
        out->variants[n].name = expect_ident(p, err);
        if (!out->variants[n].name) goto fail;
        out->variants[n].param_types = NULL;
        out->variants[n].param_count = 0;

        /* Tuple variant: Variant(Type1, Type2) */
        if (peek_type(p) == TOK_LPAREN) {
            advance(p);
            size_t tcap = 4, tn = 0;
            TypeExpr *types = malloc(tcap * sizeof(TypeExpr));
            while (peek_type(p) != TOK_RPAREN && !at_eof(p)) {
                if (tn >= tcap) { tcap *= 2; types = realloc(types, tcap * sizeof(TypeExpr)); }
                TypeExpr *te = parse_type_expr(p, err);
                if (!te) { free(types); free(out->variants[n].name); goto fail; }
                types[tn++] = *te;
                free(te);
                if (peek_type(p) != TOK_RPAREN) {
                    if (!expect(p, TOK_COMMA, err)) { free(types); free(out->variants[n].name); goto fail; }
                }
            }
            if (!expect(p, TOK_RPAREN, err)) { free(types); free(out->variants[n].name); goto fail; }
            out->variants[n].param_types = types;
            out->variants[n].param_count = tn;
        }
        n++;
        if (peek_type(p) != TOK_RBRACE) {
            if (peek_type(p) == TOK_COMMA) advance(p);
        }
    }
    out->variant_count = n;
    if (!expect(p, TOK_RBRACE, err)) goto fail;
    return true;

fail:
    free(out->name);
    for (size_t i = 0; i < n; i++) {
        free(out->variants[i].name);
        if (out->variants[i].param_types) {
            for (size_t j = 0; j < out->variants[i].param_count; j++)
                type_expr_free(&out->variants[i].param_types[j]);
            free(out->variants[i].param_types);
        }
    }
    free(out->variants);
    return false;
}

/* ── Program ── */

Program parser_parse(Parser *p, char **err) {
    Program prog;
    memset(&prog, 0, sizeof(prog));
    *err = NULL;

    /* Mode directive */
    if (peek_type(p) == TOK_MODE_DIRECTIVE) {
        Token *t = advance(p);
        if (strcmp(t->as.str_val, "strict") == 0)
            prog.mode = MODE_STRICT;
        else
            prog.mode = MODE_CASUAL;
    } else {
        prog.mode = MODE_CASUAL;
    }

    size_t cap = 8;
    size_t n = 0;
    prog.items = malloc(cap * sizeof(Item));

    while (!at_eof(p)) {
        if (n >= cap) {
            cap *= 2;
            prog.items = realloc(prog.items, cap * sizeof(Item));
        }

        if (peek_type(p) == TOK_FN) {
            prog.items[n].tag = ITEM_FUNCTION;
            if (!parse_fn_decl(p, &prog.items[n].as.fn_decl, err)) {
                prog.item_count = n;
                return prog;
            }
        } else if (peek_type(p) == TOK_STRUCT) {
            prog.items[n].tag = ITEM_STRUCT;
            if (!parse_struct_decl(p, &prog.items[n].as.struct_decl, err)) {
                prog.item_count = n;
                return prog;
            }
        } else if (peek_type(p) == TOK_TEST) {
            prog.items[n].tag = ITEM_TEST;
            if (!parse_test_decl(p, &prog.items[n].as.test_decl, err)) {
                prog.item_count = n;
                return prog;
            }
        } else if (peek_type(p) == TOK_ENUM) {
            prog.items[n].tag = ITEM_ENUM;
            if (!parse_enum_decl(p, &prog.items[n].as.enum_decl, err)) {
                prog.item_count = n;
                return prog;
            }
        } else {
            prog.items[n].tag = ITEM_STMT;
            prog.items[n].as.stmt = parse_stmt(p, err);
            if (!prog.items[n].as.stmt) {
                prog.item_count = n;
                return prog;
            }
        }
        n++;
    }

    prog.item_count = n;
    return prog;
}
