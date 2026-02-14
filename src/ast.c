#include "ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Expression constructors ── */

static Expr *expr_alloc(ExprTag tag) {
    Expr *e = calloc(1, sizeof(Expr));
    e->tag = tag;
    return e;
}

Expr *expr_int_lit(int64_t val) {
    Expr *e = expr_alloc(EXPR_INT_LIT);
    e->as.int_val = val;
    return e;
}

Expr *expr_float_lit(double val) {
    Expr *e = expr_alloc(EXPR_FLOAT_LIT);
    e->as.float_val = val;
    return e;
}

Expr *expr_string_lit(char *val) {
    Expr *e = expr_alloc(EXPR_STRING_LIT);
    e->as.str_val = val;
    return e;
}

Expr *expr_bool_lit(bool val) {
    Expr *e = expr_alloc(EXPR_BOOL_LIT);
    e->as.bool_val = val;
    return e;
}

Expr *expr_nil_lit(void) {
    return expr_alloc(EXPR_NIL_LIT);
}

Expr *expr_ident(char *name) {
    Expr *e = expr_alloc(EXPR_IDENT);
    e->as.str_val = name;
    return e;
}

Expr *expr_binop(BinOpKind op, Expr *left, Expr *right) {
    Expr *e = expr_alloc(EXPR_BINOP);
    e->as.binop.op = op;
    e->as.binop.left = left;
    e->as.binop.right = right;
    return e;
}

Expr *expr_unaryop(UnaryOpKind op, Expr *operand) {
    Expr *e = expr_alloc(EXPR_UNARYOP);
    e->as.unaryop.op = op;
    e->as.unaryop.operand = operand;
    return e;
}

Expr *expr_call(Expr *func, Expr **args, size_t arg_count) {
    Expr *e = expr_alloc(EXPR_CALL);
    e->as.call.func = func;
    e->as.call.args = args;
    e->as.call.arg_count = arg_count;
    return e;
}

Expr *expr_method_call(Expr *object, char *method, Expr **args, size_t arg_count) {
    Expr *e = expr_alloc(EXPR_METHOD_CALL);
    e->as.method_call.object = object;
    e->as.method_call.method = method;
    e->as.method_call.args = args;
    e->as.method_call.arg_count = arg_count;
    return e;
}

Expr *expr_field_access(Expr *object, char *field) {
    Expr *e = expr_alloc(EXPR_FIELD_ACCESS);
    e->as.field_access.object = object;
    e->as.field_access.field = field;
    return e;
}

Expr *expr_index(Expr *object, Expr *index) {
    Expr *e = expr_alloc(EXPR_INDEX);
    e->as.index.object = object;
    e->as.index.index = index;
    return e;
}

Expr *expr_array(Expr **elems, size_t count) {
    Expr *e = expr_alloc(EXPR_ARRAY);
    e->as.array.elems = elems;
    e->as.array.count = count;
    return e;
}

Expr *expr_struct_lit(char *name, FieldInit *fields, size_t field_count) {
    Expr *e = expr_alloc(EXPR_STRUCT_LIT);
    e->as.struct_lit.name = name;
    e->as.struct_lit.fields = fields;
    e->as.struct_lit.field_count = field_count;
    return e;
}

Expr *expr_freeze(Expr *inner, Expr *contract) {
    Expr *e = expr_alloc(EXPR_FREEZE);
    e->as.freeze.expr = inner;
    e->as.freeze.contract = contract;
    return e;
}

Expr *expr_thaw(Expr *inner) {
    Expr *e = expr_alloc(EXPR_THAW);
    e->as.freeze_expr = inner;
    return e;
}

Expr *expr_clone(Expr *inner) {
    Expr *e = expr_alloc(EXPR_CLONE);
    e->as.freeze_expr = inner;
    return e;
}

Expr *expr_spread(Expr *inner) {
    Expr *e = expr_alloc(EXPR_SPREAD);
    e->as.spread_expr = inner;
    return e;
}

Expr *expr_tuple(Expr **elems, size_t count) {
    Expr *e = expr_alloc(EXPR_TUPLE);
    e->as.tuple.elems = elems;
    e->as.tuple.count = count;
    return e;
}

Expr *expr_forge(Stmt **stmts, size_t count) {
    Expr *e = expr_alloc(EXPR_FORGE);
    e->as.block.stmts = stmts;
    e->as.block.count = count;
    return e;
}

