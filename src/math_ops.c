#include "math_ops.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

/* Helper: extract a double from an Int or Float value */
static double to_double(const LatValue *v) {
    if (v->type == VAL_INT) return (double)v->as.int_val;
    return v->as.float_val;
}

/* Helper: NaN/inf/range-checked conversion of a double to int64_t. Returns
 * false (and sets *err to msg) when d is NaN/inf or outside the representable
 * int64 range, so callers can raise a clean runtime error instead of relying on
 * the undefined behaviour of an out-of-range float->int cast. The bounds use
 * 2^63 (exactly representable as a double): any d with -2^63 <= d < 2^63
 * truncates to a value that fits in int64_t. */
static bool checked_double_to_int(double d, int64_t *out, char **err, const char *msg) {
    if (isnan(d) || isinf(d) || d < -9223372036854775808.0 || d >= 9223372036854775808.0) {
        *err = strdup(msg);
        return false;
    }
    *out = (int64_t)d;
    return true;
}

/* Helper: build a full-width (64-bit) random value from rand(), which only
 * yields RAND_MAX bits per call. Used for the full-width random_int() range. */
static uint64_t math_rand_u64(void) {
    uint64_t r = 0;
    for (int i = 0; i < 64; i += 15) r = (r << 15) | ((uint64_t)rand() & 0x7FFFu);
    return r;
}

/* ── abs ── */

LatValue math_abs(const LatValue *v, char **err) {
    if (v->type == VAL_INT) {
        int64_t x = v->as.int_val;
        if (x == INT64_MIN) {
            *err = strdup("abs(): integer overflow (INT64_MIN)");
            return value_unit();
        }
        return value_int(x < 0 ? -x : x);
    }
    if (v->type == VAL_FLOAT) { return value_float(fabs(v->as.float_val)); }
    *err = strdup("abs() expects Int or Float");
    return value_unit();
}

/* ── floor ── */

LatValue math_floor(const LatValue *v, char **err) {
    if (v->type == VAL_INT) { return value_int(v->as.int_val); }
    if (v->type == VAL_FLOAT) {
        int64_t r;
        if (!checked_double_to_int(floor(v->as.float_val), &r, err, "floor(): value out of Int range"))
            return value_unit();
        return value_int(r);
    }
    *err = strdup("floor() expects Int or Float");
    return value_unit();
}

/* ── ceil ── */

LatValue math_ceil(const LatValue *v, char **err) {
    if (v->type == VAL_INT) { return value_int(v->as.int_val); }
    if (v->type == VAL_FLOAT) {
        int64_t r;
        if (!checked_double_to_int(ceil(v->as.float_val), &r, err, "ceil(): value out of Int range"))
            return value_unit();
        return value_int(r);
    }
    *err = strdup("ceil() expects Int or Float");
    return value_unit();
}

/* ── round ── */

LatValue math_round(const LatValue *v, char **err) {
    if (v->type == VAL_INT) { return value_int(v->as.int_val); }
    if (v->type == VAL_FLOAT) {
        int64_t r;
        if (!checked_double_to_int(round(v->as.float_val), &r, err, "round(): value out of Int range"))
            return value_unit();
        return value_int(r);
    }
    *err = strdup("round() expects Int or Float");
    return value_unit();
}

/* ── sqrt ── */

LatValue math_sqrt(const LatValue *v, char **err) {
    double x = 0.0;
    if (v->type == VAL_INT) {
        x = (double)v->as.int_val;
    } else if (v->type == VAL_FLOAT) {
        x = v->as.float_val;
    } else {
        *err = strdup("sqrt() expects Int or Float");
        return value_unit();
    }
    if (x < 0.0) {
        *err = strdup("sqrt() domain error: negative input");
        return value_unit();
    }
    return value_float(sqrt(x));
}

/* ── pow ── */

