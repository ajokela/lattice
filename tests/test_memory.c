#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "memory.h"
#include "value.h"

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#endif

/* Import test macros from test_main.c */
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

/* ══════════════════════════════════════════════════════════════════════════
 * FluidHeap tests
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(fluid_heap_starts_empty) {
    FluidHeap *h = fluid_heap_new();
    ASSERT_EQ_INT(fluid_live_count(h), 0);
    ASSERT_EQ_INT(fluid_total_bytes(h), 0);
    fluid_heap_free(h);
}

TEST(fluid_heap_allocate_increments_counts) {
    FluidHeap *h = fluid_heap_new();

    void *p1 = fluid_alloc(h, 64);
    ASSERT(p1 != NULL);
    ASSERT_EQ_INT(fluid_live_count(h), 1);
    ASSERT_EQ_INT(fluid_total_bytes(h), 64);

    void *p2 = fluid_alloc(h, 128);
    ASSERT(p2 != NULL);
    ASSERT_EQ_INT(fluid_live_count(h), 2);
    ASSERT_EQ_INT(fluid_total_bytes(h), 192);

    fluid_heap_free(h);
}

TEST(fluid_heap_dealloc_decrements_live_count) {
    FluidHeap *h = fluid_heap_new();

    void *p1 = fluid_alloc(h, 32);
    void *p2 = fluid_alloc(h, 64);
    ASSERT_EQ_INT(fluid_live_count(h), 2);
    ASSERT_EQ_INT(fluid_total_bytes(h), 96);

    fluid_dealloc(h, p1);
    ASSERT_EQ_INT(fluid_live_count(h), 1);
    ASSERT_EQ_INT(fluid_total_bytes(h), 64);

    fluid_dealloc(h, p2);
    ASSERT_EQ_INT(fluid_live_count(h), 0);
    ASSERT_EQ_INT(fluid_total_bytes(h), 0);

    fluid_heap_free(h);
}

TEST(fluid_heap_dealloc_nonexistent_is_noop) {
    FluidHeap *h = fluid_heap_new();
    void *p = fluid_alloc(h, 16);
    ASSERT_EQ_INT(fluid_live_count(h), 1);

    /* Deallocating a pointer not in the heap should be a no-op. */
    int dummy;
    fluid_dealloc(h, &dummy);
    ASSERT_EQ_INT(fluid_live_count(h), 1);

    fluid_dealloc(h, p);
    ASSERT_EQ_INT(fluid_live_count(h), 0);
    fluid_heap_free(h);
}

TEST(fluid_heap_total_bytes_tracks_correctly) {
    FluidHeap *h = fluid_heap_new();

    void *p1 = fluid_alloc(h, 100);
    void *p2 = fluid_alloc(h, 200);
    void *p3 = fluid_alloc(h, 300);
    ASSERT_EQ_INT(fluid_total_bytes(h), 600);

    fluid_dealloc(h, p2);
    ASSERT_EQ_INT(fluid_total_bytes(h), 400);

    fluid_alloc(h, 50);
    ASSERT_EQ_INT(fluid_total_bytes(h), 450);

    fluid_dealloc(h, p1);
    fluid_dealloc(h, p3);
    ASSERT_EQ_INT(fluid_total_bytes(h), 50);

    fluid_heap_free(h);
}

TEST(fluid_heap_many_allocations) {
    FluidHeap *h = fluid_heap_new();
    void *ptrs[100];

    for (int i = 0; i < 100; i++) {
        ptrs[i] = fluid_alloc(h, 8);
        ASSERT(ptrs[i] != NULL);
    }
    ASSERT_EQ_INT(fluid_live_count(h), 100);
    ASSERT_EQ_INT(fluid_total_bytes(h), 800);

    /* Deallocate every other allocation. */
    for (int i = 0; i < 100; i += 2) { fluid_dealloc(h, ptrs[i]); }
    ASSERT_EQ_INT(fluid_live_count(h), 50);
    ASSERT_EQ_INT(fluid_total_bytes(h), 400);

    fluid_heap_free(h);
}

TEST(fluid_heap_alloc_data_is_writable) {
    FluidHeap *h = fluid_heap_new();
    int *val = fluid_alloc(h, sizeof(int));
    ASSERT(val != NULL);
    *val = 42;
    ASSERT_EQ_INT(*val, 42);
    fluid_heap_free(h);
}

/* ══════════════════════════════════════════════════════════════════════════
 * CrystalRegion / RegionManager tests
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(region_manager_starts_empty) {
    RegionManager *rm = region_manager_new();
    ASSERT_EQ_INT(region_count(rm), 0);
    ASSERT_EQ_INT(region_total_allocs(rm), 0);
    ASSERT_EQ_INT(region_current_epoch(rm), 0);
    region_manager_free(rm);
}

TEST(region_create_creates_region) {
    RegionManager *rm = region_manager_new();

    CrystalRegion *r = region_create(rm);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(region_count(rm), 1);
    ASSERT_EQ_INT(region_total_allocs(rm), 1);
    ASSERT_EQ_INT(r->epoch, 0);
    ASSERT(r->pages != NULL);

    region_manager_free(rm);
}

TEST(region_create_separate_regions) {
    RegionManager *rm = region_manager_new();

    CrystalRegion *r0 = region_create(rm);
    CrystalRegion *r1 = region_create(rm);

    /* Each region_create makes an independent region. */
    ASSERT(r0->id != r1->id);
    ASSERT_EQ_INT(region_count(rm), 2);
    ASSERT_EQ_INT(region_total_allocs(rm), 2);

    region_manager_free(rm);
}

TEST(region_advance_epoch_creates_separate_epochs) {
    RegionManager *rm = region_manager_new();

    CrystalRegion *r0 = region_create(rm);

    Epoch e1 = region_advance_epoch(rm);
    ASSERT_EQ_INT(e1, 1);

    CrystalRegion *r1 = region_create(rm);

    ASSERT(r0->id != r1->id);
    ASSERT_EQ_INT(region_count(rm), 2);
    ASSERT_EQ_INT(r0->epoch, 0);
    ASSERT_EQ_INT(r1->epoch, 1);

    region_manager_free(rm);
}

TEST(region_epochs_advance_monotonically) {
    RegionManager *rm = region_manager_new();
    ASSERT_EQ_INT(region_current_epoch(rm), 0);
    ASSERT_EQ_INT(region_advance_epoch(rm), 1);
    ASSERT_EQ_INT(region_advance_epoch(rm), 2);
    ASSERT_EQ_INT(region_advance_epoch(rm), 3);
    ASSERT_EQ_INT(region_current_epoch(rm), 3);
    region_manager_free(rm);
}

TEST(region_collect_frees_unreachable) {
    RegionManager *rm = region_manager_new();

    CrystalRegion *r0 = region_create(rm);
    RegionId id0 = r0->id;

    region_advance_epoch(rm);
    CrystalRegion *r1 = region_create(rm);
    RegionId id1 = r1->id;

    region_advance_epoch(rm);
    region_create(rm); /* unreachable */

    ASSERT_EQ_INT(region_count(rm), 3);

    /* Only r0 and r1 are reachable; r2 should be collected. */
    RegionId reachable[] = {id0, id1};
    size_t freed = region_collect(rm, reachable, 2);
    ASSERT_EQ_INT(freed, 1);
    ASSERT_EQ_INT(region_count(rm), 2);

    region_manager_free(rm);
}

TEST(region_collect_empty_reachable_frees_all) {
    RegionManager *rm = region_manager_new();

    region_create(rm);
    region_advance_epoch(rm);
    region_create(rm);

    ASSERT_EQ_INT(region_count(rm), 2);

    size_t freed = region_collect(rm, NULL, 0);
    ASSERT_EQ_INT(freed, 2);
    ASSERT_EQ_INT(region_count(rm), 0);

    region_manager_free(rm);
}

