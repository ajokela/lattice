#include "builtin_methods.h"
#include "ds/hashmap.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================
 * Array methods (no closures)
 * ======================================================================== */

/// @method Array.contains(val: Any) -> Bool
/// @category Array Methods
/// Check whether the array contains an element equal to val.
/// @example [1, 2, 3].contains(2)  // true
LatValue builtin_array_contains(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    bool found = false;
    for (size_t i = 0; i < obj->as.array.len; i++) {
        if (value_eq(&obj->as.array.elems[i], &args[0])) {
            found = true;
            break;
        }
    }
    return value_bool(found);
}

/// @method Array.enumerate() -> Array
/// @category Array Methods
/// Return an array of [index, value] pairs.
/// @example ["a", "b"].enumerate()  // [[0, "a"], [1, "b"]]
LatValue builtin_array_enumerate(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    size_t len = obj->as.array.len;
    LatValue *pairs = malloc(len * sizeof(LatValue));
    for (size_t i = 0; i < len; i++) {
        LatValue pair_elems[2];
        pair_elems[0] = value_int((int64_t)i);
        pair_elems[1] = value_deep_clone(&obj->as.array.elems[i]);
        pairs[i] = value_array(pair_elems, 2);
    }
    LatValue result = value_array(pairs, len);
    free(pairs);
    return result;
}

/// @method Array.reverse() -> Array
/// @category Array Methods
/// Return a new array with elements in reverse order.
/// @example [1, 2, 3].reverse()  // [3, 2, 1]
LatValue builtin_array_reverse(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    size_t len = obj->as.array.len;
    LatValue *elems = malloc(len * sizeof(LatValue));
    for (size_t i = 0; i < len; i++)
        elems[i] = value_deep_clone(&obj->as.array.elems[len - 1 - i]);
    LatValue result = value_array(elems, len);
    free(elems);
    return result;
}

/// @method Array.join(sep: String) -> String
/// @category Array Methods
/// Join all elements into a string separated by sep.
/// @example [1, 2, 3].join(", ")  // "1, 2, 3"
LatValue builtin_array_join(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    const char *sep_str = (args[0].type == VAL_STR) ? args[0].as.str_val : "";
    size_t sep_len = strlen(sep_str);
    size_t n = obj->as.array.len;
    char **parts = malloc(n * sizeof(char *));
    size_t *lens = malloc(n * sizeof(size_t));
    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        parts[i] = value_display(&obj->as.array.elems[i]);
        lens[i] = strlen(parts[i]);
        total += lens[i];
    }
    if (n > 1) total += sep_len * (n - 1);
    char *buf = malloc(total + 1);
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        if (i > 0) { memcpy(buf + pos, sep_str, sep_len); pos += sep_len; }
        memcpy(buf + pos, parts[i], lens[i]); pos += lens[i];
        free(parts[i]);
    }
    buf[pos] = '\0';
    free(parts); free(lens);
    return value_string_owned(buf);
}

/// @method Array.unique() -> Array
/// @category Array Methods
/// Return a new array with duplicate elements removed.
/// @example [1, 2, 2, 3].unique()  // [1, 2, 3]
LatValue builtin_array_unique(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    size_t n = obj->as.array.len;
    LatValue *res = malloc((n > 0 ? n : 1) * sizeof(LatValue));
    size_t rc = 0;
    for (size_t i = 0; i < n; i++) {
        bool dup = false;
        for (size_t j = 0; j < rc; j++)
            if (value_eq(&obj->as.array.elems[i], &res[j])) { dup = true; break; }
        if (!dup) res[rc++] = value_deep_clone(&obj->as.array.elems[i]);
    }
    LatValue r = value_array(res, rc);
    free(res);
    return r;
}

/// @method Array.index_of(val: Any) -> Int
/// @category Array Methods
/// Return the index of the first occurrence of val, or -1 if not found.
/// @example [10, 20, 30].index_of(20)  // 1
LatValue builtin_array_index_of(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    for (size_t i = 0; i < obj->as.array.len; i++) {
        if (value_eq(&obj->as.array.elems[i], &args[0]))
            return value_int((int64_t)i);
    }
    return value_int(-1);
}

/// @method Array.zip(other: Array) -> Array
/// @category Array Methods
/// Pair elements from two arrays into an array of [a, b] pairs.
/// @example [1, 2].zip(["a", "b"])  // [[1, "a"], [2, "b"]]
LatValue builtin_array_zip(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type != VAL_ARRAY)
        return value_array(NULL, 0);
    size_t n = obj->as.array.len < args[0].as.array.len
             ? obj->as.array.len : args[0].as.array.len;
    LatValue *pairs = malloc((n > 0 ? n : 1) * sizeof(LatValue));
    for (size_t i = 0; i < n; i++) {
        LatValue pe[2];
        pe[0] = value_deep_clone(&obj->as.array.elems[i]);
        pe[1] = value_deep_clone(&args[0].as.array.elems[i]);
        pairs[i] = value_array(pe, 2);
    }
    LatValue r = value_array(pairs, n);
    free(pairs);
    return r;
}

/// @method Array.sum() -> Int|Float
/// @category Array Methods
/// Return the sum of all numeric elements in the array.
/// @example [1, 2, 3].sum()  // 6
LatValue builtin_array_sum(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    bool has_float = false;
    int64_t isum = 0;
    double fsum = 0.0;
    for (size_t i = 0; i < obj->as.array.len; i++) {
        if (obj->as.array.elems[i].type == VAL_INT) {
            isum += obj->as.array.elems[i].as.int_val;
            fsum += (double)obj->as.array.elems[i].as.int_val;
        } else if (obj->as.array.elems[i].type == VAL_FLOAT) {
            has_float = true;
            fsum += obj->as.array.elems[i].as.float_val;
        }
    }
    return has_float ? value_float(fsum) : value_int(isum);
}

