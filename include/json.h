#ifndef JSON_H
#define JSON_H
#include "value.h"

/* Parse a JSON string into a LatValue.
 * Objects -> Map, Arrays -> Array, strings -> String, numbers -> Int or Float,
 * booleans -> Bool, null -> Unit.
 * Returns a LatValue. On error, sets *err to a heap-allocated error string. */
LatValue json_parse(const char *json, char **err);

/* Serialize a LatValue to a JSON string.
 * Maps -> objects, Arrays -> arrays, String -> quoted string,
 * Int/Float -> number, Bool -> true/false, Unit -> null.
 * Returns heap-allocated string. On error, sets *err. */
char *json_stringify(const LatValue *val, char **err);

#endif
