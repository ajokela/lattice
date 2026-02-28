#ifndef AST_H
#define AST_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "phase.h"

/* Composite phase constraint bitmask */
typedef uint8_t PhaseConstraint;
#define PCON_FLUID      0x01
#define PCON_CRYSTAL    0x02
#define PCON_SUBLIMATED 0x04
#define PCON_ANY        0x07

/* Execution mode */
typedef enum { MODE_CASUAL, MODE_STRICT } AstMode;

/* Binary operators */
typedef enum {
    BINOP_ADD,
    BINOP_SUB,
    BINOP_MUL,
    BINOP_DIV,
    BINOP_MOD,
    BINOP_EQ,
    BINOP_NEQ,
    BINOP_LT,
    BINOP_GT,
    BINOP_LTEQ,
    BINOP_GTEQ,
    BINOP_AND,
    BINOP_OR,
    BINOP_BIT_AND,
    BINOP_BIT_OR,
    BINOP_BIT_XOR,
    BINOP_LSHIFT,
    BINOP_RSHIFT,
    BINOP_NIL_COALESCE,
} BinOpKind;

/* Unary operators */
typedef enum { UNOP_NEG, UNOP_NOT, UNOP_BIT_NOT } UnaryOpKind;

/* Type expression kind */
typedef enum { TYPE_NAMED, TYPE_ARRAY } TypeKindTag;

typedef struct TypeExpr TypeExpr;
struct TypeExpr {
    AstPhase phase;
    PhaseConstraint constraint; /* composite constraint bitmask (0 = none) */
    TypeKindTag kind;
    char *name;      /* TYPE_NAMED: type name */
    TypeExpr *inner; /* TYPE_ARRAY: element type */
};

/* Forward declare */
typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Pattern Pattern;
typedef struct FnDecl FnDecl;

/* Pattern types for match expressions */
typedef enum {
    PAT_LITERAL,  /* 0, "hello", true */
    PAT_WILDCARD, /* _ */
    PAT_BINDING,  /* x (binds value to name) */
    PAT_RANGE,    /* 1..10 */
    PAT_ARRAY,    /* [x, y, ...rest] */
    PAT_STRUCT,   /* {x: 0, y} */
} PatternTag;

/* Array pattern element */
typedef struct {
    Pattern *pattern; /* sub-pattern for this element */
    bool is_rest;     /* true if ...rest pattern */
} ArrayPatElem;

/* Struct pattern field */
typedef struct {
    char *name;         /* field name */
    Pattern *value_pat; /* nullable: if NULL, bind field value to name */
} StructPatField;

struct Pattern {
    PatternTag tag;
    AstPhase phase_qualifier; /* PHASE_UNSPECIFIED = any, PHASE_FLUID/PHASE_CRYSTAL */
    union {
        Expr *literal;      /* PAT_LITERAL */
        char *binding_name; /* PAT_BINDING */
        struct {
            Expr *start;
            Expr *end;
        } range; /* PAT_RANGE */
        struct {
            ArrayPatElem *elems;
            size_t count;
        } array; /* PAT_ARRAY */
        struct {
            StructPatField *fields;
            size_t count;
        } strct; /* PAT_STRUCT */
    } as;
};

/* Match arm: pattern [if guard] => body */
typedef struct {
    Pattern *pattern;
    Expr *guard; /* nullable */
    Stmt **body;
    size_t body_count;
} MatchArm;

/* Expression types */
typedef enum {
    EXPR_INT_LIT,
    EXPR_FLOAT_LIT,
    EXPR_STRING_LIT,
    EXPR_BOOL_LIT,
    EXPR_NIL_LIT,
    EXPR_IDENT,
    EXPR_BINOP,
    EXPR_UNARYOP,
    EXPR_CALL,
    EXPR_METHOD_CALL,
    EXPR_FIELD_ACCESS,
    EXPR_INDEX,
    EXPR_ARRAY,
    EXPR_STRUCT_LIT,
    EXPR_FREEZE,
    EXPR_THAW,
    EXPR_CLONE,
    EXPR_ANNEAL,
    EXPR_FORGE,
    EXPR_IF,
    EXPR_BLOCK,
    EXPR_CLOSURE,
    EXPR_RANGE,
    EXPR_PRINT,
    EXPR_SPAWN,
    EXPR_SCOPE,
    EXPR_TRY_CATCH,
    EXPR_INTERP_STRING,
    EXPR_MATCH,
    EXPR_ENUM_VARIANT,
    EXPR_SPREAD,
    EXPR_TUPLE,
    EXPR_CRYSTALLIZE,
    EXPR_BORROW,
    EXPR_SUBLIMATE,
    EXPR_TRY_PROPAGATE,
    EXPR_SELECT,
} ExprTag;