/// @method Array.min() -> Int|Float
/// @category Array Methods
/// Return the smallest numeric element in the array.
/// @example [3, 1, 2].min()  // 1
LatValue builtin_array_min(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count;
    if (obj->as.array.len == 0) {
        *error = strdup("min() called on empty array");
        return value_unit();
    }
    LatValue best = obj->as.array.elems[0];
    for (size_t i = 1; i < obj->as.array.len; i++) {
        LatValue *el = &obj->as.array.elems[i];
        bool less = false;
        if (el->type == VAL_INT && best.type == VAL_INT)
            less = el->as.int_val < best.as.int_val;
        else if (el->type == VAL_FLOAT || best.type == VAL_FLOAT) {
            double a = el->type == VAL_FLOAT ? el->as.float_val : (double)el->as.int_val;
            double b = best.type == VAL_FLOAT ? best.as.float_val : (double)best.as.int_val;
            less = a < b;
        }
        if (less) best = *el;
    }
    return value_deep_clone(&best);
}

/// @method Array.max() -> Int|Float
/// @category Array Methods
/// Return the largest numeric element in the array.
/// @example [3, 1, 2].max()  // 3
LatValue builtin_array_max(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count;
    if (obj->as.array.len == 0) {
        *error = strdup("max() called on empty array");
        return value_unit();
    }
    LatValue best = obj->as.array.elems[0];
    for (size_t i = 1; i < obj->as.array.len; i++) {
        LatValue *el = &obj->as.array.elems[i];
        bool greater = false;
        if (el->type == VAL_INT && best.type == VAL_INT)
            greater = el->as.int_val > best.as.int_val;
        else if (el->type == VAL_FLOAT || best.type == VAL_FLOAT) {
            double a = el->type == VAL_FLOAT ? el->as.float_val : (double)el->as.int_val;
            double b = best.type == VAL_FLOAT ? best.as.float_val : (double)best.as.int_val;
            greater = a > b;
        }
        if (greater) best = *el;
    }
    return value_deep_clone(&best);
}

/// @method Array.first() -> Any|Unit
/// @category Array Methods
/// Return the first element, or unit if the array is empty.
/// @example [10, 20].first()  // 10
LatValue builtin_array_first(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    return obj->as.array.len > 0
         ? value_deep_clone(&obj->as.array.elems[0])
         : value_unit();
}

/// @method Array.last() -> Any|Unit
/// @category Array Methods
/// Return the last element, or unit if the array is empty.
/// @example [10, 20].last()  // 20
LatValue builtin_array_last(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    return obj->as.array.len > 0
         ? value_deep_clone(&obj->as.array.elems[obj->as.array.len - 1])
         : value_unit();
}

/// @method Array.take(n: Int) -> Array
/// @category Array Methods
/// Return a new array with the first n elements.
/// @example [1, 2, 3, 4].take(2)  // [1, 2]
LatValue builtin_array_take(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    int64_t n = (args[0].type == VAL_INT) ? args[0].as.int_val : 0;
    if (n <= 0) return value_array(NULL, 0);
    size_t take_n = (size_t)n;
    if (take_n > obj->as.array.len) take_n = obj->as.array.len;
    LatValue *elems = malloc((take_n > 0 ? take_n : 1) * sizeof(LatValue));
    for (size_t i = 0; i < take_n; i++)
        elems[i] = value_deep_clone(&obj->as.array.elems[i]);
    LatValue r = value_array(elems, take_n);
    free(elems);
    return r;
}

/// @method Array.drop(n: Int) -> Array
/// @category Array Methods
/// Return a new array with the first n elements removed.
/// @example [1, 2, 3, 4].drop(2)  // [3, 4]
LatValue builtin_array_drop(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    int64_t n = (args[0].type == VAL_INT) ? args[0].as.int_val : 0;
    if (n < 0) n = 0;
    size_t start = (size_t)n;
    if (start >= obj->as.array.len) return value_array(NULL, 0);
    size_t cnt = obj->as.array.len - start;
    LatValue *elems = malloc(cnt * sizeof(LatValue));
    for (size_t i = 0; i < cnt; i++)
        elems[i] = value_deep_clone(&obj->as.array.elems[start + i]);
    LatValue r = value_array(elems, cnt);
    free(elems);
    return r;
}

/// @method Array.chunk(size: Int) -> Array
/// @category Array Methods
/// Split the array into sub-arrays of the given size.
/// @example [1, 2, 3, 4, 5].chunk(2)  // [[1, 2], [3, 4], [5]]
LatValue builtin_array_chunk(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type != VAL_INT || args[0].as.int_val <= 0)
        return value_array(NULL, 0);
    int64_t cs = args[0].as.int_val;
    size_t n = obj->as.array.len;
    size_t nc = (n > 0) ? (n + (size_t)cs - 1) / (size_t)cs : 0;
    LatValue *chunks = malloc((nc > 0 ? nc : 1) * sizeof(LatValue));
    for (size_t ci = 0; ci < nc; ci++) {
        size_t s = ci * (size_t)cs, e = s + (size_t)cs;
        if (e > n) e = n;
        size_t cl = e - s;
        LatValue *ce = malloc(cl * sizeof(LatValue));
        for (size_t j = 0; j < cl; j++)
            ce[j] = value_deep_clone(&obj->as.array.elems[s + j]);
        chunks[ci] = value_array(ce, cl);
        free(ce);
    }
    LatValue r = value_array(chunks, nc);
    free(chunks);
    return r;
}

/// @method Array.flatten() -> Array
/// @category Array Methods
/// Flatten one level of nested arrays into a single array.
/// @example [[1, 2], [3]].flatten()  // [1, 2, 3]
LatValue builtin_array_flatten(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    size_t n = obj->as.array.len;
    /* First pass: count total elements */
    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        if (obj->as.array.elems[i].type == VAL_ARRAY)
            total += obj->as.array.elems[i].as.array.len;
        else
            total += 1;
    }
    if (total == 0) return value_array(NULL, 0);
    LatValue *buf = malloc(total * sizeof(LatValue));
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        if (obj->as.array.elems[i].type == VAL_ARRAY) {
            LatValue *inner = obj->as.array.elems[i].as.array.elems;
            size_t inner_len = obj->as.array.elems[i].as.array.len;
            for (size_t j = 0; j < inner_len; j++)
                buf[pos++] = value_deep_clone(&inner[j]);
        } else {
            buf[pos++] = value_deep_clone(&obj->as.array.elems[i]);
        }
    }
    LatValue result = value_array(buf, pos);
    free(buf);
    return result;
}