TEST(region_collect_all_reachable_frees_none) {
    RegionManager *rm = region_manager_new();

    CrystalRegion *r0 = region_create(rm);
    RegionId id0 = r0->id;
    region_advance_epoch(rm);
    CrystalRegion *r1 = region_create(rm);
    RegionId id1 = r1->id;

    RegionId reachable[] = {id0, id1};
    size_t freed = region_collect(rm, reachable, 2);
    ASSERT_EQ_INT(freed, 0);
    ASSERT_EQ_INT(region_count(rm), 2);

    region_manager_free(rm);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Arena allocation tests
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(arena_alloc_alignment) {
    RegionManager *rm = region_manager_new();
    CrystalRegion *r = region_create(rm);

    void *p1 = arena_alloc(r, 1);
    void *p2 = arena_alloc(r, 1);

    /* Both pointers should be 8-byte aligned */
    ASSERT(((size_t)p1 & 7) == 0);
    ASSERT(((size_t)p2 & 7) == 0);

    /* Second allocation should be 8 bytes after the first (due to alignment) */
    ASSERT_EQ_INT((char *)p2 - (char *)p1, 8);

    region_manager_free(rm);
}

TEST(arena_alloc_oversized) {
    RegionManager *rm = region_manager_new();
    CrystalRegion *r = region_create(rm);

    /* Allocate more than ARENA_PAGE_SIZE — should get a dedicated page */
    size_t big_size = ARENA_PAGE_SIZE * 2;
    void *p = arena_alloc(r, big_size);
    ASSERT(p != NULL);

    /* Write to the full range to verify memory is valid */
    memset(p, 0xAB, big_size);

    /* total_bytes should reflect the aligned size */
    ASSERT(r->total_bytes >= big_size);

    region_manager_free(rm);
}

TEST(arena_alloc_multi_page) {
    RegionManager *rm = region_manager_new();
    CrystalRegion *r = region_create(rm);

    /* Fill up the first page */
    size_t alloc_size = ARENA_PAGE_SIZE / 2;
    void *p1 = arena_alloc(r, alloc_size);
    ASSERT(p1 != NULL);

    /* This should require a new page */
    void *p2 = arena_alloc(r, alloc_size + 1);
    ASSERT(p2 != NULL);

    /* The two pages should have the data */
    ASSERT(r->pages != NULL);
    ASSERT(r->pages->next != NULL);

    /* total_bytes should reflect both allocations */
    ASSERT(r->total_bytes > alloc_size);

    region_manager_free(rm);
}

TEST(arena_strdup_copies_string) {
    RegionManager *rm = region_manager_new();
    CrystalRegion *r = region_create(rm);

    char *s = arena_strdup(r, "hello world");
    ASSERT(s != NULL);
    ASSERT(strcmp(s, "hello world") == 0);

    /* Modify and verify independence */
    s[0] = 'H';
    ASSERT(strcmp(s, "Hello world") == 0);

    region_manager_free(rm);
}

TEST(arena_calloc_zeroed) {
    RegionManager *rm = region_manager_new();
    CrystalRegion *r = region_create(rm);

    int *arr = arena_calloc(r, 10, sizeof(int));
    ASSERT(arr != NULL);

    /* Verify all elements are zero */
    for (int i = 0; i < 10; i++) { ASSERT_EQ_INT(arr[i], 0); }

    /* Write and verify */
    arr[5] = 42;
    ASSERT_EQ_INT(arr[5], 42);

    region_manager_free(rm);
}

TEST(arena_region_free_frees_all_pages) {
    RegionManager *rm = region_manager_new();
    CrystalRegion *r = region_create(rm);

    /* Make several allocations across multiple pages */
    for (int i = 0; i < 100; i++) {
        void *p = arena_alloc(r, 100);
        memset(p, (int)(unsigned char)i, 100);
    }

    /* Collecting with empty reachable set frees all pages — no leaks under ASAN */
    region_collect(rm, NULL, 0);
    ASSERT_EQ_INT(region_count(rm), 0);

    region_manager_free(rm);
}

TEST(arena_total_bytes_tracks) {
    RegionManager *rm = region_manager_new();
    CrystalRegion *r = region_create(rm);

    ASSERT_EQ_INT(r->total_bytes, 0);

    arena_alloc(r, 10); /* aligned to 16 */
    ASSERT(r->total_bytes >= 10);
    size_t after_first = r->total_bytes;

    arena_alloc(r, 20); /* aligned to 24 */
    ASSERT(r->total_bytes > after_first);

    region_manager_free(rm);
}

TEST(region_live_data_bytes_sums_arena) {
    RegionManager *rm = region_manager_new();
    CrystalRegion *r0 = region_create(rm);
    CrystalRegion *r1 = region_create(rm);

    arena_alloc(r0, 100);
    arena_alloc(r1, 200);

    size_t live = region_live_data_bytes(rm);
    ASSERT(live >= 300);
    ASSERT_EQ_INT(live, r0->total_bytes + r1->total_bytes);

    region_manager_free(rm);
}

/* ══════════════════════════════════════════════════════════════════════════
 * DualHeap tests
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(dual_heap_starts_empty) {
    DualHeap *dh = dual_heap_new();
    ASSERT(dh != NULL);
    ASSERT(dh->fluid != NULL);
    ASSERT(dh->regions != NULL);
    ASSERT_EQ_INT(fluid_live_count(dh->fluid), 0);
    ASSERT_EQ_INT(fluid_total_bytes(dh->fluid), 0);
    ASSERT_EQ_INT(region_count(dh->regions), 0);
    ASSERT_EQ_INT(region_total_allocs(dh->regions), 0);
    dual_heap_free(dh);
}

TEST(dual_heap_fluid_and_crystal_independent) {
    DualHeap *dh = dual_heap_new();

    /* Allocate in fluid heap. */
    void *fp1 = fluid_alloc(dh->fluid, 64);
    void *fp2 = fluid_alloc(dh->fluid, 128);
    ASSERT(fp1 != NULL);
    ASSERT(fp2 != NULL);
    ASSERT_EQ_INT(fluid_live_count(dh->fluid), 2);
    ASSERT_EQ_INT(fluid_total_bytes(dh->fluid), 192);

    /* Create a crystal region with arena data. */
    CrystalRegion *r = region_create(dh->regions);
    int *data = arena_alloc(r, sizeof(int));
    *data = 0xCAFE;
    ASSERT_EQ_INT(region_count(dh->regions), 1);
    ASSERT_EQ_INT(region_total_allocs(dh->regions), 1);

    /* Fluid heap is unaffected by region allocation. */
    ASSERT_EQ_INT(fluid_live_count(dh->fluid), 2);

    /* Deallocate from fluid; regions unaffected. */
    fluid_dealloc(dh->fluid, fp1);
    ASSERT_EQ_INT(fluid_live_count(dh->fluid), 1);
    ASSERT_EQ_INT(region_count(dh->regions), 1);

    /* Verify crystal data. */
    ASSERT_EQ_INT(*data, 0xCAFE);

    dual_heap_free(dh);
}

TEST(dual_heap_gc_cycle) {
    DualHeap *dh = dual_heap_new();

    /* Create regions across three epochs. */
    CrystalRegion *r0 = region_create(dh->regions);
    RegionId id0 = r0->id;

    region_advance_epoch(dh->regions);
    region_create(dh->regions); /* will be unreachable */

    region_advance_epoch(dh->regions);
    region_create(dh->regions); /* will be unreachable */

    ASSERT_EQ_INT(region_count(dh->regions), 3);

    /* Only r0 is reachable; the other two should be collected. */
    RegionId reachable[] = {id0};
    size_t freed = region_collect(dh->regions, reachable, 1);
    ASSERT_EQ_INT(freed, 2);
    ASSERT_EQ_INT(region_count(dh->regions), 1);

    /* Fluid heap is independent of crystal GC. */
    fluid_alloc(dh->fluid, 32);
    ASSERT_EQ_INT(fluid_live_count(dh->fluid), 1);

    dual_heap_free(dh);
}

