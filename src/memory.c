#include "memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#ifndef __EMSCRIPTEN__
#include <pthread.h>
#endif

/* Debug seal backstop (CbR Stage 2): POSIX-only, compiled out under NDEBUG.
 * Windows would use VirtualProtect (deferred — gauntlet targets are POSIX);
 * wasm has no mprotect (the same suite runs natively first). */
#if !defined(NDEBUG) && !defined(_WIN32) && !defined(__EMSCRIPTEN__) && (defined(__unix__) || defined(__APPLE__))
#define LATTICE_REGION_SEAL 1
#include <sys/mman.h>
#include <unistd.h>
#endif

/* ── Fluid Heap ── */

FluidHeap *fluid_heap_new(void) {
    FluidHeap *h = calloc(1, sizeof(FluidHeap));
    if (!h) return NULL;
    h->gc_threshold = 1024 * 1024; /* 1 MB default */
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
    if (!ptr) return NULL;
    FluidAlloc *a = malloc(sizeof(FluidAlloc));
    if (!a) {
        free(ptr);
        return NULL;
    }
    a->ptr = ptr;
    a->size = size;
    a->marked = false;
    a->next = h->allocs;
    h->allocs = a;
    h->total_bytes += size;
    h->alloc_count++;
    h->cumulative_bytes += size;
    if (h->total_bytes > h->peak_bytes) h->peak_bytes = h->total_bytes;
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

void *fluid_realloc(FluidHeap *h, void *ptr, size_t new_size) {
    if (!ptr) return fluid_alloc(h, new_size);
    /* Find the old block's tracked size. */
    size_t old_size = 0;
    bool tracked = false;
    for (FluidAlloc *a = h->allocs; a; a = a->next) {
        if (a->ptr == ptr) {
            old_size = a->size;
            tracked = true;
            break;
        }
    }
    if (!tracked) return realloc(ptr, new_size); /* a plain (untracked) allocation */
    void *np = fluid_alloc(h, new_size);
    if (np) memcpy(np, ptr, old_size < new_size ? old_size : new_size);
    fluid_dealloc(h, ptr); /* removes the old block from tracking and frees it */
    return np;
}

size_t fluid_live_count(const FluidHeap *h) { return h->alloc_count; }

size_t fluid_total_bytes(const FluidHeap *h) { return h->total_bytes; }

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
    for (FluidAlloc *a = h->allocs; a; a = a->next) { a->marked = false; }
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
    if (!p) return NULL;
    p->data = malloc(cap);
    if (!p->data) {
        free(p);
        return NULL;
    }
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

/* CbR Stage 2: shared-region pages are page-aligned (and page-granular) so
 * the debug seal can mprotect() them. posix_memalign memory is free()able,
 * so arena_page_free_list works unchanged. Falls back to plain pages when
 * the seal backstop is compiled out. */
static ArenaPage *arena_page_new_aligned(size_t cap) {
#ifdef LATTICE_REGION_SEAL
    long ps_raw = sysconf(_SC_PAGESIZE);
    size_t ps = ps_raw > 0 ? (size_t)ps_raw : 4096;
    size_t span = (cap + ps - 1) & ~(ps - 1);
    ArenaPage *p = malloc(sizeof(ArenaPage));
    if (!p) return NULL;
    void *data = NULL;
    if (posix_memalign(&data, ps, span) != 0) {
        free(p);
        return NULL;
    }
    p->data = data;
    p->used = 0;
    p->cap = span;
    p->next = NULL;
    return p;
#else
    return arena_page_new(cap);
#endif
}

/* ── Bump Arena ── */

BumpArena *bump_arena_new(void) {
    BumpArena *ba = calloc(1, sizeof(BumpArena));
    if (!ba) return NULL;
    ArenaPage *p = arena_page_new(ARENA_PAGE_SIZE);
    if (!p) {
        free(ba);
        return NULL;
    }
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
    for (ArenaPage *p = ba->first_page; p; p = p->next) p->used = 0;
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
    if (!r) return NULL;
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

/* ── Shared crystal regions (Crystal-by-Reference, Stage 2) ──
 *
 * Dormant in Round A: nothing in any evaluator creates these yet; the only
 * producers are value_freeze_to_region (itself uncalled by the language) and
 * unit tests. See memory.h for the atomics/memory-ordering contract. */

#ifndef __EMSCRIPTEN__
static pthread_mutex_t g_shared_reg_mutex = PTHREAD_MUTEX_INITIALIZER;
#define SHARED_REG_LOCK()   pthread_mutex_lock(&g_shared_reg_mutex)
#define SHARED_REG_UNLOCK() pthread_mutex_unlock(&g_shared_reg_mutex)
#else
#define SHARED_REG_LOCK()   ((void)0)
#define SHARED_REG_UNLOCK() ((void)0)
#endif

/* Registry: stats, leak diagnostics, and process-exit teardown ONLY.
 * Retain and non-final release never touch it. Plain malloc (never routed
 * through lat_alloc) so no thread's FluidHeap ever tracks it. */
static CrystalRegion **g_shared_regions = NULL;
static size_t g_shared_count = 0;
static size_t g_shared_cap = 0;
static bool g_shared_atexit_registered = false;
/* Atomic mirrors of the registry counts so the hot-path dormancy gate
 * (crystal_region_shared_active) never takes the mutex. */
static _Atomic size_t g_shared_live = 0;
static _Atomic size_t g_shared_created = 0;

static void crystal_region_unseal(CrystalRegion *r) {
#ifdef LATTICE_REGION_SEAL
    /* Unconditional for page-aligned regions — deliberately NOT gated on
     * r->sealed: a partially failed seal (some pages already PROT_READ when
     * a later page's mprotect fails) records sealed == false but leaves
     * read-only pages behind; gating on the flag would skip them and free()
     * would hand protected pages back to the allocator (delayed SIGSEGV on
     * reuse). mprotect(PROT_READ|PROT_WRITE) on never-protected pages is
     * harmless. */
    if (!r->page_aligned) return;
    for (ArenaPage *p = r->pages; p; p = p->next) { (void)mprotect(p->data, p->cap, PROT_READ | PROT_WRITE); }
    r->sealed = false;
#else
    (void)r;
#endif
}

void crystal_region_seal(CrystalRegion *r) {
#ifdef LATTICE_REGION_SEAL
    if (!r || !r->page_aligned || r->sealed) return;
    bool ok = true;
    for (ArenaPage *p = r->pages; p; p = p->next) {
        if (mprotect(p->data, p->cap, PROT_READ) != 0) ok = false;
    }
    /* Best-effort backstop; correctness never depends on it. On partial
     * failure (ok == false) some pages may remain PROT_READ — the
     * unconditional unseal in crystal_region_destroy_shared restores them. */
    r->sealed = ok;
#else
    (void)r;
#endif
}

/* Frees a shared region's storage unconditionally (rc==0 path and process-
 * exit teardown). O(1) per page: regions contain pure data, nothing to
 * finalize. The unseal is unconditional (not gated on r->sealed) so pages
 * left PROT_READ by a partially failed seal are restored before free(). */
static void crystal_region_destroy_shared(CrystalRegion *r) {
    crystal_region_unseal(r);
    arena_page_free_list(r->pages);
    free(r);
}

/* Debug global region counter + process-exit leak report (R29); also frees
 * whatever is still registered so ASAN-built test binaries exit clean.
 *
 * Exit-time edges (all acceptable while the registry is empty at exit —
 * Round A — and to be revisited when the evaluator holds handles):
 *   - atexit handlers run LIFO: handlers registered EARLIER than this one
 *     run AFTER it, so any such handler that value_free()s a shared handle
 *     would release into a registry this teardown has already destroyed.
 *   - A racing thread can be inside crystal_region_release with rc already
 *     at zero but shared_registry_remove not yet run; this teardown could
 *     then double-free that region. Threads must not be releasing handles
 *     during exit.
 *   - g_shared_atexit_registered is never reset, so a (hypothetical)
 *     create-after-exit-teardown would not re-register; worst case is a
 *     leak at exit, never a use-after-free. */
static void shared_registry_atexit(void) {
    SHARED_REG_LOCK();
#ifndef NDEBUG
    for (size_t i = 0; i < g_shared_count; i++) {
        CrystalRegion *r = g_shared_regions[i];
        fprintf(stderr, "lattice: leaked shared crystal region #%zu at exit (rc=%zu, %zu bytes)\n", r->id,
                atomic_load_explicit(&r->rc, memory_order_relaxed), r->total_bytes);
    }
#endif
    for (size_t i = 0; i < g_shared_count; i++) crystal_region_destroy_shared(g_shared_regions[i]);
    free(g_shared_regions);
    g_shared_regions = NULL;
    g_shared_count = 0;
    g_shared_cap = 0;
    atomic_store_explicit(&g_shared_live, 0, memory_order_relaxed);
    SHARED_REG_UNLOCK();
}

static void shared_registry_remove(CrystalRegion *r) {
    SHARED_REG_LOCK();
    for (size_t i = 0; i < g_shared_count; i++) {
        if (g_shared_regions[i] == r) {
            g_shared_regions[i] = g_shared_regions[g_shared_count - 1];
            g_shared_count--;
            break;
        }
    }
    SHARED_REG_UNLOCK();
}

CrystalRegion *crystal_region_create_shared(void) {
    CrystalRegion *r = calloc(1, sizeof(CrystalRegion));
    if (!r) return NULL;
    /* Low-bit tag soundness: malloc guarantees alignment, but the whole
     * scheme rests on it — assert anyway. */
    assert(((uintptr_t)r & 1u) == 0);
    r->shared = true;
#ifdef LATTICE_REGION_SEAL
    r->page_aligned = true;
#endif
    r->id = atomic_fetch_add_explicit(&g_shared_created, 1, memory_order_relaxed);
    r->pages = r->page_aligned ? arena_page_new_aligned(ARENA_PAGE_SIZE) : arena_page_new(ARENA_PAGE_SIZE);
    if (!r->pages) {
        free(r);
        return NULL;
    }
    atomic_store_explicit(&r->rc, 1, memory_order_relaxed);

    SHARED_REG_LOCK();
    if (!g_shared_atexit_registered) {
        atexit(shared_registry_atexit);
        g_shared_atexit_registered = true;
    }
    if (g_shared_count >= g_shared_cap) {
        size_t ncap = g_shared_cap ? g_shared_cap * 2 : 8;
        CrystalRegion **n = realloc(g_shared_regions, ncap * sizeof(CrystalRegion *));
        if (!n) {
            SHARED_REG_UNLOCK();
            crystal_region_destroy_shared(r);
            return NULL;
        }
        g_shared_regions = n;
        g_shared_cap = ncap;
    }
    g_shared_regions[g_shared_count++] = r;
    SHARED_REG_UNLOCK();
    atomic_fetch_add_explicit(&g_shared_live, 1, memory_order_relaxed);
    return r;
}

void crystal_region_retain(CrystalRegion *r) {
    assert(r && r->shared);
    atomic_fetch_add_explicit(&r->rc, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&r->dbg_retains, 1, memory_order_relaxed);
}

void crystal_region_release(CrystalRegion *r) {
    assert(r && r->shared);
    atomic_fetch_add_explicit(&r->dbg_releases, 1, memory_order_relaxed);
    if (atomic_fetch_sub_explicit(&r->rc, 1, memory_order_acq_rel) == 1) {
        atomic_thread_fence(memory_order_acquire);
        /* rc-ledger teardown assert (H16): creation ref + retains == releases */
        assert(atomic_load_explicit(&r->dbg_retains, memory_order_relaxed) + 1 ==
               atomic_load_explicit(&r->dbg_releases, memory_order_relaxed));
        shared_registry_remove(r);
        crystal_region_destroy_shared(r);
        atomic_fetch_sub_explicit(&g_shared_live, 1, memory_order_relaxed);
    }
}

size_t crystal_region_refcount(CrystalRegion *r) { return atomic_load_explicit(&r->rc, memory_order_acquire); }

size_t crystal_region_dbg_retains(CrystalRegion *r) {
    return atomic_load_explicit(&r->dbg_retains, memory_order_relaxed);
}

size_t crystal_region_dbg_releases(CrystalRegion *r) {
    return atomic_load_explicit(&r->dbg_releases, memory_order_relaxed);
}

bool crystal_region_shared_active(void) { return atomic_load_explicit(&g_shared_live, memory_order_relaxed) != 0; }

size_t crystal_region_live_count(void) { return atomic_load_explicit(&g_shared_live, memory_order_relaxed); }

size_t crystal_region_created_total(void) { return atomic_load_explicit(&g_shared_created, memory_order_relaxed); }

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
    ArenaPage *np = r->page_aligned ? arena_page_new_aligned(page_cap) : arena_page_new(page_cap);
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
    if (!rm) return NULL;
    rm->cap = 8;
    rm->regions = malloc(rm->cap * sizeof(CrystalRegion *));
    if (!rm->regions) {
        free(rm);
        return NULL;
    }
    return rm;
}

void region_manager_free(RegionManager *rm) {
    if (!rm) return;
    for (size_t i = 0; i < rm->count; i++) { crystal_region_free(rm->regions[i]); }
    free(rm->regions);
    free(rm);
}

Epoch region_advance_epoch(RegionManager *rm) { return ++rm->current_epoch; }

Epoch region_current_epoch(const RegionManager *rm) { return rm->current_epoch; }

CrystalRegion *region_create(RegionManager *rm) {
    if (rm->count >= rm->cap) {
        rm->cap *= 2;
        rm->regions = realloc(rm->regions, rm->cap * sizeof(CrystalRegion *));
    }
    CrystalRegion *r = crystal_region_new(rm->next_id++, rm->current_epoch);
    rm->regions[rm->count++] = r;
    rm->total_allocs++;
    if (rm->count > rm->peak_count) rm->peak_count = rm->count;
    return r;
}

static int regionid_cmp(const void *a, const void *b) {
    RegionId ra = *(const RegionId *)a;
    RegionId rb = *(const RegionId *)b;
    return (ra > rb) - (ra < rb);
}

static bool regionid_bsearch(const RegionId *sorted, size_t count, RegionId target) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (sorted[mid] < target) lo = mid + 1;
        else if (sorted[mid] > target) hi = mid;
        else return true;
    }
    return false;
}

size_t region_collect(RegionManager *rm, RegionId *reachable, size_t reachable_count) {
    if (reachable_count > 1) qsort(reachable, reachable_count, sizeof(RegionId), regionid_cmp);

    size_t freed = 0;
    size_t i = 0;
    while (i < rm->count) {
        if (!regionid_bsearch(reachable, reachable_count, rm->regions[i]->id)) {
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

size_t region_count(const RegionManager *rm) { return rm->count; }

size_t region_total_allocs(const RegionManager *rm) { return rm->total_allocs; }

size_t region_live_data_bytes(const RegionManager *rm) {
    size_t total = 0;
    for (size_t i = 0; i < rm->count; i++) total += rm->regions[i]->total_bytes;
    return total;
}

/* ── Dual Heap ── */

DualHeap *dual_heap_new(void) {
    DualHeap *dh = malloc(sizeof(DualHeap));
    if (!dh) return NULL;
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
