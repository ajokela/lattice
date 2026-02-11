#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "memory.h"

/* Import test macros from test_main.c */
extern void register_test(const char *name, void (*fn)(void));
extern int test_current_failed;

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        test_current_failed = 1; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_INT(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL: %s:%d: %lld != %lld\n", __FILE__, __LINE__, _a, _b); \
        test_current_failed = 1; \
        return; \
    } \
} while(0)

#define TEST(name) \
    static void name(void); \
    static void name##_register(void) __attribute__((constructor)); \
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
    for (int i = 0; i < 100; i += 2) {
        fluid_dealloc(h, ptrs[i]);
    }
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
    region_create(rm);  /* unreachable */

    ASSERT_EQ_INT(region_count(rm), 3);

    /* Only r0 and r1 are reachable; r2 should be collected. */
    RegionId reachable[] = { id0, id1 };
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

    RegionId reachable[] = { id0, id1 };
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
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ_INT(arr[i], 0);
    }

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

    arena_alloc(r, 10);  /* aligned to 16 */
    ASSERT(r->total_bytes >= 10);
    size_t after_first = r->total_bytes;

    arena_alloc(r, 20);  /* aligned to 24 */
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
    region_create(dh->regions);  /* will be unreachable */

    region_advance_epoch(dh->regions);
    region_create(dh->regions);  /* will be unreachable */

    ASSERT_EQ_INT(region_count(dh->regions), 3);

    /* Only r0 is reachable; the other two should be collected. */
    RegionId reachable[] = { id0 };
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
    (void)fluid_alloc(h, 128);  /* b: unreachable */
    void *c = fluid_alloc(h, 32);
    ASSERT_EQ_INT(fluid_live_count(h), 3);

    fluid_unmark_all(h);
    fluid_mark(h, a);  /* only a is reachable */
    fluid_mark(h, c);  /* c is reachable too */
    size_t swept = fluid_sweep(h);

    ASSERT_EQ_INT(swept, 1);  /* only b was swept */
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
