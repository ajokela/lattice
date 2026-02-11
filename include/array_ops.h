#ifndef ARRAY_OPS_H
#define ARRAY_OPS_H
#include "value.h"

/* Sort array elements. Returns a NEW sorted array (does not mutate).
 * Supports Int, Float, String. Mixed types error. */
LatValue array_sort(const LatValue *arr, char **err);

/* Flatten one level of nesting. [1, [2, 3], [4]] -> [1, 2, 3, 4] */
LatValue array_flat(const LatValue *arr);

/* Slice: returns new array from arr[start..end) */
LatValue array_slice(const LatValue *arr, int64_t start, int64_t end, char **err);

#endif
