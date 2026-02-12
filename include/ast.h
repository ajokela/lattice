#ifndef AST_H
#define AST_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Phase annotation */
typedef enum { PHASE_FLUID, PHASE_CRYSTAL, PHASE_UNSPECIFIED } AstPhase;

/* Execution mode */
typedef enum { MODE_CASUAL, MODE_STRICT } AstMode;

/* Binary operators */
typedef enum {
    BINOP_ADD, BINOP_SUB, BINOP_MUL, BINOP_DIV, BINOP_MOD,
    BINOP_EQ, BINOP_NEQ, BINOP_LT, BINOP_GT, BINOP_LTEQ, BINOP_GTEQ,
    BINOP_AND, BINOP_OR,
} BinOpKind;

/* Unary operators */
typedef enum { UNOP_NEG, UNOP_NOT } UnaryOpKind;

/* Type expression kind */
typedef enum { TYPE_NAMED, TYPE_ARRAY } TypeKindTag;

typedef struct TypeExpr TypeExpr;
struct TypeExpr {
    AstPhase phase;
    TypeKindTag kind;
    char *name;              /* TYPE_NAMED: type name */
    TypeExpr *inner;         /* TYPE_ARRAY: element type */
};

/* Forward declare */
typedef struct Expr Expr;
typedef struct Stmt Stmt;

/* Expression types */
typedef enum {
    EXPR_INT_LIT, EXPR_FLOAT_LIT, EXPR_STRING_LIT, EXPR_BOOL_LIT,
    EXPR_IDENT,
    EXPR_BINOP, EXPR_UNARYOP,
    EXPR_CALL, EXPR_METHOD_CALL, EXPR_FIELD_ACCESS, EXPR_INDEX,
    EXPR_ARRAY, EXPR_STRUCT_LIT,
    EXPR_FREEZE, EXPR_THAW, EXPR_CLONE,
    EXPR_FORGE, EXPR_IF, EXPR_BLOCK,
    EXPR_CLOSURE, EXPR_RANGE,
    EXPR_PRINT, EXPR_SPAWN,
    EXPR_SCOPE,
    EXPR_TRY_CATCH,
    EXPR_INTERP_STRING,
} ExprTag;

/* Statement types */
typedef enum {
    STMT_BINDING, STMT_ASSIGN, STMT_EXPR,
    STMT_RETURN, STMT_FOR, STMT_WHILE, STMT_LOOP,
    STMT_BREAK, STMT_CONTINUE,
} StmtTag;

/* Struct field in a struct literal */
typedef struct {
    char *name;
    Expr *value;
} FieldInit;

/* Expression node */
struct Expr {
    ExprTag tag;
    union {
        int64_t int_val;
        double  float_val;
        char   *str_val;     /* IDENT, STRING_LIT */
        bool    bool_val;

        struct { BinOpKind op; Expr *left; Expr *right; } binop;
        struct { UnaryOpKind op; Expr *operand; } unaryop;

        struct { Expr *func; Expr **args; size_t arg_count; } call;
        struct { Expr *object; char *method; Expr **args; size_t arg_count; } method_call;
        struct { Expr *object; char *field; } field_access;
        struct { Expr *object; Expr *index; } index;

        struct { Expr **elems; size_t count; } array;
        struct { char *name; FieldInit *fields; size_t field_count; } struct_lit;

        Expr *freeze_expr;   /* FREEZE, THAW, CLONE: inner expr */
        struct { Stmt **stmts; size_t count; } block;  /* FORGE, BLOCK, SPAWN */
        struct {
            Expr *cond;
            Stmt **then_stmts; size_t then_count;
            Stmt **else_stmts; size_t else_count;
        } if_expr;
        struct {
            char **params; size_t param_count; Expr *body;
            Expr **default_values;  /* param_count entries, NULL for required params */
            bool has_variadic;      /* last param is variadic */
        } closure;
        struct { Expr *start; Expr *end; } range;
        struct { Expr **args; size_t arg_count; } print;
        struct {
            Stmt **try_stmts; size_t try_count;
            char *catch_var;
            Stmt **catch_stmts; size_t catch_count;
        } try_catch;
        struct {
            char **parts;       /* count + 1 string segments */
            Expr **exprs;       /* count interpolated expressions */
            size_t count;       /* number of interpolated expressions */
        } interp;
    } as;
};

