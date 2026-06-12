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
 * These tests exercise the shared-region primitives directly (no VM).
 * Since Round B the tree-walker mints shared handles on every whole-binding
 * freeze, and since Stage 3 the stack VM does too — the invariant that
 * still matters here: every test releases all of the regions it creates
 * before returning (registry back to baseline).
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

/* ══════════════════════════════════════════════════════════════════════════
 * LAT-452 / Stage 3: STACK VM rc/lifecycle tests (TDD — RED until the
 * Stage 3 stack-VM switch lands)
 *
 * These run Lattice source through the STACK VM directly (stack_compile +
 * stackvm_run, regardless of the --backend the suite was invoked with) and
 * assert the Stage 3 contract against the shared-region registry:
 *
 *   - OP_FREEZE / OP_FREEZE_VAR of a shareable container MATERIALIZES
 *     exactly one shared region (created_total/live_count advance);
 *   - aliasing through GET_GLOBAL/GET_LOCAL/call args is an O(1) retain —
 *     never a second region (value_clone_fast borrow branch, S3-R1);
 *   - channel send of a shared crystal is +1 retain per buffered handle,
 *     not a copy (S3-R10); teardown of an undrained channel releases;
 *   - spawn/scope export retains and the joined threads release (S3-R11);
 *   - steal-pattern extraction and OP_APPEND_STR_LOCAL never pin a region
 *     (S3-R4/R7);
 *   - stackvm_free returns the registry to baseline (C2/C4 rc balance).
 *
 * Until the switch, the VM's freeze path is a bare value_freeze tag flip
 * that never touches the shared registry, so the created/live-count
 * assertions fail first (cleanly — REGION_PTR dereferences are sequenced
 * after them and never execute on pre-Stage-3 code).
 *
 * Companion semantics pins (GREEN before AND after) live in test_eval.c
 * under the cbr_s3_pin_* banner. Mirrors the cbr_rb_* tree-walker block
 * above, including the cbr_force_copy_active() self-skip convention for
 * tests that assert aliasing MECHANICS (retain counts, handle identity).
 * ══════════════════════════════════════════════════════════════════════════ */

#include "stackcompiler.h"
#include "stackvm.h"
#include "runtime.h"

/* A live stack-VM run: kept alive so tests can inspect vm.env globals and
 * registry state while root bindings still hold their retains. */
typedef struct {
    LatVec tokens;
    Program prog;
    Chunk *chunk;
    LatRuntime rt;
    StackVM vm;
} CbrS3VM;

/* Run source through the stack VM. Returns false (with everything torn
 * down) on any compile/runtime error. On success the VM stays live until
 * cbr_s3_vm_cleanup. no_regions mirrors the --no-regions CLI baseline flag
 * (rt.no_regions in main.c). */
static bool cbr_s3_vm_run_opt(CbrS3VM *s, const char *source, bool no_regions) {
    if (!cbr_rb_parse(source, &s->tokens, &s->prog)) return false;

    /* The VM backend never runs with a tree-walker TLS heap/arena. */
    value_set_heap(NULL);
    value_set_arena(NULL);

    char *comp_err = NULL;
    s->chunk = stack_compile(&s->prog, &comp_err);
    if (!s->chunk) {
        fprintf(stderr, "  cbr_s3_vm compile error: %s\n", comp_err ? comp_err : "(unknown)");
        free(comp_err);
        program_free(&s->prog);
        for (size_t i = 0; i < s->tokens.len; i++) token_free(lat_vec_get(&s->tokens, i));
        lat_vec_free(&s->tokens);
        return false;
    }

    lat_runtime_init(&s->rt);
    s->rt.no_regions = no_regions;
    stackvm_init(&s->vm, &s->rt);
    LatValue result;
    StackVMResult res = stackvm_run(&s->vm, s->chunk, &result);
    if (res != STACKVM_OK) {
        fprintf(stderr, "  cbr_s3_vm runtime error: %s\n", s->vm.error ? s->vm.error : "(unknown)");
        stackvm_free(&s->vm);
        lat_runtime_free(&s->rt);
        chunk_free(s->chunk);
        program_free(&s->prog);
        for (size_t i = 0; i < s->tokens.len; i++) token_free(lat_vec_get(&s->tokens, i));
        lat_vec_free(&s->tokens);
        return false;
    }
    value_free(&result);
    return true;
}