LatValue math_pow(const LatValue *base, const LatValue *exp, char **err) {
    if ((base->type != VAL_INT && base->type != VAL_FLOAT) || (exp->type != VAL_INT && exp->type != VAL_FLOAT)) {
        *err = strdup("pow() expects (Int|Float, Int|Float)");
        return value_unit();
    }

    /* Both Int: try integer exponentiation */
    if (base->type == VAL_INT && exp->type == VAL_INT) {
        int64_t b = base->as.int_val;
        int64_t e = exp->as.int_val;

        /* Negative exponents produce fractional results -> use float */
        if (e < 0) { return value_float(pow((double)b, (double)e)); }

        /* Integer power with overflow detection. Track the magnitude and sign
         * separately in unsigned arithmetic so the checks stay well-defined even
         * for base == INT64_MIN (where llabs(INT64_MIN) would be UB). */
        uint64_t result_mag = 1;
        bool result_neg = false;
        uint64_t base_mag = (b < 0) ? -(uint64_t)b : (uint64_t)b; /* |b|, exact for INT64_MIN */
        bool base_neg = (b < 0);
        int64_t exp_val = e;
        while (exp_val > 0) {
            if (exp_val & 1) {
                /* result *= base_val, in magnitude/sign form */
                if (base_mag != 0 && result_mag > UINT64_MAX / base_mag) {
                    /* Magnitude overflow: fall through to float */
                    return value_float(pow((double)b, (double)e));
                }
                result_mag *= base_mag;
                result_neg ^= base_neg;
            }
            exp_val >>= 1;
            if (exp_val > 0) {
                if (base_mag != 0 && base_mag > UINT64_MAX / base_mag) {
                    return value_float(pow((double)b, (double)e));
                }
                base_mag *= base_mag;
                base_neg = false; /* a square is always non-negative */
            }
        }
        /* Convert magnitude+sign back to int64_t, checking it fits the range. */
        if (result_neg) {
            if (result_mag > (uint64_t)INT64_MAX + 1u) return value_float(pow((double)b, (double)e));
            if (result_mag == (uint64_t)INT64_MAX + 1u) return value_int(INT64_MIN);
            return value_int(-(int64_t)result_mag);
        }
        if (result_mag > (uint64_t)INT64_MAX) return value_float(pow((double)b, (double)e));
        return value_int((int64_t)result_mag);
    }

    /* At least one Float: use floating-point pow */
    double b = to_double(base);
    double e = to_double(exp);
    return value_float(pow(b, e));
}

/* ── min ── */

LatValue math_min(const LatValue *a, const LatValue *b, char **err) {
    if ((a->type != VAL_INT && a->type != VAL_FLOAT) || (b->type != VAL_INT && b->type != VAL_FLOAT)) {
        *err = strdup("min() expects (Int|Float, Int|Float)");
        return value_unit();
    }

    ValueNumericCmp cmp = value_numeric_compare(a, b);
    if (cmp == VALUE_CMP_UNORDERED) {
        *err = strdup("min() cannot compare NaN");
        return value_unit();
    }
    return value_deep_clone(cmp == VALUE_CMP_GREATER ? b : a);
}

/* ── max ── */

LatValue math_max(const LatValue *a, const LatValue *b, char **err) {
    if ((a->type != VAL_INT && a->type != VAL_FLOAT) || (b->type != VAL_INT && b->type != VAL_FLOAT)) {
        *err = strdup("max() expects (Int|Float, Int|Float)");
        return value_unit();
    }

    ValueNumericCmp cmp = value_numeric_compare(a, b);
    if (cmp == VALUE_CMP_UNORDERED) {
        *err = strdup("max() cannot compare NaN");
        return value_unit();
    }
    return value_deep_clone(cmp == VALUE_CMP_LESS ? b : a);
}

/* ── random ── */

LatValue math_random(void) {
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = true;
    }
    return value_float((double)rand() / ((double)RAND_MAX + 1.0));
}

/* ── random_int ── */