/* ========================================================================
 * Array methods (with closures)
 *
 * These take a BuiltinCallback + opaque closure/ctx so both VMs can share
 * the iteration logic while each providing their own closure invocation.
 * ======================================================================== */

/// @method Array.map(fn: Closure) -> Array
/// @category Array Methods
/// Apply fn to each element and return a new array of results.
/// @example [1, 2, 3].map(|x| x * 2)  // [2, 4, 6]
LatValue builtin_array_map(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error) {
    (void)error;
    size_t len = obj->as.array.len;
    LatValue *elems = malloc((len > 0 ? len : 1) * sizeof(LatValue));
    for (size_t i = 0; i < len; i++) {
        LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
        elems[i] = cb(closure, &arg, 1, ctx);
        value_free(&arg);
    }
    LatValue result = value_array(elems, len);
    free(elems);
    return result;
}

/// @method Array.filter(fn: Closure) -> Array
/// @category Array Methods
/// Return a new array containing only elements for which fn returns true.
/// @example [1, 2, 3, 4].filter(|x| x > 2)  // [3, 4]
LatValue builtin_array_filter(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error) {
    (void)error;
    size_t len = obj->as.array.len;
    size_t cap = len > 0 ? len : 1;
    LatValue *elems = malloc(cap * sizeof(LatValue));
    size_t out_len = 0;
    for (size_t i = 0; i < len; i++) {
        LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
        LatValue pred = cb(closure, &arg, 1, ctx);
        bool keep = (pred.type == VAL_BOOL && pred.as.bool_val);
        value_free(&pred);
        if (keep) {
            elems[out_len++] = arg;
        } else {
            value_free(&arg);
        }
    }
    LatValue result = value_array(elems, out_len);
    free(elems);
    return result;
}

/// @method Array.reduce(fn: Closure, init: Any) -> Any
/// @category Array Methods
/// Reduce the array to a single value by applying fn(accumulator, element) from left to right.
/// @example [1, 2, 3].reduce(|a, b| a + b, 0)  // 6
LatValue builtin_array_reduce(LatValue *obj, LatValue *init, bool has_init,
                              void *closure, BuiltinCallback cb, void *ctx, char **error) {
    (void)error;
    LatValue acc;
    size_t start = 0;
    if (has_init) {
        acc = value_deep_clone(init);
    } else if (obj->as.array.len > 0) {
        acc = value_deep_clone(&obj->as.array.elems[0]);
        start = 1;
    } else {
        return value_nil();
    }
    for (size_t i = start; i < obj->as.array.len; i++) {
        LatValue elem = value_deep_clone(&obj->as.array.elems[i]);
        LatValue args[2] = { acc, elem };
        acc = cb(closure, args, 2, ctx);
        value_free(&args[0]);
        value_free(&args[1]);
    }
    return acc;
}

/// @method Array.each(fn: Closure) -> Unit
/// @category Array Methods
/// Call fn for each element in the array (side-effects only, returns unit).
/// @example [1, 2, 3].each(|x| print(x))
LatValue builtin_array_each(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error) {
    (void)error;
    for (size_t i = 0; i < obj->as.array.len; i++) {
        LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
        LatValue r = cb(closure, &arg, 1, ctx);
        value_free(&arg);
        value_free(&r);
    }
    return value_unit();
}

/// @method Array.find(fn: Closure) -> Any|Unit
/// @category Array Methods
/// Return the first element for which fn returns true, or unit if none match.
/// @example [1, 2, 3].find(|x| x > 1)  // 2
LatValue builtin_array_find(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error) {
    (void)error;
    for (size_t i = 0; i < obj->as.array.len; i++) {
        LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
        LatValue pred = cb(closure, &arg, 1, ctx);
        bool match = (pred.type == VAL_BOOL && pred.as.bool_val);
        value_free(&arg);
        value_free(&pred);
        if (match)
            return value_deep_clone(&obj->as.array.elems[i]);
    }
    return value_unit();
}

/// @method Array.any(fn: Closure) -> Bool
/// @category Array Methods
/// Return true if fn returns true for at least one element.
/// @example [1, 2, 3].any(|x| x > 2)  // true
LatValue builtin_array_any(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error) {
    (void)error;
    for (size_t i = 0; i < obj->as.array.len; i++) {
        LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
        LatValue pred = cb(closure, &arg, 1, ctx);
        bool match = (pred.type == VAL_BOOL && pred.as.bool_val);
        value_free(&arg);
        value_free(&pred);
        if (match) return value_bool(true);
    }
    return value_bool(false);
}

/// @method Array.all(fn: Closure) -> Bool
/// @category Array Methods
/// Return true if fn returns true for every element.
/// @example [2, 4, 6].all(|x| x % 2 == 0)  // true
LatValue builtin_array_all(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error) {
    (void)error;
    for (size_t i = 0; i < obj->as.array.len; i++) {
        LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
        LatValue pred = cb(closure, &arg, 1, ctx);
        bool match = (pred.type == VAL_BOOL && pred.as.bool_val);
        value_free(&arg);
        value_free(&pred);
        if (!match) return value_bool(false);
    }
    return value_bool(true);
}

/// @method Array.flat_map(fn: Closure) -> Array
/// @category Array Methods
/// Map each element with fn, then flatten one level. Equivalent to map then flatten.
/// @example [1, 2].flat_map(|x| [x, x * 10])  // [1, 10, 2, 20]
LatValue builtin_array_flat_map(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error) {
    (void)error;
    size_t n = obj->as.array.len;
    size_t cap = n * 2;
    if (cap == 0) cap = 1;
    LatValue *buf = malloc(cap * sizeof(LatValue));
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
        LatValue mapped = cb(closure, &arg, 1, ctx);
        value_free(&arg);
        if (mapped.type == VAL_ARRAY) {
            for (size_t j = 0; j < mapped.as.array.len; j++) {
                if (out >= cap) { cap *= 2; buf = realloc(buf, cap * sizeof(LatValue)); }
                buf[out++] = value_deep_clone(&mapped.as.array.elems[j]);
            }
            value_free(&mapped);
        } else {
            if (out >= cap) { cap *= 2; buf = realloc(buf, cap * sizeof(LatValue)); }
            buf[out++] = mapped;
        }
    }
    LatValue result = value_array(buf, out);
    free(buf);
    return result;
}