/* Statement types */
/* Destructure target kind */
typedef enum { DESTRUCT_ARRAY, DESTRUCT_STRUCT } DestructKind;

typedef enum {
    STMT_BINDING,
    STMT_ASSIGN,
    STMT_EXPR,
    STMT_RETURN,
    STMT_FOR,
    STMT_WHILE,
    STMT_LOOP,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_DESTRUCTURE,
    STMT_IMPORT,
    STMT_DEFER,
} StmtTag;

/* Struct field in a struct literal */
typedef struct {
    char *name;
    Expr *value;
} FieldInit;

/* Expression node */
struct Expr {
    ExprTag tag;
    int line; /* Source line number (from parser) */
    union {
        int64_t int_val;
        double float_val;
        char *str_val; /* IDENT, STRING_LIT */
        bool bool_val;

        struct {
            BinOpKind op;
            Expr *left;
            Expr *right;
        } binop;
        struct {
            UnaryOpKind op;
            Expr *operand;
        } unaryop;

        struct {
            Expr *func;
            Expr **args;
            size_t arg_count;
        } call;
        struct {
            Expr *object;
            char *method;
            Expr **args;
            size_t arg_count;
            bool optional;
        } method_call;
        struct {
            Expr *object;
            char *field;
            bool optional;
        } field_access;
        struct {
            Expr *object;
            Expr *index;
            bool optional;
        } index;

        struct {
            Expr **elems;
            size_t count;
        } array;
        struct {
            char *name;
            FieldInit *fields;
            size_t field_count;
            char *module_alias;
        } struct_lit;

        Expr *freeze_expr; /* THAW, CLONE: inner expr */
        struct {
            Expr *expr;
            Expr *contract;
            Expr **except_fields;
            size_t except_count;
        } freeze; /* FREEZE: inner expr + optional where contract + optional except */
        struct {
            Expr *expr;
            Expr *closure;
        } anneal;          /* ANNEAL: target + transform closure */
        Expr *spread_expr; /* SPREAD: inner expr to expand */
        struct {
            Expr **elems;
            size_t count;
        } tuple; /* TUPLE */
        struct {
            Expr *expr;
            Stmt **body;
            size_t body_count;
        } crystallize; /* CRYSTALLIZE */
        struct {
            Expr *expr;
            Stmt **body;
            size_t body_count;
        } borrow; /* BORROW */
        struct {
            Stmt **stmts;
            size_t count;
        } block; /* FORGE, BLOCK, SPAWN */
        struct {
            Expr *cond;
            Stmt **then_stmts;
            size_t then_count;
            Stmt **else_stmts;
            size_t else_count;
        } if_expr;
        struct {
            char **params;
            size_t param_count;
            Expr *body;
            Expr **default_values; /* param_count entries, NULL for required params */
            bool has_variadic;     /* last param is variadic */
        } closure;
        struct {
            Expr *start;
            Expr *end;
        } range;
        struct {
            Expr **args;
            size_t arg_count;
        } print;
        struct {
            Stmt **try_stmts;
            size_t try_count;
            char *catch_var;
            Stmt **catch_stmts;
            size_t catch_count;
        } try_catch;
        struct {
            char **parts; /* count + 1 string segments */
            Expr **exprs; /* count interpolated expressions */
            size_t count; /* number of interpolated expressions */
        } interp;
        struct {
            Expr *scrutinee;
            MatchArm *arms;
            size_t arm_count;
        } match_expr;
        struct {
            char *enum_name;
            char *variant_name;
            Expr **args;
            size_t arg_count;
            char *module_alias;
        } enum_variant;
        Expr *try_propagate_expr; /* EXPR_TRY_PROPAGATE: inner expr */
        struct {
            struct SelectArm *arms;
            size_t arm_count;
        } select_expr;
    } as;
};

/* Select arm for channel multiplexing */
typedef struct SelectArm {
    char *binding_name; /* variable to bind received value (nullable for default/timeout) */
    Expr *channel_expr; /* channel expression (nullable for default/timeout) */
    Stmt **body;
    size_t body_count;
    bool is_default;
    bool is_timeout;
    Expr *timeout_expr; /* timeout duration in ms (only if is_timeout) */
} SelectArm;

