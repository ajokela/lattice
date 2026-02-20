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
    h->cumulative_bytes += size;
    if (h->total_bytes > h->peak_bytes)
        h->peak_bytes = h->total_bytes;
    return ptr;
}

bool fluid_dealloc(FluidHeap *h, void *ptr) {
    FluidAlloc **prev = &h->allocs;
    for (FluidAlloc *a = h->allocs; a; a = a->next) {
        if (a->ptr == ptr) {
            *prev = a->next;
            h->total_bytes -= a->size;
            h->alloc_count--;
            free(a->ptr);
            free(a);
            return true;
        }
        prev = &a->next;
    }
    return false;
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

/* ── Arena Pages ── */

static ArenaPage *arena_page_new(size_t cap) {
    ArenaPage *p = malloc(sizeof(ArenaPage));
    p->data = malloc(cap);
    p->used = 0;
    p->cap = cap;
    p->next = NULL;
    return p;
}

static void arena_page_free_list(ArenaPage *p) {
    while (p) {
        ArenaPage *next = p->next;
        free(p->data);
        free(p);
        p = next;
    }
}

/* ── Bump Arena ── */

BumpArena *bump_arena_new(void) {
    BumpArena *ba = calloc(1, sizeof(BumpArena));
    ArenaPage *p = arena_page_new(ARENA_PAGE_SIZE);
    ba->pages = p;
    ba->first_page = p;
    ba->total_bytes = 0;
    return ba;
}

void bump_arena_free(BumpArena *ba) {
    if (!ba) return;
    arena_page_free_list(ba->first_page);
    free(ba);
}

void bump_arena_reset(BumpArena *ba) {
    if (!ba) return;
    for (ArenaPage *p = ba->first_page; p; p = p->next)
        p->used = 0;
    ba->pages = ba->first_page;
    ba->total_bytes = 0;
}

void *bump_alloc(BumpArena *ba, size_t size) {
    size_t aligned = (size + 7) & ~(size_t)7;
    ArenaPage *cur = ba->pages;

    /* Try current page */
    if (cur && cur->used + aligned <= cur->cap) {
        void *ptr = cur->data + cur->used;
        cur->used += aligned;
        ba->total_bytes += aligned;
        return ptr;
    }

    /* Try next page in chain (from prior reset) */
    if (cur && cur->next && cur->next->used + aligned <= cur->next->cap) {
        ba->pages = cur->next;
        void *ptr = ba->pages->data + ba->pages->used;
        ba->pages->used += aligned;
        ba->total_bytes += aligned;
        return ptr;
    }

    /* Allocate a new page */
    size_t page_cap = aligned > ARENA_PAGE_SIZE ? aligned : ARENA_PAGE_SIZE;
    ArenaPage *np = arena_page_new(page_cap);
    if (cur) {
        np->next = cur->next;
        cur->next = np;
    } else {
        ba->first_page = np;
    }
    ba->pages = np;
    void *ptr = np->data;
    np->used = aligned;
    ba->total_bytes += aligned;
    return ptr;
}

char *bump_strdup(BumpArena *ba, const char *s) {
    size_t len = strlen(s) + 1;
    char *ptr = bump_alloc(ba, len);
    memcpy(ptr, s, len);
    return ptr;
}

/* ── Crystal Region ── */

static CrystalRegion *crystal_region_new(RegionId id, Epoch epoch) {
    CrystalRegion *r = calloc(1, sizeof(CrystalRegion));
    r->id = id;
    r->epoch = epoch;
    r->pages = arena_page_new(ARENA_PAGE_SIZE);
    r->total_bytes = 0;
    return r;
}

static void crystal_region_free(CrystalRegion *r) {
    if (!r) return;
    arena_page_free_list(r->pages);
    free(r);
}

/* ── Arena allocation ── */

void *arena_alloc(CrystalRegion *r, size_t size) {
    /* 8-byte alignment */
    size_t aligned = (size + 7) & ~(size_t)7;

    /* Try to fit in the current head page */
    ArenaPage *head = r->pages;
    if (head && head->used + aligned <= head->cap) {
        void *ptr = head->data + head->used;
        head->used += aligned;
        r->total_bytes += aligned;
        return ptr;
    }

    /* Need a new page — oversized allocs get a dedicated page */
    size_t page_cap = aligned > ARENA_PAGE_SIZE ? aligned : ARENA_PAGE_SIZE;
    ArenaPage *np = arena_page_new(page_cap);
    np->next = r->pages;
    r->pages = np;

    void *ptr = np->data;
    np->used = aligned;
    r->total_bytes += aligned;
    return ptr;
}

void *arena_calloc(CrystalRegion *r, size_t count, size_t size) {
    if (count > 0 && size > SIZE_MAX / count) return NULL;
    size_t total = count * size;
    void *ptr = arena_alloc(r, total);
    memset(ptr, 0, total);
    return ptr;
}

char *arena_strdup(CrystalRegion *r, const char *s) {
    size_t len = strlen(s) + 1;
    char *ptr = arena_alloc(r, len);
    memcpy(ptr, s, len);
    return ptr;
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

CrystalRegion *region_create(RegionManager *rm) {
    if (rm->count >= rm->cap) {
        rm->cap *= 2;
        rm->regions = realloc(rm->regions, rm->cap * sizeof(CrystalRegion *));
    }
    CrystalRegion *r = crystal_region_new(rm->next_id++, rm->current_epoch);
    rm->regions[rm->count++] = r;
    rm->total_allocs++;
    if (rm->count > rm->peak_count)
        rm->peak_count = rm->count;
    return r;
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

size_t region_live_data_bytes(const RegionManager *rm) {
    size_t total = 0;
    for (size_t i = 0; i < rm->count; i++)
        total += rm->regions[i]->total_bytes;
    return total;
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
