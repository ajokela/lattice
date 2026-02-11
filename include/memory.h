#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * ── Dual-Heap Memory Architecture ──
 *
 * Lattice uses a dual-heap design separating mutable (fluid) and immutable
 * (crystal) memory:
 *
 *   FluidHeap     – GC-managed allocations for mutable (flux-phase) values.
 *                   Mark-sweep collection via gc_cycle in eval.c.
 *
 *   RegionManager – Arena-based region allocator for frozen (crystal-phase)
 *                   values.  Each freeze creates a new region with a page-
 *                   based arena.  Deep-cloning into the arena gives cache
 *                   locality and O(1) bulk deallocation.
 *
 * Invariants
 * ----------
 * 1. Heap Separation
 *    Arena-backed crystal values have completely independent pointers from
 *    the fluid heap.  Freeze deep-clones into the arena, then frees the
 *    original fluid pointers.
 *
 * 2. GC Safety
 *    Crystal region pointers are never subject to fluid sweep.  The mark
 *    phase records reachable region IDs; the sweep phase only frees
 *    unmarked fluid allocations.  Crystal values with a valid region_id
 *    are skipped during fluid marking (early return in gc_mark_value).
 *
 * 3. Lifecycle
 *    Every reachable crystal value has a region_id that appears in the
 *    reachable set passed to region_collect.  Unreachable crystal regions
 *    are freed when they are absent from the reachable set.
 *
 * 4. Environment Coverage
 *    During GC, all live environments are marked — both the current
 *    evaluator environment and any saved caller environments from
 *    closure calls (stored in saved_envs).  The shadow stack (gc_roots)
 *    protects in-flight temporaries on the C stack.
 */

/* ── Fluid Heap ── */

typedef struct FluidAlloc {
    void              *ptr;
    size_t             size;
    bool               marked;
    struct FluidAlloc *next;
} FluidAlloc;

typedef struct FluidHeap {
    FluidAlloc *allocs;
    size_t      total_bytes;
    size_t      alloc_count;
    size_t      gc_threshold;
    size_t      peak_bytes;
    size_t      cumulative_bytes;
} FluidHeap;

FluidHeap *fluid_heap_new(void);
void       fluid_heap_free(FluidHeap *h);
void      *fluid_alloc(FluidHeap *h, size_t size);
bool       fluid_dealloc(FluidHeap *h, void *ptr);
size_t     fluid_live_count(const FluidHeap *h);
size_t     fluid_total_bytes(const FluidHeap *h);

/* GC support: mark a specific pointer as reachable */
bool       fluid_mark(FluidHeap *h, void *ptr);
/* GC support: clear all marks */
void       fluid_unmark_all(FluidHeap *h);
/* GC support: sweep unmarked allocations, return count freed */
size_t     fluid_sweep(FluidHeap *h);

/* ── Arena Pages ── */

#define ARENA_PAGE_SIZE 4096

typedef struct ArenaPage {
    uint8_t          *data;
    size_t            used;
    size_t            cap;
    struct ArenaPage *next;
} ArenaPage;

/* ── Crystal Region ── */

typedef size_t RegionId;
typedef size_t Epoch;

typedef struct CrystalRegion {
    RegionId    id;
    Epoch       epoch;
    ArenaPage  *pages;       /* linked list of arena pages */
    size_t      total_bytes; /* total bytes used across all pages */
} CrystalRegion;

/* ── Region Manager ── */

typedef struct RegionManager {
    CrystalRegion **regions;
    size_t          count;
    size_t          cap;
    size_t          next_id;
    Epoch           current_epoch;
    size_t          total_allocs;
    size_t          peak_count;
    size_t          cumulative_data_bytes;
} RegionManager;

RegionManager *region_manager_new(void);
void           region_manager_free(RegionManager *rm);
Epoch          region_advance_epoch(RegionManager *rm);
Epoch          region_current_epoch(const RegionManager *rm);
CrystalRegion *region_create(RegionManager *rm);
size_t         region_collect(RegionManager *rm, const RegionId *reachable, size_t reachable_count);
size_t         region_count(const RegionManager *rm);
size_t         region_total_allocs(const RegionManager *rm);
size_t         region_live_data_bytes(const RegionManager *rm);

/* ── Arena allocation ── */

void *arena_alloc(CrystalRegion *r, size_t size);
void *arena_calloc(CrystalRegion *r, size_t count, size_t size);
char *arena_strdup(CrystalRegion *r, const char *s);

/* ── Dual Heap ── */

typedef struct DualHeap {
    FluidHeap     *fluid;
    RegionManager *regions;
} DualHeap;

DualHeap *dual_heap_new(void);
void      dual_heap_free(DualHeap *dh);

#endif /* MEMORY_H */
