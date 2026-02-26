#ifndef ITERATOR_H
#define ITERATOR_H

#include "value.h"

/* ── Iterator state types ── */

/* Array iterator: iterates over a cloned array */
typedef struct {
    LatValue *elems;
    size_t len;
    size_t index;
} IterArrayState;

/* Range iterator: lazy integer range with step */
typedef struct {
    int64_t current;
    int64_t end;
    int64_t step;
} IterRangeState;

/* Map iterator: iterates over keys of a cloned map */
typedef struct {
    char **keys;
    size_t len;
    size_t index;
} IterMapState;

/* String iterator: iterates over characters */
typedef struct {
    char *str;
    size_t len;
    size_t index;
} IterStringState;

/* Repeat iterator: yields a value n times (or infinitely) */
typedef struct {
    LatValue value;
    int64_t remaining; /* -1 = infinite */
} IterRepeatState;

/* Map-transform iterator: wraps another iterator + closure */
typedef struct {
    LatValue inner;   /* the wrapped iterator */
    LatValue closure; /* the transform closure */
    void *vm_ctx;     /* opaque VM context for calling closures */
    LatValue (*call_fn)(void *ctx, LatValue *closure, LatValue *args, int argc);
} IterMapTransformState;

/* Filter iterator: wraps another iterator + predicate */
typedef struct {
    LatValue inner;
    LatValue closure;
    void *vm_ctx;
    LatValue (*call_fn)(void *ctx, LatValue *closure, LatValue *args, int argc);
} IterFilterState;

/* Take iterator: wraps another iterator, limits to n elements */
typedef struct {
    LatValue inner;
    int64_t remaining;
} IterTakeState;

/* Skip iterator: wraps another iterator, skips first n */
typedef struct {
    LatValue inner;
    int64_t skip_count;
    bool skipped;
} IterSkipState;

/* Enumerate iterator: wraps another, yields [index, value] pairs */
typedef struct {
    LatValue inner;
    int64_t index;
} IterEnumerateState;

/* Zip iterator: pairs values from two iterators */
typedef struct {
    LatValue left;
    LatValue right;
} IterZipState;

/* ── Iterator constructors ── */

/* Create iterator from array (clones the array) */
LatValue iter_from_array(const LatValue *arr);

/* Create iterator from map (iterates over keys) */
LatValue iter_from_map(const LatValue *map);

/* Create iterator from string (iterates over characters) */
LatValue iter_from_string(const LatValue *str);

/* Create iterator from range (lazy, no allocation) */
LatValue iter_from_range(int64_t start, int64_t end);

/* Create lazy range iterator with step */
LatValue iter_range(int64_t start, int64_t end, int64_t step);

/* Create repeat iterator: yields value n times, or infinitely if count < 0 */
LatValue iter_repeat(LatValue value, int64_t count);

/* ── Chaining constructors (wrap existing iterators) ── */

LatValue iter_map_transform(LatValue inner, LatValue closure, void *vm_ctx,
                            LatValue (*call_fn)(void *, LatValue *, LatValue *, int));

LatValue iter_filter(LatValue inner, LatValue closure, void *vm_ctx,
                     LatValue (*call_fn)(void *, LatValue *, LatValue *, int));

LatValue iter_take(LatValue inner, int64_t n);

LatValue iter_skip(LatValue inner, int64_t n);

LatValue iter_enumerate(LatValue inner);

LatValue iter_zip(LatValue left, LatValue right);

/* ── Eager consumers ── */

/* Collect all remaining values into an array */
LatValue iter_collect(LatValue *iter);

/* Reduce with accumulator */
LatValue iter_reduce(LatValue *iter, LatValue init, LatValue *closure, void *vm_ctx,
                     LatValue (*call_fn)(void *, LatValue *, LatValue *, int));

/* Short-circuit boolean tests */
bool iter_any(LatValue *iter, LatValue *closure, void *vm_ctx,
              LatValue (*call_fn)(void *, LatValue *, LatValue *, int));

bool iter_all(LatValue *iter, LatValue *closure, void *vm_ctx,
              LatValue (*call_fn)(void *, LatValue *, LatValue *, int));

/* Count elements (consuming) */
int64_t iter_count(LatValue *iter);

/* ── Iterator protocol ── */

/* Call next on an iterator, returns value and sets *done */
static inline LatValue iter_next(LatValue *iter, bool *done) {
    return iter->as.iterator.next_fn(iter->as.iterator.state, done);
}

#endif /* ITERATOR_H */
