#ifndef ENV_OPS_H
#define ENV_OPS_H

#include <stdbool.h>

/* Get an environment variable. Returns heap-allocated string, or NULL if not set. */
char *envvar_get(const char *name);

/* Set an environment variable. Returns true on success, false on error.
 * Sets *err on failure. */
bool envvar_set(const char *name, const char *value, char **err);

#endif
