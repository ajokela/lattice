#ifndef VALUE_H
#define VALUE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "ds/hashmap.h"

#ifndef __EMSCRIPTEN__
#include <pthread.h> /* LatRef per-cell lock (LAT-450); winpthreads on Windows */
#endif

/* Runtime phase tag */
typedef enum { VTAG_FLUID, VTAG_CRYSTAL, VTAG_UNPHASED, VTAG_SUBLIMATED } PhaseTag;

/* Runtime value types */
typedef enum {
    VAL_INT,
    VAL_FLOAT,
    VAL_BOOL,
    VAL_STR,
    VAL_ARRAY,
    VAL_STRUCT,
    VAL_CLOSURE,
    VAL_UNIT,
    VAL_NIL,
    VAL_RANGE,
    VAL_MAP,
    VAL_CHANNEL,
    VAL_ENUM,
    VAL_SET,
    VAL_TUPLE,
    VAL_BUFFER,
    VAL_REF,
    VAL_ITERATOR,
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
    PhaseTag phase;
    size_t region_id;                 /* sentinel below, or tagged CrystalRegion* (REGION_IS_SHARED_ID) */
#define REGION_NONE      ((size_t)-1) /* normal malloc (not in any arena) */
#define REGION_EPHEMERAL ((size_t)-2) /* in ephemeral bump arena */
#define REGION_INTERNED  ((size_t)-3) /* interned string — never cloned or freed */
#define REGION_CONST     ((size_t)-4) /* constant pool string — borrowed, not freed */
    union {
        int64_t int_val;
        double float_val;
        bool bool_val;
        struct {
            char *str_val;  /* heap-allocated string */
            size_t str_len; /* cached length; 0 = unknown (recompute via strlen) */
        };
        struct {
            LatValue *elems;
            size_t len;
            size_t cap;
        } array;
        struct {
            char *name;
            char **field_names;
            LatValue *field_values;
            PhaseTag *field_phases; /* per-field phase (NULL = all inherit struct phase) */
            size_t field_count;
        } strct;
        struct {
            char **param_names;
            size_t param_count;
            struct Expr *body;            /* borrowed from AST, not owned */
            Env *captured_env;            /* owned, deep-cloned */
            struct Expr **default_values; /* borrowed, param_count entries, NULL for required */
            bool has_variadic;
            uint32_t upvalue_count; /* compiled bytecode closures: # of ObjUpvalue* in captured_env
                                     * (fits in padding before native_fn; LatValue size unchanged) */
            void *native_fn;        /* when non-NULL and body==NULL, native extension function */
        } closure;
        struct {
            int64_t start;
            int64_t end;
        } range;
        struct {
            LatMap *map;        /* heap-allocated */
            LatMap *key_phases; /* per-key phase tracking (NULL = all inherit map phase) */
        } map;
        struct {
            struct LatChannel *ch;
        } channel;
        struct {
            char *enum_name;
            char *variant_name;
            LatValue *payload;
            size_t payload_count;
        } enm;
        struct {
            LatMap *map; /* heap-allocated, keys=display strings, values=LatValue */
        } set;
        struct {
            LatValue *elems;
            size_t len;
        } tuple;
        struct {
            uint8_t *data;
            size_t len;
            size_t cap;
        } buffer;
        struct {
            LatRef *ref;
        } ref;
        struct {
            LatValue (*next_fn)(void *state, bool *done); /* C function pointer for next() */
            void *state;                                  /* opaque iterator state */
            void (*free_fn)(void *state);                 /* cleanup function */
            size_t *refcount;                             /* shared refcount for clone safety */
        } iterator;
    } as;
};

/* upvalue_count must fit in pre-existing padding: LatValue must not grow. */
#ifdef __STDC_VERSION__
#if __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(LatValue) == 72 || sizeof(void *) != 8,
               "LatValue grew — closure.upvalue_count must live in padding");
#endif
#endif