Expr *expr_if(Expr *cond, Stmt **then_s, size_t then_n, Stmt **else_s, size_t else_n) {
    Expr *e = expr_alloc(EXPR_IF);
    e->as.if_expr.cond = cond;
    e->as.if_expr.then_stmts = then_s;
    e->as.if_expr.then_count = then_n;
    e->as.if_expr.else_stmts = else_s;
    e->as.if_expr.else_count = else_n;
    return e;
}

Expr *expr_block(Stmt **stmts, size_t count) {
    Expr *e = expr_alloc(EXPR_BLOCK);
    e->as.block.stmts = stmts;
    e->as.block.count = count;
    return e;
}

Expr *expr_closure(char **params, size_t param_count, Expr *body,
                   Expr **default_values, bool has_variadic) {
    Expr *e = expr_alloc(EXPR_CLOSURE);
    e->as.closure.params = params;
    e->as.closure.param_count = param_count;
    e->as.closure.body = body;
    e->as.closure.default_values = default_values;
    e->as.closure.has_variadic = has_variadic;
    return e;
}

Expr *expr_range(Expr *start, Expr *end) {
    Expr *e = expr_alloc(EXPR_RANGE);
    e->as.range.start = start;
    e->as.range.end = end;
    return e;
}

Expr *expr_print(Expr **args, size_t arg_count) {
    Expr *e = expr_alloc(EXPR_PRINT);
    e->as.print.args = args;
    e->as.print.arg_count = arg_count;
    return e;
}

Expr *expr_spawn(Stmt **stmts, size_t count) {
    Expr *e = expr_alloc(EXPR_SPAWN);
    e->as.block.stmts = stmts;
    e->as.block.count = count;
    return e;
}

Expr *expr_scope(Stmt **stmts, size_t count) {
    Expr *e = expr_alloc(EXPR_SCOPE);
    e->as.block.stmts = stmts;
    e->as.block.count = count;
    return e;
}

Expr *expr_try_catch(Stmt **try_stmts, size_t try_count, char *catch_var,
                     Stmt **catch_stmts, size_t catch_count) {
    Expr *e = expr_alloc(EXPR_TRY_CATCH);
    e->as.try_catch.try_stmts = try_stmts;
    e->as.try_catch.try_count = try_count;
    e->as.try_catch.catch_var = catch_var;
    e->as.try_catch.catch_stmts = catch_stmts;
    e->as.try_catch.catch_count = catch_count;
    return e;
}

Expr *expr_interp_string(char **parts, Expr **exprs, size_t count) {
    Expr *e = expr_alloc(EXPR_INTERP_STRING);
    e->as.interp.parts = parts;
    e->as.interp.exprs = exprs;
    e->as.interp.count = count;
    return e;
}

Expr *expr_match(Expr *scrutinee, MatchArm *arms, size_t arm_count) {
    Expr *e = expr_alloc(EXPR_MATCH);
    e->as.match_expr.scrutinee = scrutinee;
    e->as.match_expr.arms = arms;
    e->as.match_expr.arm_count = arm_count;
    return e;
}

Expr *expr_enum_variant(char *enum_name, char *variant_name, Expr **args, size_t arg_count) {
    Expr *e = expr_alloc(EXPR_ENUM_VARIANT);
    e->as.enum_variant.enum_name = enum_name;
    e->as.enum_variant.variant_name = variant_name;
    e->as.enum_variant.args = args;
    e->as.enum_variant.arg_count = arg_count;
    return e;
}

Pattern *pattern_literal(Expr *lit) {
    Pattern *p = calloc(1, sizeof(Pattern));
    p->tag = PAT_LITERAL;
    p->as.literal = lit;
    return p;
}

Pattern *pattern_wildcard(void) {
    Pattern *p = calloc(1, sizeof(Pattern));
    p->tag = PAT_WILDCARD;
    return p;
}

Pattern *pattern_binding(char *name) {
    Pattern *p = calloc(1, sizeof(Pattern));
    p->tag = PAT_BINDING;
    p->as.binding_name = name;
    return p;
}

Pattern *pattern_range(Expr *start, Expr *end) {
    Pattern *p = calloc(1, sizeof(Pattern));
    p->tag = PAT_RANGE;
    p->as.range.start = start;
    p->as.range.end = end;
    return p;
}

void pattern_free(Pattern *p) {
    if (!p) return;
    switch (p->tag) {
        case PAT_LITERAL:
            expr_free(p->as.literal);
            break;
        case PAT_WILDCARD:
            break;
        case PAT_BINDING:
            free(p->as.binding_name);
            break;
        case PAT_RANGE:
            expr_free(p->as.range.start);
            expr_free(p->as.range.end);
            break;
    }
    free(p);
}

/* ── Clone (for lvalue AST expressions in desugaring) ── */

