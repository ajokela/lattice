#include "iterator.h"
#include "value.h"
#include <stdlib.h>
#include <string.h>

/* ── Array iterator ── */

static LatValue iter_array_next(void *state, bool *done) {
    IterArrayState *s = (IterArrayState *)state;
    if (s->index >= s->len) {
        *done = true;
        return value_nil();
    }
    *done = false;
    return value_deep_clone(&s->elems[s->index++]);
}

static void iter_array_free(void *state) {
    IterArrayState *s = (IterArrayState *)state;
    for (size_t i = 0; i < s->len; i++) value_free(&s->elems[i]);
    free(s->elems);
    free(s);
}

LatValue iter_from_array(const LatValue *arr) {
    IterArrayState *s = malloc(sizeof(IterArrayState));
    if (!s) return value_nil();
    s->len = arr->as.array.len;
    s->index = 0;
    s->elems = malloc(s->len * sizeof(LatValue));
    if (!s->elems) {
        free(s);
        return value_nil();
    }
    for (size_t i = 0; i < s->len; i++) s->elems[i] = value_deep_clone(&arr->as.array.elems[i]);
    return value_iterator(iter_array_next, s, iter_array_free);
}

/* ── Map iterator (keys) ── */

static LatValue iter_map_next(void *state, bool *done) {
    IterMapState *s = (IterMapState *)state;
    if (s->index >= s->len) {
        *done = true;
        return value_nil();
    }
    *done = false;
    return value_string(s->keys[s->index++]);
}

static void iter_map_free(void *state) {
    IterMapState *s = (IterMapState *)state;
    for (size_t i = 0; i < s->len; i++) free(s->keys[i]);
    free(s->keys);
    free(s);
}

LatValue iter_from_map(const LatValue *map) {
    IterMapState *s = malloc(sizeof(IterMapState));
    if (!s) return value_nil();
    LatMap *m = map->as.map.map;
    size_t count = lat_map_len(m);
    s->keys = malloc((count > 0 ? count : 1) * sizeof(char *));
    if (!s->keys) {
        free(s);
        return value_nil();
    }
    s->len = 0;
    s->index = 0;
    for (size_t i = 0; i < m->cap; i++) {
        if (m->entries[i].state == MAP_OCCUPIED) { s->keys[s->len++] = strdup(m->entries[i].key); }
    }
    return value_iterator(iter_map_next, s, iter_map_free);
}

/* ── String iterator ── */

static LatValue iter_string_next(void *state, bool *done) {
    IterStringState *s = (IterStringState *)state;
    if (s->index >= s->len) {
        *done = true;
        return value_nil();
    }
    *done = false;
    char ch[2] = {s->str[s->index++], '\0'};
    return value_string(ch);
}

static void iter_string_free(void *state) {
    IterStringState *s = (IterStringState *)state;
    free(s->str);
    free(s);
}

LatValue iter_from_string(const LatValue *str) {
    IterStringState *s = malloc(sizeof(IterStringState));
    if (!s) return value_nil();
    s->str = strdup(str->as.str_val);
    s->len = strlen(s->str);
    s->index = 0;
    return value_iterator(iter_string_next, s, iter_string_free);
}

/* ── Range iterator (lazy) ── */

static LatValue iter_range_next(void *state, bool *done) {
    IterRangeState *s = (IterRangeState *)state;
    if (s->step > 0 && s->current >= s->end) {
        *done = true;
        return value_nil();
    }
    if (s->step < 0 && s->current <= s->end) {
        *done = true;
        return value_nil();
    }
    *done = false;
    int64_t val = s->current;
    s->current += s->step;
    return value_int(val);
}

static void iter_range_free(void *state) { free(state); }

LatValue iter_from_range(int64_t start, int64_t end) {
    IterRangeState *s = malloc(sizeof(IterRangeState));
    if (!s) return value_nil();
    s->current = start;
    s->end = end;
    s->step = (start <= end) ? 1 : -1;
    return value_iterator(iter_range_next, s, iter_range_free);
}

LatValue iter_range(int64_t start, int64_t end, int64_t step) {
    IterRangeState *s = malloc(sizeof(IterRangeState));
    if (!s) return value_nil();
    s->current = start;
    s->end = end;
    s->step = step;
    return value_iterator(iter_range_next, s, iter_range_free);
}

/* ── Repeat iterator ── */

static LatValue iter_repeat_next(void *state, bool *done) {
    IterRepeatState *s = (IterRepeatState *)state;
    if (s->remaining == 0) {
        *done = true;
        return value_nil();
    }
    *done = false;
    if (s->remaining > 0) s->remaining--;
    return value_deep_clone(&s->value);
}

