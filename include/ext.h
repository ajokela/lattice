#ifndef EXT_H
#define EXT_H

#include "value.h"

/* Forward declaration */
typedef struct Evaluator Evaluator;

/*
 * Load a native extension by name.
 * Searches: ./extensions/<name>.dylib, ~/.lattice/ext/<name>.dylib,
 *           $LATTICE_EXT_PATH/<name>.dylib  (.so on Linux)
 * Returns a Map of callable native closures on success.
 * On error, sets *err to a heap-allocated error string and returns value_nil().
 */
LatValue ext_load(Evaluator *ev, const char *name, char **err);

/*
 * Call a native extension function.
 * Wraps LatValue args as LatExtValue pointers, invokes the function,
 * and unwraps the result back to LatValue.
 */
LatValue ext_call_native(void *fn_ptr, LatValue *args, size_t argc);

#endif /* EXT_H */