Expr *expr_clone_ast(const Expr *e) {
    if (!e) return NULL;
    switch (e->tag) {
        case EXPR_IDENT:
            return expr_ident(strdup(e->as.str_val));
        case EXPR_FIELD_ACCESS: {
            Expr *obj = expr_clone_ast(e->as.field_access.object);
            return expr_field_access(obj, strdup(e->as.field_access.field));
        }
        case EXPR_INDEX: {
            Expr *obj = expr_clone_ast(e->as.index.object);
            Expr *idx = expr_clone_ast(e->as.index.index);
            return expr_index(obj, idx);
        }
        case EXPR_INT_LIT:
            return expr_int_lit(e->as.int_val);
        default:
            fprintf(stderr, "expr_clone_ast: unsupported expression tag %d\n", e->tag);
            exit(1);
    }
}

/* ── Statement constructors ── */

static Stmt *stmt_alloc(StmtTag tag) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->tag = tag;
    return s;
}

Stmt *stmt_binding(AstPhase phase, char *name, TypeExpr *ty, Expr *value) {
    Stmt *s = stmt_alloc(STMT_BINDING);
    s->as.binding.phase = phase;
    s->as.binding.name = name;
    s->as.binding.ty = ty;
    s->as.binding.value = value;
    return s;
}

Stmt *stmt_assign(Expr *target, Expr *value) {
    Stmt *s = stmt_alloc(STMT_ASSIGN);
    s->as.assign.target = target;
    s->as.assign.value = value;
    return s;
}

Stmt *stmt_expr(Expr *expr) {
    Stmt *s = stmt_alloc(STMT_EXPR);
    s->as.expr = expr;
    return s;
}

Stmt *stmt_return(Expr *expr) {
    Stmt *s = stmt_alloc(STMT_RETURN);
    s->as.return_expr = expr;
    return s;
}

Stmt *stmt_for(char *var, Expr *iter, Stmt **body, size_t count) {
    Stmt *s = stmt_alloc(STMT_FOR);
    s->as.for_loop.var = var;
    s->as.for_loop.iter = iter;
    s->as.for_loop.body = body;
    s->as.for_loop.body_count = count;
    return s;
}

Stmt *stmt_while(Expr *cond, Stmt **body, size_t count) {
    Stmt *s = stmt_alloc(STMT_WHILE);
    s->as.while_loop.cond = cond;
    s->as.while_loop.body = body;
    s->as.while_loop.body_count = count;
    return s;
}

Stmt *stmt_loop(Stmt **body, size_t count) {
    Stmt *s = stmt_alloc(STMT_LOOP);
    s->as.loop.body = body;
    s->as.loop.body_count = count;
    return s;
}

Stmt *stmt_break(void) { return stmt_alloc(STMT_BREAK); }
Stmt *stmt_continue(void) { return stmt_alloc(STMT_CONTINUE); }

Stmt *stmt_destructure(AstPhase phase, DestructKind kind, char **names, size_t name_count,
                       char *rest_name, Expr *value) {
    Stmt *s = stmt_alloc(STMT_DESTRUCTURE);
    s->as.destructure.phase = phase;
    s->as.destructure.kind = kind;
    s->as.destructure.names = names;
    s->as.destructure.name_count = name_count;
    s->as.destructure.rest_name = rest_name;
    s->as.destructure.value = value;
    return s;
}

Stmt *stmt_import(char *path, char *alias, char **selective, size_t count) {
    Stmt *s = stmt_alloc(STMT_IMPORT);
    s->as.import.module_path = path;
    s->as.import.alias = alias;
    s->as.import.selective_names = selective;
    s->as.import.selective_count = count;
    return s;
}

/* ── Destructors ── */

void type_expr_free(TypeExpr *t) {
    if (!t) return;
    free(t->name);
    if (t->inner) {
        type_expr_free(t->inner);
        free(t->inner);
    }
}