LatValue math_random_int(const LatValue *low, const LatValue *high, char **err) {
    if (low->type != VAL_INT || high->type != VAL_INT) {
        *err = strdup("random_int() expects (Int, Int)");
        return value_unit();
    }
    int64_t lo = low->as.int_val;
    int64_t hi = high->as.int_val;
    if (lo > hi) {
        *err = strdup("random_int(): low must be <= high");
        return value_unit();
    }

    static bool seeded = false;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = true;
    }

    /* Compute the span in unsigned arithmetic to avoid the signed overflow of
     * hi - lo (e.g. lo == INT64_MIN, hi == INT64_MAX). span is the count of
     * representable values minus one. */
    uint64_t span = (uint64_t)hi - (uint64_t)lo;
    if (span == UINT64_MAX) {
        /* Full-width range [INT64_MIN, INT64_MAX]: every 64-bit value is valid.
         * (Adding 1 to span here would wrap to 0 and make rand() % 0 a SIGFPE.) */
        return value_int((int64_t)math_rand_u64());
    }
    uint64_t range = span + 1;
    uint64_t sample;
    uint64_t limit = UINT64_MAX - (UINT64_MAX % range);
    do { sample = math_rand_u64(); } while (sample >= limit);
    int64_t result = (int64_t)((uint64_t)lo + (sample % range));
    return value_int(result);
}

/* ── log (natural logarithm) ── */

LatValue math_log(const LatValue *v, char **err) {
    double x = 0.0;
    if (v->type == VAL_INT) {
        x = (double)v->as.int_val;
    } else if (v->type == VAL_FLOAT) {
        x = v->as.float_val;
    } else {
        *err = strdup("log() expects Int or Float");
        return value_unit();
    }
    if (x <= 0.0) {
        *err = strdup("log() domain error: argument must be > 0");
        return value_unit();
    }
    return value_float(log(x));
}

/* ── log2 ── */

LatValue math_log2(const LatValue *v, char **err) {
    double x = 0.0;
    if (v->type == VAL_INT) {
        x = (double)v->as.int_val;
    } else if (v->type == VAL_FLOAT) {
        x = v->as.float_val;
    } else {
        *err = strdup("log2() expects Int or Float");
        return value_unit();
    }
    if (x <= 0.0) {
        *err = strdup("log2() domain error: argument must be > 0");
        return value_unit();
    }
    return value_float(log2(x));
}

/* ── log10 ── */

LatValue math_log10(const LatValue *v, char **err) {
    double x = 0.0;
    if (v->type == VAL_INT) {
        x = (double)v->as.int_val;
    } else if (v->type == VAL_FLOAT) {
        x = v->as.float_val;
    } else {
        *err = strdup("log10() expects Int or Float");
        return value_unit();
    }
    if (x <= 0.0) {
        *err = strdup("log10() domain error: argument must be > 0");
        return value_unit();
    }
    return value_float(log10(x));
}

/* ── sin ── */

LatValue math_sin(const LatValue *v, char **err) {
    if (v->type == VAL_INT) { return value_float(sin((double)v->as.int_val)); }
    if (v->type == VAL_FLOAT) { return value_float(sin(v->as.float_val)); }
    *err = strdup("sin() expects Int or Float");
    return value_unit();
}

/* ── cos ── */

LatValue math_cos(const LatValue *v, char **err) {
    if (v->type == VAL_INT) { return value_float(cos((double)v->as.int_val)); }
    if (v->type == VAL_FLOAT) { return value_float(cos(v->as.float_val)); }
    *err = strdup("cos() expects Int or Float");
    return value_unit();
}

/* ── tan ── */

LatValue math_tan(const LatValue *v, char **err) {
    if (v->type == VAL_INT) { return value_float(tan((double)v->as.int_val)); }
    if (v->type == VAL_FLOAT) { return value_float(tan(v->as.float_val)); }
    *err = strdup("tan() expects Int or Float");
    return value_unit();
}

/* ── atan2 ── */