/* ── Shared crystal region handles (Crystal-by-Reference, Stage 2) ──
 *
 * A shared crystal is a VTAG_CRYSTAL value whose region_id is a low-bit-
 * tagged CrystalRegion* — its backing store lives entirely inside one
 * sealed, refcounted, process-global region and is aliased by O(1) bitwise
 * handle copy + retain. A legacy crystal (region_id == REGION_NONE) keeps
 * today's full pass-by-value clone semantics.
 *
 * Tag soundness: CrystalRegion* is malloc'd (>= 8-byte aligned), so its low
 * bit is always 0 and ptr|1 is unambiguous. The odd sentinels REGION_NONE
 * (-1) and REGION_INTERNED (-3) are excluded by name; the even sentinels
 * REGION_EPHEMERAL (-2) and REGION_CONST (-4) fail the bit test. A
 * memset-zeroed handle (region_id == 0) has the low bit clear and is never
 * classified shared, keeping same-handle double-free forgiving. The
 * predicate is alignment-based (not address-range), so it is sound on
 * wasm32 and Windows. */
typedef struct CrystalRegion CrystalRegion;
#define REGION_IS_SHARED_ID(rid) (((rid) & 1u) == 1u && (rid) != REGION_NONE && (rid) != REGION_INTERNED)
#define REGION_PTR(rid)          ((CrystalRegion *)((rid) & ~(size_t)1))
#define REGION_TAG(ptr)          (((size_t)(ptr)) | 1u)

/* Ref: reference-counted shared mutable wrapper.
 *
 * LAT-450 (CbR Stage 5): cells are shared BITWISE across threads —
 * value_clone_impl retains the same LatRef even in copy-out mode, and
 * spawn's env_clone carries cells into child evaluators/VMs — so the cell
 * is the one mutable object that legitimately crosses thread boundaries.
 * Two disciplines make that sound:
 *
 *   - refcount is atomic, with the CrystalRegion ordering contract
 *     (relaxed retain — a retain only happens while holding a live handle,
 *     so visibility piggybacks on whatever published the handle; acq_rel
 *     release + acquire fence before destruction). See include/memory.h.
 *   - `lock` (recursive) guards `value` across every compound window that
 *     reads-then-writes the cell: value_unshare + phase walk (thaw/freeze
 *     via set_phase_recursive), set()'s free+assign, the privatize+mutate
 *     method proxies, and clone-out reads (get/deref). Recursive because a
 *     cell can reach itself (r.set([r])). Cross-cell lock-order inversion
 *     (two refs mutually containing each other, thawed concurrently) is a
 *     documented limitation. Bare interior pointers handed out by lvalue
 *     resolution outlive any lock scope and are NOT covered — that
 *     pre-existing aliasing class (single-threaded too) is LAT-458. */
struct LatRef {
    LatValue value;
    _Atomic size_t refcount;
#ifndef __EMSCRIPTEN__
    pthread_mutex_t lock;
#endif
};

/* ── Constructors ── */
LatValue value_int(int64_t v);
LatValue value_float(double v);
LatValue value_bool(bool v);
LatValue value_string(const char *s);
LatValue value_string_owned(char *s);
LatValue value_string_owned_len(char *s, size_t len); /* owned string with cached length */
LatValue value_string_interned(const char *s);
LatValue value_array(LatValue *elems, size_t len);
LatValue value_struct(const char *name, char **field_names, LatValue *field_values, size_t count);
/* VM-optimized: borrows field names from const pool (single strdup, not double) */
LatValue value_struct_vm(const char *name, const char **field_names, LatValue *field_values, size_t count);
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
/* LAT-450: take/release the per-cell lock around any compound read/write of
 * r->value (no-ops on single-threaded wasm). Recursive: safe to re-enter on
 * the same cell (ref cycles through set_phase_recursive). */
void ref_lock(LatRef *r);
void ref_unlock(LatRef *r);
LatValue value_iterator(LatValue (*next_fn)(void *, bool *), void *state, void (*free_fn)(void *));

