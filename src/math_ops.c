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
    if (v->type == VAL_FLOAT) {
        return value_float(fabs(v->as.float_val));
    }
    *err = strdup("abs() expects Int or Float");
    return value_unit();
}

/* ── floor ── */

LatValue math_floor(const LatValue *v, char **err) {
    if (v->type == VAL_INT) {
        return value_int(v->as.int_val);
    }
    if (v->type == VAL_FLOAT) {
        return value_int((int64_t)floor(v->as.float_val));
    }
    *err = strdup("floor() expects Int or Float");
    return value_unit();
}

/* ── ceil ── */

LatValue math_ceil(const LatValue *v, char **err) {
    if (v->type == VAL_INT) {
        return value_int(v->as.int_val);
    }
    if (v->type == VAL_FLOAT) {
        return value_int((int64_t)ceil(v->as.float_val));
    }
    *err = strdup("ceil() expects Int or Float");
    return value_unit();
}

/* ── round ── */

LatValue math_round(const LatValue *v, char **err) {
    if (v->type == VAL_INT) {
        return value_int(v->as.int_val);
    }
    if (v->type == VAL_FLOAT) {
        return value_int((int64_t)round(v->as.float_val));
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
    if ((base->type != VAL_INT && base->type != VAL_FLOAT) ||
        (exp->type != VAL_INT && exp->type != VAL_FLOAT)) {
        *err = strdup("pow() expects (Int|Float, Int|Float)");
        return value_unit();
    }

    /* Both Int: try integer exponentiation */
    if (base->type == VAL_INT && exp->type == VAL_INT) {
        int64_t b = base->as.int_val;
        int64_t e = exp->as.int_val;

        /* Negative exponents produce fractional results -> use float */
        if (e < 0) {
            return value_float(pow((double)b, (double)e));
        }

        /* Integer power with overflow detection */
        int64_t result = 1;
        int64_t base_val = b;
        int64_t exp_val = e;
        while (exp_val > 0) {
            if (exp_val & 1) {
                /* Check for overflow: result * base_val */
                if (base_val != 0 && (result > INT64_MAX / llabs(base_val) ||
                    result < INT64_MIN / llabs(base_val))) {
                    /* Overflow: fall through to float */
                    return value_float(pow((double)b, (double)e));
                }
                result *= base_val;
            }
            exp_val >>= 1;
            if (exp_val > 0) {
                if (base_val != 0 && llabs(base_val) > INT64_MAX / llabs(base_val)) {
                    return value_float(pow((double)b, (double)e));
                }
                base_val *= base_val;
            }
        }
        return value_int(result);
    }

    /* At least one Float: use floating-point pow */
    double b = to_double(base);
    double e = to_double(exp);
    return value_float(pow(b, e));
}

/* ── min ── */

LatValue math_min(const LatValue *a, const LatValue *b, char **err) {
    if ((a->type != VAL_INT && a->type != VAL_FLOAT) ||
        (b->type != VAL_INT && b->type != VAL_FLOAT)) {
        *err = strdup("min() expects (Int|Float, Int|Float)");
        return value_unit();
    }

    /* Both same type */
    if (a->type == VAL_INT && b->type == VAL_INT) {
        return value_int(a->as.int_val < b->as.int_val ? a->as.int_val : b->as.int_val);
    }
    if (a->type == VAL_FLOAT && b->type == VAL_FLOAT) {
        return value_float(fmin(a->as.float_val, b->as.float_val));
    }

    /* Mixed: promote to Float */
    double da = to_double(a);
    double db = to_double(b);
    return value_float(fmin(da, db));
}

/* ── max ── */

LatValue math_max(const LatValue *a, const LatValue *b, char **err) {
    if ((a->type != VAL_INT && a->type != VAL_FLOAT) ||
        (b->type != VAL_INT && b->type != VAL_FLOAT)) {
        *err = strdup("max() expects (Int|Float, Int|Float)");
        return value_unit();
    }

    /* Both same type */
    if (a->type == VAL_INT && b->type == VAL_INT) {
        return value_int(a->as.int_val > b->as.int_val ? a->as.int_val : b->as.int_val);
    }
    if (a->type == VAL_FLOAT && b->type == VAL_FLOAT) {
        return value_float(fmax(a->as.float_val, b->as.float_val));
    }

    /* Mixed: promote to Float */
    double da = to_double(a);
    double db = to_double(b);
    return value_float(fmax(da, db));
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

    /* Compute range, avoiding overflow for large spans */
    uint64_t range = (uint64_t)(hi - lo) + 1;
    int64_t result = lo + (int64_t)((uint64_t)rand() % range);
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
    if (v->type == VAL_INT) {
        return value_float(sin((double)v->as.int_val));
    }
    if (v->type == VAL_FLOAT) {
        return value_float(sin(v->as.float_val));
    }
    *err = strdup("sin() expects Int or Float");
    return value_unit();
}

/* ── cos ── */

LatValue math_cos(const LatValue *v, char **err) {
    if (v->type == VAL_INT) {
        return value_float(cos((double)v->as.int_val));
    }
    if (v->type == VAL_FLOAT) {
        return value_float(cos(v->as.float_val));
    }
    *err = strdup("cos() expects Int or Float");
    return value_unit();
}

/* ── tan ── */

LatValue math_tan(const LatValue *v, char **err) {
    if (v->type == VAL_INT) {
        return value_float(tan((double)v->as.int_val));
    }
    if (v->type == VAL_FLOAT) {
        return value_float(tan(v->as.float_val));
    }
    *err = strdup("tan() expects Int or Float");
    return value_unit();
}

/* ── atan2 ── */

LatValue math_atan2(const LatValue *y, const LatValue *x, char **err) {
    if ((y->type != VAL_INT && y->type != VAL_FLOAT) ||
        (x->type != VAL_INT && x->type != VAL_FLOAT)) {
        *err = strdup("atan2() expects (Int|Float, Int|Float)");
        return value_unit();
    }
    double dy = to_double(y);
    double dx = to_double(x);
    return value_float(atan2(dy, dx));
}

/* ── clamp ── */

LatValue math_clamp(const LatValue *val, const LatValue *lo, const LatValue *hi, char **err) {
    if ((val->type != VAL_INT && val->type != VAL_FLOAT) ||
        (lo->type != VAL_INT && lo->type != VAL_FLOAT) ||
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

LatValue math_pi(void) {
    return value_float(3.14159265358979323846);
}

/* ── math_e ── */

LatValue math_e(void) {
    return value_float(2.71828182845904523536);
}
