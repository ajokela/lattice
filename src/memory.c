#include "memory.h"
#include <stdlib.h>
#include <string.h>

/* ── Fluid Heap ── */

FluidHeap *fluid_heap_new(void) {
    FluidHeap *h = calloc(1, sizeof(FluidHeap));
    h->gc_threshold = 1024 * 1024;  /* 1 MB default */
    return h;
}

void fluid_heap_free(FluidHeap *h) {
    if (!h) return;
    FluidAlloc *a = h->allocs;
    while (a) {
        FluidAlloc *next = a->next;
        free(a->ptr);
        free(a);
        a = next;
    }
    free(h);
}

void *fluid_alloc(FluidHeap *h, size_t size) {
    void *ptr = malloc(size);
    FluidAlloc *a = malloc(sizeof(FluidAlloc));
    a->ptr = ptr;
    a->size = size;
    a->marked = false;
    a->next = h->allocs;
    h->allocs = a;
    h->total_bytes += size;
    h->alloc_count++;
    return ptr;
}

void fluid_dealloc(FluidHeap *h, void *ptr) {
    FluidAlloc **prev = &h->allocs;
    for (FluidAlloc *a = h->allocs; a; a = a->next) {
        if (a->ptr == ptr) {
            *prev = a->next;
            h->total_bytes -= a->size;
            h->alloc_count--;
            free(a->ptr);
            free(a);
            return;
        }
        prev = &a->next;
    }
}

size_t fluid_live_count(const FluidHeap *h) {
    return h->alloc_count;
}

size_t fluid_total_bytes(const FluidHeap *h) {
    return h->total_bytes;
}

bool fluid_mark(FluidHeap *h, void *ptr) {
    for (FluidAlloc *a = h->allocs; a; a = a->next) {
        if (a->ptr == ptr) {
            a->marked = true;
            return true;
        }
    }
    return false;
}

void fluid_unmark_all(FluidHeap *h) {
    for (FluidAlloc *a = h->allocs; a; a = a->next) {
        a->marked = false;
    }
}

size_t fluid_sweep(FluidHeap *h) {
    size_t freed = 0;
    FluidAlloc **prev = &h->allocs;
    FluidAlloc *a = h->allocs;
    while (a) {
        if (!a->marked) {
            *prev = a->next;
            h->total_bytes -= a->size;
            h->alloc_count--;
            FluidAlloc *next = a->next;
            free(a->ptr);
            free(a);
            a = next;
            freed++;
        } else {
            prev = &a->next;
            a = a->next;
        }
    }
    return freed;
}

/* ── Crystal Region ── */

static CrystalRegion *crystal_region_new(RegionId id, Epoch epoch, size_t initial_cap) {
    CrystalRegion *r = calloc(1, sizeof(CrystalRegion));
    r->id = id;
    r->epoch = epoch;
    r->ref_count = 1;
    r->cap = initial_cap < 256 ? 256 : initial_cap;
    r->data = malloc(r->cap);
    r->used = 0;
    return r;
}

static void crystal_region_free(CrystalRegion *r) {
    if (!r) return;
    free(r->data);
    free(r);
}

/* ── Region Manager ── */

RegionManager *region_manager_new(void) {
    RegionManager *rm = calloc(1, sizeof(RegionManager));
    rm->cap = 8;
    rm->regions = malloc(rm->cap * sizeof(CrystalRegion *));
    return rm;
}

void region_manager_free(RegionManager *rm) {
    if (!rm) return;
    for (size_t i = 0; i < rm->count; i++) {
        crystal_region_free(rm->regions[i]);
    }
    free(rm->regions);
    free(rm);
}

Epoch region_advance_epoch(RegionManager *rm) {
    return ++rm->current_epoch;
}

Epoch region_current_epoch(const RegionManager *rm) {
    return rm->current_epoch;
}

static CrystalRegion *find_or_create_epoch_region(RegionManager *rm, Epoch epoch) {
    for (size_t i = 0; i < rm->count; i++) {
        if (rm->regions[i]->epoch == epoch)
            return rm->regions[i];
    }
    /* Create new */
    if (rm->count >= rm->cap) {
        rm->cap *= 2;
        rm->regions = realloc(rm->regions, rm->cap * sizeof(CrystalRegion *));
    }
    CrystalRegion *r = crystal_region_new(rm->next_id++, epoch, 4096);
    rm->regions[rm->count++] = r;
    return r;
}

RegionId region_allocate(RegionManager *rm, const void *data, size_t size) {
    CrystalRegion *r = find_or_create_epoch_region(rm, rm->current_epoch);
    /* Ensure capacity */
    while (r->used + size > r->cap) {
        r->cap *= 2;
        r->data = realloc(r->data, r->cap);
    }
    memcpy(r->data + r->used, data, size);
    r->used += size;
    rm->total_allocs++;
    return r->id;
}

void *region_get_data(const RegionManager *rm, RegionId id, size_t offset, size_t size) {
    for (size_t i = 0; i < rm->count; i++) {
        if (rm->regions[i]->id == id) {
            if (offset + size <= rm->regions[i]->used) {
                return rm->regions[i]->data + offset;
            }
            return NULL;
        }
    }
    return NULL;
}

CrystalRegion *region_get(const RegionManager *rm, RegionId id) {
    for (size_t i = 0; i < rm->count; i++) {
        if (rm->regions[i]->id == id) return rm->regions[i];
    }
    return NULL;
}

void region_retain(RegionManager *rm, RegionId id) {
    CrystalRegion *r = region_get(rm, id);
    if (r) r->ref_count++;
}

bool region_release(RegionManager *rm, RegionId id) {
    for (size_t i = 0; i < rm->count; i++) {
        if (rm->regions[i]->id == id) {
            rm->regions[i]->ref_count--;
            if (rm->regions[i]->ref_count == 0) {
                crystal_region_free(rm->regions[i]);
                rm->regions[i] = rm->regions[rm->count - 1];
                rm->count--;
                return true;
            }
            return false;
        }
    }
    return false;
}

size_t region_collect(RegionManager *rm, const RegionId *reachable, size_t reachable_count) {
    size_t freed = 0;
    size_t i = 0;
    while (i < rm->count) {
        bool is_reachable = false;
        for (size_t j = 0; j < reachable_count; j++) {
            if (rm->regions[i]->id == reachable[j]) {
                is_reachable = true;
                break;
            }
        }
        if (!is_reachable) {
            crystal_region_free(rm->regions[i]);
            rm->regions[i] = rm->regions[rm->count - 1];
            rm->count--;
            freed++;
        } else {
            i++;
        }
    }
    return freed;
}

size_t region_count(const RegionManager *rm) {
    return rm->count;
}

size_t region_total_allocs(const RegionManager *rm) {
    return rm->total_allocs;
}

/* ── Dual Heap ── */

DualHeap *dual_heap_new(void) {
    DualHeap *dh = malloc(sizeof(DualHeap));
    dh->fluid = fluid_heap_new();
    dh->regions = region_manager_new();
    return dh;
}

void dual_heap_free(DualHeap *dh) {
    if (!dh) return;
    fluid_heap_free(dh->fluid);
    region_manager_free(dh->regions);
    free(dh);
}