/// @method Array.sort_by(cmp: Closure) -> Array
/// @category Array Methods
/// Return a new array sorted using the comparator closure. cmp(a, b) should return negative if a < b.
/// @example [3, 1, 2].sort_by(|a, b| a - b)  // [1, 2, 3]
LatValue builtin_array_sort_by(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error) {
    (void)error;
    size_t n = obj->as.array.len;
    LatValue *buf = malloc((n > 0 ? n : 1) * sizeof(LatValue));
    for (size_t i = 0; i < n; i++)
        buf[i] = value_deep_clone(&obj->as.array.elems[i]);
    /* Insertion sort using comparator: closure(a, b) < 0 means a < b */
    for (size_t i = 1; i < n; i++) {
        LatValue key = buf[i];
        size_t j = i;
        while (j > 0) {
            LatValue ca[2];
            ca[0] = value_deep_clone(&key);
            ca[1] = value_deep_clone(&buf[j - 1]);
            LatValue cmp = cb(closure, ca, 2, ctx);
            value_free(&ca[0]); value_free(&ca[1]);
            if (cmp.type != VAL_INT || cmp.as.int_val >= 0) { value_free(&cmp); break; }
            value_free(&cmp);
            buf[j] = buf[j - 1]; j--;
        }
        buf[j] = key;
    }
    LatValue result = value_array(buf, n);
    free(buf);
    return result;
}

/// @method Array.group_by(fn: Closure) -> Map
/// @category Array Methods
/// Group elements into a map keyed by the result of fn applied to each element.
/// @example [1, 2, 3, 4].group_by(|x| x % 2)  // {"1": [1, 3], "0": [2, 4]}
LatValue builtin_array_group_by(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error) {
    (void)error;
    LatValue grp = value_map_new();
    for (size_t i = 0; i < obj->as.array.len; i++) {
        LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
        LatValue key_v = cb(closure, &arg, 1, ctx);
        value_free(&arg);
        char *gk = value_display(&key_v);
        value_free(&key_v);
        LatValue *existing = lat_map_get(grp.as.map.map, gk);
        if (existing && existing->type == VAL_ARRAY) {
            if (existing->as.array.len >= existing->as.array.cap) {
                existing->as.array.cap = existing->as.array.cap ? existing->as.array.cap * 2 : 4;
                existing->as.array.elems = realloc(existing->as.array.elems, existing->as.array.cap * sizeof(LatValue));
            }
            existing->as.array.elems[existing->as.array.len++] = value_deep_clone(&obj->as.array.elems[i]);
        } else {
            LatValue cl = value_deep_clone(&obj->as.array.elems[i]);
            LatValue na = value_array(&cl, 1);
            lat_map_set(grp.as.map.map, gk, &na);
        }
        free(gk);
    }
    return grp;
}

/* ========================================================================
 * String methods
 * ======================================================================== */

LatValue builtin_string_split(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type != VAL_STR) return value_array(NULL, 0);
    const char *s = obj->as.str_val;
    const char *sep = args[0].as.str_val;
    size_t sep_len = strlen(sep);
    size_t cap = 8;
    LatValue *parts = malloc(cap * sizeof(LatValue));
    size_t count = 0;
    if (sep_len == 0) {
        /* Split into individual characters */
        for (size_t i = 0; s[i]; i++) {
            if (count >= cap) { cap *= 2; parts = realloc(parts, cap * sizeof(LatValue)); }
            char c[2] = { s[i], '\0' };
            parts[count++] = value_string(c);
        }
    } else {
        const char *p = s;
        while (*p) {
            const char *found = strstr(p, sep);
            if (!found) {
                if (count >= cap) { cap *= 2; parts = realloc(parts, cap * sizeof(LatValue)); }
                parts[count++] = value_string(p);
                break;
            }
            if (count >= cap) { cap *= 2; parts = realloc(parts, cap * sizeof(LatValue)); }
            char *part = strndup(p, (size_t)(found - p));
            parts[count++] = value_string_owned(part);
            p = found + sep_len;
        }
        /* If string ends with separator, add empty trailing element */
        if (sep_len > 0 && strlen(s) >= sep_len) {
            size_t slen = strlen(s);
            if (slen >= sep_len && memcmp(s + slen - sep_len, sep, sep_len) == 0 && count > 0) {
                if (count >= cap) { cap *= 2; parts = realloc(parts, cap * sizeof(LatValue)); }
                parts[count++] = value_string("");
            }
        }
    }
    LatValue result = value_array(parts, count);
    free(parts);
    return result;
}

LatValue builtin_string_trim(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    const char *s = obj->as.str_val;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    const char *e = obj->as.str_val + strlen(obj->as.str_val);
    while (e > s && (*(e-1) == ' ' || *(e-1) == '\t' || *(e-1) == '\n' || *(e-1) == '\r')) e--;
    return value_string_owned(strndup(s, (size_t)(e - s)));
}

LatValue builtin_string_trim_start(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    const char *s = obj->as.str_val;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    return value_string(s);
}

LatValue builtin_string_trim_end(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    size_t len = strlen(obj->as.str_val);
    const char *e = obj->as.str_val + len;
    while (e > obj->as.str_val && (*(e-1) == ' ' || *(e-1) == '\t' || *(e-1) == '\n' || *(e-1) == '\r')) e--;
    return value_string_owned(strndup(obj->as.str_val, (size_t)(e - obj->as.str_val)));
}

