#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

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
} FluidHeap;

FluidHeap *fluid_heap_new(void);
void       fluid_heap_free(FluidHeap *h);
void      *fluid_alloc(FluidHeap *h, size_t size);
void       fluid_dealloc(FluidHeap *h, void *ptr);
size_t     fluid_live_count(const FluidHeap *h);
size_t     fluid_total_bytes(const FluidHeap *h);

/* GC support: mark a specific pointer as reachable */
bool       fluid_mark(FluidHeap *h, void *ptr);
/* GC support: clear all marks */
void       fluid_unmark_all(FluidHeap *h);
/* GC support: sweep unmarked allocations, return count freed */
size_t     fluid_sweep(FluidHeap *h);

/* ── Crystal Region ── */

typedef size_t RegionId;
typedef size_t Epoch;

typedef struct CrystalRegion {
    RegionId id;
    Epoch    epoch;
    size_t   ref_count;
    uint8_t *data;
    size_t   used;
    size_t   cap;
} CrystalRegion;

/* ── Region Manager ── */

typedef struct RegionManager {
    CrystalRegion **regions;
    size_t          count;
    size_t          cap;
    size_t          next_id;
    Epoch           current_epoch;
    size_t          total_allocs;
} RegionManager;

RegionManager *region_manager_new(void);
void           region_manager_free(RegionManager *rm);
Epoch          region_advance_epoch(RegionManager *rm);
Epoch          region_current_epoch(const RegionManager *rm);
RegionId       region_allocate(RegionManager *rm, const void *data, size_t size);
void          *region_get_data(const RegionManager *rm, RegionId id, size_t offset, size_t size);
void           region_retain(RegionManager *rm, RegionId id);
bool           region_release(RegionManager *rm, RegionId id);
size_t         region_collect(RegionManager *rm, const RegionId *reachable, size_t reachable_count);
size_t         region_count(const RegionManager *rm);
size_t         region_total_allocs(const RegionManager *rm);
CrystalRegion *region_get(const RegionManager *rm, RegionId id);

/* ── Dual Heap ── */

typedef struct DualHeap {
    FluidHeap     *fluid;
    RegionManager *regions;
} DualHeap;

DualHeap *dual_heap_new(void);
void      dual_heap_free(DualHeap *dh);

#endif /* MEMORY_H */
