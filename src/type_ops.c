#include "type_ops.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

/* ── to_int ── */

LatValue type_to_int(const LatValue *v, char **err) {
    switch (v->type) {
    case VAL_INT:
        return value_int(v->as.int_val);

    case VAL_FLOAT:
        return value_int((int64_t)v->as.float_val);

    case VAL_BOOL:
        return value_int(v->as.bool_val ? 1 : 0);

    case VAL_STR: {
        const char *s = v->as.str_val;
        char *endptr = NULL;
        errno = 0;
        int64_t result = strtoll(s, &endptr, 10);
        /* skip trailing whitespace */
        while (*endptr && isspace((unsigned char)*endptr)) endptr++;
        if (endptr == s || *endptr != '\0' || errno == ERANGE) {
            *err = strdup("to_int(): invalid string");
            return value_unit();
        }
        return value_int(result);
    }

    default:
        *err = strdup("to_int(): cannot convert this type to Int");
        return value_unit();
    }
}

/* ── to_float ── */

LatValue type_to_float(const LatValue *v, char **err) {
    switch (v->type) {
    case VAL_FLOAT:
        return value_float(v->as.float_val);

    case VAL_INT:
        return value_float((double)v->as.int_val);

    case VAL_BOOL:
        return value_float(v->as.bool_val ? 1.0 : 0.0);

    case VAL_STR: {
        const char *s = v->as.str_val;
        char *endptr = NULL;
        errno = 0;
        double result = strtod(s, &endptr);
        /* skip trailing whitespace */
        while (*endptr && isspace((unsigned char)*endptr)) endptr++;
        if (endptr == s || *endptr != '\0' || errno == ERANGE) {
            *err = strdup("to_float(): invalid string");
            return value_unit();
        }
        return value_float(result);
    }

    default:
        *err = strdup("to_float(): cannot convert this type to Float");
        return value_unit();
    }
}
