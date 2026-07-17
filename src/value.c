#include "lattice.h"
#include "value.h"
#include "env.h"
#include "memory.h"
#include "channel.h"
#include "intern.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <stdatomic.h> /* LAT-450: atomic LatRef refcount */

_Static_assert(sizeof(LatValue) <= LAT_MAP_INLINE_MAX,
               "LatValue must fit in LAT_MAP_INLINE_MAX bytes for inline hashmap storage");

/* Internal tree-walk recursion alias. Stored aliases borrow captured_env;
 * cloning one materializes a normal owning closure. */
#define WEAK_CLOSURE_PHASE ((PhaseTag)0x7e)

/* ── Heap-tracked allocation wrappers ── */

#ifdef __EMSCRIPTEN__
static DualHeap *g_heap = NULL;
static CrystalRegion *g_arena = NULL;
#else
static _Thread_local DualHeap *g_heap = NULL;
static _Thread_local CrystalRegion *g_arena = NULL;
#endif

void value_set_heap(DualHeap *heap) { g_heap = heap; }
DualHeap *value_get_heap(void) { return g_heap; }
void value_set_arena(CrystalRegion *region) { g_arena = region; }
CrystalRegion *value_get_arena(void) { return g_arena; }

/* ── Recursion-depth guard (LAT-486) ──
 *
 * value_clone_impl / value_free recurse once per level of nesting.  Runtime-
 * built data can nest arbitrarily deep (e.g.
 *   flux a = []; for i in 0..1000000 { a = [a] }
 * ) which would otherwise blow the C stack -> SIGSEGV.  VALUE_RECURSION_LIMIT
 * is far above any legitimate nesting depth (real data is rarely more than a
 * few hundred deep) yet well below the depth at which an 8 MiB C stack — the
 * size every Lattice fiber/thread is created with — overflows (each frame is
 * a few hundred bytes, so the wall is ~20k+ frames).
 *
 * Beyond the limit:
 *   - value_clone_impl truncates the clone (returns nil) instead of recursing.
 *     The original is untouched, so there is no double-free; only pathological
 *     depth loses data.  Because construction of deep data itself funnels
 *     through value_clone_impl (assignment clones), this also caps how deep a
 *     structure can grow, which in turn keeps value_free within the limit.
 *   - value_free stops descending (it still frees everything down to the
 *     limit).  TRADEOFF: a structure that somehow nests deeper than the limit
 *     leaks the tail below it — a bounded leak accepted only at a depth no
 *     real program reaches, in exchange for never crashing.  A full
 *     explicit-stack iterative free was judged too invasive to retrofit
 *     safely into this hot, central destructor.
 *
 * Thread-local so concurrent fibers each track their own depth. */
#define VALUE_RECURSION_LIMIT 10000
#ifdef __EMSCRIPTEN__
static int value_recursion_depth = 0;
#else
static _Thread_local int value_recursion_depth = 0;
#endif

/* Shared accessors for the recursion-depth guard so the VM clone paths
 * (value_clone_fast in stackvm.c, rvm_clone in regvm.c) use the SAME counter as
 * value_clone_impl/value_free here — a single deep structure cannot overflow
 * the C stack via any backend (LAT-486). value_recursion_enter() returns 0 when
 * the limit is reached (the caller must not recurse) and 1 otherwise. */
int value_recursion_enter(void) {
    if (value_recursion_depth >= VALUE_RECURSION_LIMIT) return 0;
    value_recursion_depth++;
    return 1;
}
void value_recursion_leave(void) {
    if (value_recursion_depth > 0) value_recursion_depth--;
}

/* Deep-clone a value into thread-independent (malloc-backed) storage by
 * cloning with the thread-local heap/arena temporarily detached. Force-copied
 * nodes come back with region_id REGION_NONE; a shared crystal handle (CbR
 * Stage 2) instead comes back as a retained alias keeping its tagged
 * region_id — that is equally thread-independent, since shared-region pages
 * are plain global malloc and never touch any thread's FluidHeap. Either
 * way the result survives the current thread's heap teardown and is owned by
 * whoever value_free()s it. Used when handing a value across threads (e.g.
 * through a channel), where the sender's fluid heap is freed before the
 * receiver reads the value. */
LatValue value_detach(const LatValue *v) {
    DualHeap *saved_heap = g_heap;
    CrystalRegion *saved_arena = g_arena;
    g_heap = NULL;
    g_arena = NULL;
    LatValue out = value_deep_clone(v);
    g_heap = saved_heap;
    g_arena = saved_arena;
    return out;
}

LatValue value_detach_copy(const LatValue *v) {
    DualHeap *saved_heap = g_heap;
    CrystalRegion *saved_arena = g_arena;
    g_heap = NULL;
    g_arena = NULL;
    LatValue out = value_copy_out(v);
    g_heap = saved_heap;
    g_arena = saved_arena;
    return out;
}

/* LAT-442: kinds that value_detach SHARES rather than copies (non-atomic
 * refcount on the same LatRef cell, shared captured_env, shared iterator
 * state) must never cross a channel. Walk the graph looking for them. */
static const char *find_unsendable_msg(const LatValue *v) {
    switch (v->type) {
        case VAL_REF: return "cannot send a value containing a Ref on a channel";
        case VAL_CLOSURE: return "cannot send a value containing a closure on a channel";
        case VAL_ITERATOR: return "cannot send a value containing an iterator on a channel";
        case VAL_ARRAY:
            for (size_t i = 0; i < v->as.array.len; i++) {
                const char *r = find_unsendable_msg(&v->as.array.elems[i]);
                if (r) return r;
            }
            return NULL;
        case VAL_TUPLE:
            for (size_t i = 0; i < v->as.tuple.len; i++) {
                const char *r = find_unsendable_msg(&v->as.tuple.elems[i]);
                if (r) return r;
            }
            return NULL;
        case VAL_STRUCT:
            for (size_t i = 0; i < v->as.strct.field_count; i++) {
                const char *r = find_unsendable_msg(&v->as.strct.field_values[i]);
                if (r) return r;
            }
            return NULL;
        case VAL_ENUM:
            for (size_t i = 0; i < v->as.enm.payload_count; i++) {
                const char *r = find_unsendable_msg(&v->as.enm.payload[i]);
                if (r) return r;
            }
            return NULL;
        case VAL_MAP:
            for (size_t i = 0; i < v->as.map.map->cap; i++) {
                if (v->as.map.map->entries[i].state == MAP_OCCUPIED) {
                    const char *r = find_unsendable_msg((const LatValue *)v->as.map.map->entries[i].value);
                    if (r) return r;
                }
            }
            return NULL;
        case VAL_SET:
            for (size_t i = 0; i < v->as.set.map->cap; i++) {
                if (v->as.set.map->entries[i].state == MAP_OCCUPIED) {
                    const char *r = find_unsendable_msg((const LatValue *)v->as.set.map->entries[i].value);
                    if (r) return r;
                }
            }
            return NULL;
        /* VAL_CHANNEL: channel cloning is a refcount retain that is safe to
         * share across threads, so channels-in-values are sendable. */
        default: return NULL;
    }
}

const char *value_send_ineligible(const LatValue *v) {
    bool scalar = v->type == VAL_INT || v->type == VAL_FLOAT || v->type == VAL_BOOL || v->type == VAL_UNIT;
    if (v->phase == VTAG_SUBLIMATED) return "cannot send a sublimated value on a channel";
    if (v->phase == VTAG_FLUID && !scalar) return "can only send crystal (frozen) values on a channel";
    return find_unsendable_msg(v);
}

static void *lat_alloc(size_t size) {
    if (g_arena) return arena_alloc(g_arena, size);
    if (g_heap) return fluid_alloc(g_heap->fluid, size);
    return malloc(size);
}

/* Grow a buffer in a heap-aware way. Plain realloc() on a fluid-heap-tracked
 * allocation leaves a stale pointer in the heap's allocation list (the block is
 * moved/freed by realloc), which double-frees at heap teardown. Route the
 * resize through the active fluid heap so its tracking stays consistent. */
void *lat_realloc_routed(void *ptr, size_t new_size) {
    if (g_heap && !g_arena) return fluid_realloc(g_heap->fluid, ptr, new_size);
    return realloc(ptr, new_size);
}

static void *lat_calloc(size_t count, size_t size) {
    if (count > 0 && size > SIZE_MAX / count) return NULL;
    if (g_arena) return arena_calloc(g_arena, count, size);
    if (g_heap) {
        size_t total = count * size;
        void *ptr = fluid_alloc(g_heap->fluid, total);
        memset(ptr, 0, total);
        return ptr;
    }
    return calloc(count, size);
}

static char *lat_strdup(const char *s) {
    if (g_arena) return arena_strdup(g_arena, s);
    size_t len = strlen(s) + 1;
    char *p = lat_alloc(len);
    memcpy(p, s, len);
    return p;
}

static void lat_free(void *ptr) {
    if (!ptr) return;
    if (g_arena) return; /* no-op during arena clone */
    if (g_heap && fluid_dealloc(g_heap->fluid, ptr)) return;
    free(ptr);
}

/* ── Arena-routed allocation (public, for env.c) ── */

void *lat_alloc_routed(size_t size) { return lat_alloc(size); }
void *lat_calloc_routed(size_t count, size_t size) { return lat_calloc(count, size); }
char *lat_strdup_routed(const char *s) { return lat_strdup(s); }

/* ── Constructors ── */

