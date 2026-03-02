#ifndef GC_H
#define GC_H

#include <stddef.h>
#include <stdbool.h>
#include "value.h"

/* ── GC Object Header ──
 *
 * Every GC-managed allocation is prefixed with a GCObject header that
 * links it into an intrusive linked list.  The mark bit is used during
 * the mark phase; unmarked objects are freed during sweep.
 */
typedef struct GCObject {
    struct GCObject *next; /* intrusive linked list of all GC objects */
    bool marked;           /* set during mark phase, cleared during sweep */
} GCObject;

/* ── Incremental GC Phase ──
 *
 * The incremental collector is a state machine that spreads mark-and-sweep
 * work across multiple safe points (OP_RESET_EPHEMERAL) to bound pause time.
 */
typedef enum {
    GC_PHASE_IDLE,       /* no collection in progress */
    GC_PHASE_MARK_ROOTS, /* scanning VM roots into gray worklist */
    GC_PHASE_MARK_TRACE, /* tracing gray objects (budgeted) */
    GC_PHASE_SWEEP,      /* sweeping unmarked objects (budgeted) */
} GCPhase;

/* ── Garbage Collector State ──
 *
 * The GC is a mark-and-sweep collector.  It maintains a linked list
 * of all allocated objects and triggers collection when the object count
 * exceeds next_gc.  The threshold grows adaptively after each collection.
 *
 * When incremental mode is enabled, collection work is spread across
 * multiple safe points using a tri-color marking scheme with a gray worklist.
 */
typedef struct {
    GCObject *all_objects;  /* linked list head */
    size_t object_count;    /* number of tracked allocations */
    size_t next_gc;         /* threshold for triggering next collection */
    size_t bytes_allocated; /* total bytes allocated under GC */
    bool enabled;           /* GC is enabled */
    bool stress;            /* stress mode: collect on every allocation */
    bool incremental;       /* incremental mode: spread work across safe points */
    /* Incremental state */
    GCPhase phase;          /* current state machine phase */
    LatValue **gray_stack;  /* worklist of values to trace */
    size_t gray_count;      /* number of entries in gray stack */
    size_t gray_cap;        /* capacity of gray stack */
    GCObject **sweep_prev;  /* pointer to previous node's next field */
    GCObject *sweep_cursor; /* current node being examined in sweep */
    size_t sweep_freed;     /* objects freed in current sweep cycle */
    size_t mark_budget;     /* max gray values to trace per step (default 64) */
    size_t sweep_budget;    /* max objects to sweep per step (default 128) */
    bool roots_rescanned;   /* whether post-mark root re-scan has been done */
    /* Stats */
    size_t total_collected; /* total objects freed across all cycles */
    size_t total_cycles;    /* number of GC cycles run */
} GC;

/* ── Lifecycle ── */

/* Initialize GC state (does not allocate).  enabled=false by default. */
void gc_init(GC *gc);

/* Free all remaining GC-tracked objects. */
void gc_free(GC *gc);

/* ── Allocation ── */

/* Allocate 'size' bytes tracked by the GC.  Returns a pointer to the
 * usable memory (after the GCObject header).  The allocation is added
 * to the all_objects list.  Returns NULL on OOM. */
void *gc_alloc(GC *gc, size_t size);

/* Duplicate a string under GC tracking. */
char *gc_strdup(GC *gc, const char *s);

/* Remove a specific pointer from GC tracking (for promoting to non-GC
 * ownership, e.g. interning).  Does NOT free the memory. */
bool gc_untrack(GC *gc, void *ptr);

/* ── Collection ── */

/* Run a full mark-and-sweep cycle.  The vm parameter is an opaque pointer
 * to a StackVM (avoids circular include with stackvm.h). */
void gc_collect(GC *gc, void *vm);

/* Mark a single LatValue and all values reachable from it. */
void gc_mark_value(GC *gc, LatValue *val);

/* Mark a pointer as reachable (looks it up in the GC object list). */
void gc_mark_ptr(GC *gc, void *ptr);

/* Check if a GC cycle should run, and run it if so.  vm is a StackVM*. */
void gc_maybe_collect(GC *gc, void *vm);

/* Perform one incremental GC step.  Called at each safe point when
 * gc->incremental is true.  vm is a StackVM*. */
void gc_incremental_step(GC *gc, void *vm);

#endif /* GC_H */