/* Statement node */
struct Stmt {
    StmtTag tag;
    int line; /* Source line number (from parser) */
    union {
        struct {
            AstPhase phase;
            AstPhase phase_annotation; /* @fluid/@crystal annotation (PHASE_UNSPECIFIED = none) */
            char *name;
            TypeExpr *ty; /* nullable */
            Expr *value;
        } binding;
        struct {
            Expr *target;
            Expr *value;
        } assign;
        Expr *expr;        /* STMT_EXPR */
        Expr *return_expr; /* STMT_RETURN (nullable) */
        struct {
            char *var;
            Expr *iter;
            Stmt **body;
            size_t body_count;
        } for_loop;
        struct {
            Expr *cond;
            Stmt **body;
            size_t body_count;
        } while_loop;
        struct {
            Stmt **body;
            size_t body_count;
        } loop;
        struct {
            AstPhase phase;
            DestructKind kind;
            char **names; /* variable names to bind */
            size_t name_count;
            char *rest_name; /* nullable, for ...rest in arrays */
            Expr *value;
        } destructure;
        struct {
            char *module_path;
            char *alias;            /* nullable — "as alias" name */
            char **selective_names; /* nullable — { name1, name2 } */
            size_t selective_count;
        } import;
        struct {
            Stmt **body;
            size_t body_count;
        } defer;
    } as;
};

/* Function parameter */
typedef struct {
    char *name;
    TypeExpr ty;
    Expr *default_value; /* nullable — default parameter value */
    bool is_variadic;    /* true for ...rest parameters */
} Param;

/* Contract clause for require/ensure */
typedef struct {
    Expr *condition; /* boolean expr for require, closure for ensure */
    char *message;   /* error message (nullable) */
    bool is_ensure;  /* false=require, true=ensure */
} ContractClause;

/* Function declaration */
struct FnDecl {
    char *name;
    Param *params;
    size_t param_count;
    TypeExpr *return_type;     /* nullable */
    ContractClause *contracts; /* nullable */
    size_t contract_count;
    Stmt **body;
    size_t body_count;
    FnDecl *next_overload;     /* phase-dispatch chain, NULL if none */
    AstPhase phase_annotation; /* @fluid/@crystal annotation (PHASE_UNSPECIFIED = none) */
};

/* Struct field declaration */
typedef struct {
    char *name;
    TypeExpr ty;
} FieldDecl;

/* Struct declaration */
typedef struct {
    char *name;
    FieldDecl *fields;
    size_t field_count;
} StructDecl;

/* Test declaration */
typedef struct {
    char *name;
    Stmt **body;
    size_t body_count;
} TestDecl;

/* Enum variant declaration */
typedef struct {
    char *name;
    TypeExpr *param_types; /* nullable — for tuple variants */
    size_t param_count;
} VariantDecl;

/* Enum declaration */
typedef struct {
    char *name;
    VariantDecl *variants;
    size_t variant_count;
} EnumDecl;

/* Trait method signature (no body) */
typedef struct {
    char *name;
    Param *params;
    size_t param_count;
    TypeExpr *return_type; /* nullable */
} TraitMethod;

/* Trait declaration */
typedef struct {
    char *name;
    TraitMethod *methods;
    size_t method_count;
} TraitDecl;

/* Implementation block */
typedef struct {
    char *trait_name;
    char *type_name;
    FnDecl *methods; /* array of method implementations */
    size_t method_count;
} ImplBlock;

/* Top-level item */
typedef enum { ITEM_FUNCTION, ITEM_STRUCT, ITEM_STMT, ITEM_TEST, ITEM_ENUM, ITEM_TRAIT, ITEM_IMPL } ItemTag;

typedef struct {
    ItemTag tag;
    bool exported; /* true if 'export' keyword precedes this item */
    union {
        FnDecl fn_decl;
        StructDecl struct_decl;
        Stmt *stmt;
        TestDecl test_decl;
        EnumDecl enum_decl;
        TraitDecl trait_decl;
        ImplBlock impl_block;
    } as;
} Item;

/* Program */
typedef struct {
    AstMode mode;
    Item *items;
    size_t item_count;
    char **export_names; /* NULL = export-all (legacy) */
    size_t export_count;
    size_t export_cap;
    bool has_exports; /* true if any 'export' keyword present */
} Program;