static void iter_repeat_free(void *state) {
    IterRepeatState *s = (IterRepeatState *)state;
    value_free(&s->value);
    free(s);
}

LatValue iter_repeat(LatValue value, int64_t count) {
    IterRepeatState *s = malloc(sizeof(IterRepeatState));
    if (!s) return value_nil();
    s->value = value_deep_clone(&value);
    s->remaining = count; /* -1 = infinite */
    return value_iterator(iter_repeat_next, s, iter_repeat_free);
}

/* ── Map-transform iterator ── */

static LatValue iter_map_transform_next(void *state, bool *done) {
    IterMapTransformState *s = (IterMapTransformState *)state;
    LatValue val = iter_next(&s->inner, done);
    if (*done) return val;
    /* Call the transform closure */
    LatValue args[1] = {val};
    LatValue result = s->call_fn(s->vm_ctx, &s->closure, args, 1);
    value_free(&val);
    return result;
}

static void iter_map_transform_free(void *state) {
    IterMapTransformState *s = (IterMapTransformState *)state;
    value_free(&s->inner);
    value_free(&s->closure);
    free(s);
}

LatValue iter_map_transform(LatValue inner, LatValue closure, void *vm_ctx,
                            LatValue (*call_fn)(void *, LatValue *, LatValue *, int)) {
    IterMapTransformState *s = malloc(sizeof(IterMapTransformState));
    if (!s) return value_nil();
    s->inner = inner; /* takes ownership */
    s->closure = value_deep_clone(&closure);
    s->vm_ctx = vm_ctx;
    s->call_fn = call_fn;
    return value_iterator(iter_map_transform_next, s, iter_map_transform_free);
}

/* ── Filter iterator ── */

static LatValue iter_filter_next(void *state, bool *done) {
    IterFilterState *s = (IterFilterState *)state;
    for (;;) {
        LatValue val = iter_next(&s->inner, done);
        if (*done) return val;
        /* Test the predicate */
        LatValue args[1];
        args[0] = value_deep_clone(&val);
        LatValue test = s->call_fn(s->vm_ctx, &s->closure, args, 1);
        bool passes = value_is_truthy(&test);
        value_free(&test);
        value_free(&args[0]);
        if (passes) return val;
        value_free(&val);
    }
}

static void iter_filter_free(void *state) {
    IterFilterState *s = (IterFilterState *)state;
    value_free(&s->inner);
    value_free(&s->closure);
    free(s);
}

LatValue iter_filter(LatValue inner, LatValue closure, void *vm_ctx,
                     LatValue (*call_fn)(void *, LatValue *, LatValue *, int)) {
    IterFilterState *s = malloc(sizeof(IterFilterState));
    if (!s) return value_nil();
    s->inner = inner; /* takes ownership */
    s->closure = value_deep_clone(&closure);
    s->vm_ctx = vm_ctx;
    s->call_fn = call_fn;
    return value_iterator(iter_filter_next, s, iter_filter_free);
}

/* ── Take iterator ── */

static LatValue iter_take_next(void *state, bool *done) {
    IterTakeState *s = (IterTakeState *)state;
    if (s->remaining <= 0) {
        *done = true;
        return value_nil();
    }
    LatValue val = iter_next(&s->inner, done);
    if (!*done) s->remaining--;
    return val;
}

static void iter_take_free(void *state) {
    IterTakeState *s = (IterTakeState *)state;
    value_free(&s->inner);
    free(s);
}

LatValue iter_take(LatValue inner, int64_t n) {
    IterTakeState *s = malloc(sizeof(IterTakeState));
    if (!s) return value_nil();
    s->inner = inner; /* takes ownership */
    s->remaining = n;
    return value_iterator(iter_take_next, s, iter_take_free);
}

/* ── Skip iterator ── */

static LatValue iter_skip_next(void *state, bool *done) {
    IterSkipState *s = (IterSkipState *)state;
    if (!s->skipped) {
        s->skipped = true;
        for (int64_t i = 0; i < s->skip_count; i++) {
            LatValue discard = iter_next(&s->inner, done);
            value_free(&discard);
            if (*done) return value_nil();
        }
    }
    return iter_next(&s->inner, done);
}

static void iter_skip_free(void *state) {
    IterSkipState *s = (IterSkipState *)state;
    value_free(&s->inner);
    free(s);
}

LatValue iter_skip(LatValue inner, int64_t n) {
    IterSkipState *s = malloc(sizeof(IterSkipState));
    if (!s) return value_nil();
    s->inner = inner; /* takes ownership */
    s->skip_count = n;
    s->skipped = false;
    return value_iterator(iter_skip_next, s, iter_skip_free);
}

