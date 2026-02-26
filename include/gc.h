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

/* ── Garbage Collector State ──
 *
 * The GC is a simple mark-and-sweep collector.  It maintains a linked list
 * of all allocated objects and triggers collection when the object count
 * exceeds next_gc.  The threshold grows adaptively after each collection.
 */
typedef struct {
    GCObject *all_objects;  /* linked list head */
    size_t object_count;    /* number of tracked allocations */
    size_t next_gc;         /* threshold for triggering next collection */
    size_t bytes_allocated; /* total bytes allocated under GC */
    bool enabled;           /* GC is enabled */
    bool stress;            /* stress mode: collect on every allocation */
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

#endif /* GC_H */
