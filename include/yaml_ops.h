#ifndef YAML_OPS_H
#define YAML_OPS_H

#include "value.h"

/* Parse YAML string into a Lattice value (Map or Array).
 * Returns a LatValue. On error, sets *err. */
LatValue yaml_ops_parse(const char *yaml_str, char **err);

/* Serialize a Lattice value to YAML string.
 * Returns heap-allocated string. On error, sets *err and returns NULL. */
char *yaml_ops_stringify(const LatValue *val, char **err);

#endif /* YAML_OPS_H */