LatValue math_atan2(const LatValue *y, const LatValue *x, char **err) {
    if ((y->type != VAL_INT && y->type != VAL_FLOAT) || (x->type != VAL_INT && x->type != VAL_FLOAT)) {
        *err = strdup("atan2() expects (Int|Float, Int|Float)");
        return value_unit();
    }
    double dy = to_double(y);
    double dx = to_double(x);
    return value_float(atan2(dy, dx));
}

/* ── clamp ── */

LatValue math_clamp(const LatValue *val, const LatValue *lo, const LatValue *hi, char **err) {
    if ((val->type != VAL_INT && val->type != VAL_FLOAT) || (lo->type != VAL_INT && lo->type != VAL_FLOAT) ||
        (hi->type != VAL_INT && hi->type != VAL_FLOAT)) {
        *err = strdup("clamp() expects (Int|Float, Int|Float, Int|Float)");
        return value_unit();
    }

    /* All Int: integer clamp */
    if (val->type == VAL_INT && lo->type == VAL_INT && hi->type == VAL_INT) {
        int64_t v = val->as.int_val;
        int64_t l = lo->as.int_val;
        int64_t h = hi->as.int_val;
        if (v < l) v = l;
        if (v > h) v = h;
        return value_int(v);
    }

    /* Otherwise: float clamp */
    double v = to_double(val);
    double l = to_double(lo);
    double h = to_double(hi);
    if (v < l) v = l;
    if (v > h) v = h;
    return value_float(v);
}

/* ── math_pi ── */

LatValue math_pi(void) { return value_float(3.14159265358979323846); }

/* ── math_e ── */

LatValue math_e(void) { return value_float(2.71828182845904523536); }

/* ── asin ── */

LatValue math_asin(const LatValue *v, char **err) {
    if (v->type == VAL_INT) { return value_float(asin((double)v->as.int_val)); }
    if (v->type == VAL_FLOAT) { return value_float(asin(v->as.float_val)); }
    *err = strdup("asin() expects Int or Float");
    return value_unit();
}

/* ── acos ── */

LatValue math_acos(const LatValue *v, char **err) {
    if (v->type == VAL_INT) { return value_float(acos((double)v->as.int_val)); }
    if (v->type == VAL_FLOAT) { return value_float(acos(v->as.float_val)); }
    *err = strdup("acos() expects Int or Float");
    return value_unit();
}

/* ── atan ── */

LatValue math_atan(const LatValue *v, char **err) {
    if (v->type == VAL_INT) { return value_float(atan((double)v->as.int_val)); }
    if (v->type == VAL_FLOAT) { return value_float(atan(v->as.float_val)); }
    *err = strdup("atan() expects Int or Float");
    return value_unit();
}

/* ── exp ── */

LatValue math_exp(const LatValue *v, char **err) {
    if (v->type == VAL_INT) { return value_float(exp((double)v->as.int_val)); }
    if (v->type == VAL_FLOAT) { return value_float(exp(v->as.float_val)); }
    *err = strdup("exp() expects Int or Float");
    return value_unit();
}

/* ── sign ── */

LatValue math_sign(const LatValue *v, char **err) {
    if (v->type == VAL_INT) {
        int64_t x = v->as.int_val;
        if (x < 0) return value_int(-1);
        if (x > 0) return value_int(1);
        return value_int(0);
    }
    if (v->type == VAL_FLOAT) {
        double x = v->as.float_val;
        if (x < 0.0) return value_float(-1.0);
        if (x > 0.0) return value_float(1.0);
        return value_float(0.0);
    }
    *err = strdup("sign() expects Int or Float");
    return value_unit();
}

/* ── gcd ── */