void expr_free(Expr *e) {
    if (!e) return;
    switch (e->tag) {
        case EXPR_INT_LIT:
        case EXPR_FLOAT_LIT:
        case EXPR_BOOL_LIT:
        case EXPR_NIL_LIT:
            break;
        case EXPR_STRING_LIT:
        case EXPR_IDENT:
            free(e->as.str_val);
            break;
        case EXPR_BINOP:
            expr_free(e->as.binop.left);
            expr_free(e->as.binop.right);
            break;
        case EXPR_UNARYOP:
            expr_free(e->as.unaryop.operand);
            break;
        case EXPR_CALL:
            expr_free(e->as.call.func);
            for (size_t i = 0; i < e->as.call.arg_count; i++)
                expr_free(e->as.call.args[i]);
            free(e->as.call.args);
            break;
        case EXPR_METHOD_CALL:
            expr_free(e->as.method_call.object);
            free(e->as.method_call.method);
            for (size_t i = 0; i < e->as.method_call.arg_count; i++)
                expr_free(e->as.method_call.args[i]);
            free(e->as.method_call.args);
            break;
        case EXPR_FIELD_ACCESS:
            expr_free(e->as.field_access.object);
            free(e->as.field_access.field);
            break;
        case EXPR_INDEX:
            expr_free(e->as.index.object);
            expr_free(e->as.index.index);
            break;
        case EXPR_ARRAY:
            for (size_t i = 0; i < e->as.array.count; i++)
                expr_free(e->as.array.elems[i]);
            free(e->as.array.elems);
            break;
        case EXPR_STRUCT_LIT:
            free(e->as.struct_lit.name);
            for (size_t i = 0; i < e->as.struct_lit.field_count; i++) {
                free(e->as.struct_lit.fields[i].name);
                expr_free(e->as.struct_lit.fields[i].value);
            }
            free(e->as.struct_lit.fields);
            break;
        case EXPR_FREEZE:
            expr_free(e->as.freeze.expr);
            if (e->as.freeze.contract) expr_free(e->as.freeze.contract);
            break;
        case EXPR_THAW:
        case EXPR_CLONE:
            expr_free(e->as.freeze_expr);
            break;
        case EXPR_FORGE:
        case EXPR_BLOCK:
        case EXPR_SPAWN:
        case EXPR_SCOPE:
            for (size_t i = 0; i < e->as.block.count; i++)
                stmt_free(e->as.block.stmts[i]);
            free(e->as.block.stmts);
            break;
        case EXPR_IF:
            expr_free(e->as.if_expr.cond);
            for (size_t i = 0; i < e->as.if_expr.then_count; i++)
                stmt_free(e->as.if_expr.then_stmts[i]);
            free(e->as.if_expr.then_stmts);
            for (size_t i = 0; i < e->as.if_expr.else_count; i++)
                stmt_free(e->as.if_expr.else_stmts[i]);
            free(e->as.if_expr.else_stmts);
            break;
        case EXPR_CLOSURE:
            for (size_t i = 0; i < e->as.closure.param_count; i++)
                free(e->as.closure.params[i]);
            free(e->as.closure.params);
            if (e->as.closure.default_values) {
                for (size_t i = 0; i < e->as.closure.param_count; i++)
                    if (e->as.closure.default_values[i])
                        expr_free(e->as.closure.default_values[i]);
                free(e->as.closure.default_values);
            }
            expr_free(e->as.closure.body);
            break;
        case EXPR_RANGE:
            expr_free(e->as.range.start);
            expr_free(e->as.range.end);
            break;
        case EXPR_PRINT:
            for (size_t i = 0; i < e->as.print.arg_count; i++)
                expr_free(e->as.print.args[i]);
            free(e->as.print.args);
            break;
        case EXPR_TRY_CATCH:
            for (size_t i = 0; i < e->as.try_catch.try_count; i++)
                stmt_free(e->as.try_catch.try_stmts[i]);
            free(e->as.try_catch.try_stmts);
            free(e->as.try_catch.catch_var);
            for (size_t i = 0; i < e->as.try_catch.catch_count; i++)
                stmt_free(e->as.try_catch.catch_stmts[i]);
            free(e->as.try_catch.catch_stmts);
            break;
        case EXPR_INTERP_STRING:
            for (size_t i = 0; i <= e->as.interp.count; i++)
                free(e->as.interp.parts[i]);
            free(e->as.interp.parts);
            for (size_t i = 0; i < e->as.interp.count; i++)
                expr_free(e->as.interp.exprs[i]);
            free(e->as.interp.exprs);
            break;
        case EXPR_MATCH:
            expr_free(e->as.match_expr.scrutinee);
            for (size_t i = 0; i < e->as.match_expr.arm_count; i++) {
                MatchArm *arm = &e->as.match_expr.arms[i];
                pattern_free(arm->pattern);
                if (arm->guard) expr_free(arm->guard);
                for (size_t j = 0; j < arm->body_count; j++)
                    stmt_free(arm->body[j]);
                free(arm->body);
            }
            free(e->as.match_expr.arms);
            break;
        case EXPR_ENUM_VARIANT:
            free(e->as.enum_variant.enum_name);
            free(e->as.enum_variant.variant_name);
            for (size_t i = 0; i < e->as.enum_variant.arg_count; i++)
                expr_free(e->as.enum_variant.args[i]);
            free(e->as.enum_variant.args);
            break;
        case EXPR_SPREAD:
            expr_free(e->as.spread_expr);
            break;
        case EXPR_TUPLE:
            for (size_t i = 0; i < e->as.tuple.count; i++)
                expr_free(e->as.tuple.elems[i]);
            free(e->as.tuple.elems);
            break;
    }
    free(e);
}