LatValue builtin_string_to_upper(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    char *s = strdup(obj->as.str_val);
    for (char *p = s; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    return value_string_owned(s);
}

LatValue builtin_string_to_lower(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    char *s = strdup(obj->as.str_val);
    for (char *p = s; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p += 32;
    return value_string_owned(s);
}

LatValue builtin_string_starts_with(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type == VAL_STR)
        return value_bool(strncmp(obj->as.str_val, args[0].as.str_val, strlen(args[0].as.str_val)) == 0);
    return value_bool(false);
}

LatValue builtin_string_ends_with(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type == VAL_STR) {
        size_t slen = strlen(obj->as.str_val);
        size_t plen = strlen(args[0].as.str_val);
        return value_bool(plen <= slen && strcmp(obj->as.str_val + slen - plen, args[0].as.str_val) == 0);
    }
    return value_bool(false);
}

LatValue builtin_string_replace(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type != VAL_STR || args[1].type != VAL_STR)
        return value_deep_clone(obj);
    const char *s = obj->as.str_val;
    const char *from = args[0].as.str_val;
    const char *to = args[1].as.str_val;
    size_t from_len = strlen(from), to_len = strlen(to);
    if (from_len == 0) return value_deep_clone(obj);
    size_t cap = strlen(s) + 64;
    char *buf = malloc(cap);
    size_t pos = 0;
    while (*s) {
        if (strncmp(s, from, from_len) == 0) {
            while (pos + to_len >= cap) { cap *= 2; buf = realloc(buf, cap); }
            memcpy(buf + pos, to, to_len); pos += to_len; s += from_len;
        } else {
            if (pos + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
            buf[pos++] = *s++;
        }
    }
    buf[pos] = '\0';
    return value_string_owned(buf);
}

LatValue builtin_string_contains(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type == VAL_STR)
        return value_bool(strstr(obj->as.str_val, args[0].as.str_val) != NULL);
    return value_bool(false);
}

LatValue builtin_string_chars(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    size_t len = strlen(obj->as.str_val);
    LatValue *elems = malloc((len > 0 ? len : 1) * sizeof(LatValue));
    for (size_t i = 0; i < len; i++) {
        char c[2] = { obj->as.str_val[i], '\0' };
        elems[i] = value_string(c);
    }
    LatValue r = value_array(elems, len);
    free(elems);
    return r;
}

LatValue builtin_string_bytes(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    size_t len = strlen(obj->as.str_val);
    LatValue *elems = malloc((len > 0 ? len : 1) * sizeof(LatValue));
    for (size_t i = 0; i < len; i++)
        elems[i] = value_int((int64_t)(unsigned char)obj->as.str_val[i]);
    LatValue r = value_array(elems, len);
    free(elems);
    return r;
}

LatValue builtin_string_reverse(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    size_t len = strlen(obj->as.str_val);
    char *buf = malloc(len + 1);
    for (size_t i = 0; i < len; i++) buf[i] = obj->as.str_val[len - 1 - i];
    buf[len] = '\0';
    return value_string_owned(buf);
}

LatValue builtin_string_repeat(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type != VAL_INT || args[0].as.int_val < 0)
        return value_string("");
    int64_t n = args[0].as.int_val;
    size_t slen = strlen(obj->as.str_val);
    char *buf = malloc(slen * (size_t)n + 1);
    for (int64_t i = 0; i < n; i++)
        memcpy(buf + i * (int64_t)slen, obj->as.str_val, slen);
    buf[slen * (size_t)n] = '\0';
    return value_string_owned(buf);
}

LatValue builtin_string_pad_left(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)error;
    int64_t n = (args[0].type == VAL_INT) ? args[0].as.int_val : 0;
    char pad = (arg_count >= 2 && args[1].type == VAL_STR && args[1].as.str_val[0])
             ? args[1].as.str_val[0] : ' ';
    size_t slen = strlen(obj->as.str_val);
    if ((int64_t)slen >= n) return value_deep_clone(obj);
    size_t plen = (size_t)n - slen;
    char *buf = malloc((size_t)n + 1);
    memset(buf, pad, plen);
    memcpy(buf + plen, obj->as.str_val, slen);
    buf[(size_t)n] = '\0';
    return value_string_owned(buf);
}

LatValue builtin_string_pad_right(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)error;
    int64_t n = (args[0].type == VAL_INT) ? args[0].as.int_val : 0;
    char pad = (arg_count >= 2 && args[1].type == VAL_STR && args[1].as.str_val[0])
             ? args[1].as.str_val[0] : ' ';
    size_t slen = strlen(obj->as.str_val);
    if ((int64_t)slen >= n) return value_deep_clone(obj);
    char *buf = malloc((size_t)n + 1);
    memcpy(buf, obj->as.str_val, slen);
    memset(buf + slen, pad, (size_t)n - slen);
    buf[(size_t)n] = '\0';
    return value_string_owned(buf);
}

LatValue builtin_string_count(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    int64_t cnt = 0;
    if (args[0].type == VAL_STR && args[0].as.str_val[0]) {
        const char *p = obj->as.str_val;
        size_t nlen = strlen(args[0].as.str_val);
        while ((p = strstr(p, args[0].as.str_val)) != NULL) { cnt++; p += nlen; }
    }
    return value_int(cnt);
}

LatValue builtin_string_is_empty(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    return value_bool(obj->as.str_val[0] == '\0');
}

LatValue builtin_string_index_of(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type == VAL_STR) {
        const char *found = strstr(obj->as.str_val, args[0].as.str_val);
        return found ? value_int((int64_t)(found - obj->as.str_val)) : value_int(-1);
    }
    return value_int(-1);
}

LatValue builtin_string_substring(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)error;
    size_t slen = strlen(obj->as.str_val);
    int64_t start = (args[0].type == VAL_INT) ? args[0].as.int_val : 0;
    int64_t end = (arg_count >= 2 && args[1].type == VAL_INT) ? args[1].as.int_val : (int64_t)slen;
    if (start < 0) start += (int64_t)slen;
    if (end < 0) end += (int64_t)slen;
    if (start < 0) start = 0;
    if (end > (int64_t)slen) end = (int64_t)slen;
    if (start >= end) return value_string("");
    return value_string_owned(strndup(obj->as.str_val + start, (size_t)(end - start)));
}

