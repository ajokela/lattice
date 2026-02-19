#ifndef VALUE_H
#define VALUE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "ds/hashmap.h"

/* Runtime phase tag */
typedef enum { VTAG_FLUID, VTAG_CRYSTAL, VTAG_UNPHASED, VTAG_SUBLIMATED } PhaseTag;

/* Runtime value types */
typedef enum {
    VAL_INT, VAL_FLOAT, VAL_BOOL, VAL_STR, VAL_ARRAY,
    VAL_STRUCT, VAL_CLOSURE, VAL_UNIT, VAL_NIL, VAL_RANGE,
    VAL_MAP,
    VAL_CHANNEL,
    VAL_ENUM,
    VAL_SET,
    VAL_TUPLE,
    VAL_BUFFER,
    VAL_REF,
} ValueType;

/* Forward declarations */
typedef struct LatValue LatValue;
typedef struct LatRef LatRef;
typedef struct Env Env;
struct Expr;
struct LatChannel;

/* Runtime value */
struct LatValue {
    ValueType type;
    PhaseTag  phase;
    size_t    region_id;  /* Crystal region ID ((size_t)-1 = not in a region) */
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
            PhaseTag *field_phases;   /* per-field phase (NULL = all inherit struct phase) */
            size_t    field_count;
        } strct;
        struct {
            char  **param_names;
            size_t  param_count;
            struct Expr *body;     /* borrowed from AST, not owned */
            Env   *captured_env;   /* owned, deep-cloned */
            struct Expr **default_values;  /* borrowed, param_count entries, NULL for required */
            bool   has_variadic;
            void  *native_fn;     /* when non-NULL and body==NULL, native extension function */
        } closure;
        struct {
            int64_t start;
            int64_t end;
        } range;
        struct {
            LatMap *map;         /* heap-allocated */
            LatMap *key_phases;  /* per-key phase tracking (NULL = all inherit map phase) */
        } map;
        struct {
            struct LatChannel *ch;
        } channel;
        struct {
            char     *enum_name;
            char     *variant_name;
            LatValue *payload;
            size_t    payload_count;
        } enm;
        struct {
            LatMap *map;     /* heap-allocated, keys=display strings, values=LatValue */
        } set;
        struct {
            LatValue *elems;
            size_t    len;
        } tuple;
        struct {
            uint8_t *data;
            size_t   len;
            size_t   cap;
        } buffer;
        struct {
            LatRef *ref;
        } ref;
    } as;
};

/* Ref: reference-counted shared mutable wrapper */
struct LatRef {
    LatValue value;
    size_t   refcount;
};

/* ── Constructors ── */
LatValue value_int(int64_t v);
LatValue value_float(double v);
LatValue value_bool(bool v);
LatValue value_string(const char *s);
LatValue value_string_owned(char *s);
LatValue value_array(LatValue *elems, size_t len);
LatValue value_struct(const char *name, char **field_names, LatValue *field_values, size_t count);
LatValue value_closure(char **param_names, size_t param_count, struct Expr *body, Env *captured,
                       struct Expr **default_values, bool has_variadic);
LatValue value_unit(void);
LatValue value_nil(void);
LatValue value_range(int64_t start, int64_t end);
LatValue value_map_new(void);
LatValue value_channel(struct LatChannel *ch);
LatValue value_enum(const char *enum_name, const char *variant_name, LatValue *payload, size_t count);
LatValue value_set_new(void);
LatValue value_tuple(LatValue *elems, size_t len);
LatValue value_buffer(const uint8_t *data, size_t len);
LatValue value_buffer_alloc(size_t cap);
LatValue value_ref(LatValue inner);
void ref_retain(LatRef *r);
void ref_release(LatRef *r);

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
/* Returns heap-allocated repr string (strings quoted, otherwise like display) */
char *value_repr(const LatValue *v);

/* ── Type name ── */
const char *value_type_name(const LatValue *v);

/* ── Equality ── */
bool value_eq(const LatValue *a, const LatValue *b);

/* ── Heap integration ── */
struct DualHeap;
struct CrystalRegion;
void value_set_heap(struct DualHeap *heap);
void value_set_arena(struct CrystalRegion *region);
struct CrystalRegion *value_get_arena(void);

/* ── Arena-routed allocation (for use by env.c) ── */
void *lat_alloc_routed(size_t size);
void *lat_calloc_routed(size_t count, size_t size);
char *lat_strdup_routed(const char *s);

/* ── Destructor ── */
void value_free(LatValue *v);

/* ── Truthiness ── */
bool value_is_truthy(const LatValue *v);

#endif /* VALUE_H */
