#ifndef TYPE_OPS_H
#define TYPE_OPS_H

#include "value.h"

/* Type coercion builtins.
 * They set *err on invalid conversions. */

LatValue type_to_int(const LatValue *v, char **err);
LatValue type_to_float(const LatValue *v, char **err);

#endif
