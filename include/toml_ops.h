#ifndef TOML_OPS_H
#define TOML_OPS_H

#include "value.h"

/* Parse TOML string into a Lattice Map.
 * Returns a LatValue (typically VAL_MAP). On error, sets *err. */
LatValue toml_ops_parse(const char *toml_str, char **err);

/* Serialize a Lattice value to TOML string.
 * Returns heap-allocated string. On error, sets *err and returns NULL. */
char *toml_ops_stringify(const LatValue *val, char **err);

#endif /* TOML_OPS_H */