/* ========================================================================
 * Map methods (no closures)
 * ======================================================================== */

LatValue builtin_map_keys(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    size_t cap = obj->as.map.map->cap;
    LatValue *keys = malloc((cap > 0 ? cap : 1) * sizeof(LatValue));
    size_t count = 0;
    for (size_t i = 0; i < cap; i++) {
        if (obj->as.map.map->entries[i].state == MAP_OCCUPIED)
            keys[count++] = value_string(obj->as.map.map->entries[i].key);
    }
    LatValue result = value_array(keys, count);
    free(keys);
    return result;
}

LatValue builtin_map_values(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    size_t cap = obj->as.map.map->cap;
    LatValue *vals = malloc((cap > 0 ? cap : 1) * sizeof(LatValue));
    size_t count = 0;
    for (size_t i = 0; i < cap; i++) {
        if (obj->as.map.map->entries[i].state == MAP_OCCUPIED)
            vals[count++] = value_deep_clone((LatValue *)obj->as.map.map->entries[i].value);
    }
    LatValue result = value_array(vals, count);
    free(vals);
    return result;
}

LatValue builtin_map_entries(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    size_t cap = obj->as.map.map->cap;
    LatValue *entries = malloc((cap > 0 ? cap : 1) * sizeof(LatValue));
    size_t count = 0;
    for (size_t i = 0; i < cap; i++) {
        if (obj->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
        LatValue pair[2];
        pair[0] = value_string(obj->as.map.map->entries[i].key);
        pair[1] = value_deep_clone((LatValue *)obj->as.map.map->entries[i].value);
        entries[count++] = value_array(pair, 2);
    }
    LatValue result = value_array(entries, count);
    free(entries);
    return result;
}

LatValue builtin_map_get(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type == VAL_STR) {
        LatValue *val = lat_map_get(obj->as.map.map, args[0].as.str_val);
        return val ? value_deep_clone(val) : value_nil();
    }
    return value_nil();
}

LatValue builtin_map_has(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type == VAL_STR)
        return value_bool(lat_map_get(obj->as.map.map, args[0].as.str_val) != NULL);
    return value_bool(false);
}

LatValue builtin_map_remove(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type == VAL_STR)
        lat_map_remove(obj->as.map.map, args[0].as.str_val);
    return value_unit();
}

LatValue builtin_map_merge(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type == VAL_MAP) {
        for (size_t i = 0; i < args[0].as.map.map->cap; i++) {
            if (args[0].as.map.map->entries[i].state != MAP_OCCUPIED) continue;
            LatValue cloned = value_deep_clone((LatValue *)args[0].as.map.map->entries[i].value);
            lat_map_set(obj->as.map.map, args[0].as.map.map->entries[i].key, &cloned);
        }
    }
    return value_unit();
}

/* ========================================================================
 * Buffer methods
 * ======================================================================== */

LatValue builtin_buffer_push(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type == VAL_INT) {
        if (obj->as.buffer.len >= obj->as.buffer.cap) {
            obj->as.buffer.cap = obj->as.buffer.cap ? obj->as.buffer.cap * 2 : 8;
            obj->as.buffer.data = realloc(obj->as.buffer.data, obj->as.buffer.cap);
        }
        obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)(args[0].as.int_val & 0xFF);
    }
    return value_unit();
}

LatValue builtin_buffer_push_u16(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type == VAL_INT) {
        uint16_t v = (uint16_t)(args[0].as.int_val & 0xFFFF);
        while (obj->as.buffer.len + 2 > obj->as.buffer.cap) {
            obj->as.buffer.cap = obj->as.buffer.cap ? obj->as.buffer.cap * 2 : 8;
            obj->as.buffer.data = realloc(obj->as.buffer.data, obj->as.buffer.cap);
        }
        obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)(v & 0xFF);
        obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)((v >> 8) & 0xFF);
    }
    return value_unit();
}

LatValue builtin_buffer_push_u32(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type == VAL_INT) {
        uint32_t v = (uint32_t)(args[0].as.int_val & 0xFFFFFFFF);
        while (obj->as.buffer.len + 4 > obj->as.buffer.cap) {
            obj->as.buffer.cap = obj->as.buffer.cap ? obj->as.buffer.cap * 2 : 8;
            obj->as.buffer.data = realloc(obj->as.buffer.data, obj->as.buffer.cap);
        }
        obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)(v & 0xFF);
        obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)((v >> 8) & 0xFF);
        obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)((v >> 16) & 0xFF);
        obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)((v >> 24) & 0xFF);
    }
    return value_unit();
}

LatValue builtin_buffer_read_u8(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count;
    if (args[0].type != VAL_INT || args[0].as.int_val < 0 || (size_t)args[0].as.int_val >= obj->as.buffer.len) {
        *error = strdup("Buffer.read_u8: index out of bounds");
        return value_int(0);
    }
    return value_int((int64_t)obj->as.buffer.data[args[0].as.int_val]);
}

LatValue builtin_buffer_write_u8(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count;
    if (args[0].type != VAL_INT || args[0].as.int_val < 0 || (size_t)args[0].as.int_val >= obj->as.buffer.len) {
        *error = strdup("Buffer.write_u8: index out of bounds");
        return value_unit();
    }
    obj->as.buffer.data[args[0].as.int_val] = (uint8_t)(args[1].as.int_val & 0xFF);
    return value_unit();
}

LatValue builtin_buffer_read_u16(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count;
    if (args[0].type != VAL_INT || args[0].as.int_val < 0 || (size_t)args[0].as.int_val + 2 > obj->as.buffer.len) {
        *error = strdup("Buffer.read_u16: index out of bounds");
        return value_int(0);
    }
    size_t i = (size_t)args[0].as.int_val;
    uint16_t v = (uint16_t)(obj->as.buffer.data[i] | (obj->as.buffer.data[i+1] << 8));
    return value_int((int64_t)v);
}

