#ifndef MATH_OPS_H
#define MATH_OPS_H

#include "value.h"

/* All math functions take LatValue args and return LatValue.
 * They set *err on bad input. */

LatValue math_abs(const LatValue *v, char **err);
LatValue math_floor(const LatValue *v, char **err);
LatValue math_ceil(const LatValue *v, char **err);
LatValue math_round(const LatValue *v, char **err);
LatValue math_sqrt(const LatValue *v, char **err);
LatValue math_pow(const LatValue *base, const LatValue *exp, char **err);
LatValue math_min(const LatValue *a, const LatValue *b, char **err);
LatValue math_max(const LatValue *a, const LatValue *b, char **err);
LatValue math_random(void);      /* returns Float in [0, 1) */
LatValue math_random_int(const LatValue *low, const LatValue *high, char **err); /* returns Int in [low, high] */

#endif