static bool cbr_s3_vm_run(CbrS3VM *s, const char *source) { return cbr_s3_vm_run_opt(s, source, false); }

static void cbr_s3_vm_cleanup(CbrS3VM *s) {
    stackvm_free(&s->vm);
    lat_runtime_free(&s->rt);
    chunk_free(s->chunk);
    program_free(&s->prog);
    for (size_t i = 0; i < s->tokens.len; i++) token_free(lat_vec_get(&s->tokens, i));
    lat_vec_free(&s->tokens);
}

/* RED until Stage 3: a top-level fix binding of a shareable container on the
 * stack VM materializes exactly one live shared region (OP_FREEZE ->
 * value_freeze_to_region, S3-R2); stackvm_free releases the global's
 * retain and returns the registry to baseline. */
TEST(cbr_s3_vm_freeze_materializes_live_region) {
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    CbrS3VM s;
    ASSERT(cbr_s3_vm_run(&s, "fix g = [1, 2, 3, \"vm freeze materialize padded string 0123456789 0123456789\"]\n"));

    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    cbr_s3_vm_cleanup(&s);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* RED until Stage 3: global aliases of a shared crystal are retains of the
 * SAME region (one region total); env_get handles borrow (S3-R1) and the
 * rc ledger balances when they are released. */
TEST(cbr_s3_vm_alias_is_retain_not_new_region) {
    if (cbr_force_copy_active()) return; /* asserts aliasing mechanics */
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    CbrS3VM s;
    ASSERT(cbr_s3_vm_run(&s, "fix g = [1, 2, 3, \"vm alias retain padded string 0123456789 0123456789\"]\n"
                             "let h = g\n"
                             "let k = h\n"));

    /* Three global bindings, ONE region. */
    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    LatValue vg, vh;
    ASSERT(env_get(s.vm.env, "g", &vg));
    ASSERT(env_get(s.vm.env, "h", &vh));
    ASSERT(REGION_IS_SHARED_ID(vg.region_id));
    ASSERT(REGION_IS_SHARED_ID(vh.region_id));
    ASSERT(REGION_PTR(vg.region_id) == REGION_PTR(vh.region_id));
    ASSERT(vg.phase == VTAG_CRYSTAL);

    CrystalRegion *r = REGION_PTR(vg.region_id);
    size_t rc_with_handles = crystal_region_refcount(r);
    /* At least the 3 global bindings + our 2 test handles. */
    ASSERT(rc_with_handles >= 5);
    value_free(&vg);
    value_free(&vh);
    ASSERT_EQ_INT(crystal_region_refcount(r), rc_with_handles - 2);

    cbr_s3_vm_cleanup(&s);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* RED until Stage 3: regions minted by fix bindings inside a function frame
 * are released when the frame's slots are popped (eager value_free
 * discipline) — no region survives main()'s return. */
TEST(cbr_s3_vm_scope_exit_releases_region) {
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    CbrS3VM s;
    ASSERT(cbr_s3_vm_run(&s, "fn work() {\n"
                             "    fix local = [1, 2, \"vm scope exit padded string 0123456789 0123456789\"]\n"
                             "    let alias = local\n"
                             "}\n"
                             "fn main() {\n"
                             "    work()\n"
                             "    work()\n"
                             "}\n"));

    /* Two calls -> two regions created; both released at frame exit while
     * the VM is still alive. */
    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 2);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);

    cbr_s3_vm_cleanup(&s);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* RED until Stage 3 (S3-R4): steal-pattern extraction in a loop — struct
 * field, tuple member, map value and array element pulled out of a frozen
 * global through dying stack temporaries — must be rc-balanced: exactly one
 * region, rc back to (binding + our handle) after the run, registry at
 * baseline after teardown. A missed steal guard over-releases (ASan/ledger
 * abort) or leaks retains (rc inflated, region pinned past cleanup). */
