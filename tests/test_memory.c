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

TEST(region_allocate_creates_region) {
    RegionManager *rm = region_manager_new();

    int data = 0xBEEF;
    RegionId rid = region_allocate(rm, &data, sizeof(data));
    ASSERT_EQ_INT(region_count(rm), 1);
    ASSERT_EQ_INT(region_total_allocs(rm), 1);

    CrystalRegion *r = region_get(rm, rid);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(r->epoch, 0);
    ASSERT_EQ_INT(r->ref_count, 1);

    region_manager_free(rm);
}

TEST(region_multiple_allocs_same_epoch_share_region) {
    RegionManager *rm = region_manager_new();

    int a = 10, b = 20, c = 30;
    RegionId r1 = region_allocate(rm, &a, sizeof(a));
    RegionId r2 = region_allocate(rm, &b, sizeof(b));
    RegionId r3 = region_allocate(rm, &c, sizeof(c));

    /* All allocations in the same epoch should share a region. */
    ASSERT_EQ_INT(r1, r2);
    ASSERT_EQ_INT(r2, r3);
    ASSERT_EQ_INT(region_count(rm), 1);
    ASSERT_EQ_INT(region_total_allocs(rm), 3);

    region_manager_free(rm);
}

TEST(region_advance_epoch_creates_separate_regions) {
    RegionManager *rm = region_manager_new();

    int data = 1;
    RegionId r0 = region_allocate(rm, &data, sizeof(data));

    Epoch e1 = region_advance_epoch(rm);
    ASSERT_EQ_INT(e1, 1);

    data = 2;
    RegionId r1 = region_allocate(rm, &data, sizeof(data));

    /* Different epochs produce different regions. */
    ASSERT(r0 != r1);
    ASSERT_EQ_INT(region_count(rm), 2);

    CrystalRegion *reg0 = region_get(rm, r0);
    CrystalRegion *reg1 = region_get(rm, r1);
    ASSERT(reg0 != NULL);
    ASSERT(reg1 != NULL);
    ASSERT_EQ_INT(reg0->epoch, 0);
    ASSERT_EQ_INT(reg1->epoch, 1);

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

TEST(region_retain_increments_ref_count) {
    RegionManager *rm = region_manager_new();

    int data = 42;
    RegionId rid = region_allocate(rm, &data, sizeof(data));
    ASSERT_EQ_INT(region_get(rm, rid)->ref_count, 1);

    region_retain(rm, rid);
    ASSERT_EQ_INT(region_get(rm, rid)->ref_count, 2);

    region_retain(rm, rid);
    ASSERT_EQ_INT(region_get(rm, rid)->ref_count, 3);

    region_manager_free(rm);
}

TEST(region_release_decrements_ref_count) {
    RegionManager *rm = region_manager_new();

    int data = 42;
    RegionId rid = region_allocate(rm, &data, sizeof(data));
    region_retain(rm, rid);  /* ref_count = 2 */

    bool freed = region_release(rm, rid);
    ASSERT(!freed);
    ASSERT_EQ_INT(region_get(rm, rid)->ref_count, 1);

    region_manager_free(rm);
}

TEST(region_release_to_zero_frees_region) {
    RegionManager *rm = region_manager_new();

    int data = 42;
    RegionId rid = region_allocate(rm, &data, sizeof(data));
    ASSERT_EQ_INT(region_count(rm), 1);

    bool freed = region_release(rm, rid);
    ASSERT(freed);
    ASSERT_EQ_INT(region_count(rm), 0);
    ASSERT(region_get(rm, rid) == NULL);

    region_manager_free(rm);
}

TEST(region_release_nonexistent_returns_false) {
    RegionManager *rm = region_manager_new();
    bool freed = region_release(rm, 999);
    ASSERT(!freed);
    region_manager_free(rm);
}

TEST(region_collect_frees_unreachable) {
    RegionManager *rm = region_manager_new();

    int data = 1;
    RegionId r0 = region_allocate(rm, &data, sizeof(data));

    region_advance_epoch(rm);
    data = 2;
    RegionId r1 = region_allocate(rm, &data, sizeof(data));

    region_advance_epoch(rm);
    data = 3;
    region_allocate(rm, &data, sizeof(data));  /* r2: unreachable */

    ASSERT_EQ_INT(region_count(rm), 3);

    /* Only r0 and r1 are reachable; r2 should be collected. */
    RegionId reachable[] = { r0, r1 };
    size_t freed = region_collect(rm, reachable, 2);
    ASSERT_EQ_INT(freed, 1);
    ASSERT_EQ_INT(region_count(rm), 2);
    ASSERT(region_get(rm, r0) != NULL);
    ASSERT(region_get(rm, r1) != NULL);

    region_manager_free(rm);
}

TEST(region_collect_empty_reachable_frees_all) {
    RegionManager *rm = region_manager_new();

    int data = 1;
    region_allocate(rm, &data, sizeof(data));
    region_advance_epoch(rm);
    data = 2;
    region_allocate(rm, &data, sizeof(data));

    ASSERT_EQ_INT(region_count(rm), 2);

    size_t freed = region_collect(rm, NULL, 0);
    ASSERT_EQ_INT(freed, 2);
    ASSERT_EQ_INT(region_count(rm), 0);

    region_manager_free(rm);
}

TEST(region_collect_all_reachable_frees_none) {
    RegionManager *rm = region_manager_new();

    int data = 1;
    RegionId r0 = region_allocate(rm, &data, sizeof(data));
    region_advance_epoch(rm);
    data = 2;
    RegionId r1 = region_allocate(rm, &data, sizeof(data));

    RegionId reachable[] = { r0, r1 };
    size_t freed = region_collect(rm, reachable, 2);
    ASSERT_EQ_INT(freed, 0);
    ASSERT_EQ_INT(region_count(rm), 2);

    region_manager_free(rm);
}

TEST(region_total_allocs_span_epochs) {
    RegionManager *rm = region_manager_new();

    int data = 0;
    region_allocate(rm, &data, sizeof(data));
    region_allocate(rm, &data, sizeof(data));
    region_advance_epoch(rm);
    region_allocate(rm, &data, sizeof(data));

    ASSERT_EQ_INT(region_total_allocs(rm), 3);

    region_manager_free(rm);
}

/* ══════════════════════════════════════════════════════════════════════════
 * region_get_data tests
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(region_get_data_retrieves_stored_value) {
    RegionManager *rm = region_manager_new();

    int value = 0xDEAD;
    RegionId rid = region_allocate(rm, &value, sizeof(value));

    int *retrieved = (int *)region_get_data(rm, rid, 0, sizeof(int));
    ASSERT(retrieved != NULL);
    ASSERT_EQ_INT(*retrieved, 0xDEAD);

    region_manager_free(rm);
}

TEST(region_get_data_multiple_values) {
    RegionManager *rm = region_manager_new();

    int v1 = 111;
    RegionId rid = region_allocate(rm, &v1, sizeof(v1));

    int v2 = 222;
    region_allocate(rm, &v2, sizeof(v2));  /* Same epoch, same region. */

    /* First value at offset 0. */
    int *r1 = (int *)region_get_data(rm, rid, 0, sizeof(int));
    ASSERT(r1 != NULL);
    ASSERT_EQ_INT(*r1, 111);

    /* Second value at offset sizeof(int). */
    int *r2 = (int *)region_get_data(rm, rid, sizeof(int), sizeof(int));
    ASSERT(r2 != NULL);
    ASSERT_EQ_INT(*r2, 222);

    region_manager_free(rm);
}