void stmt_free(Stmt *s) {
    if (!s) return;
    switch (s->tag) {
        case STMT_BINDING:
            free(s->as.binding.name);
            if (s->as.binding.ty) {
                type_expr_free(s->as.binding.ty);
                free(s->as.binding.ty);
            }
            expr_free(s->as.binding.value);
            break;
        case STMT_ASSIGN:
            expr_free(s->as.assign.target);
            expr_free(s->as.assign.value);
            break;
        case STMT_EXPR:
            expr_free(s->as.expr);
            break;
        case STMT_RETURN:
            expr_free(s->as.return_expr);
            break;
        case STMT_FOR:
            free(s->as.for_loop.var);
            expr_free(s->as.for_loop.iter);
            for (size_t i = 0; i < s->as.for_loop.body_count; i++)
                stmt_free(s->as.for_loop.body[i]);
            free(s->as.for_loop.body);
            break;
        case STMT_WHILE:
            expr_free(s->as.while_loop.cond);
            for (size_t i = 0; i < s->as.while_loop.body_count; i++)
                stmt_free(s->as.while_loop.body[i]);
            free(s->as.while_loop.body);
            break;
        case STMT_LOOP:
            for (size_t i = 0; i < s->as.loop.body_count; i++)
                stmt_free(s->as.loop.body[i]);
            free(s->as.loop.body);
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
            break;
        case STMT_DESTRUCTURE:
            for (size_t i = 0; i < s->as.destructure.name_count; i++)
                free(s->as.destructure.names[i]);
            free(s->as.destructure.names);
            free(s->as.destructure.rest_name);
            expr_free(s->as.destructure.value);
            break;
        case STMT_IMPORT:
            free(s->as.import.module_path);
            free(s->as.import.alias);
            if (s->as.import.selective_names) {
                for (size_t i = 0; i < s->as.import.selective_count; i++)
                    free(s->as.import.selective_names[i]);
                free(s->as.import.selective_names);
            }
            break;
    }
    free(s);
}

void fn_decl_free(FnDecl *f) {
    free(f->name);
    for (size_t i = 0; i < f->param_count; i++) {
        free(f->params[i].name);
        type_expr_free(&f->params[i].ty);
        if (f->params[i].default_value)
            expr_free(f->params[i].default_value);
    }
    free(f->params);
    if (f->return_type) {
        type_expr_free(f->return_type);
        free(f->return_type);
    }
    for (size_t i = 0; i < f->body_count; i++)
        stmt_free(f->body[i]);
    free(f->body);
    f->next_overload = NULL;
}

void struct_decl_free(StructDecl *s) {
    free(s->name);
    for (size_t i = 0; i < s->field_count; i++) {
        free(s->fields[i].name);
        type_expr_free(&s->fields[i].ty);
    }
    free(s->fields);
}

void test_decl_free(TestDecl *t) {
    free(t->name);
    for (size_t i = 0; i < t->body_count; i++)
        stmt_free(t->body[i]);
    free(t->body);
}

void enum_decl_free(EnumDecl *e) {
    free(e->name);
    for (size_t i = 0; i < e->variant_count; i++) {
        free(e->variants[i].name);
        if (e->variants[i].param_types) {
            for (size_t j = 0; j < e->variants[i].param_count; j++)
                type_expr_free(&e->variants[i].param_types[j]);
            free(e->variants[i].param_types);
        }
    }
    free(e->variants);
}

void item_free(Item *item) {
    switch (item->tag) {
        case ITEM_FUNCTION:
            fn_decl_free(&item->as.fn_decl);
            break;
        case ITEM_STRUCT:
            struct_decl_free(&item->as.struct_decl);
            break;
        case ITEM_STMT:
            stmt_free(item->as.stmt);
            break;
        case ITEM_TEST:
            test_decl_free(&item->as.test_decl);
            break;
        case ITEM_ENUM:
            enum_decl_free(&item->as.enum_decl);
            break;
    }
}

void program_free(Program *p) {
    for (size_t i = 0; i < p->item_count; i++) {
        item_free(&p->items[i]);
    }
    free(p->items);
}
