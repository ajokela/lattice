#ifndef VALUE_H
#define VALUE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "ds/hashmap.h"

/* Runtime phase tag */
typedef enum { VTAG_FLUID, VTAG_CRYSTAL, VTAG_UNPHASED } PhaseTag;

/* Runtime value types */
typedef enum {
    VAL_INT, VAL_FLOAT, VAL_BOOL, VAL_STR, VAL_ARRAY,
    VAL_STRUCT, VAL_CLOSURE, VAL_UNIT, VAL_RANGE,
    VAL_MAP,
} ValueType;

/* Forward declarations */
typedef struct LatValue LatValue;
typedef struct Env Env;
struct Expr;

/* Runtime value */
struct LatValue {
    ValueType type;
    PhaseTag  phase;
    union {
        int64_t int_val;
        double  float_val;
        bool    bool_val;
        char   *str_val;     /* heap-allocated string */
        struct {
            LatValue *elems;
            size_t    len;
            size_t    cap;
        } array;
        struct {
            char     *name;
            char    **field_names;
            LatValue *field_values;
            size_t    field_count;
        } strct;
        struct {
            char  **param_names;
            size_t  param_count;
            struct Expr *body;     /* borrowed from AST, not owned */
            Env   *captured_env;   /* owned, deep-cloned */
        } closure;
        struct {
            int64_t start;
            int64_t end;
        } range;
        struct {
            LatMap *map;     /* heap-allocated */
        } map;
    } as;
};

/* ── Constructors ── */
LatValue value_int(int64_t v);
LatValue value_float(double v);
LatValue value_bool(bool v);
LatValue value_string(const char *s);
LatValue value_string_owned(char *s);
LatValue value_array(LatValue *elems, size_t len);
LatValue value_struct(const char *name, char **field_names, LatValue *field_values, size_t count);
LatValue value_closure(char **param_names, size_t param_count, struct Expr *body, Env *captured);
LatValue value_unit(void);
LatValue value_range(int64_t start, int64_t end);
LatValue value_map_new(void);

/* ── Phase helpers ── */
bool value_is_fluid(const LatValue *v);
bool value_is_crystal(const LatValue *v);

/* ── Deep operations ── */
LatValue value_deep_clone(const LatValue *v);
LatValue value_freeze(LatValue v);
LatValue value_thaw(const LatValue *v);

/* ── Display ── */
/* Writes display representation to stdout */
void value_print(const LatValue *v, FILE *out);
/* Returns heap-allocated display string */
char *value_display(const LatValue *v);

/* ── Type name ── */
const char *value_type_name(const LatValue *v);

/* ── Equality ── */
bool value_eq(const LatValue *a, const LatValue *b);

/* ── Destructor ── */
void value_free(LatValue *v);

/* ── Truthiness ── */
bool value_is_truthy(const LatValue *v);

#endif /* VALUE_H */