LatValue math_gcd(const LatValue *a, const LatValue *b, char **err) {
    if (a->type != VAL_INT || b->type != VAL_INT) {
        *err = strdup("gcd() expects (Int, Int)");
        return value_unit();
    }
    int64_t x = a->as.int_val;
    int64_t y = b->as.int_val;
    uint64_t ux = x < 0 ? (uint64_t)(-(x + 1)) + 1u : (uint64_t)x;
    uint64_t uy = y < 0 ? (uint64_t)(-(y + 1)) + 1u : (uint64_t)y;
    while (uy != 0) {
        uint64_t t = uy;
        uy = ux % uy;
        ux = t;
    }
    if (ux > (uint64_t)INT64_MAX) {
        *err = strdup("gcd(): result does not fit in Int");
        return value_unit();
    }
    return value_int((int64_t)ux);
}

/* ── lcm ── */

LatValue math_lcm(const LatValue *a, const LatValue *b, char **err) {
    if (a->type != VAL_INT || b->type != VAL_INT) {
        *err = strdup("lcm() expects (Int, Int)");
        return value_unit();
    }
    int64_t x = a->as.int_val;
    int64_t y = b->as.int_val;
    if (x == 0 || y == 0) return value_int(0);
    /* compute gcd first */
    uint64_t ax = x < 0 ? (uint64_t)(-(x + 1)) + 1u : (uint64_t)x;
    uint64_t ay = y < 0 ? (uint64_t)(-(y + 1)) + 1u : (uint64_t)y;
    uint64_t gx = ax, gy = ay;
    while (gy != 0) {
        uint64_t t = gy;
        gy = gx % gy;
        gx = t;
    }
    uint64_t result = (ax / gx) * ay;
    if (result > (uint64_t)INT64_MAX) {
        *err = strdup("lcm(): result does not fit in Int");
        return value_unit();
    }
    return value_int((int64_t)result);
}

/* ── is_nan ── */

LatValue math_is_nan(const LatValue *v, char **err) {
    if (v->type == VAL_INT) { return value_bool(false); }
    if (v->type == VAL_FLOAT) { return value_bool(isnan(v->as.float_val)); }
    *err = strdup("is_nan() expects Int or Float");
    return value_unit();
}

/* ── is_inf ── */

LatValue math_is_inf(const LatValue *v, char **err) {
    if (v->type == VAL_INT) { return value_bool(false); }
    if (v->type == VAL_FLOAT) { return value_bool(isinf(v->as.float_val)); }
    *err = strdup("is_inf() expects Int or Float");
    return value_unit();
}

/* ── sinh ── */

LatValue math_sinh(const LatValue *v, char **err) {
    if (v->type == VAL_INT) { return value_float(sinh((double)v->as.int_val)); }
    if (v->type == VAL_FLOAT) { return value_float(sinh(v->as.float_val)); }
    *err = strdup("sinh() expects Int or Float");
    return value_unit();
}

/* ── cosh ── */

LatValue math_cosh(const LatValue *v, char **err) {
    if (v->type == VAL_INT) { return value_float(cosh((double)v->as.int_val)); }
    if (v->type == VAL_FLOAT) { return value_float(cosh(v->as.float_val)); }
    *err = strdup("cosh() expects Int or Float");
    return value_unit();
}

/* ── tanh ── */

LatValue math_tanh(const LatValue *v, char **err) {
    if (v->type == VAL_INT) { return value_float(tanh((double)v->as.int_val)); }
    if (v->type == VAL_FLOAT) { return value_float(tanh(v->as.float_val)); }
    *err = strdup("tanh() expects Int or Float");
    return value_unit();
}

/* ── lerp ── */

LatValue math_lerp(const LatValue *a, const LatValue *b, const LatValue *t, char **err) {
    if ((a->type != VAL_INT && a->type != VAL_FLOAT) || (b->type != VAL_INT && b->type != VAL_FLOAT) ||
        (t->type != VAL_INT && t->type != VAL_FLOAT)) {
        *err = strdup("lerp() expects (Int|Float, Int|Float, Int|Float)");
        return value_unit();
    }
    double da = to_double(a);
    double db = to_double(b);
    double dt = to_double(t);
    return value_float(da + (db - da) * dt);
}
