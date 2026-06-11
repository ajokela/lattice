/*
 * Feasibility spike: ownership TRANSFER of fluid values across threads.
 *
 * Question: can a fluid value's allocations be re-homed out of the sending
 * thread's thread-local fluid heap registry (to plain malloc ownership)
 * WITHOUT deep-copying, such that a receiving thread can mutate and free the
 * value safely, and neither thread's heap double-frees at teardown?
 *
 * Mechanism under test ("disown-to-malloc"):
 *   1. Walk the value recursively, collecting every owned REGION_NONE pointer
 *      (exactly the pointers value_free() would lat_free()).
 *   2. Single pass over the sender's FluidHeap allocation list, unlinking the
 *      tracking nodes for collected pointers (the data blocks themselves are
 *      plain malloc blocks — only the registry entry is removed).
 *   3. Push the raw LatValue through the channel, no crystal check, no clone.
 *   4. Receiver mutates and value_free()s it; lat_free() falls through to
 *      free() for untracked pointers, so mixed ownership (moved blocks +
 *      receiver-heap blocks added by mutation) frees correctly.
 *
 * This is test-only code: nothing in src/ changes. It pokes at FluidHeap,
 * LatMap and LatChannel internals on purpose — the point is to verify the
 * mechanics before designing the real feature.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <pthread.h>
#endif

#include "value.h"
#include "memory.h"
#include "channel.h"
#include "ds/hashmap.h"
#include "ds/vec.h"

extern void register_test(const char *name, void (*fn)(void));
extern int test_current_failed;

#define ASSERT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            test_current_failed = 1;                                           \
            return;                                                            \
        }                                                                      \
    } while (0)

#define ASSERT_EQ_INT(a, b)                                                               \
    do {                                                                                  \
        long long _a = (long long)(a), _b = (long long)(b);                               \
        if (_a != _b) {                                                                   \
            fprintf(stderr, "  FAIL: %s:%d: %lld != %lld\n", __FILE__, __LINE__, _a, _b); \
            test_current_failed = 1;                                                      \
            return;                                                                       \
        }                                                                                 \
    } while (0)

#define TEST(name)                                                    \
    static void name(void);                                           \
    static void name##_register(void) __attribute__((constructor));   \
    static void name##_register(void) { register_test(#name, name); } \
    static void name(void)

#ifndef _WIN32 /* the whole experiment depends on pthreads */

/* ══════════════════════════════════════════════════════════════════════════
 * Pointer set: open-addressing hash set of void* (for the single-pass unlink)
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    void **slots;
    size_t cap; /* power of two */
    size_t len;
} PtrSet;

static void ptrset_init(PtrSet *s, size_t hint) {
    size_t cap = 64;
    while (cap < hint * 2) cap <<= 1;
    s->slots = calloc(cap, sizeof(void *));
    s->cap = cap;
    s->len = 0;
}

static void ptrset_free(PtrSet *s) { free(s->slots); }

static size_t ptr_hash(const void *p) {
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 17;
    x *= 0x9E3779B97F4A7C15ULL;
    x ^= x >> 31;
    return (size_t)x;
}

static void ptrset_add(PtrSet *s, void *p) {
    if (!p) return;
    if ((s->len + 1) * 4 > s->cap * 3) { /* grow at 75% */
        PtrSet bigger;
        ptrset_init(&bigger, s->cap); /* hint*2 doubles */
        for (size_t i = 0; i < s->cap; i++) {
            if (s->slots[i]) ptrset_add(&bigger, s->slots[i]);
        }
        free(s->slots);
        *s = bigger;
    }
    size_t idx = ptr_hash(p) & (s->cap - 1);
    while (s->slots[idx]) {
        if (s->slots[idx] == p) return;
        idx = (idx + 1) & (s->cap - 1);
    }
    s->slots[idx] = p;
    s->len++;
}

