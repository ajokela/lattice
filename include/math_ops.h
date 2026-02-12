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
LatValue math_log(const LatValue *v, char **err);
LatValue math_log2(const LatValue *v, char **err);
LatValue math_log10(const LatValue *v, char **err);
LatValue math_sin(const LatValue *v, char **err);
LatValue math_cos(const LatValue *v, char **err);
LatValue math_tan(const LatValue *v, char **err);
LatValue math_atan2(const LatValue *y, const LatValue *x, char **err);
LatValue math_clamp(const LatValue *val, const LatValue *lo, const LatValue *hi, char **err);
LatValue math_pi(void);
LatValue math_e(void);
LatValue math_asin(const LatValue *v, char **err);
LatValue math_acos(const LatValue *v, char **err);
LatValue math_atan(const LatValue *v, char **err);
LatValue math_exp(const LatValue *v, char **err);
LatValue math_sign(const LatValue *v, char **err);
LatValue math_gcd(const LatValue *a, const LatValue *b, char **err);
LatValue math_lcm(const LatValue *a, const LatValue *b, char **err);
LatValue math_is_nan(const LatValue *v, char **err);
LatValue math_is_inf(const LatValue *v, char **err);

#endif