/* ── Phase helpers ── */
bool value_is_fluid(const LatValue *v);
bool value_is_crystal(const LatValue *v);

/* ── Deep operations ── */
LatValue value_deep_clone(const LatValue *v);
/* Clone walker shared by value_deep_clone/value_copy_out. allow_share=true
 * may alias a shared crystal region (retain + bitwise handle copy);
 * allow_share=false force-copies recursively. allow_share is threaded
 * through the recursion as a parameter (never TLS) so reentrant evaluation
 * cannot observe or corrupt it. */
LatValue value_clone_impl(const LatValue *v, bool allow_share);
/* True when the LATTICE_FORCE_COPY=1 differential oracle is active (borrow
 * fast paths disabled — every clone is a physical deep copy). Exposed so VM
 * clone funnels outside value.c share the SAME gate as value_clone_impl. */
bool value_clone_force_copy_active(void);
/* Recursive force-copy: guaranteed physically independent, REGION_NONE. */
LatValue value_copy_out(const LatValue *v);
/* If *v is a shared-region handle, replace it with a private force-copy and
 * release the original. Required before ANY in-place write to a possibly
 * shared handle. No-op otherwise. */
void value_unshare(LatValue *v);
/* value_unshare with the TLS heap/arena masked off (malloc-backed private
 * copy, the value_detach discipline). Required when the privatized copy
 * lands in a shared LatRef cell that outlives the calling thread. */
void value_unshare_detached(LatValue *v);
/* Cheap recursive pre-scan: false if v transitively contains a closure, Ref,
 * iterator, channel, or any sublimated member (those kinds never regionize). */
bool value_is_shareable(const LatValue *v);
/* Freeze *v in place. Shareable values (above a small size threshold)
 * materialize into a new sealed shared region and *v becomes a shared-region
 * handle (returns true). Unshareable/small values fall back to today's
 * value_freeze tag flip — legacy crystal, REGION_NONE (returns false).
 * Refreezing an already-shared crystal is an O(1) no-op (returns true). */
bool value_freeze_to_region(LatValue *v);
/* Deep-clone into thread-independent (malloc-backed) storage; see value.c. */
LatValue value_detach(const LatValue *v);
/* Returns NULL if v may be sent over a channel, else a static error message
 * (harmonized send-eligibility rule shared by all three backends). */
const char *value_send_ineligible(const LatValue *v);
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

/* ── Hash key ── */
/* Returns a deterministic string for use as a hash key.
 * For structs, this hashes only data fields (skipping closures/methods),
 * so two struct instances with the same data hash identically. */
char *value_hash_key(const LatValue *v);

/* ── Heap integration ── */
struct DualHeap;
struct CrystalRegion;
void value_set_heap(struct DualHeap *heap);
struct DualHeap *value_get_heap(void);
void value_set_arena(struct CrystalRegion *region);
struct CrystalRegion *value_get_arena(void);

/* ── Arena-routed allocation (for use by env.c) ── */
void *lat_alloc_routed(size_t size);
void *lat_calloc_routed(size_t count, size_t size);
char *lat_strdup_routed(const char *s);
/* Heap-aware realloc: avoids leaving a stale pointer in a fluid heap's tracking
 * list (which double-frees at teardown). See value.c. */
void *lat_realloc_routed(void *ptr, size_t new_size);

/* ── Destructor ── */
void value_free(LatValue *v);

/* Inline fast-path: skip function call for primitive types with no heap data */
static inline void value_free_inline(LatValue *v) {
    if (v->type <= VAL_BOOL || v->type == VAL_UNIT || v->type == VAL_NIL || v->type == VAL_RANGE) {
        v->type = VAL_NIL;
        v->region_id = REGION_NONE;
        return;
    }
    value_free(v);
}

/* ── Truthiness ── */
bool value_is_truthy(const LatValue *v);

#endif /* VALUE_H */