static bool ptrset_contains(const PtrSet *s, const void *p) {
    if (!p || s->len == 0) return false;
    size_t idx = ptr_hash(p) & (s->cap - 1);
    while (s->slots[idx]) {
        if (s->slots[idx] == p) return true;
        idx = (idx + 1) & (s->cap - 1);
    }
    return false;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Step 1: collect a value's owned pointers (mirrors value_free's lat_free
 * sites). Returns false if the value contains a kind that cannot be re-homed
 * by this mechanism (closures, refs, iterators, arena/ephemeral-backed data);
 * the caller must fall back to value_detach (deep copy).
 * ══════════════════════════════════════════════════════════════════════════ */

static bool xfer_collect(const LatValue *v, PtrSet *set) {
    /* Borrowed, process-global storage: nothing to move, nothing to free. */
    if (v->region_id == REGION_INTERNED || v->region_id == REGION_CONST) return true;
    /* Ephemeral bump-arena data dies at the next statement boundary, and
     * crystal-region data is owned by the sender's RegionManager. Neither can
     * be re-homed by unlinking fluid registry entries. */
    if (v->region_id != REGION_NONE) return false;

    switch (v->type) {
        case VAL_INT:
        case VAL_FLOAT:
        case VAL_BOOL:
        case VAL_UNIT:
        case VAL_NIL:
        case VAL_RANGE: return true;

        case VAL_STR: ptrset_add(set, v->as.str_val); return true;

        case VAL_ARRAY:
            ptrset_add(set, v->as.array.elems);
            for (size_t i = 0; i < v->as.array.len; i++) {
                if (!xfer_collect(&v->as.array.elems[i], set)) return false;
            }
            return true;

        case VAL_STRUCT:
            ptrset_add(set, v->as.strct.name);
            ptrset_add(set, v->as.strct.field_names); /* the array; names are interned */
            ptrset_add(set, v->as.strct.field_values);
            ptrset_add(set, v->as.strct.field_phases);
            for (size_t i = 0; i < v->as.strct.field_count; i++) {
                if (!xfer_collect(&v->as.strct.field_values[i], set)) return false;
            }
            return true;

        case VAL_MAP:
            /* Only the LatMap struct itself is heap-routed (lat_alloc in
             * value_map_new). The entries array and keys are plain
             * calloc/strdup inside hashmap.c — already process-global malloc,
             * nothing to re-home. Entry values are stored inline in entries
             * and recursed below. */
            ptrset_add(set, v->as.map.map);
            if (v->as.map.map) {
                for (size_t i = 0; i < v->as.map.map->cap; i++) {
                    if (v->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                    LatValue *mv = (LatValue *)v->as.map.map->entries[i].value;
                    if (!xfer_collect(mv, set)) return false;
                }
            }
            ptrset_add(set, v->as.map.key_phases);
            return true;

        case VAL_SET:
            ptrset_add(set, v->as.set.map);
            if (v->as.set.map) {
                for (size_t i = 0; i < v->as.set.map->cap; i++) {
                    if (v->as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                    LatValue *sv = (LatValue *)v->as.set.map->entries[i].value;
                    if (!xfer_collect(sv, set)) return false;
                }
            }
            return true;

        case VAL_TUPLE:
            ptrset_add(set, v->as.tuple.elems);
            for (size_t i = 0; i < v->as.tuple.len; i++) {
                if (!xfer_collect(&v->as.tuple.elems[i], set)) return false;
            }
            return true;

        case VAL_BUFFER: ptrset_add(set, v->as.buffer.data); return true;

        case VAL_ENUM:
            ptrset_add(set, v->as.enm.enum_name);
            ptrset_add(set, v->as.enm.variant_name);
            ptrset_add(set, v->as.enm.payload);
            for (size_t i = 0; i < v->as.enm.payload_count; i++) {
                if (!xfer_collect(&v->as.enm.payload[i], set)) return false;
            }
            return true;

        case VAL_CHANNEL:
            /* Refcounted, plain malloc — moving the LatValue moves the
             * refcount with it. Nothing in the fluid registry. */
            return true;

        case VAL_CLOSURE:
            /* captured_env (tree-walk) is a refcounted Env whose internals
             * were routed through lat_alloc; bytecode closures hold
             * ObjUpvalue** owned by the sender's VM. Neither is re-homeable
             * by registry unlinking alone. Refuse — caller deep-copies. */
            return false;

        case VAL_REF:
            /* Shared refcounted alias: the other holders still point at the
             * same LatRef from the sender's thread. Moving one alias is
             * unsound. Refuse. */
            return false;

        case VAL_ITERATOR:
            /* Opaque state with unknown allocation provenance. Refuse. */
            return false;
    }
    return false;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Step 2: single pass over the fluid registry, unlinking tracking nodes for
 * collected pointers WITHOUT freeing the data blocks. O(heap + value), not
 * O(heap × value) like per-pointer fluid_dealloc would be.
 * ══════════════════════════════════════════════════════════════════════════ */

static size_t fluid_disown_set(FluidHeap *h, const PtrSet *set) {
    size_t moved = 0;
    FluidAlloc **prev = &h->allocs;
    FluidAlloc *a = h->allocs;
    while (a) {
        if (ptrset_contains(set, a->ptr)) {
            *prev = a->next;
            h->total_bytes -= a->size;
            h->alloc_count--;
            FluidAlloc *next = a->next;
            free(a); /* tracking node only — a->ptr lives on, now malloc-owned */
            a = next;
            moved++;
        } else {
            prev = &a->next;
            a = a->next;
        }
    }
    return moved;
}

/* Naive variant for the benchmark: per-pointer list walk (what calling
 * fluid_dealloc-minus-the-free per pointer would cost). */
static size_t fluid_disown_naive(FluidHeap *h, const PtrSet *set) {
    size_t moved = 0;
    for (size_t i = 0; i < set->cap; i++) {
        void *p = set->slots[i];
        if (!p) continue;
        FluidAlloc **prev = &h->allocs;
        for (FluidAlloc *a = h->allocs; a; a = a->next) {
            if (a->ptr == p) {
                *prev = a->next;
                h->total_bytes -= a->size;
                h->alloc_count--;
                free(a);
                moved++;
                break;
            }
            prev = &a->next;
        }
    }
    return moved;
}

typedef struct {
    size_t collected; /* owned pointers found in the value */
    size_t disowned;  /* registry entries actually unlinked */
} MoveStats;

/* Move a value out of the current thread's fluid heap. After this returns
 * true, the value's backing is plain malloc (indistinguishable from a
 * value_detach result) and the sender's heap no longer tracks it. */
static bool value_move_out(LatValue *v, MoveStats *stats) {
    PtrSet set;
    ptrset_init(&set, 64);
    if (!xfer_collect(v, &set)) {
        ptrset_free(&set);
        return false;
    }
    DualHeap *heap = value_get_heap();
    size_t moved = heap ? fluid_disown_set(heap->fluid, &set) : 0;
    if (stats) {
        stats->collected = set.len;
        stats->disowned = moved;
    }
    ptrset_free(&set);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Step 3: send the raw value through a channel — no crystal check, no clone.
 * Falls back to the existing detach+deep-copy channel_send when the value
 * contains a non-transferable kind.
 * ══════════════════════════════════════════════════════════════════════════ */

static bool channel_send_move(LatChannel *ch, LatValue val, MoveStats *stats, bool *was_moved) {
    if (!value_move_out(&val, stats)) {
        if (was_moved) *was_moved = false;
        return channel_send(ch, val); /* deep-copy fallback */
    }
    if (was_moved) *was_moved = true;
    pthread_mutex_lock(&ch->mutex);
    if (ch->closed) {
        pthread_mutex_unlock(&ch->mutex);
        value_free(&val); /* untracked → free() path */
        return false;
    }
    lat_vec_push(&ch->buffer, &val);
    pthread_cond_signal(&ch->cond_notempty);
    for (LatSelectWaiter *w = ch->waiters; w; w = w->next) {
        pthread_mutex_lock(w->mutex);
        pthread_cond_signal(w->cond);
        pthread_mutex_unlock(w->mutex);
    }
    pthread_mutex_unlock(&ch->mutex);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Receiver thread: own fluid heap, recv, mutate the moved value (in-place
 * writes, entry replacement, NEW allocations on the receiver's heap), free.
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    LatChannel *ch;
    size_t expect_len; /* expected map length, 0 = skip check */
    int failed;        /* 0 = ok */
} RecvCtx;

static void *receiver_main(void *arg) {
    RecvCtx *ctx = arg;
    DualHeap *heap = dual_heap_new();
    value_set_heap(heap);

    bool ok = false;
    LatValue v = channel_recv(ctx->ch, &ok);
    if (!ok) {
        ctx->failed = 1;
        goto done;
    }
    if (v.type != VAL_MAP) {
        ctx->failed = 2;
        value_free(&v);
        goto done;
    }
    if (ctx->expect_len && lat_map_len(v.as.map.map) != ctx->expect_len) {
        ctx->failed = 3;
        value_free(&v);
        goto done;
    }

    /* Mutate 1: in-place write into a moved string (ASan screams if this
     * memory was freed or poisoned). */
    LatValue *e0 = (LatValue *)lat_map_get(v.as.map.map, "k0");
    if (!e0 || e0->type != VAL_STR) {
        ctx->failed = 4;
        value_free(&v);
        goto done;
    }
    e0->as.str_val[0] = 'X';

    /* Mutate 2: replace an entry's value — frees a moved allocation on the
     * RECEIVER thread (free() path, sender registry no longer involved). */
    LatValue *e1 = (LatValue *)lat_map_get(v.as.map.map, "k1");
    if (e1) {
        value_free(e1);
        LatValue replacement = value_int(424242);
        memcpy(e1, &replacement, sizeof(LatValue));
    }

    /* Mutate 3: insert a brand-new entry — allocated on the receiver's own
     * fluid heap, producing a mixed-ownership container. */
    LatValue fresh = value_string("added-by-receiver");
    lat_map_set(v.as.map.map, "receiver_key", &fresh);

    /* Free the whole thing on the receiver thread. Moved blocks free via the
     * plain free() fallback; the fresh entry frees via the receiver's heap. */
    value_free(&v);
    ctx->failed = 0;

done:
    value_set_heap(NULL);
    dual_heap_free(heap); /* must not double-free anything */
    return NULL;
}

/* Build a fluid map: keys k0..k{n-1}, values "value-<i>" strings, plus one
 * nested array under "nested". */
static LatValue build_test_map(size_t n) {
    LatValue m = value_map_new();
    m.phase = VTAG_FLUID;
    char key[32], val[48];
    for (size_t i = 0; i < n; i++) {
        snprintf(key, sizeof key, "k%zu", i);
        snprintf(val, sizeof val, "value-%zu-padding-padding", i);
        LatValue s = value_string(val);
        s.phase = VTAG_FLUID;
        lat_map_set(m.as.map.map, key, &s);
    }
    LatValue elems[3] = {value_int(1), value_string("two"), value_int(3)};
    LatValue arr = value_array(elems, 3);
    arr.phase = VTAG_FLUID;
    lat_map_set(m.as.map.map, "nested", &arr);
    return m;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(transfer_move_map_to_thread_no_copy) {
    DualHeap *saved = value_get_heap();
    DualHeap *heap = dual_heap_new();
    value_set_heap(heap);

    LatValue m = build_test_map(100);
    size_t before = fluid_live_count(heap->fluid);
    ASSERT(before >= 101); /* 100 strings + nested array elems + map struct */

    LatChannel *ch = channel_new();
    pthread_t t;
    RecvCtx ctx = {.ch = ch, .expect_len = 101, .failed = -1};
    pthread_create(&t, NULL, receiver_main, &ctx);

    MoveStats st = {0};
    bool was_moved = false;
    ASSERT(channel_send_move(ch, m, &st, &was_moved));
    ASSERT(was_moved);
    /* Every collected pointer was found in and unlinked from the registry. */
    ASSERT_EQ_INT(st.disowned, st.collected);
    ASSERT(st.collected >= 101);
    /* The sender's registry shrank by exactly the moved entries: no copy. */
    ASSERT_EQ_INT(fluid_live_count(heap->fluid), before - st.disowned);

    pthread_join(t, NULL);
    ASSERT_EQ_INT(ctx.failed, 0);

    channel_release(ch);
    /* Sender heap teardown after the receiver freed the moved value: if any
     * moved block were still registered here, this would double-free (ASan). */
    value_set_heap(saved);
    dual_heap_free(heap);
}

TEST(transfer_sender_use_after_send_is_the_open_problem) {
    /* Document the hazard the language design must close: after a move, the
     * sender's LatValue struct still holds dangling pointers. We do NOT
     * dereference them — we just verify the struct is bitwise untouched,
     * which is exactly why a moved-from binding needs invalidation (nil-out,
     * VTAG_MOVED, or compile-time error) before this can ship. */
    DualHeap *saved = value_get_heap();
    DualHeap *heap = dual_heap_new();
    value_set_heap(heap);

    LatValue m = build_test_map(4);
    void *raw_map_ptr = (void *)m.as.map.map;

    LatChannel *ch = channel_new();
    pthread_t t;
    RecvCtx ctx = {.ch = ch, .expect_len = 5, .failed = -1};
    pthread_create(&t, NULL, receiver_main, &ctx);
    MoveStats st = {0};
    ASSERT(channel_send_move(ch, m, &st, NULL));
    pthread_join(t, NULL);
    ASSERT_EQ_INT(ctx.failed, 0);

    /* The sender-side copy of the struct still points at memory now owned
     * (and by now freed) by the receiver. A runtime must not let code reach
     * this state with the binding still live. */
    ASSERT(m.as.map.map == raw_map_ptr); /* dangling, untouched */

    channel_release(ch);
    value_set_heap(saved);
    dual_heap_free(heap);
}

TEST(transfer_interned_strings_are_borrowed_not_moved) {
    DualHeap *saved = value_get_heap();
    DualHeap *heap = dual_heap_new();
    value_set_heap(heap);

    LatValue m = value_map_new();
    m.phase = VTAG_FLUID;
    LatValue s = value_string_interned("interned-payload");
    lat_map_set(m.as.map.map, "k", &s);

    MoveStats st = {0};
    ASSERT(value_move_out(&m, &st));
    /* Only the LatMap struct itself is registry-tracked; the interned string
     * contributes no owned pointer. */
    ASSERT_EQ_INT(st.collected, 1);
    ASSERT_EQ_INT(st.disowned, 1);

    value_free(&m); /* untracked now: free() path; interned key untouched */
    value_set_heap(saved);
    dual_heap_free(heap);
}

TEST(transfer_closure_refuses_falls_back_to_copy) {
    DualHeap *saved = value_get_heap();
    DualHeap *heap = dual_heap_new();
    value_set_heap(heap);

    LatValue m = value_map_new();
    m.phase = VTAG_FLUID;
    /* A closure with a fake (never dereferenced) body. captured_env NULL so
     * value_free doesn't env_release. */
    LatValue clo = value_closure(NULL, 0, (struct Expr *)0x1, NULL, NULL, false);
    lat_map_set(m.as.map.map, "fn", &clo);

    MoveStats st = {0};
    ASSERT(!value_move_out(&m, &st)); /* refused: closures can't re-home */

    /* The fallback path (channel_send) still works on such values only if
     * they're deep-clonable; here just free locally. */
    value_free(&m);
    value_set_heap(saved);
    dual_heap_free(heap);
}

TEST(transfer_ephemeral_region_refuses) {
    DualHeap *saved = value_get_heap();
    DualHeap *heap = dual_heap_new();
    value_set_heap(heap);

    LatValue s = value_string("bump-backed");
    s.region_id = REGION_EPHEMERAL; /* simulate OP_ADD concat temporary */
    MoveStats st = {0};
    ASSERT(!value_move_out(&s, &st));
    s.region_id = REGION_NONE; /* restore so we can free it properly */
    value_free(&s);

    LatValue c = value_string("crystal-backed");
    size_t real_region = 7;
    c.region_id = real_region; /* simulate arena-backed crystal */
    ASSERT(!value_move_out(&c, &st));
    c.region_id = REGION_NONE;
    value_free(&c);

    value_set_heap(saved);
    dual_heap_free(heap);
}

TEST(transfer_bench_64k_map_move_vs_detach) {
    enum { N = 65536 };
    DualHeap *saved = value_get_heap();
    DualHeap *heap = dual_heap_new();
    value_set_heap(heap);

    /* ── value_detach (today's deep-copy path) ── */
    LatValue m1 = build_test_map(N);
    double t0 = now_ms();
    LatValue detached = value_detach(&m1);
    double detach_ms = now_ms() - t0;
    value_free(&detached); /* malloc-backed, frees via free() fallback */
    value_free(&m1);

    /* ── move (collect + single-pass disown) ── */
    LatValue m2 = build_test_map(N);
    size_t before = fluid_live_count(heap->fluid);
    LatChannel *ch = channel_new();
    pthread_t t;
    RecvCtx ctx = {.ch = ch, .expect_len = N + 1, .failed = -1};
    pthread_create(&t, NULL, receiver_main, &ctx);

    MoveStats st = {0};
    t0 = now_ms();
    bool sent = channel_send_move(ch, m2, &st, NULL);
    double move_ms = now_ms() - t0;
    ASSERT(sent);
    ASSERT_EQ_INT(st.disowned, st.collected);
    ASSERT(st.collected > N); /* N strings + map struct + nested array bits */
    ASSERT_EQ_INT(fluid_live_count(heap->fluid), before - st.disowned);

    pthread_join(t, NULL);
    ASSERT_EQ_INT(ctx.failed, 0);
    channel_release(ch);

    /* ── naive per-pointer disown, smaller N, to show the O(n²) cliff ── */
    enum { NAIVE_N = 8192 };
    LatValue m3 = build_test_map(NAIVE_N);
    PtrSet set;
    ptrset_init(&set, 64);
    ASSERT(xfer_collect(&m3, &set));
    t0 = now_ms();
    size_t naive_moved = fluid_disown_naive(heap->fluid, &set);
    double naive_ms = now_ms() - t0;
    ASSERT_EQ_INT(naive_moved, set.len);
    ptrset_free(&set);
    value_free(&m3); /* now untracked: free() path */

    fprintf(stderr,
            "\n  [bench] 64k-entry map transfer:\n"
            "    value_detach (deep copy):        %8.2f ms\n"
            "    move (collect + 1-pass disown):  %8.2f ms   (%.1fx faster, %zu ptrs)\n"
            "    naive per-ptr disown (8k map):   %8.2f ms   (O(heap*value) cliff)\n",
            detach_ms, move_ms, detach_ms / (move_ms > 0.0001 ? move_ms : 0.0001), st.collected, naive_ms);

    value_set_heap(saved);
    dual_heap_free(heap);
}

TEST(transfer_cost_scales_with_whole_heap_not_value) {
    /* Gotcha measurement: the disown pass walks the ENTIRE thread heap
     * registry, so moving a tiny value from a busy heap pays O(heap). */
    enum { NOISE = 65536 };
    DualHeap *saved = value_get_heap();
    DualHeap *heap = dual_heap_new();
    value_set_heap(heap);

    void **noise = malloc(NOISE * sizeof(void *));
    for (size_t i = 0; i < NOISE; i++) noise[i] = fluid_alloc(heap->fluid, 16);

    LatValue s = value_string("tiny");
    s.phase = VTAG_FLUID;
    MoveStats st = {0};
    double t0 = now_ms();
    ASSERT(value_move_out(&s, &st));
    double tiny_ms = now_ms() - t0;
    ASSERT_EQ_INT(st.disowned, 1);
    value_free(&s);

    fprintf(stderr, "  [bench] move of 1 string with 64k unrelated heap entries: %.3f ms\n", tiny_ms);

    free(noise); /* blocks themselves are freed by heap teardown */
    value_set_heap(saved);
    dual_heap_free(heap);
}

#endif /* !_WIN32 */