/* ── GC mark/sweep tests ── */

TEST(fluid_mark_and_sweep) {
    FluidHeap *h = fluid_heap_new();
    void *a = fluid_alloc(h, 64);
    (void)fluid_alloc(h, 128); /* b: unreachable */
    void *c = fluid_alloc(h, 32);
    ASSERT_EQ_INT(fluid_live_count(h), 3);

    fluid_unmark_all(h);
    fluid_mark(h, a); /* only a is reachable */
    fluid_mark(h, c); /* c is reachable too */
    size_t swept = fluid_sweep(h);

    ASSERT_EQ_INT(swept, 1); /* only b was swept */
    ASSERT_EQ_INT(fluid_live_count(h), 2);
    ASSERT_EQ_INT(fluid_total_bytes(h), 64 + 32);

    /* a and c should still be usable */
    memset(a, 0, 64);
    memset(c, 0, 32);

    fluid_heap_free(h);
}

TEST(fluid_sweep_all_unmarked) {
    FluidHeap *h = fluid_heap_new();
    fluid_alloc(h, 100);
    fluid_alloc(h, 200);
    fluid_alloc(h, 300);
    ASSERT_EQ_INT(fluid_live_count(h), 3);

    fluid_unmark_all(h);
    size_t swept = fluid_sweep(h);

    ASSERT_EQ_INT(swept, 3);
    ASSERT_EQ_INT(fluid_live_count(h), 0);
    ASSERT_EQ_INT(fluid_total_bytes(h), 0);

    fluid_heap_free(h);
}

TEST(fluid_sweep_all_marked) {
    FluidHeap *h = fluid_heap_new();
    void *a = fluid_alloc(h, 10);
    void *b = fluid_alloc(h, 20);

    fluid_unmark_all(h);
    fluid_mark(h, a);
    fluid_mark(h, b);
    size_t swept = fluid_sweep(h);

    ASSERT_EQ_INT(swept, 0);
    ASSERT_EQ_INT(fluid_live_count(h), 2);

    fluid_heap_free(h);
}

