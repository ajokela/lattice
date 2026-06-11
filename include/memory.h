#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

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
    void *ptr;
    size_t size;
    bool marked;
    struct FluidAlloc *next;
} FluidAlloc;

typedef struct FluidHeap {
    FluidAlloc *allocs;
    size_t total_bytes;
    size_t alloc_count;
    size_t gc_threshold;
    size_t peak_bytes;
    size_t cumulative_bytes;
} FluidHeap;

FluidHeap *fluid_heap_new(void);
void fluid_heap_free(FluidHeap *h);
void *fluid_alloc(FluidHeap *h, size_t size);
bool fluid_dealloc(FluidHeap *h, void *ptr);
/* Resize a block, keeping the heap's allocation tracking consistent (plain
 * realloc() on a tracked block leaves a stale pointer that double-frees at
 * teardown). If ptr is not tracked by this heap it is realloc()'d directly. */
void *fluid_realloc(FluidHeap *h, void *ptr, size_t new_size);
size_t fluid_live_count(const FluidHeap *h);
size_t fluid_total_bytes(const FluidHeap *h);

/* GC support: mark a specific pointer as reachable */
bool fluid_mark(FluidHeap *h, void *ptr);
/* GC support: clear all marks */
void fluid_unmark_all(FluidHeap *h);
/* GC support: sweep unmarked allocations, return count freed */
size_t fluid_sweep(FluidHeap *h);

/* ── Arena Pages ── */

#define ARENA_PAGE_SIZE 4096

typedef struct ArenaPage {
    uint8_t *data;
    size_t used;
    size_t cap;
    struct ArenaPage *next;
} ArenaPage;

/* ── Crystal Region ── */

typedef size_t RegionId;
typedef size_t Epoch;

typedef struct CrystalRegion {
    RegionId id;
    Epoch epoch;
    ArenaPage *pages;   /* linked list of arena pages */
    size_t total_bytes; /* total bytes used across all pages */
    /* ── Shared crystal regions (Crystal-by-Reference, Stage 2) ──
     * The fields below are zero on legacy RegionManager regions (which are
     * calloc'd) and are only used by regions from
     * crystal_region_create_shared(). */
    _Atomic size_t rc; /* shared-region refcount (0 = legacy region) */
    bool shared;       /* process-global refcounted region, never RegionManager-owned */
    bool page_aligned; /* pages are page-aligned (required for the debug seal) */
    bool sealed;       /* debug backstop: pages are mprotect(PROT_READ)ed */
    /* rc-ledger (H16 verification): total retains (excluding the creation
     * reference) and total releases; the final release asserts
     * retains + 1 == releases in debug builds. */
    _Atomic size_t dbg_retains;
    _Atomic size_t dbg_releases;
} CrystalRegion;

/* ── Region Manager ── */

typedef struct RegionManager {
    CrystalRegion **regions;
    size_t count;
    size_t cap;
    size_t next_id;
    Epoch current_epoch;
    size_t total_allocs;
    size_t peak_count;
    size_t cumulative_data_bytes;
} RegionManager;

RegionManager *region_manager_new(void);
void region_manager_free(RegionManager *rm);
Epoch region_advance_epoch(RegionManager *rm);
Epoch region_current_epoch(const RegionManager *rm);
CrystalRegion *region_create(RegionManager *rm);
size_t region_collect(RegionManager *rm, RegionId *reachable, size_t reachable_count);
size_t region_count(const RegionManager *rm);
size_t region_total_allocs(const RegionManager *rm);
size_t region_live_data_bytes(const RegionManager *rm);

/* ── Arena allocation ── */

void *arena_alloc(CrystalRegion *r, size_t size);
void *arena_calloc(CrystalRegion *r, size_t count, size_t size);
char *arena_strdup(CrystalRegion *r, const char *s);