LatValue builtin_buffer_write_u16(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count;
    if (args[0].type != VAL_INT || args[0].as.int_val < 0 || (size_t)args[0].as.int_val + 2 > obj->as.buffer.len) {
        *error = strdup("Buffer.write_u16: index out of bounds");
        return value_unit();
    }
    size_t i = (size_t)args[0].as.int_val;
    uint16_t v = (uint16_t)(args[1].as.int_val & 0xFFFF);
    obj->as.buffer.data[i] = (uint8_t)(v & 0xFF);
    obj->as.buffer.data[i+1] = (uint8_t)((v >> 8) & 0xFF);
    return value_unit();
}

LatValue builtin_buffer_read_u32(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count;
    if (args[0].type != VAL_INT || args[0].as.int_val < 0 || (size_t)args[0].as.int_val + 4 > obj->as.buffer.len) {
        *error = strdup("Buffer.read_u32: index out of bounds");
        return value_int(0);
    }
    size_t i = (size_t)args[0].as.int_val;
    uint32_t v = (uint32_t)obj->as.buffer.data[i]
               | ((uint32_t)obj->as.buffer.data[i+1] << 8)
               | ((uint32_t)obj->as.buffer.data[i+2] << 16)
               | ((uint32_t)obj->as.buffer.data[i+3] << 24);
    return value_int((int64_t)v);
}

LatValue builtin_buffer_write_u32(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count;
    if (args[0].type != VAL_INT || args[0].as.int_val < 0 || (size_t)args[0].as.int_val + 4 > obj->as.buffer.len) {
        *error = strdup("Buffer.write_u32: index out of bounds");
        return value_unit();
    }
    size_t i = (size_t)args[0].as.int_val;
    uint32_t v = (uint32_t)(args[1].as.int_val & 0xFFFFFFFF);
    obj->as.buffer.data[i]   = (uint8_t)(v & 0xFF);
    obj->as.buffer.data[i+1] = (uint8_t)((v >> 8) & 0xFF);
    obj->as.buffer.data[i+2] = (uint8_t)((v >> 16) & 0xFF);
    obj->as.buffer.data[i+3] = (uint8_t)((v >> 24) & 0xFF);
    return value_unit();
}

LatValue builtin_buffer_read_i8(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count;
    if (args[0].type != VAL_INT || args[0].as.int_val < 0 || (size_t)args[0].as.int_val >= obj->as.buffer.len) {
        *error = strdup("Buffer.read_i8: index out of bounds");
        return value_int(0);
    }
    return value_int((int8_t)obj->as.buffer.data[args[0].as.int_val]);
}

LatValue builtin_buffer_read_i16(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count;
    if (args[0].type != VAL_INT || args[0].as.int_val < 0 || (size_t)args[0].as.int_val + 2 > obj->as.buffer.len) {
        *error = strdup("Buffer.read_i16: index out of bounds");
        return value_int(0);
    }
    size_t i = (size_t)args[0].as.int_val;
    int16_t v;
    memcpy(&v, obj->as.buffer.data + i, 2);
    return value_int(v);
}

LatValue builtin_buffer_read_i32(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count;
    if (args[0].type != VAL_INT || args[0].as.int_val < 0 || (size_t)args[0].as.int_val + 4 > obj->as.buffer.len) {
        *error = strdup("Buffer.read_i32: index out of bounds");
        return value_int(0);
    }
    size_t i = (size_t)args[0].as.int_val;
    int32_t v;
    memcpy(&v, obj->as.buffer.data + i, 4);
    return value_int(v);
}

LatValue builtin_buffer_read_f32(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count;
    if (args[0].type != VAL_INT || args[0].as.int_val < 0 || (size_t)args[0].as.int_val + 4 > obj->as.buffer.len) {
        *error = strdup("Buffer.read_f32: index out of bounds");
        return value_float(0.0);
    }
    size_t i = (size_t)args[0].as.int_val;
    float v;
    memcpy(&v, obj->as.buffer.data + i, 4);
    return value_float((double)v);
}

LatValue builtin_buffer_read_f64(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count;
    if (args[0].type != VAL_INT || args[0].as.int_val < 0 || (size_t)args[0].as.int_val + 8 > obj->as.buffer.len) {
        *error = strdup("Buffer.read_f64: index out of bounds");
        return value_float(0.0);
    }
    size_t i = (size_t)args[0].as.int_val;
    double v;
    memcpy(&v, obj->as.buffer.data + i, 8);
    return value_float(v);
}

LatValue builtin_buffer_slice(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)error;
    int64_t start = (args[0].type == VAL_INT) ? args[0].as.int_val : 0;
    int64_t end = (arg_count >= 2 && args[1].type == VAL_INT) ? args[1].as.int_val : (int64_t)obj->as.buffer.len;
    if (start < 0) start = 0;
    if (end > (int64_t)obj->as.buffer.len) end = (int64_t)obj->as.buffer.len;
    if (start >= end) return value_buffer(NULL, 0);
    return value_buffer(obj->as.buffer.data + start, (size_t)(end - start));
}

LatValue builtin_buffer_clear(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    obj->as.buffer.len = 0;
    return value_unit();
}

LatValue builtin_buffer_fill(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    uint8_t byte = (args[0].type == VAL_INT) ? (uint8_t)(args[0].as.int_val & 0xFF) : 0;
    memset(obj->as.buffer.data, byte, obj->as.buffer.len);
    return value_unit();
}

LatValue builtin_buffer_resize(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type != VAL_INT || args[0].as.int_val < 0) return value_unit();
    size_t new_len = (size_t)args[0].as.int_val;
    if (new_len > obj->as.buffer.cap) {
        obj->as.buffer.cap = new_len;
        obj->as.buffer.data = realloc(obj->as.buffer.data, obj->as.buffer.cap);
    }
    if (new_len > obj->as.buffer.len)
        memset(obj->as.buffer.data + obj->as.buffer.len, 0, new_len - obj->as.buffer.len);
    obj->as.buffer.len = new_len;
    return value_unit();
}

LatValue builtin_buffer_to_string(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    char *s = malloc(obj->as.buffer.len + 1);
    memcpy(s, obj->as.buffer.data, obj->as.buffer.len);
    s[obj->as.buffer.len] = '\0';
    return value_string_owned(s);
}