TEST(fluid_mark_nonexistent_returns_false) {
    FluidHeap *h = fluid_heap_new();
    fluid_alloc(h, 64);
    int dummy;
    ASSERT(!fluid_mark(h, &dummy));
    fluid_heap_free(h);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Shared crystal regions (Crystal-by-Reference Stage 2, Round A)
 *
 * These tests exercise the dormant shared-region primitives directly.
 * Nothing in the evaluators calls them yet — they create the only shared
 * region ids that exist in the process, and every test releases all of them
 * before returning (the crystal_region_shared_active() dormancy gate must
 * read false again before any .lat evaluation runs).
 * ══════════════════════════════════════════════════════════════════════════ */

/* Helper: a long string that clears the regionization size threshold. */
static const char *cbr_long_str = "a long enough string that freeze_to_region considers worth sharing 0123456789";

/* Helper: build a fluid array [int 1, long string]. */
static LatValue cbr_make_array(void) {
    LatValue elems[2] = {value_int(1), value_string(cbr_long_str)};
    LatValue arr = value_array(elems, 2);
    arr.phase = VTAG_FLUID;
    return arr;
}

/* Helper: a minimal closure value that value_free can safely tear down. */
static LatValue cbr_make_closure(void) {
    LatValue clo = {.type = VAL_CLOSURE, .phase = VTAG_FLUID, .region_id = REGION_NONE};
    memset(&clo.as, 0, sizeof(clo.as));
    return clo;
}

TEST(region_shared_predicate_macros) {
    /* Sentinels: -1 and -3 are odd and must be excluded by name; -2 and -4
     * are even and fail the low-bit test. A memset-zeroed handle (0) has the
     * low bit clear and is never classified shared. */
    ASSERT(!REGION_IS_SHARED_ID(REGION_NONE));
    ASSERT(!REGION_IS_SHARED_ID(REGION_EPHEMERAL));
    ASSERT(!REGION_IS_SHARED_ID(REGION_INTERNED));
    ASSERT(!REGION_IS_SHARED_ID(REGION_CONST));
    ASSERT(!REGION_IS_SHARED_ID((size_t)0));

    CrystalRegion *r = crystal_region_create_shared();
    ASSERT(r != NULL);
    /* malloc'd region pointer is at least 2-byte aligned: bit 0 is free. */
    ASSERT(((size_t)r & 1u) == 0);
    size_t tag = REGION_TAG(r);
    ASSERT(REGION_IS_SHARED_ID(tag));
    ASSERT(REGION_PTR(tag) == r);
    crystal_region_release(r);
}

TEST(region_shared_rc_lifecycle) {
    size_t base = crystal_region_live_count();
    CrystalRegion *r = crystal_region_create_shared();
    ASSERT(r != NULL);
    ASSERT(r->shared);
    ASSERT_EQ_INT(crystal_region_refcount(r), 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base + 1);
    ASSERT(crystal_region_shared_active());

    crystal_region_retain(r);
    ASSERT_EQ_INT(crystal_region_refcount(r), 2);
    crystal_region_release(r);
    ASSERT_EQ_INT(crystal_region_refcount(r), 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base + 1);

    crystal_region_release(r); /* rc hits 0: O(1) page free + unregister */
    ASSERT_EQ_INT(crystal_region_live_count(), base);
}

TEST(region_shared_rc_ledger_counts) {
    CrystalRegion *r = crystal_region_create_shared();
    ASSERT(r != NULL);
    crystal_region_retain(r);
    crystal_region_retain(r);
    crystal_region_release(r);
    /* Ledger: 2 retains, 1 release so far (creation's rc=1 is not a retain). */
    ASSERT_EQ_INT(crystal_region_dbg_retains(r), 2);
    ASSERT_EQ_INT(crystal_region_dbg_releases(r), 1);
    ASSERT_EQ_INT(crystal_region_refcount(r), 2);
    crystal_region_release(r);
    crystal_region_release(r); /* final: teardown assert retains+1 == releases */
}

TEST(region_freeze_to_region_materializes_array) {
    size_t base = crystal_region_live_count();
    LatValue arr = cbr_make_array();
    ASSERT(value_is_shareable(&arr));

    bool shared = value_freeze_to_region(&arr);
    ASSERT(shared);
    ASSERT(arr.phase == VTAG_CRYSTAL);
    ASSERT(REGION_IS_SHARED_ID(arr.region_id));
    ASSERT_EQ_INT(crystal_region_live_count(), base + 1);
    ASSERT_EQ_INT(crystal_region_refcount(REGION_PTR(arr.region_id)), 1);

    /* Self-containment: every interior node carries the same tagged id and
     * is uniformly crystal. */
    ASSERT(arr.as.array.elems[0].region_id == arr.region_id);
    ASSERT(arr.as.array.elems[0].phase == VTAG_CRYSTAL);
    ASSERT(arr.as.array.elems[1].region_id == arr.region_id);
    ASSERT(arr.as.array.elems[1].phase == VTAG_CRYSTAL);
    ASSERT_EQ_INT(arr.as.array.elems[0].as.int_val, 1);
    ASSERT(strcmp(arr.as.array.elems[1].as.str_val, cbr_long_str) == 0);
    ASSERT(REGION_PTR(arr.region_id)->total_bytes > 0);

    /* value_free on the handle releases the region (keyed on the tag). */
    value_free(&arr);
    ASSERT_EQ_INT(crystal_region_live_count(), base);
}

TEST(region_freeze_to_region_covers_all_pure_kinds) {
    size_t base = crystal_region_live_count();

    LatValue m = value_map_new();
    LatValue mv = value_int(7);
    lat_map_set(m.as.map.map, "k", &mv);

    LatValue st = value_set_new();
    LatValue sv = value_int(9);
    lat_map_set(st.as.set.map, "9", &sv);

    LatValue payload = value_int(3);
    LatValue en = value_enum("E", "V", &payload, 1);
    value_free(&payload);

    LatValue buf = value_buffer((const uint8_t *)"abc", 3);
    LatValue inner[2] = {value_int(5), value_string(cbr_long_str)};
    LatValue tup = value_tuple(inner, 2);
    value_free(&inner[0]);
    value_free(&inner[1]);

    LatValue elems[5] = {m, st, en, buf, tup};
    LatValue arr = value_array(elems, 5);
    /* value_array bit-copies the element values in: ownership moved. */
    arr.phase = VTAG_FLUID;

    bool shared = value_freeze_to_region(&arr);
    ASSERT(shared);
    ASSERT(REGION_IS_SHARED_ID(arr.region_id));
    size_t rid = arr.region_id;

    LatValue *e = arr.as.array.elems;
    ASSERT(e[0].type == VAL_MAP && e[0].region_id == rid && e[0].phase == VTAG_CRYSTAL);
    LatValue *got = lat_map_get(e[0].as.map.map, "k");
    ASSERT(got && got->region_id == rid && got->phase == VTAG_CRYSTAL);
    ASSERT(e[1].type == VAL_SET && e[1].region_id == rid);
    ASSERT(e[2].type == VAL_ENUM && e[2].region_id == rid);
    ASSERT(e[2].as.enm.payload[0].region_id == rid);
    ASSERT(e[2].as.enm.payload[0].phase == VTAG_CRYSTAL);
    ASSERT(e[3].type == VAL_BUFFER && e[3].region_id == rid);
    /* VAL_TUPLE: the case the old eval.c tagger was missing (H8). */
    ASSERT(e[4].type == VAL_TUPLE && e[4].region_id == rid);
    ASSERT(e[4].as.tuple.elems[0].region_id == rid);
    ASSERT(e[4].as.tuple.elems[1].region_id == rid);
    ASSERT(e[4].as.tuple.elems[1].phase == VTAG_CRYSTAL);

    value_free(&arr);
    ASSERT_EQ_INT(crystal_region_live_count(), base);
}

TEST(region_tagger_normalizes_phase_metadata) {
    size_t base = crystal_region_live_count();

    /* Struct with stale per-field phases. */
    LatValue fvals[2] = {value_int(1), value_int(2)};
    char *fnames[2] = {"a", "b"};
    LatValue s = value_struct("P", fnames, fvals, 2);
    s.as.strct.field_phases = malloc(2 * sizeof(PhaseTag));
    s.as.strct.field_phases[0] = VTAG_FLUID;
    s.as.strct.field_phases[1] = VTAG_CRYSTAL;

    /* Map with stale key_phases (the freeze-except → refreeze stale bug). */
    LatValue m = value_map_new();
    LatValue mv = value_int(7);
    lat_map_set(m.as.map.map, "k", &mv);
    m.as.map.key_phases = malloc(sizeof(LatMap));
    *m.as.map.key_phases = lat_map_new(sizeof(PhaseTag));
    PhaseTag pt = VTAG_FLUID;
    lat_map_set(m.as.map.key_phases, "k", &pt);

    LatValue elems[2] = {s, m};
    LatValue tup = value_tuple(elems, 2); /* deep-clones elems */
    value_free(&s);
    value_free(&m);

    bool shared = value_freeze_to_region(&tup);
    ASSERT(shared);
    size_t rid = tup.region_id;
    ASSERT(REGION_IS_SHARED_ID(rid));

    LatValue *ts = &tup.as.tuple.elems[0];
    LatValue *tm = &tup.as.tuple.elems[1];
    ASSERT(ts->region_id == rid && tm->region_id == rid);
    /* Everything in a region is uniformly crystal: per-field phases are
     * normalized, per-key phases dropped. The dropped key_phases' entries
     * array and keys are plain heap (lat_map_new/lat_map_set never route
     * through the arena), so the tagger must lat_map_free them rather than
     * just unlink — pinned by the sanitizer gauntlet over this test. */
    if (ts->as.strct.field_phases) {
        ASSERT(ts->as.strct.field_phases[0] == VTAG_CRYSTAL);
        ASSERT(ts->as.strct.field_phases[1] == VTAG_CRYSTAL);
    }
    ASSERT(tm->as.map.key_phases == NULL);

    value_free(&tup);
    ASSERT_EQ_INT(crystal_region_live_count(), base);
}

TEST(region_freeze_idempotent_same_handle) {
    size_t base = crystal_region_live_count();
    LatValue arr = cbr_make_array();
    ASSERT(value_freeze_to_region(&arr));
    size_t rid = arr.region_id;
    LatValue *elems = arr.as.array.elems;
    ASSERT_EQ_INT(crystal_region_refcount(REGION_PTR(rid)), 1);

    /* Refreeze of an already-shared crystal: O(1), same handle, rc balanced
     * (the consumed handle and the produced handle are the same reference). */
    ASSERT(value_freeze_to_region(&arr));
    ASSERT(arr.region_id == rid);
    ASSERT(arr.as.array.elems == elems);
    ASSERT_EQ_INT(crystal_region_refcount(REGION_PTR(rid)), 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base + 1);

    value_free(&arr);
    ASSERT_EQ_INT(crystal_region_live_count(), base);
}

TEST(region_shareability_scan_rejects_impure_kinds) {
    size_t base = crystal_region_live_count();

    /* Closure inside a struct (idiomatic Lattice OOP) → legacy crystal. */
    LatValue fvals[2] = {value_int(1), cbr_make_closure()};
    char *fnames[2] = {"x", "method"};
    LatValue s = value_struct("Obj", fnames, fvals, 2);
    s.phase = VTAG_FLUID;
    ASSERT(!value_is_shareable(&s));
    ASSERT(!value_freeze_to_region(&s));
    ASSERT(s.phase == VTAG_CRYSTAL);
    ASSERT(s.region_id == REGION_NONE); /* legacy: tag flip only */
    ASSERT_EQ_INT(crystal_region_live_count(), base);
    value_free(&s);

    /* Ref → legacy. */
    LatValue relems[1] = {value_ref(value_int(1))};
    LatValue ra = value_array(relems, 1);
    ra.phase = VTAG_FLUID;
    ASSERT(!value_is_shareable(&ra));
    ASSERT(!value_freeze_to_region(&ra));
    ASSERT(ra.region_id == REGION_NONE);
    value_free(&ra);

    /* Transitively sublimated member → legacy. */
    LatValue selems[1] = {value_int(1)};
    LatValue sa = value_array(selems, 1);
    sa.as.array.elems[0].phase = VTAG_SUBLIMATED;
    ASSERT(!value_is_shareable(&sa));
    ASSERT(!value_freeze_to_region(&sa));
    ASSERT(sa.region_id == REGION_NONE);
    value_free(&sa);

    ASSERT_EQ_INT(crystal_region_live_count(), base);
}

TEST(region_scalars_and_short_strings_stay_legacy) {
    size_t base = crystal_region_live_count();

    LatValue i = value_int(42);
    ASSERT(!value_freeze_to_region(&i)); /* scalar: copy beats refcount traffic */
    ASSERT(i.phase == VTAG_CRYSTAL && i.region_id == REGION_NONE);
    value_free(&i);

    LatValue s = value_string("hi");
    ASSERT(!value_freeze_to_region(&s));
    ASSERT(s.phase == VTAG_CRYSTAL && s.region_id == REGION_NONE);
    value_free(&s);

    LatValue r = value_range(1, 9);
    ASSERT(!value_freeze_to_region(&r));
    ASSERT(r.region_id == REGION_NONE);
    value_free(&r);

    /* Long strings DO regionize. */
    LatValue ls = value_string(cbr_long_str);
    ASSERT(value_freeze_to_region(&ls));
    ASSERT(REGION_IS_SHARED_ID(ls.region_id));
    value_free(&ls);

    ASSERT_EQ_INT(crystal_region_live_count(), base);
}

/* The LATTICE_FORCE_COPY=1 differential oracle disables the borrow fast
 * path process-wide. Tests that assert aliasing MECHANICS (retain counts,
 * bitwise handle identity, region sharing) are definitionally inapplicable
 * under it and skip; semantics tests must pass unchanged in both modes —
 * that asymmetry is the oracle's contract. */
static bool cbr_force_copy_active(void) {
    const char *e = getenv("LATTICE_FORCE_COPY");
    return e && *e && strcmp(e, "0") != 0;
}

TEST(region_borrow_clone_retains_same_backing) {
    if (cbr_force_copy_active()) return;
    size_t base = crystal_region_live_count();
    LatValue arr = cbr_make_array();
    ASSERT(value_freeze_to_region(&arr));
    CrystalRegion *r = REGION_PTR(arr.region_id);

    /* value_deep_clone == value_clone_impl(v, true): borrow fast path. */
    LatValue alias = value_deep_clone(&arr);
    ASSERT_EQ_INT(crystal_region_refcount(r), 2);
    ASSERT(alias.region_id == arr.region_id);
    ASSERT(alias.as.array.elems == arr.as.array.elems); /* bitwise handle copy */

    LatValue alias2 = value_clone_impl(&arr, true);
    ASSERT_EQ_INT(crystal_region_refcount(r), 3);
    ASSERT(alias2.as.array.elems == arr.as.array.elems);

    value_free(&alias);
    value_free(&alias2);
    ASSERT_EQ_INT(crystal_region_refcount(r), 1);
    value_free(&arr);
    ASSERT_EQ_INT(crystal_region_live_count(), base);
}

TEST(region_copy_out_is_independent) {
    size_t base = crystal_region_live_count();
    LatValue arr = cbr_make_array();
    ASSERT(value_freeze_to_region(&arr));
    CrystalRegion *r = REGION_PTR(arr.region_id);

    LatValue priv = value_copy_out(&arr);
    ASSERT_EQ_INT(crystal_region_refcount(r), 1); /* no retain: force-copy */
    ASSERT(priv.region_id == REGION_NONE);
    ASSERT(priv.as.array.elems != arr.as.array.elems);
    ASSERT(priv.as.array.elems[1].as.str_val != arr.as.array.elems[1].as.str_val);
    ASSERT(strcmp(priv.as.array.elems[1].as.str_val, cbr_long_str) == 0);

    /* Mutating the private copy never touches region memory. */
    priv.as.array.elems[0].as.int_val = 99;
    ASSERT_EQ_INT(arr.as.array.elems[0].as.int_val, 1);

    value_free(&priv);
    ASSERT_EQ_INT(crystal_region_refcount(r), 1);
    value_free(&arr);
    ASSERT_EQ_INT(crystal_region_live_count(), base);
}

TEST(region_unshare_privatizes_handle) {
    if (cbr_force_copy_active()) return;
    size_t base = crystal_region_live_count();
    LatValue arr = cbr_make_array();
    ASSERT(value_freeze_to_region(&arr));
    CrystalRegion *r = REGION_PTR(arr.region_id);

    LatValue alias = value_deep_clone(&arr);
    ASSERT_EQ_INT(crystal_region_refcount(r), 2);

    value_unshare(&alias);
    ASSERT_EQ_INT(crystal_region_refcount(r), 1);
    ASSERT(alias.region_id == REGION_NONE);
    ASSERT(alias.as.array.elems != arr.as.array.elems);

    alias.as.array.elems[0].as.int_val = 777;
    ASSERT_EQ_INT(arr.as.array.elems[0].as.int_val, 1);

    /* Unshare on a non-shared value is a no-op. */
    LatValue plain = value_int(5);
    value_unshare(&plain);
    ASSERT(plain.region_id == REGION_NONE);

    value_free(&alias);
    value_free(&arr);
    ASSERT_EQ_INT(crystal_region_live_count(), base);
}

TEST(region_value_free_releases_and_double_free_forgiving) {
    if (cbr_force_copy_active()) return;
    size_t base = crystal_region_live_count();
    LatValue arr = cbr_make_array();
    ASSERT(value_freeze_to_region(&arr));
    CrystalRegion *r = REGION_PTR(arr.region_id);

    LatValue alias = value_deep_clone(&arr);
    ASSERT_EQ_INT(crystal_region_refcount(r), 2);

    value_free(&alias); /* releases + memsets the handle to zero */
    ASSERT_EQ_INT(crystal_region_refcount(r), 1);
    ASSERT(alias.region_id == 0);
    value_free(&alias); /* same-handle double free: zero handle, no release */
    ASSERT_EQ_INT(crystal_region_refcount(r), 1);

    value_free(&arr);
    ASSERT_EQ_INT(crystal_region_live_count(), base);
}

TEST(region_shared_inside_fluid_container_rc_balance) {
    if (cbr_force_copy_active()) return;
    size_t base = crystal_region_live_count();
    LatValue inner = cbr_make_array();
    ASSERT(value_freeze_to_region(&inner));
    CrystalRegion *r = REGION_PTR(inner.region_id);

    /* Move the shared handle into a fluid container (ownership transfer). */
    LatValue outer = value_array(NULL, 0);
    outer.phase = VTAG_FLUID;
    outer.as.array.elems[0] = inner;
    outer.as.array.len = 1;
    ASSERT_EQ_INT(crystal_region_refcount(r), 1);

    /* Cloning the fluid container borrows the nested shared element. */
    LatValue clone = value_deep_clone(&outer);
    ASSERT_EQ_INT(crystal_region_refcount(r), 2);
    ASSERT(clone.as.array.elems[0].as.array.elems == outer.as.array.elems[0].as.array.elems);

    /* Thaw of the container force-copies — region memory is never written. */
    LatValue thawed = value_thaw(&outer);
    ASSERT_EQ_INT(crystal_region_refcount(r), 2); /* no retain, no release */
    ASSERT(thawed.region_id == REGION_NONE);
    ASSERT(thawed.as.array.elems[0].region_id == REGION_NONE);
    ASSERT(thawed.as.array.elems[0].phase == VTAG_FLUID);
    ASSERT(outer.as.array.elems[0].phase == VTAG_CRYSTAL); /* original untouched */

    value_free(&thawed);
    value_free(&clone);
    ASSERT_EQ_INT(crystal_region_refcount(r), 1);
    value_free(&outer);
    ASSERT_EQ_INT(crystal_region_live_count(), base);
}

TEST(region_detach_borrows_shared_handle) {
    if (cbr_force_copy_active()) return;
    size_t base = crystal_region_live_count();
    LatValue arr = cbr_make_array();
    ASSERT(value_freeze_to_region(&arr));
    CrystalRegion *r = REGION_PTR(arr.region_id);

    /* Region pages are plain global malloc — detach is retain + bitwise copy. */
    LatValue det = value_detach(&arr);
    ASSERT_EQ_INT(crystal_region_refcount(r), 2);
    ASSERT(det.as.array.elems == arr.as.array.elems);

    value_free(&det);
    value_free(&arr);
    ASSERT_EQ_INT(crystal_region_live_count(), base);
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
TEST(region_seal_tripwire_blocks_stray_writes) {
    size_t base = crystal_region_live_count();
    LatValue arr = cbr_make_array();
    ASSERT(value_freeze_to_region(&arr));
    CrystalRegion *r = REGION_PTR(arr.region_id);

    if (!r->sealed) {
        /* Seal backstop not compiled in (NDEBUG / non-POSIX): nothing to trip. */
        value_free(&arr);
        ASSERT_EQ_INT(crystal_region_live_count(), base);
        return;
    }

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        /* Child: a stray write through the handle must die loudly. Silence
         * the crash report at fd level (dup2, NOT freopen — freopen would
         * flush the stdio buffer inherited from the parent, duplicating
         * already-printed test output into the shared pipe). */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        arr.as.array.elems[0].as.int_val = 1234; /* write into sealed page */
        _exit(0);                                /* reached only if seal failed */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    /* The child must NOT have exited cleanly. */
    ASSERT(!(WIFEXITED(status) && WEXITSTATUS(status) == 0));

    value_free(&arr);
    ASSERT_EQ_INT(crystal_region_live_count(), base);
}

#define CBR_THREADS 8
#define CBR_ITERS   20000
static void *cbr_retain_release_loop(void *arg) {
    CrystalRegion *r = arg;
    for (int i = 0; i < CBR_ITERS; i++) {
        crystal_region_retain(r);
        crystal_region_release(r);
    }
    return NULL;
}

TEST(region_atomic_rc_under_contention) {
    size_t base = crystal_region_live_count();
    CrystalRegion *r = crystal_region_create_shared();
    ASSERT(r != NULL);

    pthread_t threads[CBR_THREADS];
    for (int i = 0; i < CBR_THREADS; i++) pthread_create(&threads[i], NULL, cbr_retain_release_loop, r);
    for (int i = 0; i < CBR_THREADS; i++) pthread_join(threads[i], NULL);

    /* Exact final count: the creation reference only. */
    ASSERT_EQ_INT(crystal_region_refcount(r), 1);
    ASSERT_EQ_INT(crystal_region_dbg_retains(r), (long long)CBR_THREADS * CBR_ITERS);
    ASSERT_EQ_INT(crystal_region_dbg_releases(r), (long long)CBR_THREADS * CBR_ITERS);
    crystal_region_release(r);
    ASSERT_EQ_INT(crystal_region_live_count(), base);
}
#endif /* !_WIN32 && !__EMSCRIPTEN__ */

TEST(region_registry_counts_live_and_created) {
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    CrystalRegion *a = crystal_region_create_shared();
    CrystalRegion *b = crystal_region_create_shared();
    ASSERT(a && b);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 2);
    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 2);
    ASSERT(crystal_region_shared_active());

    crystal_region_release(a);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);
    crystal_region_release(b);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 2);
}