/* ── Shared crystal regions (Crystal-by-Reference, Stage 2) ──
 *
 * Sealed, atomically-refcounted, process-global regions backing shared
 * crystal values (see REGION_IS_SHARED_ID in value.h). Pages are plain
 * global malloc — never routed through any thread's FluidHeap — so they
 * survive sender-thread teardown and the last release may safely run on a
 * different thread than the one that created the region.
 *
 * Atomics / memory-ordering contract (design §2.7). The memory-order choice
 * here is deliberate — relaxed retain, acq_rel release plus an acquire
 * fence on the rc==0 path — and stronger-reasoned than the channel refcount
 * precedent (src/channel.c uses __atomic_add_fetch/__atomic_sub_fetch
 * builtins with SEQ_CST throughout; this code uses C11 stdatomic with the
 * minimal orders the refcount idiom actually requires):
 *   - retain: fetch_add(relaxed) — bumping an already-owned reference needs
 *     no ordering. A retain must HAPPEN-BEFORE the handle becomes visible
 *     to another thread; the channel mutex and pthread_create provide that
 *     edge — retain/release themselves only guarantee the count, not
 *     publication.
 *   - release: fetch_sub(acq_rel) + acquire fence, then an O(1) page-list
 *     free when rc hits zero — the release half orders each thread's prior
 *     uses of the region before its decrement, and the acquire half/fence
 *     makes all of them visible to the freeing thread. The O(1) free is
 *     sound only because regions contain pure data (no channels/refs/
 *     iterators/envs to finalize).
 *
 * The mutex-protected global registry behind these functions exists ONLY
 * for stats, leak diagnostics, and process-exit teardown; retain and
 * non-final release never touch it. It lives in memory.c so it is linked
 * by ALL targets (clat, clat-run, wasm_api, LSP/DAP). On wasm
 * (single-threaded) the atomics compile to plain ops and the registry
 * mutex compiles away. */
CrystalRegion *crystal_region_create_shared(void); /* rc = 1, registered */
void crystal_region_retain(CrystalRegion *r);
void crystal_region_release(CrystalRegion *r); /* frees region at rc == 0 */
/* Debug backstop: mprotect(PROT_READ) the region's pages so any stray write
 * after seal segfaults loudly. No-op when unsupported (Windows/wasm) or in
 * NDEBUG builds; r->sealed reports whether the seal was applied. */
void crystal_region_seal(CrystalRegion *r);
size_t crystal_region_refcount(CrystalRegion *r);
size_t crystal_region_dbg_retains(CrystalRegion *r);
size_t crystal_region_dbg_releases(CrystalRegion *r);
/* Round A dormancy gate: true while any shared region is live. While the
 * tree-walker's legacy numeric region ids (which can be odd) still
 * circulate, every consumer of REGION_IS_SHARED_ID must ALSO check this
 * gate; it is constant false in evaluator runs until Round B retires the
 * numeric ids (then the gate check can be dropped). */
bool crystal_region_shared_active(void);
size_t crystal_region_live_count(void);    /* registry: live shared regions */
size_t crystal_region_created_total(void); /* registry: ever created */

/* ── Bump Arena (ephemeral allocator) ── */

typedef struct BumpArena {
    ArenaPage *pages;      /* current page pointer */
    ArenaPage *first_page; /* head of chain (kept across resets) */
    size_t total_bytes;
} BumpArena;

BumpArena *bump_arena_new(void);
void bump_arena_free(BumpArena *ba);
void bump_arena_reset(BumpArena *ba); /* reset all pages, keep chain */
void *bump_alloc(BumpArena *ba, size_t size);
char *bump_strdup(BumpArena *ba, const char *s);

/* ── Dual Heap ── */

typedef struct DualHeap {
    FluidHeap *fluid;
    RegionManager *regions;
} DualHeap;

DualHeap *dual_heap_new(void);
void dual_heap_free(DualHeap *dh);

#endif /* MEMORY_H */