TEST(region_get_data_out_of_bounds_returns_null) {
    RegionManager *rm = region_manager_new();

    int value = 42;
    RegionId rid = region_allocate(rm, &value, sizeof(value));

    /* Request data beyond what was written. */
    void *data = region_get_data(rm, rid, sizeof(int), sizeof(int));
    ASSERT(data == NULL);

    region_manager_free(rm);
}

TEST(region_get_data_invalid_id_returns_null) {
    RegionManager *rm = region_manager_new();
    void *data = region_get_data(rm, 999, 0, 4);
    ASSERT(data == NULL);
    region_manager_free(rm);
}

TEST(region_get_data_struct_roundtrip) {
    typedef struct {
        int x;
        int y;
        double z;
    } Point3;

    RegionManager *rm = region_manager_new();

    Point3 p = { .x = 10, .y = 20, .z = 3.14 };
    RegionId rid = region_allocate(rm, &p, sizeof(p));

    Point3 *retrieved = (Point3 *)region_get_data(rm, rid, 0, sizeof(Point3));
    ASSERT(retrieved != NULL);
    ASSERT_EQ_INT(retrieved->x, 10);
    ASSERT_EQ_INT(retrieved->y, 20);
    ASSERT(retrieved->z > 3.13 && retrieved->z < 3.15);

    region_manager_free(rm);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Multiple epochs with different regions
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(multiple_epochs_isolate_data) {
    RegionManager *rm = region_manager_new();

    /* Epoch 0: store value 100. */
    int v0 = 100;
    RegionId r0 = region_allocate(rm, &v0, sizeof(v0));

    /* Epoch 1: store value 200. */
    region_advance_epoch(rm);
    int v1 = 200;
    RegionId r1 = region_allocate(rm, &v1, sizeof(v1));

    /* Epoch 2: store value 300. */
    region_advance_epoch(rm);
    int v2 = 300;
    RegionId r2 = region_allocate(rm, &v2, sizeof(v2));

    ASSERT_EQ_INT(region_count(rm), 3);
    ASSERT(r0 != r1);
    ASSERT(r1 != r2);
    ASSERT(r0 != r2);

    /* Verify data is correctly stored per epoch. */
    int *d0 = (int *)region_get_data(rm, r0, 0, sizeof(int));
    int *d1 = (int *)region_get_data(rm, r1, 0, sizeof(int));
    int *d2 = (int *)region_get_data(rm, r2, 0, sizeof(int));
    ASSERT(d0 != NULL);
    ASSERT(d1 != NULL);
    ASSERT(d2 != NULL);
    ASSERT_EQ_INT(*d0, 100);
    ASSERT_EQ_INT(*d1, 200);
    ASSERT_EQ_INT(*d2, 300);

    /* Verify epoch assignments. */
    ASSERT_EQ_INT(region_get(rm, r0)->epoch, 0);
    ASSERT_EQ_INT(region_get(rm, r1)->epoch, 1);
    ASSERT_EQ_INT(region_get(rm, r2)->epoch, 2);

    region_manager_free(rm);
}

TEST(multiple_epochs_collect_old_keep_new) {
    RegionManager *rm = region_manager_new();

    int data = 0;
    region_allocate(rm, &data, sizeof(data));  /* epoch 0 */

    region_advance_epoch(rm);
    region_allocate(rm, &data, sizeof(data));  /* epoch 1 */

    region_advance_epoch(rm);
    RegionId r2 = region_allocate(rm, &data, sizeof(data));  /* epoch 2 */

    region_advance_epoch(rm);
    RegionId r3 = region_allocate(rm, &data, sizeof(data));  /* epoch 3 */

    ASSERT_EQ_INT(region_count(rm), 4);

    /* Keep only the latest two epochs. */
    RegionId reachable[] = { r2, r3 };
    size_t freed = region_collect(rm, reachable, 2);
    ASSERT_EQ_INT(freed, 2);
    ASSERT_EQ_INT(region_count(rm), 2);

    ASSERT(region_get(rm, r2) != NULL);
    ASSERT(region_get(rm, r3) != NULL);

    region_manager_free(rm);
}

TEST(multiple_epochs_alloc_counts_accumulate) {
    RegionManager *rm = region_manager_new();

    int data = 0;

    /* Epoch 0: 2 allocations. */
    region_allocate(rm, &data, sizeof(data));
    region_allocate(rm, &data, sizeof(data));

    /* Epoch 1: 3 allocations. */
    region_advance_epoch(rm);
    region_allocate(rm, &data, sizeof(data));
    region_allocate(rm, &data, sizeof(data));
    region_allocate(rm, &data, sizeof(data));

    /* Epoch 2: 1 allocation. */
    region_advance_epoch(rm);
    region_allocate(rm, &data, sizeof(data));

    ASSERT_EQ_INT(region_count(rm), 3);
    ASSERT_EQ_INT(region_total_allocs(rm), 6);

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

    /* Allocate in crystal regions. */
    int data = 0xCAFE;
    RegionId rid = region_allocate(dh->regions, &data, sizeof(data));
    ASSERT_EQ_INT(region_count(dh->regions), 1);
    ASSERT_EQ_INT(region_total_allocs(dh->regions), 1);

    /* Fluid heap is unaffected by region allocation. */
    ASSERT_EQ_INT(fluid_live_count(dh->fluid), 2);

    /* Deallocate from fluid; regions unaffected. */
    fluid_dealloc(dh->fluid, fp1);
    ASSERT_EQ_INT(fluid_live_count(dh->fluid), 1);
    ASSERT_EQ_INT(region_count(dh->regions), 1);

    /* Verify crystal data. */
    int *crystal_val = (int *)region_get_data(dh->regions, rid, 0, sizeof(int));
    ASSERT(crystal_val != NULL);
    ASSERT_EQ_INT(*crystal_val, 0xCAFE);

    dual_heap_free(dh);
}

TEST(dual_heap_simulate_freeze_thaw_cycle) {
    DualHeap *dh = dual_heap_new();

    /*
     * Simulate the Lattice crystallization lifecycle:
     * 1. Allocate mutable data in fluid heap.
     * 2. Freeze: deallocate from fluid, store in crystal region.
     * 3. Thaw: read from crystal, allocate back in fluid.
     */

    /* Phase 1: create mutable data. */
    int *val = (int *)fluid_alloc(dh->fluid, sizeof(int));
    *val = 42;
    ASSERT_EQ_INT(fluid_live_count(dh->fluid), 1);

    /* Phase 2: freeze -- move to crystal region. */
    RegionId rid = region_allocate(dh->regions, val, sizeof(int));
    fluid_dealloc(dh->fluid, val);
    ASSERT_EQ_INT(fluid_live_count(dh->fluid), 0);
    ASSERT_EQ_INT(region_count(dh->regions), 1);

    /* Phase 3: thaw -- copy back to fluid. */
    int *crystal_data = (int *)region_get_data(dh->regions, rid, 0, sizeof(int));
    ASSERT(crystal_data != NULL);
    int *thawed = (int *)fluid_alloc(dh->fluid, sizeof(int));
    *thawed = *crystal_data;
    ASSERT_EQ_INT(*thawed, 42);
    ASSERT_EQ_INT(fluid_live_count(dh->fluid), 1);
    /* Crystal remains intact (thaw is a copy, not a move). */
    ASSERT_EQ_INT(region_count(dh->regions), 1);

    dual_heap_free(dh);
}

TEST(dual_heap_gc_cycle) {
    DualHeap *dh = dual_heap_new();

    int data = 0;

    /* Create regions across three epochs. */
    RegionId r0 = region_allocate(dh->regions, &data, sizeof(data));

    region_advance_epoch(dh->regions);
    region_allocate(dh->regions, &data, sizeof(data));  /* r1: will be unreachable */

    region_advance_epoch(dh->regions);
    region_allocate(dh->regions, &data, sizeof(data));  /* r2: will be unreachable */

    ASSERT_EQ_INT(region_count(dh->regions), 3);

    /* Only r0 is reachable; the other two should be collected. */
    RegionId reachable[] = { r0 };
    size_t freed = region_collect(dh->regions, reachable, 1);
    ASSERT_EQ_INT(freed, 2);
    ASSERT_EQ_INT(region_count(dh->regions), 1);

    /* Fluid heap is independent of crystal GC. */
    fluid_alloc(dh->fluid, 32);
    ASSERT_EQ_INT(fluid_live_count(dh->fluid), 1);

    dual_heap_free(dh);
}

TEST(dual_heap_full_lifecycle) {
    DualHeap *dh = dual_heap_new();

    /* Phase 1: create mutable data in fluid heap. */
    int *a = (int *)fluid_alloc(dh->fluid, sizeof(int));
    int *b = (int *)fluid_alloc(dh->fluid, sizeof(int));
    *a = 10;
    *b = 20;
    ASSERT_EQ_INT(fluid_live_count(dh->fluid), 2);

    /* Phase 2: freeze both into epoch 0. */
    RegionId r0a = region_allocate(dh->regions, a, sizeof(int));
    RegionId r0b = region_allocate(dh->regions, b, sizeof(int));
    fluid_dealloc(dh->fluid, a);
    fluid_dealloc(dh->fluid, b);
    ASSERT_EQ_INT(r0a, r0b);  /* Same epoch, same region. */
    ASSERT_EQ_INT(fluid_live_count(dh->fluid), 0);
    ASSERT_EQ_INT(region_count(dh->regions), 1);

    /* Phase 3: advance epoch, create and freeze more. */
    region_advance_epoch(dh->regions);
    int *c = (int *)fluid_alloc(dh->fluid, sizeof(int));
    *c = 30;
    RegionId r1 = region_allocate(dh->regions, c, sizeof(int));
    fluid_dealloc(dh->fluid, c);
    ASSERT(r0a != r1);
    ASSERT_EQ_INT(region_count(dh->regions), 2);

    /* Phase 4: thaw a crystal value for mutation. */
    int *crystal_a = (int *)region_get_data(dh->regions, r0a, 0, sizeof(int));
    int *thawed = (int *)fluid_alloc(dh->fluid, sizeof(int));
    *thawed = *crystal_a;
    ASSERT_EQ_INT(*thawed, 10);
    ASSERT_EQ_INT(fluid_live_count(dh->fluid), 1);

    /* Phase 5: GC -- only epoch 1 region is reachable. */
    RegionId reachable[] = { r1 };
    size_t freed = region_collect(dh->regions, reachable, 1);
    ASSERT_EQ_INT(freed, 1);
    ASSERT_EQ_INT(region_count(dh->regions), 1);

    /* Phase 6: release the remaining region via refcounting. */
    bool removed = region_release(dh->regions, r1);
    ASSERT(removed);
    ASSERT_EQ_INT(region_count(dh->regions), 0);

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