/* ══════════════════════════════════════════════════════════════════════════
 * LAT-449 Round B: tree-walker rc/lifecycle tests (TDD — RED until the
 * Round B switch lands)
 *
 * These run Lattice source through the TREE-WALK evaluator directly
 * (regardless of the --backend the suite was invoked with) and assert the
 * Round B contract against the shared-region registry:
 *
 *   - a tree-walker freeze of a shareable container MATERIALIZES exactly one
 *     shared region (crystal_region_created_total / live_count advance);
 *   - aliasing a shared crystal is an O(1) retain — never a second region;
 *   - refreeze of an already-shared crystal reuses the region (idempotence);
 *   - scope exit releases; evaluator teardown leaves the registry at
 *     baseline (C3: the atexit teardown assumes an empty registry);
 *   - retired numeric region ids (R8): no evaluator value may carry a
 *     region_id that satisfies REGION_IS_SHARED_ID unless it is a genuine
 *     tagged CrystalRegion pointer.
 *
 * Until the switch, the evaluator's freeze path still mints legacy
 * RegionManager numeric ids and never touches the shared registry, so the
 * registry-count assertions below fail first (cleanly — the REGION_PTR
 * dereferences are sequenced after them and never execute on the old code).
 * ══════════════════════════════════════════════════════════════════════════ */