TEST(cbr_s3_vm_steal_extraction_rc_balanced) {
    if (cbr_force_copy_active()) return; /* asserts retain-count mechanics */
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    CbrS3VM s;
    ASSERT(cbr_s3_vm_run(&s,
                         "struct Cfg { host: String, items: any }\n"
                         "fix g = Cfg { host: \"vm steal rc padded string 0123456789 0123456789\", items: [1, 2, 3] }\n"
                         "fn main() {\n"
                         "    flux acc = 0\n"
                         "    for i in 0..20 {\n"
                         "        let h = g.host\n"
                         "        let it = g.items\n"
                         "        let e = g.items[0]\n"
                         "        acc = acc + it[1] + e + h.len()\n"
                         "    }\n"
                         "    assert(acc == 20 * (2 + 1 + g.host.len()), \"steal loop sum wrong: ${acc}\")\n"
                         "}\n"));

    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    LatValue vg;
    ASSERT(env_get(s.vm.env, "g", &vg));
    ASSERT(REGION_IS_SHARED_ID(vg.region_id));
    CrystalRegion *r = REGION_PTR(vg.region_id);
    /* Exactly the global binding + our handle: nothing extracted in the
     * loop may still hold a retain. */
    ASSERT_EQ_INT(crystal_region_refcount(r), 2);
    value_free(&vg);

    cbr_s3_vm_cleanup(&s);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* RED until Stage 3 (S3-R10): channel send of a shared crystal is +1 retain
 * per buffered handle, NOT a deep copy; an undrained channel torn down at
 * VM free releases its buffered retains (rc balance with zero recvs). The
 * channel is a global so the buffered handles are still live after the run. */
TEST(cbr_s3_vm_channel_send_is_retain_then_teardown_releases) {
    if (cbr_force_copy_active()) return; /* asserts retain-count mechanics */
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    CbrS3VM s;
    ASSERT(cbr_s3_vm_run(&s, "fix g = [1, 2, 3, \"vm channel retain padded string 0123456789 0123456789\"]\n"
                             "let ch = Channel::new(2)\n"
                             "fn main() {\n"
                             "    ch.send(g)\n"
                             "    ch.send(g)\n"
                             "}\n"));

    /* One region; two sends buffered, never received. */
    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    LatValue vg;
    ASSERT(env_get(s.vm.env, "g", &vg));
    ASSERT(REGION_IS_SHARED_ID(vg.region_id));
    CrystalRegion *r = REGION_PTR(vg.region_id);
    /* global binding + 2 buffered handles + our handle = 4 retains. A
     * deep-copy send would show exactly 2 here. */
    ASSERT_EQ_INT(crystal_region_refcount(r), 4);
    value_free(&vg);

    /* Teardown frees the channel global -> channel_release value_frees the
     * buffered handles -> registry back to baseline. */
    cbr_s3_vm_cleanup(&s);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* RED until Stage 3 (S3-R11): spawn/scope export of a frozen global retains;
 * joined threads release. After the scope, the only retains left are the
 * global binding (+ our probe handle); thread teardown on the child side
 * must be rc-balanced even though releases run on other threads. */
TEST(cbr_s3_vm_spawn_export_retains_and_releases) {
    if (cbr_force_copy_active()) return; /* asserts retain-count mechanics */
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    CbrS3VM s;
    ASSERT(cbr_s3_vm_run(&s, "fix dataset = [1, 1, 1, \"vm spawn export padded string 0123456789 0123456789\"]\n"
                             "fn main() {\n"
                             "    let ch = Channel::new(3)\n"
                             "    scope {\n"
                             "        spawn { ch.send(dataset[0]) }\n"
                             "        spawn { ch.send(dataset[1]) }\n"
                             "        spawn { ch.send(dataset[2]) }\n"
                             "    }\n"
                             "    let total = ch.recv() + ch.recv() + ch.recv()\n"
                             "    assert(total == 3, \"spawn reader sum wrong\")\n"
                             "}\n"));

    /* Reading a frozen dataset from N threads creates ONE region, ever. */
    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    LatValue vd;
    ASSERT(env_get(s.vm.env, "dataset", &vd));
    ASSERT(REGION_IS_SHARED_ID(vd.region_id));
    CrystalRegion *r = REGION_PTR(vd.region_id);
    /* Global binding + our handle only: every spawn-export retain was
     * released when its thread was torn down. */
    ASSERT_EQ_INT(crystal_region_refcount(r), 2);
    value_free(&vd);

    cbr_s3_vm_cleanup(&s);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* RED until Stage 3 (S3-R10/C4 sender-death ordering): a spawned thread
 * freezes containers, sends them, and dies (its FluidHeap torn down) before
 * the parent drains one of three; the remaining buffered handles die with
 * the channel at main()'s return. live_count must already be back to
 * baseline BEFORE VM teardown — nothing may pin a region. */
TEST(cbr_s3_vm_sender_death_then_partial_drain) {
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    CbrS3VM s;
    ASSERT(cbr_s3_vm_run(&s, "fn main() {\n"
                             "    let ch = Channel::new(4)\n"
                             "    scope {\n"
                             "        spawn {\n"
                             "            fix one = [1, \"vm sender death padded string 0123456789 0123456789\"]\n"
                             "            fix two = [2, \"vm sender death padded string 0123456789 0123456789\"]\n"
                             "            fix three = [3, \"vm sender death padded string 0123456789 0123456789\"]\n"
                             "            ch.send(one)\n"
                             "            ch.send(two)\n"
                             "            ch.send(three)\n"
                             "        }\n"
                             "    }\n"
                             "    let got = ch.recv()\n"
                             "    assert(got[0] == 1, \"recv wrong\")\n"
                             "}\n"));

    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 3);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);

    cbr_s3_vm_cleanup(&s);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* RED until Stage 3 (S3-R7): OP_APPEND_STR_LOCAL fed from a shared frozen
 * string must privatize without leaking the retain — a missed release in
 * the append handler's region-owned branch pins the region forever (rc
 * inflated while live, registry not at baseline after teardown). */
TEST(cbr_s3_vm_append_str_local_does_not_pin_region) {
    if (cbr_force_copy_active()) return; /* asserts retain-count mechanics */
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    CbrS3VM s;
    ASSERT(cbr_s3_vm_run(&s, "fix g = \"vm append no-pin base string padded 0123456789 0123456789\"\n"
                             "fn main() {\n"
                             "    flux total = 0\n"
                             "    for i in 0..10 {\n"
                             "        flux st = g\n"
                             "        st += \"x\"\n"
                             "        total = total + st.len()\n"
                             "    }\n"
                             "    assert(total == 10 * (g.len() + 1), \"append totals wrong\")\n"
                             "}\n"));

    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    LatValue vg;
    ASSERT(env_get(s.vm.env, "g", &vg));
    ASSERT(REGION_IS_SHARED_ID(vg.region_id));
    CrystalRegion *r = REGION_PTR(vg.region_id);
    /* Global binding + our handle: the 10 appended locals must each have
     * released their borrow. */
    ASSERT_EQ_INT(crystal_region_refcount(r), 2);
    value_free(&vg);

    cbr_s3_vm_cleanup(&s);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* RED until Stage 3 (S3-R12): select arms receiving shared crystals — the
 * fired arm's binding takes ownership of the buffered retain (handle
 * transfer, no extra rc traffic) and the arm scope releases it; default arm
 * paths leave no received-but-unbound handles. Registry at baseline before
 * and after teardown. */
TEST(cbr_s3_vm_select_arm_handle_transfer_rc_balanced) {
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    CbrS3VM s;
    ASSERT(cbr_s3_vm_run(&s, "fix payload = [7, 8, \"vm select transfer padded string 0123456789 0123456789\"]\n"
                             "fn main() {\n"
                             "    let ch = Channel::new(2)\n"
                             "    ch.send(payload)\n"
                             "    let got = select {\n"
                             "        v from ch => { v[0] }\n"
                             "    }\n"
                             "    assert(got == 7, \"select recv wrong\")\n"
                             "    let dflt = select {\n"
                             "        v from ch => { v[0] }\n"
                             "        default => { -1 }\n"
                             "    }\n"
                             "    assert(dflt == -1, \"default arm not taken\")\n"
                             "    ch.send(payload)\n"
                             "    let timed = select {\n"
                             "        v from ch => { v[1] }\n"
                             "        timeout(50) => { -2 }\n"
                             "    }\n"
                             "    assert(timed == 8, \"ready arm beat timeout\")\n"
                             "}\n"));

    /* One region (the payload); both selected handles released with their
     * arm scopes. */
    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    cbr_s3_vm_cleanup(&s);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* LAT-452 thread-stress (S3-R15 pulled forward / work item 4): six threads
 * churn ONE shared region's atomic rc through the full VM concurrency
 * surface — 4 sender spawns alias the frozen dataset and send the crystal
 * itself through a channel 25 times each (100 retain+enqueue transfers),
 * while 2 receiver spawns concurrently drain, index, and drop the handles
 * (releases on threads that never created the region). After the scope
 * joins, rc must have drained back to exactly the global binding (+ our
 * probe); registry to baseline after teardown. Under ASan this is the C4
 * safety-story test: any over/under-release on the cross-thread paths
 * (send detach, recv move, iteration-scoped frees, child env teardown)
 * surfaces here. */
TEST(cbr_s3_vm_thread_stress_channel_churn_rc_drains) {
    if (cbr_force_copy_active()) return; /* asserts retain-count mechanics */
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    CbrS3VM s;
    ASSERT(cbr_s3_vm_run(&s, "fix dataset = [3, 4, \"vm thread stress padded string 0123456789 0123456789\"]\n"
                             "let done = Channel::new(2)\n"
                             "fn sender(ch: any) {\n"
                             "    for i in 0..25 {\n"
                             "        let a = dataset\n"
                             "        ch.send(a)\n"
                             "    }\n"
                             "}\n"
                             "fn receiver(ch: any) {\n"
                             "    flux acc = 0\n"
                             "    for i in 0..50 {\n"
                             "        let v = ch.recv()\n"
                             "        acc = acc + v[0]\n"
                             "    }\n"
                             "    done.send(acc)\n"
                             "}\n"
                             "fn main() {\n"
                             "    let ch = Channel::new(128)\n"
                             "    scope {\n"
                             "        spawn { sender(ch) }\n"
                             "        spawn { sender(ch) }\n"
                             "        spawn { sender(ch) }\n"
                             "        spawn { sender(ch) }\n"
                             "        spawn { receiver(ch) }\n"
                             "        spawn { receiver(ch) }\n"
                             "    }\n"
                             "    let total = done.recv() + done.recv()\n"
                             "    assert(total == 100 * dataset[0], \"stress sum wrong: ${total}\")\n"
                             "}\n"));

    /* 100 cross-thread transfers of one dataset: ONE region, ever. */
    ASSERT_EQ_INT(crystal_region_created_total(), base_created + 1);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live + 1);

    LatValue vd;
    ASSERT(env_get(s.vm.env, "dataset", &vd));
    ASSERT(REGION_IS_SHARED_ID(vd.region_id));
    CrystalRegion *r = REGION_PTR(vd.region_id);
    /* Global binding + our probe handle: every sender alias, buffered
     * handle, receiver binding and spawn-export retain has drained. */
    ASSERT_EQ_INT(crystal_region_refcount(r), 2);
    value_free(&vd);

    cbr_s3_vm_cleanup(&s);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include "channel.h"

/* LAT-452 C-level channel teardown stress (S3-R10/C4): N pthreads
 * concurrently retain+send aliases of one shared crystal into a channel
 * that is never drained, then the channel's LAST reference is released on
 * yet another child thread — the buffered crystal handles are freed
 * entirely off the thread that minted the region (channel_release ->
 * value_free -> tag-keyed crystal_region_release, atomic). Exact rc
 * arithmetic: each send is clone(+1) + detach-borrow(+1) + free(-1) =
 * one buffer-owned retain. */
#define CBR_S3_STRESS_SENDERS 4
#define CBR_S3_STRESS_SENDS   64

typedef struct {
    LatChannel *ch;
    const LatValue *handle;
} CbrS3SendCtx;

static void *cbr_s3_sender_thread(void *arg) {
    CbrS3SendCtx *ctx = arg;
    for (int i = 0; i < CBR_S3_STRESS_SENDS; i++) {
        LatValue alias = value_deep_clone(ctx->handle); /* borrow: retain */
        channel_send(ctx->ch, alias);                   /* detach borrows (+1), frees alias (-1) */
    }
    return NULL;
}

static void *cbr_s3_release_thread(void *arg) {
    channel_release((LatChannel *)arg); /* last ref: buffered teardown HERE */
    return NULL;
}

TEST(cbr_s3_channel_abandoned_buffered_release_on_child_thread) {
    if (cbr_force_copy_active()) return; /* asserts retain-count mechanics */
    size_t base_live = crystal_region_live_count();

    LatValue arr = cbr_make_array();
    ASSERT(value_freeze_to_region(&arr));
    ASSERT(REGION_IS_SHARED_ID(arr.region_id));
    CrystalRegion *r = REGION_PTR(arr.region_id);
    ASSERT_EQ_INT(crystal_region_refcount(r), 1);

    LatChannel *ch = channel_new();
    ASSERT(ch != NULL);

    pthread_t senders[CBR_S3_STRESS_SENDERS];
    CbrS3SendCtx ctx = {ch, &arr};
    for (int i = 0; i < CBR_S3_STRESS_SENDERS; i++) pthread_create(&senders[i], NULL, cbr_s3_sender_thread, &ctx);
    for (int i = 0; i < CBR_S3_STRESS_SENDERS; i++) pthread_join(senders[i], NULL);

    /* Our handle + one retain per buffered (never-drained) handle. */
    ASSERT_EQ_INT(crystal_region_refcount(r), 1 + CBR_S3_STRESS_SENDERS * CBR_S3_STRESS_SENDS);

    /* Abandon the channel: hand its last ref to a child thread. */
    pthread_t releaser;
    pthread_create(&releaser, NULL, cbr_s3_release_thread, ch);
    pthread_join(releaser, NULL);

    /* Every buffered retain released off-thread; only ours remains. */
    ASSERT_EQ_INT(crystal_region_refcount(r), 1);
    value_free(&arr);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}
#endif /* !_WIN32 && !__EMSCRIPTEN__ */

/* GREEN today AND after Stage 3 (S3-R13 boundary pin, unshareable side):
 * an UNSHAREABLE fix binding (struct holding a closure) on the stack VM
 * stays a legacy tag-flip crystal — no region is ever created, and no
 * handle the VM hands out may satisfy the shared-bit predicate. Mirrors
 * cbr_rb_unshareable_fallback_no_shared_bit on the tree-walker. */
TEST(cbr_s3_vm_unshareable_fallback_no_shared_bit) {
    size_t base_live = crystal_region_live_count();
    size_t base_created = crystal_region_created_total();

    CbrS3VM s;
    ASSERT(cbr_s3_vm_run(&s, "struct Counter { n: Int, bump: Fn }\n"
                             "fix g = Counter { n: 1, bump: |x| { x } }\n"));

    LatValue vg;
    ASSERT(env_get(s.vm.env, "g", &vg));
    ASSERT(vg.phase == VTAG_CRYSTAL);
    ASSERT(!REGION_IS_SHARED_ID(vg.region_id));
    value_free(&vg);

    ASSERT_EQ_INT(crystal_region_created_total(), base_created);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);

    cbr_s3_vm_cleanup(&s);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

/* LAT-452 follow-up regression pin (RED before the unwind-release fix):
 * exception-unwind paths that reset vm->stack_top past live slots
 * (stackvm_handle_error / stackvm_handle_native_error / OP_THROW caught
 * branch / OP_TRY_UNWRAP err branch / stackvm_call_closure error path)
 * abandoned those slots without value_free — every abandoned shared-crystal
 * handle orphaned a region retain, pinning the region forever. Loop all
 * three unwind shapes (catch-mid-expression, callee-frame unwind,
 * `?`-propagation) with shared aliases live at unwind time: the registry
 * must be back at baseline as soon as main() returns, and stay there after
 * teardown. Pre-fix this pinned the region with rc in the hundreds. */
TEST(cbr_s3_vm_exception_unwind_releases_abandoned_slots) {
    if (cbr_force_copy_active()) return; /* asserts retain/release mechanics */
    size_t base_live = crystal_region_live_count();

    CbrS3VM s;
    ASSERT(cbr_s3_vm_run(&s, "fn risky(a: any) {\n"
                             "    let alias = a\n"
                             "    let boom = [1][5]\n"
                             "    return alias\n"
                             "}\n"
                             "fn err_result() {\n"
                             "    flux m = Map::new()\n"
                             "    m[\"tag\"] = \"err\"\n"
                             "    m[\"error\"] = \"nope\"\n"
                             "    return m\n"
                             "}\n"
                             "fn unwrapper(a: any) {\n"
                             "    let alias = a\n"
                             "    let v = err_result()?\n"
                             "    return v\n"
                             "}\n"
                             "fn main() {\n"
                             "    let pad = \"unwind padding string 0123456789 0123456789 0123456789\"\n"
                             "    fix shared = [pad, pad, pad, pad]\n"
                             "    flux i = 0\n"
                             "    while i < 60 {\n"
                             "        try {\n"
                             "            let pair = [shared, [9][9]]\n"
                             "        } catch e {\n"
                             "        }\n"
                             "        try {\n"
                             "            let r = risky(shared)\n"
                             "        } catch e {\n"
                             "        }\n"
                             "        let u = unwrapper(shared)\n"
                             "        i = i + 1\n"
                             "    }\n"
                             "}\n"));

    /* main() returned and `shared` (a main local) was released at frame
     * exit: no abandoned-slot orphan may keep the region alive. */
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);

    cbr_s3_vm_cleanup(&s);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
/* LAT-452 follow-up (--no-regions inheritance): spawned children are built
 * by stackvm_clone_for_thread (spawn/scope in stackvm.c AND async_iter via
 * runtime.c) with a fresh calloc'd LatRuntime — before the fix the
 * parent's no_regions flag was not copied, so a `fix` inside `spawn`
 * regionized even under --no-regions baseline mode. Pin: with no_regions
 * set, a child-side freeze must create ZERO shared regions. */
TEST(cbr_s3_vm_no_regions_inherited_by_spawned_children) {
    size_t base_created = crystal_region_created_total();
    size_t base_live = crystal_region_live_count();

    CbrS3VM s;
    ASSERT(cbr_s3_vm_run_opt(
        &s,
        "fn main() {\n"
        "    let ch = Channel::new(2)\n"
        "    scope {\n"
        "        spawn {\n"
        "            fix local = [1, 2, 3, \"no-regions child padded string 0123456789 0123456789\"]\n"
        "            ch.send(local[0])\n"
        "        }\n"
        "    }\n"
        "    let v = ch.recv()\n"
        "    assert(v == 1, \"spawn result wrong\")\n"
        "}\n",
        /*no_regions=*/true));

    ASSERT_EQ_INT(crystal_region_created_total(), base_created);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);

    cbr_s3_vm_cleanup(&s);
    ASSERT_EQ_INT(crystal_region_live_count(), base_live);
}
#endif /* !_WIN32 && !__EMSCRIPTEN__ */