LatValue value_int(int64_t v) {
    LatValue val = {.type = VAL_INT, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    val.as.int_val = v;
    return val;
}

LatValue value_float(double v) {
    LatValue val = {.type = VAL_FLOAT, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    val.as.float_val = v;
    return val;
}

LatValue value_bool(bool v) {
    LatValue val = {.type = VAL_BOOL, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    val.as.bool_val = v;
    return val;
}

LatValue value_string(const char *s) {
    LatValue val = {.type = VAL_STR, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    val.as.str_val = lat_strdup(s);
    return val;
}

LatValue value_string_owned(char *s) {
    LatValue val = {.type = VAL_STR, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    val.as.str_val = s;
    return val;
}

LatValue value_string_owned_len(char *s, size_t len) {
    LatValue val = {.type = VAL_STR, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    val.as.str_val = s;
    val.as.str_len = len;
    return val;
}

LatValue value_string_interned(const char *s) {
    LatValue val = {.type = VAL_STR, .phase = VTAG_UNPHASED, .region_id = REGION_INTERNED};
    val.as.str_val = (char *)intern(s);
    return val;
}

/* MBA-1336: canonical byte-length for a String value. Uses the cached str_len when
 * present (0 = unknown), else strlen. Binary-safe callers must use this, never a bare
 * strlen, since Strings can carry an explicit length past an embedded NUL. */
size_t value_string_length(const LatValue *v) {
    if (!v || v->type != VAL_STR) return 0;
    if (v->as.str_len) return v->as.str_len;
    return v->as.str_val ? strlen(v->as.str_val) : 0;
}

/* MBA-1336: does the String contain a NUL byte within its byte length? Such values are
 * legal (JSON escapes, process stdout, buffers, sockets) but must not be routed through
 * C-string-only paths (strdup/strcmp/the intern table) that would truncate them. */
bool value_string_has_nul(const LatValue *v) {
    size_t n = value_string_length(v);
    return n > 0 && memchr(v->as.str_val, '\0', n) != NULL;
}

/* MBA-1336: length-aware lexicographic byte compare for String VALUES. strcmp stops at
 * the first NUL, so sorts/compares would disagree with the length-aware ==; this orders
 * by byte content over the full length (shorter string first on a shared prefix). */
int value_string_compare(const LatValue *a, const LatValue *b) {
    size_t la = value_string_length(a), lb = value_string_length(b);
    size_t n = la < lb ? la : lb;
    int c = n ? memcmp(a->as.str_val, b->as.str_val, n) : 0;
    if (c != 0) return c;
    return (la > lb) - (la < lb);
}

/* MBA-1336: portable byte-window search (memmem is not in C11). Empty needle matches at
 * 0 to mirror the historical strstr behavior the method libraries were built on. */
long value_bytes_find(const char *hay, size_t hay_len, const char *needle, size_t needle_len) {
    if (needle_len == 0) return 0;
    if (!hay || !needle || needle_len > hay_len) return -1;
    const char *p = hay;
    const char *end = hay + hay_len - needle_len + 1;
    while ((p = memchr(p, needle[0], (size_t)(end - p))) != NULL) {
        if (memcmp(p, needle, needle_len) == 0) return (long)(p - hay);
        p++;
    }
    return -1;
}

LatValue value_array(LatValue *elems, size_t len) {
    LatValue val = {.type = VAL_ARRAY, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    size_t cap = len < 4 ? 4 : len;
    val.as.array.elems = lat_alloc(cap * sizeof(LatValue));
    if (len > 0) memcpy(val.as.array.elems, elems, len * sizeof(LatValue));
    val.as.array.len = len;
    val.as.array.cap = cap;
    return val;
}

LatValue value_struct(const char *name, char **field_names, LatValue *field_values, size_t count) {
    LatValue val = {.type = VAL_STRUCT, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    val.as.strct.name = lat_strdup(name);
    val.as.strct.field_names = lat_alloc(count * sizeof(char *));
    val.as.strct.field_values = lat_alloc(count * sizeof(LatValue));
    val.as.strct.field_phases = NULL; /* lazy-allocated on first field freeze */
    for (size_t i = 0; i < count; i++) {
        val.as.strct.field_names[i] = (char *)intern(field_names[i]);
        val.as.strct.field_values[i] = field_values[i];
    }
    val.as.strct.field_count = count;
    return val;
}

LatValue value_struct_vm(const char *name, const char **field_names, LatValue *field_values, size_t count) {
    LatValue val = {.type = VAL_STRUCT, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    val.as.strct.name = lat_strdup(name);
    val.as.strct.field_names = lat_alloc(count * sizeof(char *));
    val.as.strct.field_values = lat_alloc(count * sizeof(LatValue));
    val.as.strct.field_phases = NULL;
    for (size_t i = 0; i < count; i++) {
        val.as.strct.field_names[i] = (char *)intern(field_names[i]);
        val.as.strct.field_values[i] = field_values[i];
    }
    val.as.strct.field_count = count;
    return val;
}

LatValue value_closure(char **param_names, size_t param_count, struct Expr *body, Env *captured,
                       struct Expr **default_values, bool has_variadic) {
    LatValue val = {.type = VAL_CLOSURE, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    val.as.closure.param_names = lat_alloc(param_count * sizeof(char *));
    for (size_t i = 0; i < param_count; i++) { val.as.closure.param_names[i] = lat_strdup(param_names[i]); }
    val.as.closure.param_count = param_count;
    val.as.closure.body = body; /* borrowed reference */
    val.as.closure.captured_env = captured;
    val.as.closure.default_values = default_values; /* borrowed from AST */
    val.as.closure.has_variadic = has_variadic;
    return val;
}

LatValue value_unit(void) { return (LatValue){.type = VAL_UNIT, .phase = VTAG_UNPHASED, .region_id = (size_t)-1}; }

LatValue value_nil(void) { return (LatValue){.type = VAL_NIL, .phase = VTAG_UNPHASED, .region_id = (size_t)-1}; }

LatValue value_range(int64_t start, int64_t end) {
    LatValue val = {.type = VAL_RANGE, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    val.as.range.start = start;
    val.as.range.end = end;
    return val;
}

LatValue value_map_new(void) {
    LatValue val = {.type = VAL_MAP, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    val.as.map.map = lat_alloc(sizeof(LatMap));
    *val.as.map.map = lat_map_new(sizeof(LatValue));
    val.as.map.key_phases = NULL; /* lazy-allocated on first key freeze */
    return val;
}

LatValue value_channel(struct LatChannel *ch) {
    LatValue val = {.type = VAL_CHANNEL, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    channel_retain(ch);
    val.as.channel.ch = ch;
    return val;
}

LatValue value_enum(const char *enum_name, const char *variant_name, LatValue *payload, size_t count) {
    LatValue val = {.type = VAL_ENUM, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    val.as.enm.enum_name = lat_strdup(enum_name);
    val.as.enm.variant_name = lat_strdup(variant_name);
    if (count > 0 && payload) {
        val.as.enm.payload = lat_alloc(count * sizeof(LatValue));
        for (size_t i = 0; i < count; i++) val.as.enm.payload[i] = value_deep_clone(&payload[i]);
        val.as.enm.payload_count = count;
    } else {
        val.as.enm.payload = NULL;
        val.as.enm.payload_count = 0;
    }
    return val;
}

LatValue value_set_new(void) {
    LatValue val = {.type = VAL_SET, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    val.as.set.map = lat_alloc(sizeof(LatMap));
    *val.as.set.map = lat_map_new(sizeof(LatValue));
    return val;
}

LatValue value_tuple(LatValue *elems, size_t len) {
    LatValue val = {.type = VAL_TUPLE, .phase = VTAG_CRYSTAL, .region_id = (size_t)-1};
    val.as.tuple.elems = lat_alloc(len * sizeof(LatValue));
    for (size_t i = 0; i < len; i++) { val.as.tuple.elems[i] = value_deep_clone(&elems[i]); }
    val.as.tuple.len = len;
    return val;
}

LatValue value_buffer(const uint8_t *data, size_t len) {
    LatValue val = {.type = VAL_BUFFER, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    size_t cap = len < 8 ? 8 : len;
    val.as.buffer.data = lat_alloc(cap);
    if (len > 0 && data) memcpy(val.as.buffer.data, data, len);
    val.as.buffer.len = len;
    val.as.buffer.cap = cap;
    return val;
}

LatValue value_buffer_alloc(size_t size) {
    LatValue val = {.type = VAL_BUFFER, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    size_t cap = size < 8 ? 8 : size;
    val.as.buffer.data = lat_calloc(cap, 1);
    val.as.buffer.len = size;
    val.as.buffer.cap = cap;
    return val;
}

LatValue value_iterator(LatValue (*next_fn)(void *, bool *), void *state, void (*free_fn)(void *)) {
    LatValue val = {.type = VAL_ITERATOR, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    val.as.iterator.next_fn = next_fn;
    val.as.iterator.state = state;
    val.as.iterator.free_fn = free_fn;
    val.as.iterator.refcount = malloc(sizeof(size_t));
    if (val.as.iterator.refcount) *val.as.iterator.refcount = 1;
    return val;
}

LatValue value_ref(LatValue inner) {
    LatValue val = {.type = VAL_REF, .phase = VTAG_UNPHASED, .region_id = (size_t)-1};
    LatRef *r = malloc(sizeof(LatRef));
    if (!r) return value_nil();
    /* LAT-460: the cell is shared bitwise across spawned evaluators and outlives
     * any one thread, so its inner must be detached (malloc-backed, untracked by
     * any thread's DualHeap) — exactly as Ref.set / map-set already store
     * (Stage 5). A heap-tracked clone here is the residual free-direction
     * double-free: a spawned writer frees this original inner, but the creator's
     * DualHeap still tracks the pointer and re-frees it at teardown. */
    r->value = value_detach(&inner);
    atomic_init(&r->refcount, 1);
#ifndef __EMSCRIPTEN__
    /* LAT-450: recursive — set_phase_recursive can re-enter the same cell
     * through a ref cycle (r.set([r])). */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&r->lock, &attr);
    pthread_mutexattr_destroy(&attr);
#endif
    val.as.ref.ref = r;
    return val;
}

/* LAT-450 (CbR Stage 5): cells cross threads via spawn env_clone, so the
 * count is atomic with the CrystalRegion memory-ordering contract — relaxed
 * retain (a retain only happens while holding a live handle; publication of
 * the handle — pthread_create, channel mutex — provides the visibility
 * edge), acq_rel release + acquire fence before destruction. See
 * include/memory.h "Atomics / memory-ordering contract". */
void ref_retain(LatRef *r) {
    if (r) atomic_fetch_add_explicit(&r->refcount, 1, memory_order_relaxed);
}

void ref_release(LatRef *r) {
    if (!r) return;
    if (atomic_fetch_sub_explicit(&r->refcount, 1, memory_order_acq_rel) == 1) {
        atomic_thread_fence(memory_order_acquire);
#ifndef __EMSCRIPTEN__
        pthread_mutex_destroy(&r->lock);
#endif
        value_free(&r->value);
        free(r);
    }
}

void ref_lock(LatRef *r) {
#ifndef __EMSCRIPTEN__
    pthread_mutex_lock(&r->lock);
#else
    (void)r;
#endif
}

void ref_unlock(LatRef *r) {
#ifndef __EMSCRIPTEN__
    pthread_mutex_unlock(&r->lock);
#else
    (void)r;
#endif
}

/* ── Phase helpers ── */

bool value_is_fluid(const LatValue *v) { return v->phase == VTAG_FLUID; }
bool value_is_crystal(const LatValue *v) { return v->phase == VTAG_CRYSTAL; }

/* ── Deep clone ── */

/* CbR Stage 2 (Round B): shared-region handles are classified by the
 * region-id tag alone plus phase == VTAG_CRYSTAL (a phase-anomalous handle
 * copies safely rather than aliases — H11). The Round A dormancy gate
 * (crystal_region_shared_active) is gone: the tree-walker no longer mints
 * numeric region ids, so any id satisfying REGION_IS_SHARED_ID is a genuine
 * tagged CrystalRegion pointer. */
static inline bool value_region_is_shared(const LatValue *v) { return REGION_IS_SHARED_ID(v->region_id); }

/* LATTICE_FORCE_COPY=1 differential oracle (design H16 / C2 detection net):
 * disables the borrow fast path so every clone is a physical deep copy.
 * Running the full suite under it and diffing outputs bit-for-bit flushes
 * out any site that depends on aliasing where it shouldn't (or vice versa).
 * Read once into a static — the env cannot change mid-process. */
static bool clone_force_copy(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("LATTICE_FORCE_COPY");
        mode = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return mode == 1;
}

bool value_clone_force_copy_active(void) { return clone_force_copy(); }

/* CbR Stage 3: primitives are EXCLUDED from the borrow fast path. Interior
 * scalars inside a region carry the shared tag (region_tag_recursive stamps
 * every node), but a borrowed scalar would be an rc liability the VMs cannot
 * honor — value_free_inline (stackvm/regvm hot paths) never releases
 * primitive types, and the *_INT fast opcodes abandon popped slots without
 * any free. Copying a scalar with region_id = REGION_NONE is cheaper than
 * the rc traffic anyway and preserves the system-wide invariant that scalar
 * handles own nothing (mirrors value_worth_regionizing's scalar exemption).
 * rc-neutral for the tree-walker: no retain taken, and value_free of a
 * REGION_NONE scalar releases nothing. */
static inline bool value_type_is_primitive(ValueType t) {
    return t == VAL_INT || t == VAL_FLOAT || t == VAL_BOOL || t == VAL_UNIT || t == VAL_NIL || t == VAL_RANGE;
}

LatValue value_clone_impl(const LatValue *v, bool allow_share) {
    /* Borrow fast path: aliasing a shared crystal is retain + bitwise copy. */
    if (allow_share && !clone_force_copy() && v->phase == VTAG_CRYSTAL && value_region_is_shared(v) &&
        !value_type_is_primitive(v->type)) {
        crystal_region_retain(REGION_PTR(v->region_id));
        return *v; /* bitwise handle copy */
    }

    /* LAT-486: cap recursion depth so pathologically deep data truncates
     * rather than overflowing the C stack.  The check precedes the depth
     * bump, and the borrow fast path above returns without bumping, so every
     * increment is balanced by the decrement before the single return. */
    if (value_recursion_depth >= VALUE_RECURSION_LIMIT) return value_nil();
    value_recursion_depth++;

    LatValue out = {.type = v->type, .phase = v->phase, .region_id = (size_t)-1};

    switch (v->type) {
        case VAL_INT: out.as.int_val = v->as.int_val; break;
        case VAL_FLOAT: out.as.float_val = v->as.float_val; break;
        case VAL_BOOL: out.as.bool_val = v->as.bool_val; break;
        case VAL_STR:
            if (v->region_id == REGION_INTERNED) {
                out.as.str_val = v->as.str_val;
                out.region_id = REGION_INTERNED;
            } else {
                size_t len = v->as.str_len ? v->as.str_len : strlen(v->as.str_val);
                out.as.str_val = lat_alloc(len + 1);
                memcpy(out.as.str_val, v->as.str_val, len);
                out.as.str_val[len] = '\0';
                out.as.str_len = len;
            }
            break;
        case VAL_ARRAY: {
            size_t len = v->as.array.len;
            size_t cap = v->as.array.cap;
            out.as.array.elems = lat_alloc(cap * sizeof(LatValue));
            for (size_t i = 0; i < len; i++) {
                out.as.array.elems[i] = value_clone_impl(&v->as.array.elems[i], allow_share);
            }
            out.as.array.len = len;
            out.as.array.cap = cap;
            break;
        }
        case VAL_STRUCT: {
            size_t fc = v->as.strct.field_count;
            out.as.strct.name = lat_strdup(v->as.strct.name);
            out.as.strct.field_names = lat_alloc(fc * sizeof(char *));
            out.as.strct.field_values = lat_alloc(fc * sizeof(LatValue));
            for (size_t i = 0; i < fc; i++) {
                out.as.strct.field_names[i] = (char *)intern(v->as.strct.field_names[i]);
                out.as.strct.field_values[i] = value_clone_impl(&v->as.strct.field_values[i], allow_share);
            }
            out.as.strct.field_count = fc;
            if (v->as.strct.field_phases) {
                out.as.strct.field_phases = lat_alloc(fc * sizeof(PhaseTag));
                memcpy(out.as.strct.field_phases, v->as.strct.field_phases, fc * sizeof(PhaseTag));
            } else {
                out.as.strct.field_phases = NULL;
            }
            break;
        }
        case VAL_CLOSURE: {
            bool weak_alias = v->phase == WEAK_CLOSURE_PHASE;
            size_t pc = v->as.closure.param_count;
            if (v->as.closure.param_names) {
                out.as.closure.param_names = lat_alloc(pc * sizeof(char *));
                for (size_t i = 0; i < pc; i++) {
                    out.as.closure.param_names[i] = lat_strdup(v->as.closure.param_names[i]);
                }
            } else {
                out.as.closure.param_names = NULL;
            }
            out.as.closure.param_count = pc;
            out.as.closure.body = v->as.closure.body; /* borrowed */
            /* Compiled bytecode closures store ObjUpvalue** in captured_env (not Env*).
             * Shallow-copy the pointer; the VM manages upvalue lifetime. */
            if (v->as.closure.body == NULL && v->as.closure.native_fn != NULL) {
                out.as.closure.captured_env = v->as.closure.captured_env;
            } else {
                /* Tree-walk closures: share the env (refcounted) so mutations
                 * inside the closure persist across calls.
                 * Exception: when an arena is active (during freeze), we must
                 * clone the env into the arena so GC can safely skip crystal
                 * values without traversing their fluid-heap env pointers. */
                if (weak_alias) {
                    out.as.closure.captured_env = v->as.closure.captured_env;
                    env_retain(v->as.closure.captured_env);
                } else if (value_get_arena()) {
                    out.as.closure.captured_env = env_clone(v->as.closure.captured_env);
                } else {
                    out.as.closure.captured_env = v->as.closure.captured_env;
                    env_retain(v->as.closure.captured_env);
                }
            }
            out.as.closure.default_values = v->as.closure.default_values; /* borrowed */
            out.as.closure.has_variadic = v->as.closure.has_variadic;
            out.as.closure.upvalue_count = v->as.closure.upvalue_count;
            out.as.closure.native_fn = v->as.closure.native_fn; /* shared, not owned */
            if (weak_alias) {
                out.phase = (PhaseTag)v->as.closure.upvalue_count;
                out.as.closure.upvalue_count = 0;
            }
            break;
        }
        case VAL_UNIT: break;
        case VAL_NIL: break;
        case VAL_RANGE:
            out.as.range.start = v->as.range.start;
            out.as.range.end = v->as.range.end;
            break;
        case VAL_CHANNEL:
            channel_retain(v->as.channel.ch);
            out.as.channel.ch = v->as.channel.ch;
            break;
        case VAL_ENUM: {
            out.as.enm.enum_name = lat_strdup(v->as.enm.enum_name);
            out.as.enm.variant_name = lat_strdup(v->as.enm.variant_name);
            if (v->as.enm.payload_count > 0) {
                out.as.enm.payload = lat_alloc(v->as.enm.payload_count * sizeof(LatValue));
                for (size_t i = 0; i < v->as.enm.payload_count; i++)
                    out.as.enm.payload[i] = value_clone_impl(&v->as.enm.payload[i], allow_share);
                out.as.enm.payload_count = v->as.enm.payload_count;
            } else {
                out.as.enm.payload = NULL;
                out.as.enm.payload_count = 0;
            }
            break;
        }
        case VAL_MAP: {
            LatMap *src = v->as.map.map;
            if (src->cmi && !g_arena && !g_heap && !v->as.map.key_phases) {
                /* Crystallized map (stack VM path: plain malloc territory):
                 * clone the whole CMI block with one memcpy + pointer rebase,
                 * then deep-clone each dense entry's value in place. Preserves
                 * the optimized layout across clones (pass-by-value, write-
                 * backs). Guarded off the tree-walker heap/arena paths so GC
                 * tracking assumptions there are untouched. */
                LatMapEntry *dense = NULL;
                CrystalMapHeader *blk = lat_map_cmi_clone_block(src, &dense);
                if (blk) {
                    LatMap *dst = malloc(sizeof(LatMap));
                    if (!dst) {
                        free(blk);
                    } else {
                        *dst = *src;
                        dst->cmi = blk;
                        dst->entries = dense;
                        for (uint64_t i = 0; i < blk->n; i++) {
                            LatValue tmp = *(LatValue *)dense[i].value;
                            *(LatValue *)dense[i].value = value_clone_impl(&tmp, allow_share);
                        }
                        out.as.map.map = dst;
                        out.as.map.key_phases = NULL;
                        break;
                    }
                }
                /* alloc failure: fall through to the rebuild path below */
            }
            if (g_arena) {
                /* Arena mode: build map internals through lat_alloc/lat_calloc
                 * so everything goes into the arena. No rehashing possible. */
                LatMap *dst = lat_alloc(sizeof(LatMap));
                dst->value_size = src->value_size;
                dst->cap = src->cap;
                dst->count = src->count; /* preserve tombstone count for probe chains */
                dst->live = src->live;
                dst->cmi = NULL; /* arena clones always use the sparse layout */
                dst->entries = lat_calloc(src->cap, sizeof(LatMapEntry));
                for (size_t i = 0; i < src->cap; i++) {
                    dst->entries[i].value = dst->entries[i]._ibuf;
                    if (src->entries[i].state == MAP_OCCUPIED) {
                        dst->entries[i].state = MAP_OCCUPIED;
                        dst->entries[i].key = lat_strdup(src->entries[i].key);
                        LatValue *sv = (LatValue *)src->entries[i].value;
                        LatValue cloned = value_clone_impl(sv, allow_share);
                        *(LatValue *)dst->entries[i].value = cloned;
                    } else if (src->entries[i].state == MAP_TOMBSTONE) {
                        dst->entries[i].state = MAP_TOMBSTONE;
                    }
                }
                out.as.map.map = dst;
            } else {
                /* Normal path: lat_map_new + lat_map_set */
                out.as.map.map = lat_alloc(sizeof(LatMap));
                *out.as.map.map = lat_map_new(sizeof(LatValue));
                for (size_t i = 0; i < src->cap; i++) {
                    if (src->entries[i].state == MAP_OCCUPIED) {
                        LatValue *sv = (LatValue *)src->entries[i].value;
                        LatValue cloned = value_clone_impl(sv, allow_share);
                        lat_map_set(out.as.map.map, src->entries[i].key, &cloned);
                    }
                }
            }
            /* Clone per-key phases if present */
            if (v->as.map.key_phases) {
                LatMap *ksrc = v->as.map.key_phases;
                out.as.map.key_phases = lat_alloc(sizeof(LatMap));
                *out.as.map.key_phases = lat_map_new(sizeof(PhaseTag));
                for (size_t i = 0; i < ksrc->cap; i++) {
                    if (ksrc->entries[i].state == MAP_OCCUPIED) {
                        lat_map_set(out.as.map.key_phases, ksrc->entries[i].key, ksrc->entries[i].value);
                    }
                }
            } else {
                out.as.map.key_phases = NULL;
            }
            break;
        }
        case VAL_SET: {
            LatMap *src = v->as.set.map;
            if (g_arena) {
                LatMap *dst = lat_alloc(sizeof(LatMap));
                dst->value_size = src->value_size;
                dst->cap = src->cap;
                dst->count = src->count; /* preserve tombstone count for probe chains */
                dst->live = src->live;
                dst->cmi = NULL; /* sets are never crystallized in v1 */
                dst->entries = lat_calloc(src->cap, sizeof(LatMapEntry));
                for (size_t i = 0; i < src->cap; i++) {
                    dst->entries[i].value = dst->entries[i]._ibuf;
                    if (src->entries[i].state == MAP_OCCUPIED) {
                        dst->entries[i].state = MAP_OCCUPIED;
                        dst->entries[i].key = lat_strdup(src->entries[i].key);
                        LatValue *sv = (LatValue *)src->entries[i].value;
                        LatValue cloned = value_clone_impl(sv, allow_share);
                        *(LatValue *)dst->entries[i].value = cloned;
                    } else if (src->entries[i].state == MAP_TOMBSTONE) {
                        dst->entries[i].state = MAP_TOMBSTONE;
                    }
                }
                out.as.set.map = dst;
            } else {
                out.as.set.map = lat_alloc(sizeof(LatMap));
                *out.as.set.map = lat_map_new(sizeof(LatValue));
                for (size_t i = 0; i < src->cap; i++) {
                    if (src->entries[i].state == MAP_OCCUPIED) {
                        LatValue *sv = (LatValue *)src->entries[i].value;
                        LatValue cloned = value_clone_impl(sv, allow_share);
                        lat_map_set(out.as.set.map, src->entries[i].key, &cloned);
                    }
                }
            }
            break;
        }
        case VAL_TUPLE: {
            out.as.tuple.elems = lat_alloc(v->as.tuple.len * sizeof(LatValue));
            for (size_t i = 0; i < v->as.tuple.len; i++) {
                out.as.tuple.elems[i] = value_clone_impl(&v->as.tuple.elems[i], allow_share);
            }
            out.as.tuple.len = v->as.tuple.len;
            break;
        }
        case VAL_BUFFER: {
            size_t cap = v->as.buffer.cap;
            out.as.buffer.data = lat_alloc(cap);
            memcpy(out.as.buffer.data, v->as.buffer.data, v->as.buffer.len);
            out.as.buffer.len = v->as.buffer.len;
            out.as.buffer.cap = cap;
            break;
        }
        case VAL_REF:
            ref_retain(v->as.ref.ref);
            out.as.ref.ref = v->as.ref.ref;
            break;
        case VAL_ITERATOR:
            /* Shared refcount: shallow copy, bump refcount */
            out.as.iterator.next_fn = v->as.iterator.next_fn;
            out.as.iterator.state = v->as.iterator.state;
            out.as.iterator.free_fn = v->as.iterator.free_fn;
            out.as.iterator.refcount = v->as.iterator.refcount;
            if (out.as.iterator.refcount) (*out.as.iterator.refcount)++;
            break;
    }
    value_recursion_depth--;
    return out;
}

LatValue value_deep_clone(const LatValue *v) { return value_clone_impl(v, true); }
LatValue value_copy_out(const LatValue *v) { return value_clone_impl(v, false); } /* recursive force-copy */

/* CbR Stage 2: privatize a possibly-shared handle before an in-place write.
 * Keyed on value_region_is_shared (the region-id tag alone since Round B):
 * a tagged handle gets a private force-copy and drops its region
 * reference; anything else is left untouched. */
void value_unshare(LatValue *v) {
    if (value_region_is_shared(v)) {
        LatValue priv = value_copy_out(v);
        crystal_region_release(REGION_PTR(v->region_id));
        *v = priv;
    }
}

/* LAT-450 (Stage 5): value_unshare into thread-independent (malloc-backed)
 * storage — the TLS heap/arena are masked off for the private copy, the
 * value_detach discipline. Required whenever the privatized copy lands in a
 * shared LatRef cell that outlives the calling thread (spawn children free
 * their DualHeap/arena at join, so a heap-tracked private copy would dangle
 * inside the cell). Used by set_phase_recursive's VAL_REF branch and every
 * ref-proxy unshare-before-write in the VMs. */
void value_unshare_detached(LatValue *v) {
    if (!value_region_is_shared(v)) return;
    DualHeap *saved_heap = g_heap;
    CrystalRegion *saved_arena = g_arena;
    g_heap = NULL;
    g_arena = NULL;
    value_unshare(v);
    g_heap = saved_heap;
    g_arena = saved_arena;
}

/* ── Freeze ── */

static void val_dealloc(LatValue *v, void *ptr); /* defined in the Free section below */

static void set_phase_recursive(LatValue *v, PhaseTag phase) {
    /* CbR master invariant: shared region memory — including the phase tags
     * stored inside it — is NEVER written after seal. A shared handle is
     * already uniformly crystal, so re-freezing it is a no-op; any other
     * phase write must value_unshare() first (debug assert, H9: no FLUID
     * value ever carries a shared region_id). */
    if (value_region_is_shared(v)) {
        assert(phase == VTAG_CRYSTAL);
        return;
    }
    v->phase = phase;
    if (v->type == VAL_ARRAY) {
        for (size_t i = 0; i < v->as.array.len; i++) { set_phase_recursive(&v->as.array.elems[i], phase); }
    } else if (v->type == VAL_STRUCT) {
        for (size_t i = 0; i < v->as.strct.field_count; i++) {
            set_phase_recursive(&v->as.strct.field_values[i], phase);
        }
        /* Update field-level phases */
        if (v->as.strct.field_phases) {
            if (phase == VTAG_CRYSTAL) {
                for (size_t i = 0; i < v->as.strct.field_count; i++) v->as.strct.field_phases[i] = VTAG_CRYSTAL;
            } else {
                /* Thawing: clear per-field phases */
                lat_free(v->as.strct.field_phases);
                v->as.strct.field_phases = NULL;
            }
        }
    } else if (v->type == VAL_MAP) {
        for (size_t i = 0; i < v->as.map.map->cap; i++) {
            if (v->as.map.map->entries[i].state == MAP_OCCUPIED) {
                LatValue *mv = (LatValue *)v->as.map.map->entries[i].value;
                set_phase_recursive(mv, phase);
            }
        }
        /* LAT-441: a full freeze/thaw overrides any earlier freeze-except
         * holes — drop per-key phase tracking so all keys inherit the
         * parent phase (NULL key_phases == inherit). */
        if (v->as.map.key_phases) {
            lat_map_free(v->as.map.key_phases);
            val_dealloc(v, v->as.map.key_phases);
            v->as.map.key_phases = NULL;
        }
    } else if (v->type == VAL_ENUM) {
        for (size_t i = 0; i < v->as.enm.payload_count; i++) set_phase_recursive(&v->as.enm.payload[i], phase);
    } else if (v->type == VAL_SET) {
        for (size_t i = 0; i < v->as.set.map->cap; i++) {
            if (v->as.set.map->entries[i].state == MAP_OCCUPIED) {
                LatValue *sv = (LatValue *)v->as.set.map->entries[i].value;
                set_phase_recursive(sv, phase);
            }
        }
    } else if (v->type == VAL_TUPLE) {
        for (size_t i = 0; i < v->as.tuple.len; i++) { set_phase_recursive(&v->as.tuple.elems[i], phase); }
    }
    /* VAL_BUFFER: just set phase tag (no nested values) */
    else if (v->type == VAL_REF) {
        /* Refs are SHARED cells (value_clone_impl retains the same LatRef
         * even in copy-out mode), so a thaw can reach a shared crystal
         * handle stored inside one. Privatize it first: the inner handle
         * must not be tag-flipped fluid over sealed region memory (H9).
         *
         * LAT-450 (Stage 5): the cell crosses threads via spawn env_clone,
         * and value_unshare is a non-atomic check/copy/release/store window
         * — two concurrent thaws of containers holding the same cell
         * double-released the cell's single region retain and tore the
         * write. The per-cell lock serializes the whole unshare + phase
         * walk (recursive mutex, so ref cycles re-enter safely). Bare
         * interior pointers from lvalue resolution are NOT covered: that
         * pre-existing aliasing class is LAT-458. */
        LatRef *cell = v->as.ref.ref;
        ref_lock(cell);
        if (phase != VTAG_CRYSTAL && REGION_IS_SHARED_ID(cell->value.region_id)) {
            /* Privatize DETACHED (malloc-backed): the cell outlives the
             * thawing thread (spawn children free their FluidHeap/arena at
             * join), so the private copy must never land on thread-local
             * storage. */
            value_unshare_detached(&cell->value);
        }
        set_phase_recursive(&cell->value, phase);
        ref_unlock(cell);
    }
}

LatValue value_freeze(LatValue v) {
    set_phase_recursive(&v, VTAG_CRYSTAL);
    return v;
}

/* ── Freeze to shared region (Crystal-by-Reference, Stage 2) ──
 *
 * Live since Round B: the tree-walker's whole-binding freeze sites all
 * funnel through value_freeze_to_region (eval.c freeze_to_region helper). */

/* Cheap recursive pre-scan: a value is UNSHAREABLE if it transitively
 * contains a closure (either flavor), Ref, iterator, channel, or any
 * sublimated member. Those kinds carry non-atomic foreign refcounts or
 * thread-confined state, so excluding them is what keeps shared regions
 * pure data (and the O(1) page free sound). REGION_EPHEMERAL-backed data is
 * NOT a rejection — it force-copies into the region during
 * materialization. */
bool value_is_shareable(const LatValue *v) {
    if (v->phase == VTAG_SUBLIMATED) return false;
    switch (v->type) {
        case VAL_CLOSURE:
        case VAL_REF:
        case VAL_ITERATOR:
        case VAL_CHANNEL: return false;
        case VAL_ARRAY:
            for (size_t i = 0; i < v->as.array.len; i++) {
                if (!value_is_shareable(&v->as.array.elems[i])) return false;
            }
            return true;
        case VAL_TUPLE:
            for (size_t i = 0; i < v->as.tuple.len; i++) {
                if (!value_is_shareable(&v->as.tuple.elems[i])) return false;
            }
            return true;
        case VAL_STRUCT:
            for (size_t i = 0; i < v->as.strct.field_count; i++) {
                if (!value_is_shareable(&v->as.strct.field_values[i])) return false;
            }
            return true;
        case VAL_ENUM:
            for (size_t i = 0; i < v->as.enm.payload_count; i++) {
                if (!value_is_shareable(&v->as.enm.payload[i])) return false;
            }
            return true;
        case VAL_MAP:
            for (size_t i = 0; i < v->as.map.map->cap; i++) {
                if (v->as.map.map->entries[i].state == MAP_OCCUPIED &&
                    !value_is_shareable((const LatValue *)v->as.map.map->entries[i].value))
                    return false;
            }
            return true;
        case VAL_SET:
            for (size_t i = 0; i < v->as.set.map->cap; i++) {
                if (v->as.set.map->entries[i].state == MAP_OCCUPIED &&
                    !value_is_shareable((const LatValue *)v->as.set.map->entries[i].value))
                    return false;
            }
            return true;
        default: return true; /* INT, FLOAT, BOOL, UNIT, NIL, RANGE, STR, BUFFER */
    }
}

/* Regionization size threshold (design §2.8 item 2): scalars and short
 * strings skip regionization and stay legacy crystals — copying them is
 * cheaper than refcount traffic. */
#define REGION_SHARE_MIN_STR_LEN 32

static bool value_worth_regionizing(const LatValue *v) {
    switch (v->type) {
        case VAL_INT:
        case VAL_FLOAT:
        case VAL_BOOL:
        case VAL_UNIT:
        case VAL_NIL:
        case VAL_RANGE: return false;
        case VAL_STR: {
            if (!v->as.str_val) return false;
            size_t len = v->as.str_len ? v->as.str_len : strlen(v->as.str_val);
            return len >= REGION_SHARE_MIN_STR_LEN;
        }
        default: return true;
    }
}

/* Rewritten region tagger (replaces eval.c's set_region_id_recursive for the
 * shared path, fixing H8). Runs after materialization, BEFORE seal. Stamps
 * the tagged region id and a uniform VTAG_CRYSTAL phase onto every node,
 * covering arrays, structs, maps, sets, enum payloads, tuples (the case the
 * old tagger was missing), buffers and strings, and normalizes stale phase
 * metadata (key_phases/field_phases) since everything inside a region is
 * uniformly crystal. Closures/refs/iterators/channels are excluded up front
 * by the shareability scan, so the old ObjUpvalue** type-confusion can never
 * recur (debug assert). Interned strings keep REGION_INTERNED. */
static void region_tag_recursive(LatValue *v, size_t tagged_rid) {
    assert(v->type != VAL_CLOSURE && v->type != VAL_REF && v->type != VAL_ITERATOR && v->type != VAL_CHANNEL);
    v->phase = VTAG_CRYSTAL;
    if (!(v->type == VAL_STR && v->region_id == REGION_INTERNED)) v->region_id = tagged_rid;
    switch (v->type) {
        case VAL_ARRAY:
            for (size_t i = 0; i < v->as.array.len; i++) region_tag_recursive(&v->as.array.elems[i], tagged_rid);
            break;
        case VAL_TUPLE:
            for (size_t i = 0; i < v->as.tuple.len; i++) region_tag_recursive(&v->as.tuple.elems[i], tagged_rid);
            break;
        case VAL_STRUCT:
            for (size_t i = 0; i < v->as.strct.field_count; i++)
                region_tag_recursive(&v->as.strct.field_values[i], tagged_rid);
            /* Normalize stale per-field phases: uniformly crystal. */
            if (v->as.strct.field_phases) {
                for (size_t i = 0; i < v->as.strct.field_count; i++) v->as.strct.field_phases[i] = VTAG_CRYSTAL;
            }
            break;
        case VAL_MAP:
            for (size_t i = 0; i < v->as.map.map->cap; i++) {
                if (v->as.map.map->entries[i].state == MAP_OCCUPIED)
                    region_tag_recursive((LatValue *)v->as.map.map->entries[i].value, tagged_rid);
            }
            /* Drop stale per-key phases (the freeze-except → refreeze bug):
             * NULL means all keys inherit the map phase. Ownership is mixed:
             * the LatMap header was lat_alloc'd while g_arena pointed at this
             * region (arena memory, freed wholesale with the pages), but
             * lat_map_new callocs the entries array and lat_map_set strdups
             * keys with plain malloc (src/ds/hashmap.c) — those must be freed
             * here or they leak. Same idiom as set_phase_recursive's LAT-441
             * normalization: lat_map_free releases entries + keys; the
             * arena-backed header is left for the region (val_dealloc would
             * no-op on it anyway since region_id is already tagged). */
            if (v->as.map.key_phases) {
                lat_map_free(v->as.map.key_phases);
                v->as.map.key_phases = NULL;
            }
            break;
        case VAL_SET:
            for (size_t i = 0; i < v->as.set.map->cap; i++) {
                if (v->as.set.map->entries[i].state == MAP_OCCUPIED)
                    region_tag_recursive((LatValue *)v->as.set.map->entries[i].value, tagged_rid);
            }
            break;
        case VAL_ENUM:
            for (size_t i = 0; i < v->as.enm.payload_count; i++)
                region_tag_recursive(&v->as.enm.payload[i], tagged_rid);
            break;
        default: break; /* scalars, STR, BUFFER: no children */
    }
}

/* Runtime kill switch (LAT-459 rollout decision): LATTICE_SHARE_CRYSTALS=0
 * disables region materialization process-wide, making every freeze a plain
 * tag flip — equivalent to --no-regions but available without touching the
 * command line (embedded hosts, clat-run, CI bisection). Read live rather
 * than cached: getenv is cheap next to the O(n) materialization it gates,
 * and a live read keeps the switch testable and toggleable in-process.
 * Already-shared handles remain valid and refcounted when the switch is
 * flipped mid-process; only NEW freezes stop materializing. */
static bool share_disabled_by_env(void) {
    const char *e = getenv("LATTICE_SHARE_CRYSTALS");
    return e && e[0] == '0' && e[1] == '\0';
}

bool value_freeze_to_region(LatValue *v) {
    /* Idempotent refreeze of an already-shared crystal: same handle, O(1).
     * The consumed input and the produced output are the same reference, so
     * no retain is needed (net rc change zero). */
    if (v->phase == VTAG_CRYSTAL && value_region_is_shared(v)) return true;

    /* Unshareable or too small: today's tag flip — legacy crystal, full
     * pass-by-value semantics (safe fallback retiring H6/H7/H22/H23). */
    if (share_disabled_by_env() || !value_is_shareable(v) || !value_worth_regionizing(v)) {
        *v = value_freeze(*v);
        return false;
    }

    CrystalRegion *r = crystal_region_create_shared();
    if (!r) {
        *v = value_freeze(*v);
        return false;
    }

    /* Materialize: arena-clone with FORCE-COPY (allow_share=false) so nested
     * shared crystals are copied INTO the new region and nested legacy
     * crystals likewise — no region ever points at another region
     * (self-contained: refcounting is trivially cycle-free). */
    CrystalRegion *saved_arena = g_arena;
    g_arena = r;
    LatValue clone = value_clone_impl(v, false);
    /* CbR Stage 3: a TOP-LEVEL interned string would keep REGION_INTERNED
     * through the clone and region_tag_recursive would never stamp the
     * shared tag on it — leaving an empty pinned region (rc=1, 0 bytes,
     * leaked) and a handle that doesn't participate in rc. Copy the bytes
     * into the region so the handle carries the tag. (Interior interned
     * strings are fine: their parent container node carries the tag.) The
     * stack VM interns every constant-pool/concat string <= 64 bytes, so
     * this is the common case for `fix s = "literal"` there. */
    if (clone.type == VAL_STR && clone.region_id == REGION_INTERNED) {
        clone.as.str_val = lat_strdup(clone.as.str_val); /* arena-routed: g_arena == r */
        clone.region_id = REGION_NONE;                   /* taggable below */
    }
    g_arena = saved_arena;

    region_tag_recursive(&clone, REGION_TAG(r));
    crystal_region_seal(r); /* debug backstop: stray writes now segfault */

    value_free(v); /* free the original (releases any nested shared handles) */
    *v = clone;
    return true;
}

/* ── Crystallized layout (freeze-time read optimization) ── */

/* Minimum live keys before a frozen map is rebuilt into the CMI layout.
 * Protects tight freeze/thaw loops on small values; below this the build
 * cost cannot amortize. */
#define CMI_MIN_KEYS 16

void value_crystallize(LatValue *v) {
    switch (v->type) {
        case VAL_ARRAY:
            for (size_t i = 0; i < v->as.array.len; i++) value_crystallize(&v->as.array.elems[i]);
            break;
        case VAL_STRUCT:
            for (size_t i = 0; i < v->as.strct.field_count; i++) value_crystallize(&v->as.strct.field_values[i]);
            break;
        case VAL_TUPLE:
            for (size_t i = 0; i < v->as.tuple.len; i++) value_crystallize(&v->as.tuple.elems[i]);
            break;
        case VAL_ENUM:
            for (size_t i = 0; i < v->as.enm.payload_count; i++) value_crystallize(&v->as.enm.payload[i]);
            break;
        case VAL_MAP: {
            LatMap *m = v->as.map.map;
            /* Recurse into nested values FIRST (their LatMap* pointers stay
             * stable inside the inline value buffers after the dense copy). */
            for (size_t i = 0; i < m->cap; i++)
                if (m->entries[i].state == MAP_OCCUPIED) value_crystallize((LatValue *)m->entries[i].value);
            /* Eligibility: fully frozen, no per-key phases (partial freezes
             * keep the sparse layout), plain-malloc residency (stack VM path;
             * never arena/fluid-heap-tracked), LatValue payloads only. */
            if (v->phase == VTAG_CRYSTAL && !v->as.map.key_phases && v->region_id == REGION_NONE && !m->cmi &&
                m->value_size == sizeof(LatValue) && m->live >= CMI_MIN_KEYS && !g_arena && !g_heap) {
                lat_map_crystallize(m); /* failure = stay sparse, semantics identical */
            }
            break;
        }
        /* VAL_SET shares LatMap but is excluded in v1. VAL_REF (shared
         * mutable wrapper) is deliberately skipped. */
        default: break;
    }
}

void value_decrystallize(LatValue *v) {
    switch (v->type) {
        case VAL_ARRAY:
            for (size_t i = 0; i < v->as.array.len; i++) value_decrystallize(&v->as.array.elems[i]);
            break;
        case VAL_STRUCT:
            for (size_t i = 0; i < v->as.strct.field_count; i++) value_decrystallize(&v->as.strct.field_values[i]);
            break;
        case VAL_TUPLE:
            for (size_t i = 0; i < v->as.tuple.len; i++) value_decrystallize(&v->as.tuple.elems[i]);
            break;
        case VAL_ENUM:
            for (size_t i = 0; i < v->as.enm.payload_count; i++) value_decrystallize(&v->as.enm.payload[i]);
            break;
        case VAL_MAP: {
            LatMap *m = v->as.map.map;
            if (m->cmi) lat_map_decrystallize(m);
            for (size_t i = 0; i < m->cap; i++)
                if (m->entries[i].state == MAP_OCCUPIED) value_decrystallize((LatValue *)m->entries[i].value);
            break;
        }
        default: break;
    }
}

/* ── Thaw ── */

LatValue value_thaw(const LatValue *v) {
    if (v->type == VAL_REF) {
        /* Thaw breaks sharing: new LatRef with deep-cloned inner.
         * LAT-450: hold the cell lock across the clone-out so a sibling
         * thread's unshare/set window can't free the value under us. */
        ref_lock(v->as.ref.ref);
        LatValue inner_clone = value_copy_out(&v->as.ref.ref->value);
        ref_unlock(v->as.ref.ref);
        value_decrystallize(&inner_clone); /* a thawed map must be mutable — rebuild sparse layout */
        set_phase_recursive(&inner_clone, VTAG_FLUID);
        LatValue result = value_ref(inner_clone);
        value_free(&inner_clone);
        result.phase = VTAG_FLUID;
        return result;
    }
    /* CbR Stage 2 (R18): thaw always takes a private force-copy and marks it
     * fluid — shared region memory (including its phase tags) is never
     * written, so every other alias on any thread observes nothing. There is
     * deliberately no rc==1 in-place fast path: arena-interleaved buffers
     * cannot be realloc'd/grown. (value_copy_out == value_deep_clone when no
     * shared handle is involved, so legacy behavior is bit-identical. The
     * consumed handle is released by the caller's value_free of the
     * original, per the existing non-consuming const* contract.) */
    LatValue cloned = value_copy_out(v);
    /* A thawed map must be mutable again — rebuild the sparse open-addressing table. */
    value_decrystallize(&cloned);
    set_phase_recursive(&cloned, VTAG_FLUID);
    return cloned;
}

/* ── Display ── */

void value_print(const LatValue *v, FILE *out) {
    char *s = value_display(v);
    fprintf(out, "%s", s);
    free(s);
}

char *value_display(const LatValue *v) {
    char *buf = NULL;
    switch (v->type) {
        case VAL_INT: lat_asprintf(&buf, "%lld", (long long)v->as.int_val); break;
        case VAL_FLOAT: {
            lat_asprintf(&buf, "%g", v->as.float_val);
            break;
        }
        case VAL_BOOL: buf = strdup(v->as.bool_val ? "true" : "false"); break;
        case VAL_STR: buf = strdup(v->as.str_val); break;
        case VAL_ARRAY: {
            size_t cap = 64;
            buf = malloc(cap);
            if (!buf) return strdup("[]");
            size_t pos = 0;
            buf[pos++] = '[';
            for (size_t i = 0; i < v->as.array.len; i++) {
                if (i > 0) {
                    buf[pos++] = ',';
                    buf[pos++] = ' ';
                }
                char *elem = value_display(&v->as.array.elems[i]);
                size_t elen = strlen(elem);
                while (pos + elen + 4 > cap) {
                    cap *= 2;
                    buf = realloc(buf, cap);
                }
                memcpy(buf + pos, elem, elen);
                pos += elen;
                free(elem);
            }
            buf[pos++] = ']';
            buf[pos] = '\0';
            break;
        }
        case VAL_STRUCT: {
            size_t cap = 64;
            buf = malloc(cap);
            if (!buf) return strdup("<Struct>");
            size_t pos = 0;
            size_t nlen = strlen(v->as.strct.name);
            while (pos + nlen + 8 > cap) {
                cap *= 2;
                buf = realloc(buf, cap);
            }
            memcpy(buf + pos, v->as.strct.name, nlen);
            pos += nlen;
            buf[pos++] = ' ';
            buf[pos++] = '{';
            buf[pos++] = ' ';
            for (size_t i = 0; i < v->as.strct.field_count; i++) {
                if (i > 0) {
                    buf[pos++] = ',';
                    buf[pos++] = ' ';
                }
                const char *fname = v->as.strct.field_names[i];
                size_t flen = strlen(fname);
                char *fval = value_display(&v->as.strct.field_values[i]);
                size_t vlen = strlen(fval);
                while (pos + flen + vlen + 8 > cap) {
                    cap *= 2;
                    buf = realloc(buf, cap);
                }
                memcpy(buf + pos, fname, flen);
                pos += flen;
                buf[pos++] = ':';
                buf[pos++] = ' ';
                memcpy(buf + pos, fval, vlen);
                pos += vlen;
                free(fval);
            }
            buf[pos++] = ' ';
            buf[pos++] = '}';
            buf[pos] = '\0';
            break;
        }
        case VAL_CLOSURE: {
            size_t cap = 64;
            buf = malloc(cap);
            if (!buf) return strdup("<closure>");
            size_t pos = 0;
            const char *prefix = "<closure|";
            size_t plen = strlen(prefix);
            memcpy(buf, prefix, plen);
            pos = plen;
            if (v->as.closure.param_names) {
                for (size_t i = 0; i < v->as.closure.param_count; i++) {
                    if (i > 0) {
                        buf[pos++] = ',';
                        buf[pos++] = ' ';
                    }
                    const char *pn = v->as.closure.param_names[i];
                    size_t nl = strlen(pn);
                    while (pos + nl + 4 > cap) {
                        cap *= 2;
                        buf = realloc(buf, cap);
                    }
                    memcpy(buf + pos, pn, nl);
                    pos += nl;
                }
            }
            buf[pos++] = '|';
            buf[pos++] = '>';
            buf[pos] = '\0';
            break;
        }
        case VAL_UNIT: buf = strdup("()"); break;
        case VAL_NIL: buf = strdup("nil"); break;
        case VAL_RANGE:
            lat_asprintf(&buf, "%lld..%lld", (long long)v->as.range.start, (long long)v->as.range.end);
            break;
        case VAL_CHANNEL: buf = strdup("<Channel>"); break;
        case VAL_ENUM: {
            if (v->as.enm.payload_count == 0) {
                lat_asprintf(&buf, "%s::%s", v->as.enm.enum_name, v->as.enm.variant_name);
            } else {
                size_t ecap = 64;
                buf = malloc(ecap);
                if (!buf) return strdup("<Enum>");
                size_t epos = 0;
                size_t enlen = strlen(v->as.enm.enum_name);
                size_t vnlen = strlen(v->as.enm.variant_name);
                while (epos + enlen + vnlen + 8 > ecap) {
                    ecap *= 2;
                    buf = realloc(buf, ecap);
                }
                memcpy(buf + epos, v->as.enm.enum_name, enlen);
                epos += enlen;
                buf[epos++] = ':';
                buf[epos++] = ':';
                memcpy(buf + epos, v->as.enm.variant_name, vnlen);
                epos += vnlen;
                buf[epos++] = '(';
                for (size_t i = 0; i < v->as.enm.payload_count; i++) {
                    if (i > 0) {
                        buf[epos++] = ',';
                        buf[epos++] = ' ';
                    }
                    char *elem = value_display(&v->as.enm.payload[i]);
                    size_t elen = strlen(elem);
                    while (epos + elen + 4 > ecap) {
                        ecap *= 2;
                        buf = realloc(buf, ecap);
                    }
                    memcpy(buf + epos, elem, elen);
                    epos += elen;
                    free(elem);
                }
                buf[epos++] = ')';
                buf[epos] = '\0';
            }
            break;
        }
        case VAL_SET: {
            size_t cap2 = 64;
            buf = malloc(cap2);
            if (!buf) return strdup("Set{}");
            size_t pos2 = 0;
            memcpy(buf, "Set{", 4);
            pos2 = 4;
            bool sfirst = true;
            for (size_t i = 0; i < v->as.set.map->cap; i++) {
                if (v->as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                if (!sfirst) {
                    while (pos2 + 3 > cap2) {
                        cap2 *= 2;
                        buf = realloc(buf, cap2);
                    }
                    buf[pos2++] = ',';
                    buf[pos2++] = ' ';
                }
                sfirst = false;
                LatValue *sv = (LatValue *)v->as.set.map->entries[i].value;
                char *elem = value_display(sv);
                size_t elen = strlen(elem);
                while (pos2 + elen + 4 > cap2) {
                    cap2 *= 2;
                    buf = realloc(buf, cap2);
                }
                memcpy(buf + pos2, elem, elen);
                pos2 += elen;
                free(elem);
            }
            while (pos2 + 2 > cap2) {
                cap2 *= 2;
                buf = realloc(buf, cap2);
            }
            buf[pos2++] = '}';
            buf[pos2] = '\0';
            break;
        }
        case VAL_TUPLE: {
            size_t tcap = 64;
            buf = malloc(tcap);
            if (!buf) return strdup("()");
            size_t tpos = 0;
            buf[tpos++] = '(';
            for (size_t i = 0; i < v->as.tuple.len; i++) {
                if (i > 0) {
                    while (tpos + 3 > tcap) {
                        tcap *= 2;
                        buf = realloc(buf, tcap);
                    }
                    buf[tpos++] = ',';
                    buf[tpos++] = ' ';
                }
                char *elem = value_display(&v->as.tuple.elems[i]);
                size_t elen = strlen(elem);
                while (tpos + elen + 4 > tcap) {
                    tcap *= 2;
                    buf = realloc(buf, tcap);
                }
                memcpy(buf + tpos, elem, elen);
                tpos += elen;
                free(elem);
            }
            if (v->as.tuple.len == 1) {
                while (tpos + 3 > tcap) {
                    tcap *= 2;
                    buf = realloc(buf, tcap);
                }
                buf[tpos++] = ',';
            }
            while (tpos + 2 > tcap) {
                tcap *= 2;
                buf = realloc(buf, tcap);
            }
            buf[tpos++] = ')';
            buf[tpos] = '\0';
            break;
        }
        case VAL_BUFFER: lat_asprintf(&buf, "Buffer<%zu bytes>", v->as.buffer.len); break;
        case VAL_REF: lat_asprintf(&buf, "Ref<%s>", value_type_name(&v->as.ref.ref->value)); break;
        case VAL_ITERATOR: buf = strdup("<Iterator>"); break;
        case VAL_MAP: {
            size_t cap2 = 64;
            buf = malloc(cap2);
            if (!buf) return strdup("{}");
            size_t pos2 = 0;
            buf[pos2++] = '{';
            bool first = true;
            for (size_t i = 0; i < v->as.map.map->cap; i++) {
                if (v->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                if (!first) {
                    buf[pos2++] = ',';
                    buf[pos2++] = ' ';
                }
                first = false;
                const char *key = v->as.map.map->entries[i].key;
                LatValue *mval = (LatValue *)v->as.map.map->entries[i].value;
                char *vstr = value_display(mval);
                size_t klen = strlen(key);
                size_t vlen = strlen(vstr);
                while (pos2 + klen + vlen + 8 > cap2) {
                    cap2 *= 2;
                    buf = realloc(buf, cap2);
                }
                buf[pos2++] = '"';
                memcpy(buf + pos2, key, klen);
                pos2 += klen;
                buf[pos2++] = '"';
                buf[pos2++] = ':';
                buf[pos2++] = ' ';
                memcpy(buf + pos2, vstr, vlen);
                pos2 += vlen;
                free(vstr);
            }
            while (pos2 + 2 > cap2) {
                cap2 *= 2;
                buf = realloc(buf, cap2);
            }
            buf[pos2++] = '}';
            buf[pos2] = '\0';
            break;
        }
    }
    return buf;
}

char *value_repr(const LatValue *v) {
    if (v->type == VAL_STR) {
        /* Wrap strings in double quotes */
        size_t slen = strlen(v->as.str_val);
        char *buf = malloc(slen + 3);
        if (!buf) return strdup("\"\"");
        buf[0] = '"';
        memcpy(buf + 1, v->as.str_val, slen);
        buf[slen + 1] = '"';
        buf[slen + 2] = '\0';
        return buf;
    }
    if (v->type == VAL_BUFFER) {
        /* Buffer<N bytes: XX XX XX ...> (show first 8 bytes hex) */
        size_t show = v->as.buffer.len < 8 ? v->as.buffer.len : 8;
        size_t cap = 64 + show * 3;
        char *buf = malloc(cap);
        if (!buf) return strdup("Buffer<>");
        int pos = snprintf(buf, cap, "Buffer<%zu bytes:", v->as.buffer.len);
        for (size_t i = 0; i < show; i++) pos += snprintf(buf + pos, cap - (size_t)pos, " %02x", v->as.buffer.data[i]);
        if (v->as.buffer.len > 8) pos += snprintf(buf + pos, cap - (size_t)pos, " ...");
        snprintf(buf + pos, cap - (size_t)pos, ">");
        return buf;
    }
    /* Everything else uses standard display */
    return value_display(v);
}

/* ── Type name ── */

const char *value_type_name(const LatValue *v) {
    switch (v->type) {
        case VAL_INT: return "Int";
        case VAL_FLOAT: return "Float";
        case VAL_BOOL: return "Bool";
        case VAL_STR: return "String";
        case VAL_ARRAY: return "Array";
        case VAL_STRUCT: return "Struct";
        case VAL_CLOSURE: return "Closure";
        case VAL_UNIT: return "Unit";
        case VAL_NIL: return "Nil";
        case VAL_RANGE: return "Range";
        case VAL_MAP: return "Map";
        case VAL_CHANNEL: return "Channel";
        case VAL_ENUM: return "Enum";
        case VAL_SET: return "Set";
        case VAL_TUPLE: return "Tuple";
        case VAL_BUFFER: return "Buffer";
        case VAL_REF: return "Ref";
        case VAL_ITERATOR: return "Iterator";
    }
    return "?";
}

/* ── Equality ── */

static ValueNumericCmp compare_int_float(int64_t integer, double floating) {
    if (isnan(floating)) return VALUE_CMP_UNORDERED;
    if (floating >= 0x1p63) return VALUE_CMP_LESS;
    if (floating < -0x1p63) return VALUE_CMP_GREATER;

    double whole = 0.0;
    double fraction = modf(floating, &whole);
    int64_t float_integer = (int64_t)whole;
    if (integer < float_integer) return VALUE_CMP_LESS;
    if (integer > float_integer) return VALUE_CMP_GREATER;
    if (fraction > 0.0) return VALUE_CMP_LESS;
    if (fraction < 0.0) return VALUE_CMP_GREATER;
    return VALUE_CMP_EQUAL;
}

ValueNumericCmp value_numeric_compare(const LatValue *a, const LatValue *b) {
    bool a_numeric = a->type == VAL_INT || a->type == VAL_FLOAT;
    bool b_numeric = b->type == VAL_INT || b->type == VAL_FLOAT;
    if (!a_numeric || !b_numeric) return VALUE_CMP_NOT_NUMERIC;

    if (a->type == VAL_INT && b->type == VAL_INT) {
        if (a->as.int_val < b->as.int_val) return VALUE_CMP_LESS;
        if (a->as.int_val > b->as.int_val) return VALUE_CMP_GREATER;
        return VALUE_CMP_EQUAL;
    }
    if (a->type == VAL_FLOAT && b->type == VAL_FLOAT) {
        if (isnan(a->as.float_val) || isnan(b->as.float_val)) return VALUE_CMP_UNORDERED;
        if (a->as.float_val < b->as.float_val) return VALUE_CMP_LESS;
        if (a->as.float_val > b->as.float_val) return VALUE_CMP_GREATER;
        return VALUE_CMP_EQUAL;
    }
    if (a->type == VAL_INT) return compare_int_float(a->as.int_val, b->as.float_val);

    ValueNumericCmp reversed = compare_int_float(b->as.int_val, a->as.float_val);
    if (reversed == VALUE_CMP_LESS) return VALUE_CMP_GREATER;
    if (reversed == VALUE_CMP_GREATER) return VALUE_CMP_LESS;
    return reversed;
}

bool value_eq(const LatValue *a, const LatValue *b) {
    if ((a->type == VAL_INT || a->type == VAL_FLOAT) && (b->type == VAL_INT || b->type == VAL_FLOAT)) {
        return value_numeric_compare(a, b) == VALUE_CMP_EQUAL;
    }
    if (a->type != b->type) return false;
    switch (a->type) {
        case VAL_INT: return a->as.int_val == b->as.int_val;
        case VAL_FLOAT: return a->as.float_val == b->as.float_val;
        case VAL_BOOL: return a->as.bool_val == b->as.bool_val;
        case VAL_STR: {
            if (a->as.str_val == b->as.str_val) return true;
            /* MBA-1336: length-aware compare. Strings may contain NUL bytes, so strcmp
             * would treat distinct suffixes after a NUL as equal (silent backend
             * divergence: tree-walk/VMs would disagree on the same engine JSON field). */
            size_t la = value_string_length(a), lb = value_string_length(b);
            return la == lb && (la == 0 || memcmp(a->as.str_val, b->as.str_val, la) == 0);
        }
        case VAL_UNIT: return true;
        case VAL_NIL: return true;
        case VAL_RANGE: return a->as.range.start == b->as.range.start && a->as.range.end == b->as.range.end;
        case VAL_ARRAY:
            if (a->as.array.len != b->as.array.len) return false;
            for (size_t i = 0; i < a->as.array.len; i++) {
                if (!value_eq(&a->as.array.elems[i], &b->as.array.elems[i])) return false;
            }
            return true;
        case VAL_STRUCT:
            if (strcmp(a->as.strct.name, b->as.strct.name) != 0) return false;
            if (a->as.strct.field_count != b->as.strct.field_count) return false;
            for (size_t i = 0; i < a->as.strct.field_count; i++) {
                if (strcmp(a->as.strct.field_names[i], b->as.strct.field_names[i]) != 0) return false;
                if (!value_eq(&a->as.strct.field_values[i], &b->as.strct.field_values[i])) return false;
            }
            return true;
        case VAL_CLOSURE: return false;
        case VAL_CHANNEL: return a->as.channel.ch == b->as.channel.ch;
        case VAL_ENUM:
            if (strcmp(a->as.enm.enum_name, b->as.enm.enum_name) != 0) return false;
            if (strcmp(a->as.enm.variant_name, b->as.enm.variant_name) != 0) return false;
            if (a->as.enm.payload_count != b->as.enm.payload_count) return false;
            for (size_t i = 0; i < a->as.enm.payload_count; i++) {
                if (!value_eq(&a->as.enm.payload[i], &b->as.enm.payload[i])) return false;
            }
            return true;
        case VAL_MAP: {
            if (lat_map_len(a->as.map.map) != lat_map_len(b->as.map.map)) return false;
            for (size_t i = 0; i < a->as.map.map->cap; i++) {
                if (a->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                const char *key = a->as.map.map->entries[i].key;
                LatValue *av = (LatValue *)a->as.map.map->entries[i].value;
                LatValue *bv = (LatValue *)lat_map_get(b->as.map.map, key);
                if (!bv || !value_eq(av, bv)) return false;
            }
            return true;
        }
        case VAL_SET: {
            return value_set_equal(a, b);
        }
        case VAL_TUPLE:
            if (a->as.tuple.len != b->as.tuple.len) return false;
            for (size_t i = 0; i < a->as.tuple.len; i++) {
                if (!value_eq(&a->as.tuple.elems[i], &b->as.tuple.elems[i])) return false;
            }
            return true;
        case VAL_BUFFER:
            if (a->as.buffer.len != b->as.buffer.len) return false;
            return memcmp(a->as.buffer.data, b->as.buffer.data, a->as.buffer.len) == 0;
        case VAL_REF: return a->as.ref.ref == b->as.ref.ref;
        case VAL_ITERATOR: return false; /* iterators are stateful, never equal */
    }
    return false;
}

/* ── Hash key ── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool ok;
} HashKeyBuilder;

static bool hash_key_reserve(HashKeyBuilder *b, size_t extra) {
    if (!b->ok) return false;
    if (extra > SIZE_MAX - b->len - 1) {
        b->ok = false;
        return false;
    }
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    char *grown = realloc(b->data, cap);
    if (!grown) {
        b->ok = false;
        return false;
    }
    b->data = grown;
    b->cap = cap;
    return true;
}

static bool hash_key_append(HashKeyBuilder *b, const char *data, size_t len) {
    if (!hash_key_reserve(b, len)) return false;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
    return true;
}

static bool hash_key_append_cstr(HashKeyBuilder *b, const char *text) { return hash_key_append(b, text, strlen(text)); }

static bool hash_key_append_size(HashKeyBuilder *b, size_t value) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%zu", value);
    return n >= 0 && hash_key_append(b, tmp, (size_t)n);
}

/* Map keys are NUL-terminated strings, so arbitrary string/buffer bytes are
 * encoded as hex rather than copied into the key. */
static bool hash_key_append_bytes(HashKeyBuilder *b, const unsigned char *bytes, size_t len) {
    static const char hex[] = "0123456789abcdef";
    if (len > SIZE_MAX / 2 || !hash_key_reserve(b, len * 2)) return false;
    for (size_t i = 0; i < len; i++) {
        b->data[b->len++] = hex[bytes[i] >> 4];
        b->data[b->len++] = hex[bytes[i] & 0x0f];
    }
    b->data[b->len] = '\0';
    return true;
}

static bool hash_key_append_blob(HashKeyBuilder *b, const char *tag, const void *data, size_t len) {
    return hash_key_append_cstr(b, tag) && hash_key_append_size(b, len) && hash_key_append_cstr(b, ":") &&
           hash_key_append_bytes(b, data, len) && hash_key_append_cstr(b, ";");
}

static int hash_key_cstr_cmp(const void *a, const void *b) {
    const char *const *sa = a;
    const char *const *sb = b;
    return strcmp(*sa, *sb);
}

static int hash_key_entry_cmp(const void *a, const void *b) {
    const LatMapEntry *const *ea = a;
    const LatMapEntry *const *eb = b;
    return strcmp((*ea)->key, (*eb)->key);
}

static bool hash_key_append_value(HashKeyBuilder *b, const LatValue *v, size_t depth);

static char *hash_key_make(const LatValue *v, size_t depth) {
    HashKeyBuilder b = {0};
    b.ok = true;
    if (!hash_key_append_value(&b, v, depth) || !b.ok) {
        free(b.data);
        return NULL;
    }
    if (!b.data) return strdup("");
    return b.data;
}

static bool hash_key_append_pointer(HashKeyBuilder *b, const char *tag, const void *ptr) {
    char tmp[2 * sizeof(uintptr_t) + 8];
    int n = snprintf(tmp, sizeof(tmp), "%s%p;", tag, ptr);
    return n >= 0 && hash_key_append(b, tmp, (size_t)n);
}

static bool hash_key_append_value(HashKeyBuilder *b, const LatValue *v, size_t depth) {
    if (depth >= VALUE_RECURSION_LIMIT) return hash_key_append_cstr(b, "depth;");

    char tmp[96];
    int n;
    switch (v->type) {
        case VAL_INT:
            n = snprintf(tmp, sizeof(tmp), "n%lld;", (long long)v->as.int_val);
            return n >= 0 && hash_key_append(b, tmp, (size_t)n);
        case VAL_FLOAT: {
            double whole = 0.0;
            if (isfinite(v->as.float_val) && modf(v->as.float_val, &whole) == 0.0 && whole >= -0x1p63 &&
                whole < 0x1p63) {
                n = snprintf(tmp, sizeof(tmp), "n%lld;", (long long)(int64_t)whole);
                return n >= 0 && hash_key_append(b, tmp, (size_t)n);
            }
            uint64_t bits = 0;
            if (v->as.float_val != 0.0) memcpy(&bits, &v->as.float_val, sizeof(bits));
            n = snprintf(tmp, sizeof(tmp), "f%016llx;", (unsigned long long)bits);
            return n >= 0 && hash_key_append(b, tmp, (size_t)n);
        }
        case VAL_BOOL: return hash_key_append_cstr(b, v->as.bool_val ? "b1;" : "b0;");
        case VAL_STR: {
            size_t len = v->as.str_len ? v->as.str_len : strlen(v->as.str_val);
            return hash_key_append_blob(b, "s", v->as.str_val, len);
        }
        case VAL_ARRAY:
            if (!hash_key_append_cstr(b, "a") || !hash_key_append_size(b, v->as.array.len) ||
                !hash_key_append_cstr(b, "["))
                return false;
            for (size_t i = 0; i < v->as.array.len; i++)
                if (!hash_key_append_value(b, &v->as.array.elems[i], depth + 1)) return false;
            return hash_key_append_cstr(b, "]");
        case VAL_STRUCT: {
            size_t data_fields = 0;
            for (size_t i = 0; i < v->as.strct.field_count; i++)
                if (v->as.strct.field_values[i].type != VAL_CLOSURE) data_fields++;
            if (!hash_key_append_blob(b, "r", v->as.strct.name, strlen(v->as.strct.name)) ||
                !hash_key_append_size(b, data_fields) || !hash_key_append_cstr(b, "{"))
                return false;
            for (size_t i = 0; i < v->as.strct.field_count; i++) {
                if (v->as.strct.field_values[i].type == VAL_CLOSURE) continue;
                const char *name = v->as.strct.field_names[i];
                if (!hash_key_append_blob(b, "n", name, strlen(name)) ||
                    !hash_key_append_value(b, &v->as.strct.field_values[i], depth + 1))
                    return false;
            }
            return hash_key_append_cstr(b, "}");
        }
        case VAL_CLOSURE:
            if (!hash_key_append_pointer(b, "cbody", v->as.closure.body) ||
                !hash_key_append_pointer(b, "cenv", v->as.closure.captured_env))
                return false;
            return hash_key_append_pointer(b, "cnative", v->as.closure.native_fn);
        case VAL_UNIT: return hash_key_append_cstr(b, "u;");
        case VAL_NIL: return hash_key_append_cstr(b, "n;");
        case VAL_RANGE:
            n = snprintf(tmp, sizeof(tmp), "g%lld:%lld;", (long long)v->as.range.start, (long long)v->as.range.end);
            return n >= 0 && hash_key_append(b, tmp, (size_t)n);
        case VAL_MAP: {
            size_t len = lat_map_len(v->as.map.map);
            /* entries is an array of LatMapEntry pointers; sizeof of the pointer
             * element is intended (not sizeof of the struct). */
            // NOLINTNEXTLINE(bugprone-sizeof-expression)
            LatMapEntry **entries = len ? malloc(len * sizeof(*entries)) : NULL;
            if (len && !entries) return false;
            size_t count = 0;
            for (size_t i = 0; i < v->as.map.map->cap; i++)
                if (v->as.map.map->entries[i].state == MAP_OCCUPIED) entries[count++] = &v->as.map.map->entries[i];
            // NOLINTNEXTLINE(bugprone-sizeof-expression)
            if (count > 1) qsort(entries, count, sizeof(*entries), hash_key_entry_cmp);
            bool ok = hash_key_append_cstr(b, "m") && hash_key_append_size(b, count) && hash_key_append_cstr(b, "{");
            for (size_t i = 0; ok && i < count; i++) {
                ok = hash_key_append_blob(b, "k", entries[i]->key, strlen(entries[i]->key)) &&
                     hash_key_append_value(b, entries[i]->value, depth + 1);
            }
            free(entries);
            return ok && hash_key_append_cstr(b, "}");
        }
        case VAL_CHANNEL: return hash_key_append_pointer(b, "h", v->as.channel.ch);
        case VAL_ENUM:
            if (!hash_key_append_blob(b, "e", v->as.enm.enum_name, strlen(v->as.enm.enum_name)) ||
                !hash_key_append_blob(b, "v", v->as.enm.variant_name, strlen(v->as.enm.variant_name)) ||
                !hash_key_append_size(b, v->as.enm.payload_count) || !hash_key_append_cstr(b, "("))
                return false;
            for (size_t i = 0; i < v->as.enm.payload_count; i++)
                if (!hash_key_append_value(b, &v->as.enm.payload[i], depth + 1)) return false;
            return hash_key_append_cstr(b, ")");
        case VAL_SET: {
            size_t len = lat_map_len(v->as.set.map);
            char **keys = len ? malloc(len * sizeof(*keys)) : NULL;
            if (len && !keys) return false;
            size_t count = 0;
            bool ok = true;
            for (size_t i = 0; i < v->as.set.map->cap; i++) {
                if (v->as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                keys[count] = hash_key_make(v->as.set.map->entries[i].value, depth + 1);
                if (!keys[count]) {
                    ok = false;
                    break;
                }
                count++;
            }
            if (count > 1) qsort(keys, count, sizeof(*keys), hash_key_cstr_cmp);
            if (ok) ok = hash_key_append_cstr(b, "t") && hash_key_append_size(b, count) && hash_key_append_cstr(b, "{");
            for (size_t i = 0; ok && i < count; i++)
                ok = hash_key_append_size(b, strlen(keys[i])) && hash_key_append_cstr(b, ":") &&
                     hash_key_append_cstr(b, keys[i]) && hash_key_append_cstr(b, ";");
            for (size_t i = 0; i < count; i++) free(keys[i]);
            free(keys);
            return ok && hash_key_append_cstr(b, "}");
        }
        case VAL_TUPLE:
            if (!hash_key_append_cstr(b, "q") || !hash_key_append_size(b, v->as.tuple.len) ||
                !hash_key_append_cstr(b, "("))
                return false;
            for (size_t i = 0; i < v->as.tuple.len; i++)
                if (!hash_key_append_value(b, &v->as.tuple.elems[i], depth + 1)) return false;
            return hash_key_append_cstr(b, ")");
        case VAL_BUFFER: return hash_key_append_blob(b, "x", v->as.buffer.data, v->as.buffer.len);
        case VAL_REF: return hash_key_append_pointer(b, "p", v->as.ref.ref);
        case VAL_ITERATOR:
            if (!hash_key_append_pointer(b, "istate", v->as.iterator.state)) return false;
            return hash_key_append_pointer(b, "iref", v->as.iterator.refcount);
    }
    return false;
}

char *value_hash_key(const LatValue *v) {
    char *key = hash_key_make(v, 0);
    if (key) return key;
    /* The Set API cannot surface allocation failure. Keep the fallback typed
     * so even a degraded key cannot recreate the original display collision. */
    char *fallback = NULL;
    lat_asprintf(&fallback, "oom:%d:%p", (int)v->type, (const void *)v);
    return fallback ? fallback : strdup("oom");
}

/* Set map keys are canonical structural hashes. Unequal values with the same
 * hash occupy compact numbered slots (`hash`, `hash#1`, ...); equality, never
 * the slot name, decides membership. Keeping each element as an ordinary
 * LatValue map entry preserves the existing clone/GC/iteration representation. */
static char *value_set_storage_key(const char *hash, size_t slot) {
    if (slot == 0) return strdup(hash);
    char *key = NULL;
    lat_asprintf(&key, "%s#%zu", hash, slot);
    return key;
}

static bool value_set_item_equal(const LatValue *left, const LatValue *right) {
    if (left == right) return true;
    if (!left || !right) return false;
    if ((left->type == VAL_INT || left->type == VAL_FLOAT) && (right->type == VAL_INT || right->type == VAL_FLOAT))
        return value_numeric_compare(left, right) == VALUE_CMP_EQUAL;
    if (left->type != right->type) return false;

    switch (left->type) {
        case VAL_CLOSURE:
            return left->as.closure.body == right->as.closure.body &&
                   left->as.closure.native_fn == right->as.closure.native_fn &&
                   left->as.closure.captured_env == right->as.closure.captured_env &&
                   left->as.closure.upvalue_count == right->as.closure.upvalue_count;
        case VAL_ITERATOR:
            return left->as.iterator.next_fn == right->as.iterator.next_fn &&
                   left->as.iterator.state == right->as.iterator.state &&
                   left->as.iterator.refcount == right->as.iterator.refcount;
        case VAL_ARRAY:
            if (left->as.array.len != right->as.array.len) return false;
            for (size_t i = 0; i < left->as.array.len; i++)
                if (!value_set_item_equal(&left->as.array.elems[i], &right->as.array.elems[i])) return false;
            return true;
        case VAL_STRUCT:
            if (strcmp(left->as.strct.name, right->as.strct.name) != 0 ||
                left->as.strct.field_count != right->as.strct.field_count)
                return false;
            for (size_t i = 0; i < left->as.strct.field_count; i++) {
                if (strcmp(left->as.strct.field_names[i], right->as.strct.field_names[i]) != 0) return false;
                /* `eq` is the struct's equality method, not data identity. */
                if (strcmp(left->as.strct.field_names[i], "eq") == 0 &&
                    left->as.strct.field_values[i].type == VAL_CLOSURE &&
                    right->as.strct.field_values[i].type == VAL_CLOSURE)
                    continue;
                if (!value_set_item_equal(&left->as.strct.field_values[i], &right->as.strct.field_values[i]))
                    return false;
            }
            return true;
        case VAL_MAP:
            if (lat_map_len(left->as.map.map) != lat_map_len(right->as.map.map)) return false;
            for (size_t i = 0; i < left->as.map.map->cap; i++) {
                if (left->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                LatValue *right_value = lat_map_get(right->as.map.map, left->as.map.map->entries[i].key);
                if (!right_value || !value_set_item_equal((LatValue *)left->as.map.map->entries[i].value, right_value))
                    return false;
            }
            return true;
        case VAL_ENUM:
            if (strcmp(left->as.enm.enum_name, right->as.enm.enum_name) != 0 ||
                strcmp(left->as.enm.variant_name, right->as.enm.variant_name) != 0 ||
                left->as.enm.payload_count != right->as.enm.payload_count)
                return false;
            for (size_t i = 0; i < left->as.enm.payload_count; i++)
                if (!value_set_item_equal(&left->as.enm.payload[i], &right->as.enm.payload[i])) return false;
            return true;
        case VAL_SET: return value_set_equal(left, right);
        case VAL_TUPLE:
            if (left->as.tuple.len != right->as.tuple.len) return false;
            for (size_t i = 0; i < left->as.tuple.len; i++)
                if (!value_set_item_equal(&left->as.tuple.elems[i], &right->as.tuple.elems[i])) return false;
            return true;
        default: return value_eq(left, right);
    }
}

static bool value_set_find(const LatValue *set, const LatValue *value, const char *hash, char **matched_key,
                           size_t *matched_slot, size_t *next_slot) {
    if (matched_key) *matched_key = NULL;
    if (matched_slot) *matched_slot = 0;
    if (next_slot) *next_slot = SIZE_MAX;
    if (!set || set->type != VAL_SET || !set->as.set.map) return false;

    LatValue *stored = lat_map_get(set->as.set.map, hash);
    if (!stored) {
        if (next_slot) *next_slot = 0;
        return false;
    }
    if (value_set_item_equal(stored, value)) {
        if (matched_key) *matched_key = strdup(hash);
        return !matched_key || *matched_key != NULL;
    }

    for (size_t slot = 1; slot != SIZE_MAX; slot++) {
        char *key = value_set_storage_key(hash, slot);
        if (!key) return false;
        stored = lat_map_get(set->as.set.map, key);
        if (!stored) {
            if (next_slot) *next_slot = slot;
            free(key);
            return false;
        }
        if (value_set_item_equal(stored, value)) {
            if (matched_key) *matched_key = key;
            else free(key);
            if (matched_slot) *matched_slot = slot;
            return true;
        }
        free(key);
    }
    return false;
}

size_t value_set_length(const LatValue *set) {
    return set && set->type == VAL_SET && set->as.set.map ? lat_map_len(set->as.set.map) : 0;
}

bool value_set_contains(const LatValue *set, const LatValue *value) {
    if (!set || set->type != VAL_SET || !value) return false;
    char *hash = value_hash_key(value);
    if (!hash) return false;
    bool found = value_set_find(set, value, hash, NULL, NULL, NULL);
    free(hash);
    return found;
}

bool value_set_insert(LatValue *set, const LatValue *value) {
    if (!set || set->type != VAL_SET || !set->as.set.map || !value) return false;
    char *hash = value_hash_key(value);
    if (!hash) return false;
    size_t next_slot = 0;
    if (value_set_find(set, value, hash, NULL, NULL, &next_slot)) {
        free(hash);
        return false;
    }

    char *storage_key = next_slot == SIZE_MAX ? NULL : value_set_storage_key(hash, next_slot);
    if (!storage_key) {
        free(hash);
        return false;
    }

    LatValue stored = value_deep_clone(value);
    lat_map_set(set->as.set.map, storage_key, &stored);
    free(storage_key);
    free(hash);
    return true;
}

bool value_set_remove(LatValue *set, const LatValue *value) {
    if (!set || set->type != VAL_SET || !set->as.set.map || !value) return false;
    char *hash = value_hash_key(value);
    if (!hash) return false;
    char *storage_key = NULL;
    size_t slot = 0;
    if (!value_set_find(set, value, hash, &storage_key, &slot, NULL) || !storage_key) {
        free(hash);
        return false;
    }

    size_t last_slot = slot;
    while (last_slot != SIZE_MAX - 1) {
        char *next_key = value_set_storage_key(hash, last_slot + 1);
        if (!next_key) {
            free(storage_key);
            free(hash);
            return false;
        }
        bool present = lat_map_contains(set->as.set.map, next_key);
        free(next_key);
        if (!present) break;
        last_slot++;
    }

    char *last_key = NULL;
    LatValue *last = NULL;
    if (last_slot != slot) {
        last_key = value_set_storage_key(hash, last_slot);
        last = last_key ? lat_map_get(set->as.set.map, last_key) : NULL;
    }
    LatValue *removed = lat_map_get(set->as.set.map, storage_key);
    if (!removed || (last_slot != slot && !last)) {
        free(last_key);
        free(storage_key);
        free(hash);
        return false;
    }
    value_free(removed);

    if (last_slot == slot) {
        lat_map_remove(set->as.set.map, storage_key);
    } else {
        LatValue moved = *last;
        *last = value_nil();
        lat_map_remove(set->as.set.map, storage_key);
        lat_map_remove(set->as.set.map, last_key);
        lat_map_set(set->as.set.map, storage_key, &moved);
    }
    free(last_key);
    free(storage_key);
    free(hash);
    return true;
}

void value_set_clear(LatValue *set) {
    if (!set || set->type != VAL_SET || !set->as.set.map) return;
    for (size_t i = 0; i < set->as.set.map->cap; i++) {
        if (set->as.set.map->entries[i].state == MAP_OCCUPIED)
            value_free((LatValue *)set->as.set.map->entries[i].value);
    }
    lat_map_free(set->as.set.map);
    *set->as.set.map = lat_map_new(sizeof(LatValue));
}

static void value_set_insert_all(LatValue *result, const LatValue *source) {
    if (!source || source->type != VAL_SET || !source->as.set.map) return;
    for (size_t i = 0; i < source->as.set.map->cap; i++) {
        if (source->as.set.map->entries[i].state == MAP_OCCUPIED)
            value_set_insert(result, (LatValue *)source->as.set.map->entries[i].value);
    }
}

LatValue value_set_union(const LatValue *left, const LatValue *right) {
    LatValue result = value_set_new();
    value_set_insert_all(&result, left);
    value_set_insert_all(&result, right);
    return result;
}

LatValue value_set_intersection(const LatValue *left, const LatValue *right) {
    LatValue result = value_set_new();
    if (!left || left->type != VAL_SET || !right || right->type != VAL_SET) return result;
    for (size_t i = 0; i < left->as.set.map->cap; i++) {
        if (left->as.set.map->entries[i].state != MAP_OCCUPIED) continue;
        LatValue *value = (LatValue *)left->as.set.map->entries[i].value;
        if (value_set_contains(right, value)) value_set_insert(&result, value);
    }
    return result;
}

LatValue value_set_difference(const LatValue *left, const LatValue *right) {
    LatValue result = value_set_new();
    if (!left || left->type != VAL_SET || !right || right->type != VAL_SET) return result;
    for (size_t i = 0; i < left->as.set.map->cap; i++) {
        if (left->as.set.map->entries[i].state != MAP_OCCUPIED) continue;
        LatValue *value = (LatValue *)left->as.set.map->entries[i].value;
        if (!value_set_contains(right, value)) value_set_insert(&result, value);
    }
    return result;
}

LatValue value_set_symmetric_difference(const LatValue *left, const LatValue *right) {
    LatValue result = value_set_difference(left, right);
    if (!left || left->type != VAL_SET || !right || right->type != VAL_SET) return result;
    for (size_t i = 0; i < right->as.set.map->cap; i++) {
        if (right->as.set.map->entries[i].state != MAP_OCCUPIED) continue;
        LatValue *value = (LatValue *)right->as.set.map->entries[i].value;
        if (!value_set_contains(left, value)) value_set_insert(&result, value);
    }
    return result;
}

bool value_set_is_subset(const LatValue *subset, const LatValue *superset) {
    if (!subset || subset->type != VAL_SET || !superset || superset->type != VAL_SET) return false;
    if (value_set_length(subset) > value_set_length(superset)) return false;
    for (size_t i = 0; i < subset->as.set.map->cap; i++) {
        if (subset->as.set.map->entries[i].state != MAP_OCCUPIED) continue;
        if (!value_set_contains(superset, (LatValue *)subset->as.set.map->entries[i].value)) return false;
    }
    return true;
}

bool value_set_equal(const LatValue *left, const LatValue *right) {
    if (!left || left->type != VAL_SET || !right || right->type != VAL_SET) return false;
    return value_set_length(left) == value_set_length(right) && value_set_is_subset(left, right);
}

/* ── Free ── */

static void val_dealloc(LatValue *v, void *ptr) {
    if (!ptr) return;
    if (v->region_id != (size_t)-1) return; /* arena-backed: no-op */
    lat_free(ptr);
}

void value_free(LatValue *v) {
    if (v->region_id != (size_t)-1) {
        /* CbR Stage 2: a shared-region handle owns one reference on its
         * region — drop it. CRITICAL asymmetry (H11): this release branch
         * keys on the region-id tag ALONE, NOT on phase, so a tag-flipped
         * handle (sublimated, or marked fluid before its guard) still
         * releases and cannot leak; the borrow branch in value_clone_impl
         * keys on tag AND phase so a phase-anomalous handle copies safely
         * rather than aliases. The memset below zeroes the handle, keeping
         * same-handle double-free forgiving (a zero region_id is never
         * classified shared). */
        if (value_region_is_shared(v)) crystal_region_release(REGION_PTR(v->region_id));
        memset(v, 0, sizeof(*v)); /* arena owns everything */
        return;
    }
    /* LAT-486: bounded recursion. Beyond the limit, stop descending into
     * children rather than overflowing the C stack. TRADEOFF: the subtree
     * below this node leaks — a bounded leak accepted only at a depth no real
     * program reaches (value_clone_impl caps construction at the same limit,
     * so normally-built data never gets here). The shared-region branch above
     * does not recurse, so guarding here keeps every increment balanced. */
    if (value_recursion_depth >= VALUE_RECURSION_LIMIT) {
        memset(v, 0, sizeof(*v));
        return;
    }
    value_recursion_depth++;
    switch (v->type) {
        case VAL_STR: val_dealloc(v, v->as.str_val); break;
        case VAL_ARRAY:
            for (size_t i = 0; i < v->as.array.len; i++) value_free(&v->as.array.elems[i]);
            val_dealloc(v, v->as.array.elems);
            break;
        case VAL_STRUCT:
            val_dealloc(v, v->as.strct.name);
            for (size_t i = 0; i < v->as.strct.field_count; i++) {
                /* field_names[i] are interned — owned by intern table, not freed here */
                value_free(&v->as.strct.field_values[i]);
            }
            val_dealloc(v, v->as.strct.field_names);
            val_dealloc(v, v->as.strct.field_values);
            val_dealloc(v, v->as.strct.field_phases);
            break;
        case VAL_CLOSURE:
            /* Guard: param_names must be a valid heap pointer (> page size).
             * Marker values like 0x1/0x2 or corruption can place low addresses
             * here via union aliasing; dereferencing them would SEGV. */
            if (v->as.closure.param_names && (uintptr_t)v->as.closure.param_names > 0xFFF) {
                for (size_t i = 0; i < v->as.closure.param_count; i++) val_dealloc(v, v->as.closure.param_names[i]);
                val_dealloc(v, v->as.closure.param_names);
            }
            /* Don't free compiled bytecode closures' env — they store ObjUpvalue**,
             * not Env*. The VM manages upvalue lifetime. */
            if (v->phase != WEAK_CLOSURE_PHASE && v->as.closure.captured_env &&
                !(v->as.closure.body == NULL && v->as.closure.native_fn != NULL))
                env_release(v->as.closure.captured_env);
            break;
        case VAL_MAP:
            if (v->as.map.map) {
                /* Free each stored LatValue before freeing the map */
                for (size_t i = 0; i < v->as.map.map->cap; i++) {
                    if (v->as.map.map->entries[i].state == MAP_OCCUPIED) {
                        LatValue *mv = (LatValue *)v->as.map.map->entries[i].value;
                        value_free(mv);
                    }
                }
                lat_map_free(v->as.map.map);
                val_dealloc(v, v->as.map.map);
            }
            if (v->as.map.key_phases) {
                lat_map_free(v->as.map.key_phases);
                val_dealloc(v, v->as.map.key_phases);
            }
            break;
        case VAL_CHANNEL:
            if (v->as.channel.ch) channel_release(v->as.channel.ch);
            break;
        case VAL_ENUM:
            val_dealloc(v, v->as.enm.enum_name);
            val_dealloc(v, v->as.enm.variant_name);
            if (v->as.enm.payload) {
                for (size_t i = 0; i < v->as.enm.payload_count; i++) value_free(&v->as.enm.payload[i]);
                val_dealloc(v, v->as.enm.payload);
            }
            break;
        case VAL_SET:
            if (v->as.set.map) {
                for (size_t i = 0; i < v->as.set.map->cap; i++) {
                    if (v->as.set.map->entries[i].state == MAP_OCCUPIED) {
                        LatValue *sv = (LatValue *)v->as.set.map->entries[i].value;
                        value_free(sv);
                    }
                }
                lat_map_free(v->as.set.map);
                val_dealloc(v, v->as.set.map);
            }
            break;
        case VAL_TUPLE:
            for (size_t i = 0; i < v->as.tuple.len; i++) value_free(&v->as.tuple.elems[i]);
            val_dealloc(v, v->as.tuple.elems);
            break;
        case VAL_BUFFER: val_dealloc(v, v->as.buffer.data); break;
        case VAL_REF: ref_release(v->as.ref.ref); break;
        case VAL_ITERATOR:
            if (v->as.iterator.refcount) {
                if (--(*v->as.iterator.refcount) == 0) {
                    if (v->as.iterator.free_fn && v->as.iterator.state) v->as.iterator.free_fn(v->as.iterator.state);
                    free(v->as.iterator.refcount);
                }
            }
            break;
        default: break;
    }
    value_recursion_depth--;
    memset(v, 0, sizeof(*v));
}

/* ── Truthiness ── */

bool value_is_truthy(const LatValue *v) {
    switch (v->type) {
        case VAL_BOOL: return v->as.bool_val;
        case VAL_INT: return v->as.int_val != 0;
        case VAL_FLOAT: return v->as.float_val != 0.0;
        case VAL_STR: return v->as.str_val[0] != '\0';
        case VAL_UNIT: return false;
        case VAL_NIL: return false;
        case VAL_MAP: return lat_map_len(v->as.map.map) > 0;
        case VAL_SET: return lat_map_len(v->as.set.map) > 0;
        case VAL_TUPLE: return v->as.tuple.len > 0;
        case VAL_CHANNEL: return true;
        case VAL_BUFFER: return v->as.buffer.len > 0;
        case VAL_REF: return true;
        case VAL_ITERATOR: return true;
        default: return true;
    }
}