#include "lexer.h"
#include "parser.h"
#include "eval.h"

/* Lex+parse only (REPL-style persistence tests run several programs on one
 * persistent evaluator; tokens/programs must stay alive until it is freed). */
static bool cbr_rb_parse(const char *source, LatVec *tokens_out, Program *prog_out) {
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    *tokens_out = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        fprintf(stderr, "  cbr_rb_parse lex error: %s\n", lex_err);
        free(lex_err);
        lat_vec_free(tokens_out);
        return false;
    }
    Parser parser = parser_new(tokens_out);
    char *parse_err = NULL;
    *prog_out = parser_parse(&parser, &parse_err);
    if (parse_err) {
        fprintf(stderr, "  cbr_rb_parse parse error: %s\n", parse_err);
        free(parse_err);
        program_free(prog_out);
        for (size_t i = 0; i < tokens_out->len; i++) token_free(lat_vec_get(tokens_out, i));
        lat_vec_free(tokens_out);
        return false;
    }
    return true;
}

/* Run source through the tree-walk evaluator; returns the live Evaluator so
 * callers can inspect bindings/registry while root bindings are still
 * retained. Mirrors run_with_stats in test_eval.c. no_regions mirrors the
 * --no-regions CLI flag (legacy tag-flip baseline mode, R38). */
static Evaluator *cbr_rb_eval_opt(const char *source, LatVec *tokens_out, Program *prog_out, bool no_regions) {
    if (!cbr_rb_parse(source, tokens_out, prog_out)) return NULL;

    Evaluator *ev = evaluator_new();
    if (no_regions) evaluator_set_no_regions(ev, true);
    char *eval_err = evaluator_run(ev, prog_out);
    if (eval_err) {
        fprintf(stderr, "  cbr_rb_eval eval error: %s\n", eval_err);
        free(eval_err);
        evaluator_free(ev);
        program_free(prog_out);
        for (size_t i = 0; i < tokens_out->len; i++) token_free(lat_vec_get(tokens_out, i));
        lat_vec_free(tokens_out);
        return NULL;
    }
    return ev;
}

static Evaluator *cbr_rb_eval(const char *source, LatVec *tokens_out, Program *prog_out) {
    return cbr_rb_eval_opt(source, tokens_out, prog_out, false);
}

static void cbr_rb_cleanup(Evaluator *ev, LatVec *tokens, Program *prog) {
    evaluator_free(ev);
    program_free(prog);
    for (size_t i = 0; i < tokens->len; i++) token_free(lat_vec_get(tokens, i));
    lat_vec_free(tokens);
}

/* RED until Round B: a top-level fix binding of a large array materializes
 * exactly one live shared region; evaluator teardown releases it. */