/* ── Constructors ── */
Expr *expr_int_lit(int64_t val);
Expr *expr_float_lit(double val);
Expr *expr_string_lit(char *val);
Expr *expr_bool_lit(bool val);
Expr *expr_nil_lit(void);
Expr *expr_ident(char *name);
Expr *expr_binop(BinOpKind op, Expr *left, Expr *right);
Expr *expr_unaryop(UnaryOpKind op, Expr *operand);
Expr *expr_call(Expr *func, Expr **args, size_t arg_count);
Expr *expr_method_call(Expr *object, char *method, Expr **args, size_t arg_count);
Expr *expr_field_access(Expr *object, char *field);
Expr *expr_index(Expr *object, Expr *index);
Expr *expr_array(Expr **elems, size_t count);
Expr *expr_struct_lit(char *name, FieldInit *fields, size_t field_count);
Expr *expr_freeze(Expr *inner, Expr *contract);
Expr *expr_freeze_except(Expr *inner, Expr *contract, Expr **except_fields, size_t except_count);
Expr *expr_thaw(Expr *inner);
Expr *expr_clone(Expr *inner);
Expr *expr_anneal(Expr *target, Expr *closure);
Expr *expr_forge(Stmt **stmts, size_t count);
Expr *expr_if(Expr *cond, Stmt **then_s, size_t then_n, Stmt **else_s, size_t else_n);
Expr *expr_block(Stmt **stmts, size_t count);
Expr *expr_closure(char **params, size_t param_count, Expr *body, Expr **default_values, bool has_variadic);
Expr *expr_range(Expr *start, Expr *end);
Expr *expr_print(Expr **args, size_t arg_count);
Expr *expr_spawn(Stmt **stmts, size_t count);
Expr *expr_scope(Stmt **stmts, size_t count);
Expr *expr_try_catch(Stmt **try_stmts, size_t try_count, char *catch_var, Stmt **catch_stmts, size_t catch_count);
Expr *expr_interp_string(char **parts, Expr **exprs, size_t count);
Expr *expr_match(Expr *scrutinee, MatchArm *arms, size_t arm_count);
Expr *expr_enum_variant(char *enum_name, char *variant_name, Expr **args, size_t arg_count);
Expr *expr_spread(Expr *inner);
Expr *expr_tuple(Expr **elems, size_t count);
Expr *expr_crystallize(Expr *expr, Stmt **body, size_t body_count);
Expr *expr_borrow(Expr *expr, Stmt **body, size_t body_count);
Expr *expr_sublimate(Expr *inner);
Expr *expr_try_propagate(Expr *inner);
Expr *expr_select(SelectArm *arms, size_t arm_count);

/* Pattern constructors */
Pattern *pattern_literal(Expr *lit);
Pattern *pattern_wildcard(void);
Pattern *pattern_binding(char *name);
Pattern *pattern_range(Expr *start, Expr *end);
Pattern *pattern_array(ArrayPatElem *elems, size_t count);
Pattern *pattern_struct(StructPatField *fields, size_t count);

/* Pattern destructor */
void pattern_free(Pattern *p);

Stmt *stmt_binding(AstPhase phase, char *name, TypeExpr *ty, Expr *value);
Stmt *stmt_assign(Expr *target, Expr *value);
Stmt *stmt_expr(Expr *expr);
Stmt *stmt_return(Expr *expr);
Stmt *stmt_for(char *var, Expr *iter, Stmt **body, size_t count);
Stmt *stmt_while(Expr *cond, Stmt **body, size_t count);
Stmt *stmt_loop(Stmt **body, size_t count);
Stmt *stmt_break(void);
Stmt *stmt_continue(void);
Stmt *stmt_destructure(AstPhase phase, DestructKind kind, char **names, size_t name_count, char *rest_name,
                       Expr *value);
Stmt *stmt_import(char *path, char *alias, char **selective, size_t count);
Stmt *stmt_defer(Stmt **body, size_t count);

/* ── Clone (for lvalue AST expressions in desugaring) ── */
Expr *expr_clone_ast(const Expr *e);

/* ── Destructors ── */
void expr_free(Expr *e);
void stmt_free(Stmt *s);
void type_expr_free(TypeExpr *t);
void fn_decl_free(FnDecl *f);
void struct_decl_free(StructDecl *s);
void test_decl_free(TestDecl *t);
void enum_decl_free(EnumDecl *e);
void trait_decl_free(TraitDecl *t);
void impl_block_free(ImplBlock *ib);
void item_free(Item *item);
void program_free(Program *p);

/* Check whether a name should be exported from a module.
 * If no 'export' keywords are present, all names are exported (legacy mode).
 * Otherwise, only explicitly exported names are included. */
bool module_should_export(const char *name, const char **export_names, size_t export_count, bool has_exports);

#endif /* AST_H */