/* Statement node */
struct Stmt {
    StmtTag tag;
    union {
        struct {
            AstPhase phase;
            char *name;
            TypeExpr *ty;    /* nullable */
            Expr *value;
        } binding;
        struct { Expr *target; Expr *value; } assign;
        Expr *expr;          /* STMT_EXPR */
        Expr *return_expr;   /* STMT_RETURN (nullable) */
        struct {
            char *var;
            Expr *iter;
            Stmt **body; size_t body_count;
        } for_loop;
        struct {
            Expr *cond;
            Stmt **body; size_t body_count;
        } while_loop;
        struct {
            Stmt **body; size_t body_count;
        } loop;
    } as;
};

/* Function parameter */
typedef struct {
    char *name;
    TypeExpr ty;
    Expr *default_value;  /* nullable — default parameter value */
    bool is_variadic;     /* true for ...rest parameters */
} Param;

/* Function declaration */
typedef struct {
    char    *name;
    Param   *params;
    size_t   param_count;
    TypeExpr *return_type;  /* nullable */
    Stmt   **body;
    size_t   body_count;
} FnDecl;

/* Struct field declaration */
typedef struct {
    char *name;
    TypeExpr ty;
} FieldDecl;

/* Struct declaration */
typedef struct {
    char      *name;
    FieldDecl *fields;
    size_t     field_count;
} StructDecl;

/* Test declaration */
typedef struct {
    char   *name;
    Stmt  **body;
    size_t  body_count;
} TestDecl;

/* Top-level item */
typedef enum { ITEM_FUNCTION, ITEM_STRUCT, ITEM_STMT, ITEM_TEST } ItemTag;

typedef struct {
    ItemTag tag;
    union {
        FnDecl     fn_decl;
        StructDecl struct_decl;
        Stmt      *stmt;
        TestDecl   test_decl;
    } as;
} Item;

/* Program */
typedef struct {
    AstMode mode;
    Item   *items;
    size_t  item_count;
} Program;

/* ── Constructors ── */
Expr *expr_int_lit(int64_t val);
Expr *expr_float_lit(double val);
Expr *expr_string_lit(char *val);
Expr *expr_bool_lit(bool val);
Expr *expr_ident(char *name);
Expr *expr_binop(BinOpKind op, Expr *left, Expr *right);
Expr *expr_unaryop(UnaryOpKind op, Expr *operand);
Expr *expr_call(Expr *func, Expr **args, size_t arg_count);
Expr *expr_method_call(Expr *object, char *method, Expr **args, size_t arg_count);
Expr *expr_field_access(Expr *object, char *field);
Expr *expr_index(Expr *object, Expr *index);
Expr *expr_array(Expr **elems, size_t count);
Expr *expr_struct_lit(char *name, FieldInit *fields, size_t field_count);
Expr *expr_freeze(Expr *inner);
Expr *expr_thaw(Expr *inner);
Expr *expr_clone(Expr *inner);
Expr *expr_forge(Stmt **stmts, size_t count);
Expr *expr_if(Expr *cond, Stmt **then_s, size_t then_n, Stmt **else_s, size_t else_n);
Expr *expr_block(Stmt **stmts, size_t count);
Expr *expr_closure(char **params, size_t param_count, Expr *body,
                   Expr **default_values, bool has_variadic);
Expr *expr_range(Expr *start, Expr *end);
Expr *expr_print(Expr **args, size_t arg_count);
Expr *expr_spawn(Stmt **stmts, size_t count);
Expr *expr_scope(Stmt **stmts, size_t count);
Expr *expr_try_catch(Stmt **try_stmts, size_t try_count, char *catch_var,
                     Stmt **catch_stmts, size_t catch_count);
Expr *expr_interp_string(char **parts, Expr **exprs, size_t count);

Stmt *stmt_binding(AstPhase phase, char *name, TypeExpr *ty, Expr *value);
Stmt *stmt_assign(Expr *target, Expr *value);
Stmt *stmt_expr(Expr *expr);
Stmt *stmt_return(Expr *expr);
Stmt *stmt_for(char *var, Expr *iter, Stmt **body, size_t count);
Stmt *stmt_while(Expr *cond, Stmt **body, size_t count);
Stmt *stmt_loop(Stmt **body, size_t count);
Stmt *stmt_break(void);
Stmt *stmt_continue(void);

/* ── Clone (for lvalue AST expressions in desugaring) ── */
Expr *expr_clone_ast(const Expr *e);

/* ── Destructors ── */
void expr_free(Expr *e);
void stmt_free(Stmt *s);
void type_expr_free(TypeExpr *t);
void fn_decl_free(FnDecl *f);
void struct_decl_free(StructDecl *s);
void test_decl_free(TestDecl *t);
void item_free(Item *item);
void program_free(Program *p);

#endif /* AST_H */