LatValue builtin_buffer_to_array(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    size_t len = obj->as.buffer.len;
    LatValue *elems = malloc((len > 0 ? len : 1) * sizeof(LatValue));
    for (size_t i = 0; i < len; i++)
        elems[i] = value_int((int64_t)obj->as.buffer.data[i]);
    LatValue arr = value_array(elems, len);
    free(elems);
    return arr;
}

LatValue builtin_buffer_to_hex(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    size_t len = obj->as.buffer.len;
    char *hex = malloc(len * 2 + 1);
    for (size_t i = 0; i < len; i++)
        snprintf(hex + i * 2, 3, "%02x", obj->as.buffer.data[i]);
    hex[len * 2] = '\0';
    return value_string_owned(hex);
}

/* ========================================================================
 * Set methods
 * ======================================================================== */

LatValue builtin_set_has(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    char *key = value_display(&args[0]);
    bool found = lat_map_contains(obj->as.set.map, key);
    free(key);
    return value_bool(found);
}

LatValue builtin_set_add(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    char *key = value_display(&args[0]);
    LatValue clone = value_deep_clone(&args[0]);
    lat_map_set(obj->as.set.map, key, &clone);
    free(key);
    return value_unit();
}

LatValue builtin_set_remove(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    char *key = value_display(&args[0]);
    lat_map_remove(obj->as.set.map, key);
    free(key);
    return value_unit();
}

LatValue builtin_set_to_array(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    size_t len = lat_map_len(obj->as.set.map);
    LatValue *elems = malloc((len > 0 ? len : 1) * sizeof(LatValue));
    size_t idx = 0;
    for (size_t i = 0; i < obj->as.set.map->cap; i++) {
        if (obj->as.set.map->entries[i].state == MAP_OCCUPIED) {
            LatValue *v = (LatValue *)obj->as.set.map->entries[i].value;
            elems[idx++] = value_deep_clone(v);
        }
    }
    LatValue arr = value_array(elems, len);
    free(elems);
    return arr;
}

LatValue builtin_set_union(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    LatValue result = value_set_new();
    for (size_t i = 0; i < obj->as.set.map->cap; i++) {
        if (obj->as.set.map->entries[i].state == MAP_OCCUPIED) {
            LatValue *v = (LatValue *)obj->as.set.map->entries[i].value;
            LatValue c = value_deep_clone(v);
            lat_map_set(result.as.set.map, obj->as.set.map->entries[i].key, &c);
        }
    }
    if (args[0].type == VAL_SET) {
        for (size_t i = 0; i < args[0].as.set.map->cap; i++) {
            if (args[0].as.set.map->entries[i].state == MAP_OCCUPIED) {
                LatValue *v = (LatValue *)args[0].as.set.map->entries[i].value;
                LatValue c = value_deep_clone(v);
                lat_map_set(result.as.set.map, args[0].as.set.map->entries[i].key, &c);
            }
        }
    }
    return result;
}

LatValue builtin_set_intersection(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    LatValue result = value_set_new();
    if (args[0].type == VAL_SET) {
        for (size_t i = 0; i < obj->as.set.map->cap; i++) {
            if (obj->as.set.map->entries[i].state == MAP_OCCUPIED &&
                lat_map_contains(args[0].as.set.map, obj->as.set.map->entries[i].key)) {
                LatValue *v = (LatValue *)obj->as.set.map->entries[i].value;
                LatValue c = value_deep_clone(v);
                lat_map_set(result.as.set.map, obj->as.set.map->entries[i].key, &c);
            }
        }
    }
    return result;
}

LatValue builtin_set_difference(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    LatValue result = value_set_new();
    if (args[0].type == VAL_SET) {
        for (size_t i = 0; i < obj->as.set.map->cap; i++) {
            if (obj->as.set.map->entries[i].state == MAP_OCCUPIED &&
                !lat_map_contains(args[0].as.set.map, obj->as.set.map->entries[i].key)) {
                LatValue *v = (LatValue *)obj->as.set.map->entries[i].value;
                LatValue c = value_deep_clone(v);
                lat_map_set(result.as.set.map, obj->as.set.map->entries[i].key, &c);
            }
        }
    }
    return result;
}

LatValue builtin_set_is_subset(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type != VAL_SET) return value_bool(false);
    for (size_t i = 0; i < obj->as.set.map->cap; i++) {
        if (obj->as.set.map->entries[i].state == MAP_OCCUPIED &&
            !lat_map_contains(args[0].as.set.map, obj->as.set.map->entries[i].key))
            return value_bool(false);
    }
    return value_bool(true);
}

LatValue builtin_set_is_superset(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type != VAL_SET) return value_bool(false);
    for (size_t i = 0; i < args[0].as.set.map->cap; i++) {
        if (args[0].as.set.map->entries[i].state == MAP_OCCUPIED &&
            !lat_map_contains(obj->as.set.map, args[0].as.set.map->entries[i].key))
            return value_bool(false);
    }
    return value_bool(true);
}

/* ========================================================================
 * Enum methods
 * ======================================================================== */

LatValue builtin_enum_tag(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    return value_string(obj->as.enm.variant_name);
}

LatValue builtin_enum_enum_name(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    return value_string(obj->as.enm.enum_name);
}

LatValue builtin_enum_payload(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)args; (void)arg_count; (void)error;
    if (obj->as.enm.payload_count > 0) {
        LatValue *elems = malloc(obj->as.enm.payload_count * sizeof(LatValue));
        for (size_t pi = 0; pi < obj->as.enm.payload_count; pi++)
            elems[pi] = value_deep_clone(&obj->as.enm.payload[pi]);
        LatValue r = value_array(elems, obj->as.enm.payload_count);
        free(elems);
        return r;
    }
    return value_array(NULL, 0);
}

LatValue builtin_enum_is_variant(LatValue *obj, LatValue *args, int arg_count, char **error) {
    (void)arg_count; (void)error;
    if (args[0].type == VAL_STR)
        return value_bool(strcmp(obj->as.enm.variant_name, args[0].as.str_val) == 0);
    return value_bool(false);
}