/* ── Enumerate iterator ── */

static LatValue iter_enumerate_next(void *state, bool *done) {
    IterEnumerateState *s = (IterEnumerateState *)state;
    LatValue val = iter_next(&s->inner, done);
    if (*done) return val;
    LatValue pair[2];
    pair[0] = value_int(s->index++);
    pair[1] = val;
    LatValue arr = value_array(pair, 2);
    return arr;
}

static void iter_enumerate_free(void *state) {
    IterEnumerateState *s = (IterEnumerateState *)state;
    value_free(&s->inner);
    free(s);
}

LatValue iter_enumerate(LatValue inner) {
    IterEnumerateState *s = malloc(sizeof(IterEnumerateState));
    if (!s) return value_nil();
    s->inner = inner; /* takes ownership */
    s->index = 0;
    return value_iterator(iter_enumerate_next, s, iter_enumerate_free);
}

/* ── Zip iterator ── */

static LatValue iter_zip_next(void *state, bool *done) {
    IterZipState *s = (IterZipState *)state;
    bool left_done = false, right_done = false;
    LatValue l = iter_next(&s->left, &left_done);
    LatValue r = iter_next(&s->right, &right_done);
    if (left_done || right_done) {
        value_free(&l);
        value_free(&r);
        *done = true;
        return value_nil();
    }
    *done = false;
    LatValue pair[2] = {l, r};
    LatValue arr = value_array(pair, 2);
    return arr;
}

static void iter_zip_free(void *state) {
    IterZipState *s = (IterZipState *)state;
    value_free(&s->left);
    value_free(&s->right);
    free(s);
}

LatValue iter_zip(LatValue left, LatValue right) {
    IterZipState *s = malloc(sizeof(IterZipState));
    if (!s) return value_nil();
    s->left = left;   /* takes ownership */
    s->right = right; /* takes ownership */
    return value_iterator(iter_zip_next, s, iter_zip_free);
}

/* ── Eager consumers ── */

LatValue iter_collect(LatValue *iter) {
    size_t cap = 8;
    size_t len = 0;
    LatValue *elems = malloc(cap * sizeof(LatValue));
    if (!elems) return value_array(NULL, 0);

    for (;;) {
        bool done = false;
        LatValue val = iter_next(iter, &done);
        if (done) break;
        if (len >= cap) {
            cap *= 2;
            elems = realloc(elems, cap * sizeof(LatValue));
            if (!elems) return value_array(NULL, 0);
        }
        elems[len++] = val;
    }

    LatValue result = value_array(elems, len);
    free(elems);
    return result;
}

LatValue iter_reduce(LatValue *iter, LatValue init, LatValue *closure, void *vm_ctx,
                     LatValue (*call_fn)(void *, LatValue *, LatValue *, int)) {
    LatValue acc = value_deep_clone(&init);

    for (;;) {
        bool done = false;
        LatValue val = iter_next(iter, &done);
        if (done) break;
        LatValue args[2];
        args[0] = acc;
        args[1] = val;
        acc = call_fn(vm_ctx, closure, args, 2);
        value_free(&args[0]);
        value_free(&args[1]);
    }
    return acc;
}

bool iter_any(LatValue *iter, LatValue *closure, void *vm_ctx,
              LatValue (*call_fn)(void *, LatValue *, LatValue *, int)) {
    for (;;) {
        bool done = false;
        LatValue val = iter_next(iter, &done);
        if (done) return false;
        LatValue args[1];
        args[0] = val;
        LatValue test = call_fn(vm_ctx, closure, args, 1);
        bool truthy = value_is_truthy(&test);
        value_free(&test);
        value_free(&val);
        if (truthy) return true;
    }
}

bool iter_all(LatValue *iter, LatValue *closure, void *vm_ctx,
              LatValue (*call_fn)(void *, LatValue *, LatValue *, int)) {
    for (;;) {
        bool done = false;
        LatValue val = iter_next(iter, &done);
        if (done) return true;
        LatValue args[1];
        args[0] = val;
        LatValue test = call_fn(vm_ctx, closure, args, 1);
        bool truthy = value_is_truthy(&test);
        value_free(&test);
        value_free(&val);
        if (!truthy) return false;
    }
}

int64_t iter_count(LatValue *iter) {
    int64_t n = 0;
    for (;;) {
        bool done = false;
        LatValue val = iter_next(iter, &done);
        if (done) break;
        value_free(&val);
        n++;
    }
    return n;
}