TEST(cbr_rb_freeze_materializes_live_region) {
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    LatVec tokens;
    Program prog;
    Evaluator *ev = cbr_rb_eval("fix g = [1, 2, 3, \"a fairly long string to clear any size threshold 0123456789\"]\n",
                                &tokens, &prog);
    ASSERT(ev != NULL);

    /* The freeze created one shared region, kept live by the root binding. */
    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    /* Teardown releases the binding's reference: registry back to baseline
     * before the memory.c atexit hook runs (C3). */
    cbr_rb_cleanup(ev, &tokens, &prog);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* RED until Round B: `let h = g` on a shared crystal retains the SAME
 * region (one region total) and each live binding holds one reference. */
TEST(cbr_rb_alias_is_retain_not_new_region) {
    if (cbr_force_copy_active()) return;
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    LatVec tokens;
    Program prog;
    Evaluator *ev = cbr_rb_eval("fix g = [1, 2, 3, \"alias retain probe long string 0123456789\"]\n"
                                "let h = g\n"
                                "let k = h\n",
                                &tokens, &prog);
    ASSERT(ev != NULL);

    /* Three bindings, ONE region. */
    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    /* env_get deep-clones — post-switch the borrow fast path makes that a
     * retained alias of the same region. (These dereferences only run once
     * the count assertions above pass, i.e. on Round B code.) */
    LatValue vg, vh;
    ASSERT(env_get(ev->env, "g", &vg));
    ASSERT(env_get(ev->env, "h", &vh));
    ASSERT(REGION_IS_SHARED_ID(vg.region_id));
    ASSERT(REGION_IS_SHARED_ID(vh.region_id));
    ASSERT(REGION_PTR(vg.region_id) == REGION_PTR(vh.region_id));
    ASSERT(vg.phase == VTAG_CRYSTAL);

    CrystalRegion *r = REGION_PTR(vg.region_id);
    size_t rc_with_handles = crystal_region_refcount(r);
    /* At least the 3 root bindings + our 2 test handles. */
    ASSERT(rc_with_handles >= 5);
    value_free(&vg);
    value_free(&vh);
    /* Releasing our handles drops the rc by exactly 2 (rc ledger balance). */
    ASSERT_EQ_INT(crystal_region_refcount(r), rc_with_handles - 2);

    cbr_rb_cleanup(ev, &tokens, &prog);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* RED until Round B: freeze(g) on an already-shared crystal is idempotent at
 * the region level — no second region; g and g2 share one region (C-level
 * companion to the cbr_pin_double_freeze_idempotent semantics pin). */
TEST(cbr_rb_refreeze_shares_single_region) {
    if (cbr_force_copy_active()) return;
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    LatVec tokens;
    Program prog;
    Evaluator *ev = cbr_rb_eval("fix g = [4, 5, \"refreeze single region long string 0123456789\"]\n"
                                "let g2 = freeze(g)\n",
                                &tokens, &prog);
    ASSERT(ev != NULL);

    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    LatValue vg, vg2;
    ASSERT(env_get(ev->env, "g", &vg));
    ASSERT(env_get(ev->env, "g2", &vg2));
    ASSERT(REGION_IS_SHARED_ID(vg.region_id));
    ASSERT(REGION_PTR(vg.region_id) == REGION_PTR(vg2.region_id));
    value_free(&vg);
    value_free(&vg2);

    cbr_rb_cleanup(ev, &tokens, &prog);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* RED until Round B: regions created by freezes inside a function body are
 * refcount-released the moment the bindings go out of scope — no GC cycle
 * needed (R28: region_collect is demoted to a leak detector). */
TEST(cbr_rb_scope_exit_releases_region) {
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    LatVec tokens;
    Program prog;
    Evaluator *ev = cbr_rb_eval("fn work() {\n"
                                "    fix local = [1, 2, \"scope exit release long string 0123456789\"]\n"
                                "    let alias = local\n"
                                "}\n"
                                "fn main() {\n"
                                "    work()\n"
                                "    work()\n"
                                "}\n",
                                &tokens, &prog);
    ASSERT(ev != NULL);

    /* Two calls -> two regions created; both released at scope exit while
     * the evaluator is still alive. */
    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 2);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);

    cbr_rb_cleanup(ev, &tokens, &prog);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* GREEN today AND after the switch (R8 numeric-id-retirement pin): an
 * UNSHAREABLE freeze (struct holding a closure) must fall back to a bare
 * tag-flip crystal — no shared region, and no handle the evaluator hands
 * out may carry a region_id satisfying the shared-bit predicate. Today this
 * holds only because env_get's deep clone resets region_id to REGION_NONE;
 * the legacy binding itself carries an odd RegionManager id (1, 3, ...)
 * that would trip REGION_IS_SHARED_ID — the transitional hazard C1 forbids.
 * Post-switch env_get aliases instead of cloning, so this test then directly
 * guards that unshareable values are never regionized or tagged shared. */
TEST(cbr_rb_unshareable_fallback_no_shared_bit) {
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    LatVec tokens;
    Program prog;
    Evaluator *ev = cbr_rb_eval("struct Counter { n: Int, bump: Fn }\n"
                                "fix g = Counter { n: 1, bump: |x| { x } }\n",
                                &tokens, &prog);
    ASSERT(ev != NULL);

    LatValue vg;
    ASSERT(env_get(ev->env, "g", &vg));
    ASSERT(vg.phase == VTAG_CRYSTAL);
    /* No numeric region id may ever satisfy the shared predicate. */
    ASSERT(!REGION_IS_SHARED_ID(vg.region_id));
    value_free(&vg);

    /* Unshareable freeze never touches the shared registry. */
    ASSERT_EQ_INT(crystal_region_created_total(), base_created);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);

    cbr_rb_cleanup(ev, &tokens, &prog);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* RED until Round B: end-to-end freeze/alias/thaw churn is rc-balanced —
 * per-iteration regions die with their bindings, persistent globals keep
 * exactly one region live, and teardown empties the registry (exercises the
 * memory.c rc-ledger assert on every release). */
TEST(cbr_rb_churn_rc_balanced_teardown_empty) {
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    LatVec tokens;
    Program prog;
    Evaluator *ev = cbr_rb_eval("fix a = [1, 2, 3, \"persistent global long string 0123456789\"]\n"
                                "let b = a\n"
                                "fn churn(n: Int) -> Int {\n"
                                "    flux total = 0\n"
                                "    for i in 0..n {\n"
                                "        fix local = [i, i + 1, \"churn local long string 0123456789\"]\n"
                                "        let alias = local\n"
                                "        flux t = thaw(alias)\n"
                                "        total = total + t[0]\n"
                                "    }\n"
                                "    return total\n"
                                "}\n"
                                "fn main() {\n"
                                "    let r = churn(10)\n"
                                "    assert(r == 45, \"churn wrong\")\n"
                                "}\n",
                                &tokens, &prog);
    ASSERT(ev != NULL);

    /* 1 persistent region (a/b) + 10 churn iterations created-and-released. */
    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 11);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    cbr_rb_cleanup(ev, &tokens, &prog);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* ══════════════════════════════════════════════════════════════════════════
 * LAT-449 Round B hardening pass: rc-observing matrix tests that could not
 * be written pre-switch (forge/grow/bond region accounting, --no-regions
 * baseline, REPL persistence, channel teardown, leak-detector advisory).
 * ══════════════════════════════════════════════════════════════════════════ */

/* (e) EXPR_FORGE result freeze materializes exactly one shared region with
 * a correct rc; teardown releases it. */
TEST(cbr_rb_forge_result_shared_region) {
    if (cbr_force_copy_active()) return; /* asserts handle-tag mechanics */
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    LatVec tokens;
    Program prog;
    Evaluator *ev = cbr_rb_eval("let r = forge {\n"
                                "    flux acc = []\n"
                                "    acc.push(\"forge region long padding string 0123456789\")\n"
                                "    acc.push(42)\n"
                                "    acc\n"
                                "}\n"
                                "let ralias = r\n",
                                &tokens, &prog);
    ASSERT(ev != NULL);

    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    LatValue vr;
    ASSERT(env_get(ev->env, "r", &vr));
    ASSERT(vr.phase == VTAG_CRYSTAL);
    ASSERT(REGION_IS_SHARED_ID(vr.region_id));
    value_free(&vr);

    cbr_rb_cleanup(ev, &tokens, &prog);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* (e) grow() (seed-validated freeze) materializes a shared region for a
 * shareable container; teardown releases. */
TEST(cbr_rb_grow_seed_freeze_shared_region) {
    if (cbr_force_copy_active()) return; /* asserts handle-tag mechanics */
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    LatVec tokens;
    Program prog;
    Evaluator *ev = cbr_rb_eval("flux cfg = [8080, \"grow region long padding string 0123456789\"]\n"
                                "seed(cfg, |v| { v[0] > 0 })\n"
                                "grow(\"cfg\")\n",
                                &tokens, &prog);
    ASSERT(ev != NULL);

    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    LatValue vc;
    ASSERT(env_get(ev->env, "cfg", &vc));
    ASSERT(vc.phase == VTAG_CRYSTAL);
    ASSERT(REGION_IS_SHARED_ID(vc.region_id));
    value_free(&vc);

    cbr_rb_cleanup(ev, &tokens, &prog);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* (b) bond mirror cascade region accounting: cascading onto a shareable
 * container dep materializes exactly ONE region for it; a scalar target and
 * an unshareable (closure) dep never touch the registry; refreezing an
 * already-crystal dep short-circuits (no extra region). */
TEST(cbr_rb_bond_mirror_cascade_regions) {
    if (cbr_force_copy_active()) return; /* asserts handle-tag/aliasing mechanics */
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    LatVec tokens;
    Program prog;
    Evaluator *ev = cbr_rb_eval("flux t = 1\n"
                                "flux dep = [7, 8, \"bond cascade region long padding string 0123456789\"]\n"
                                "flux f = |x| { x }\n"
                                "bond(t, dep)\n"
                                "bond(t, f)\n"
                                "freeze(t)\n"
                                "let again = freeze(dep)\n",
                                &tokens, &prog);
    ASSERT(ev != NULL);

    /* Exactly one region: dep (shareable container). t is a scalar (below
     * threshold, legacy tag flip), f is a closure (unshareable), and the
     * explicit refreeze of the cascaded dep is idempotent. */
    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    LatValue vd, va;
    ASSERT(env_get(ev->env, "dep", &vd));
    ASSERT(env_get(ev->env, "again", &va));
    ASSERT(vd.phase == VTAG_CRYSTAL);
    ASSERT(REGION_IS_SHARED_ID(vd.region_id));
    /* Refreeze aliased the same region. */
    ASSERT(REGION_PTR(vd.region_id) == REGION_PTR(va.region_id));
    LatValue vf;
    ASSERT(env_get(ev->env, "f", &vf));
    ASSERT(vf.phase == VTAG_CRYSTAL);
    ASSERT(!REGION_IS_SHARED_ID(vf.region_id)); /* unshareable: legacy crystal */
    value_free(&vd);
    value_free(&va);
    value_free(&vf);

    cbr_rb_cleanup(ev, &tokens, &prog);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* (R38) --no-regions baseline: the registry is never touched, freeze is the
 * legacy tag flip (region_id == REGION_NONE), and aliasing semantics are
 * v0.4-identical (asserted inside the script). */
TEST(cbr_rb_no_regions_registry_untouched) {
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    LatVec tokens;
    Program prog;
    Evaluator *ev = cbr_rb_eval_opt("fix a = [1, 2, 3, \"no-regions long padding string 0123456789\"]\n"
                                    "let b = a\n"
                                    "flux t = thaw(b)\n"
                                    "fn main() {\n"
                                    "    t[0] = 99\n"
                                    "    assert(a[0] == 1, \"a mutated\")\n"
                                    "    assert(b[0] == 1, \"b mutated\")\n"
                                    "    assert(phase_of(a) == \"crystal\", \"a not crystal\")\n"
                                    "}\n",
                                    &tokens, &prog, /*no_regions=*/true);
    ASSERT(ev != NULL);

    ASSERT_EQ_INT(crystal_region_created_total(), base_created);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);

    LatValue va;
    ASSERT(env_get(ev->env, "a", &va));
    ASSERT(va.phase == VTAG_CRYSTAL);
    ASSERT(!REGION_IS_SHARED_ID(va.region_id)); /* legacy tag-flip crystal */
    value_free(&va);

    cbr_rb_cleanup(ev, &tokens, &prog);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* (d) REPL persistence: a persistent evaluator across four separate
 * programs — freeze on "line" 1, alias on line 2, thaw+mutate on line 3,
 * original intact on line 4; registry balanced at teardown. */
TEST(cbr_rb_repl_persistence_across_programs) {
    size_t base_live = crystal_region_live_count();

    const char *lines[4] = {
        "fix d = [1, 2, 3, \"repl persistent long padding string 0123456789\"]\n",
        "let a = d\n",
        "flux t = thaw(a)\nt[0] = 99\n",
        "assert(d[0] == 1, \"d mutated across REPL lines\")\n"
        "assert(a[0] == 1, \"alias mutated across REPL lines\")\n"
        "assert(t[0] == 99, \"thawed write lost\")\n",
    };
    LatVec tokens[4];
    Program progs[4];
    Evaluator *ev = evaluator_new();
    for (int i = 0; i < 4; i++) {
        ASSERT(cbr_rb_parse(lines[i], &tokens[i], &progs[i]));
        char *err = evaluator_run(ev, &progs[i]);
        if (err) fprintf(stderr, "  repl line %d error: %s\n", i + 1, err);
        ASSERT(err == NULL);
    }

    /* One region (d/a alias it); still live while the evaluator persists. */
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    evaluator_free(ev);
    for (int i = 0; i < 4; i++) {
        program_free(&progs[i]);
        for (size_t j = 0; j < tokens[i].len; j++) token_free(lat_vec_get(&tokens[i], j));
        lat_vec_free(&tokens[i]);
    }
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* (f) buffered-channel teardown with shared crystals: a sender thread exits
 * before the receiver drains; some frozen containers die IN the channel
 * buffer at channel teardown, the drained one dies with its binding. The
 * registry must return to baseline either way (cross-thread rc balance). */
TEST(cbr_rb_channel_teardown_releases_shared) {
    size_t base_live = crystal_region_live_count();

    LatVec tokens;
    Program prog;
    Evaluator *ev = cbr_rb_eval("fn main() {\n"
                                "    let ch = Channel::new(4)\n"
                                "    scope {\n"
                                "        spawn {\n"
                                "            fix one = [1, \"channel teardown long padding string 0123456789\"]\n"
                                "            fix two = [2, \"channel teardown long padding string 0123456789\"]\n"
                                "            fix three = [3, \"channel teardown long padding string 0123456789\"]\n"
                                "            ch.send(one)\n"
                                "            ch.send(two)\n"
                                "            ch.send(three)\n"
                                "        }\n"
                                "    }\n"
                                "    let got = ch.recv()\n"
                                "    assert(got[0] == 1, \"recv wrong\")\n"
                                "}\n",
                                &tokens, &prog);
    ASSERT(ev != NULL);

    /* main() returned: bindings dead, channel collected; nothing pinned. */
    cbr_rb_cleanup(ev, &tokens, &prog);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* (h) leak-detector advisory (R28): a live region absent from a reachable
 * set is COUNTED but never freed — the report is advisory only. */
TEST(cbr_rb_report_unreachable_is_advisory) {
    size_t base_live = crystal_region_live_count();

    LatValue arr = cbr_make_array();
    ASSERT(value_freeze_to_region(&arr));
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    /* Empty reachable set: our region (at least) is reported... */
    size_t unreachable = crystal_region_report_unreachable(NULL, 0, NULL);
    ASSERT(unreachable >= 1);
    /* ...but NOT freed: the handle is still valid and rc-owned. */
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);
    ASSERT(arr.as.array.len == 2);

    /* With the region's tagged id in the reachable set it is not counted. */
    size_t tag = arr.region_id;
    size_t still = crystal_region_report_unreachable(&tag, 1, NULL);
    ASSERT(still == unreachable - 1);

    value_free(&arr);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}
