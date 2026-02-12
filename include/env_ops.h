#ifndef ENV_OPS_H
#define ENV_OPS_H

#include <stdbool.h>
#include <stddef.h>

/* Get an environment variable. Returns heap-allocated string, or NULL if not set. */
char *envvar_get(const char *name);

/* Set an environment variable. Returns true on success, false on error.
 * Sets *err on failure. */
bool envvar_set(const char *name, const char *value, char **err);

/* Get all environment variable names. Sets *out_keys to a heap-allocated array
 * of heap-allocated strings, and *out_count to the number of keys.
 * Caller must free each key and the array. */
void envvar_keys(char ***out_keys, size_t *out_count);

#endif
