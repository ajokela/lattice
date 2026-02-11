#include "array_ops.h"
#include <stdlib.h>
#include <string.h>

/* ── Sort ── */

/* Static error flag for qsort comparator (qsort can't propagate errors) */
static int sort_error = 0;
static ValueType sort_type = VAL_UNIT;

static int sort_comparator(const void *a, const void *b) {
    const LatValue *va = (const LatValue *)a;
    const LatValue *vb = (const LatValue *)b;

    if (va->type != vb->type) {
        sort_error = 1;
        return 0;
    }

    switch (va->type) {
        case VAL_INT: {
            int64_t ai = va->as.int_val, bi = vb->as.int_val;
            return (ai > bi) - (ai < bi);
        }
        case VAL_FLOAT: {
            double af = va->as.float_val, bf = vb->as.float_val;
            return (af > bf) - (af < bf);
        }
        case VAL_STR:
            return strcmp(va->as.str_val, vb->as.str_val);
        default:
            sort_error = 1;
            return 0;
    }
}

LatValue array_sort(const LatValue *arr, char **err) {
    size_t n = arr->as.array.len;
    if (n == 0) {
        *err = NULL;
        LatValue empty;
        return value_array(&empty, 0);
    }

    /* Validate element types - must be homogeneous and comparable */
    sort_type = arr->as.array.elems[0].type;
    if (sort_type != VAL_INT && sort_type != VAL_FLOAT && sort_type != VAL_STR) {
        *err = strdup(".sort() only supports Int, Float, or String arrays");
        return value_unit();
    }
    for (size_t i = 1; i < n; i++) {
        if (arr->as.array.elems[i].type != sort_type) {
            *err = strdup(".sort() requires all elements to be the same type");
            return value_unit();
        }
    }

    /* Deep-clone elements into a working buffer */
    LatValue *buf = malloc(n * sizeof(LatValue));
    for (size_t i = 0; i < n; i++) {
        buf[i] = value_deep_clone(&arr->as.array.elems[i]);
    }

    sort_error = 0;
    qsort(buf, n, sizeof(LatValue), sort_comparator);

    if (sort_error) {
        for (size_t i = 0; i < n; i++) value_free(&buf[i]);
        free(buf);
        *err = strdup(".sort() encountered incomparable types");
        return value_unit();
    }

    *err = NULL;
    LatValue result = value_array(buf, n);
    /* value_array does a shallow memcpy, so it now owns the element data.
     * Only free the container array, not the individual elements. */
    free(buf);
    return result;
}

/* ── Flat ── */

LatValue array_flat(const LatValue *arr) {
    size_t n = arr->as.array.len;

    /* First pass: count total elements */
    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        if (arr->as.array.elems[i].type == VAL_ARRAY) {
            total += arr->as.array.elems[i].as.array.len;
        } else {
            total += 1;
        }
    }

    if (total == 0) {
        LatValue empty;
        return value_array(&empty, 0);
    }

    LatValue *buf = malloc(total * sizeof(LatValue));
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        if (arr->as.array.elems[i].type == VAL_ARRAY) {
            LatValue *inner = arr->as.array.elems[i].as.array.elems;
            size_t inner_len = arr->as.array.elems[i].as.array.len;
            for (size_t j = 0; j < inner_len; j++) {
                buf[pos++] = value_deep_clone(&inner[j]);
            }
        } else {
            buf[pos++] = value_deep_clone(&arr->as.array.elems[i]);
        }
    }

    LatValue result = value_array(buf, pos);
    free(buf);
    return result;
}

/* ── Slice ── */

LatValue array_slice(const LatValue *arr, int64_t start, int64_t end, char **err) {
    int64_t len = (int64_t)arr->as.array.len;

    /* Clamp to [0, len] */
    if (start < 0) start = 0;
    if (start > len) start = len;
    if (end < 0) end = 0;
    if (end > len) end = len;
    if (end < start) end = start;

    size_t count = (size_t)(end - start);
    if (count == 0) {
        *err = NULL;
        LatValue empty;
        return value_array(&empty, 0);
    }

    LatValue *buf = malloc(count * sizeof(LatValue));
    for (size_t i = 0; i < count; i++) {
        buf[i] = value_deep_clone(&arr->as.array.elems[start + (int64_t)i]);
    }

    *err = NULL;
    LatValue result = value_array(buf, count);
    free(buf);
    return result;
}
