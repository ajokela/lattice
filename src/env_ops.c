#include "env_ops.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

char *envvar_get(const char *name) {
    const char *val = getenv(name);
    if (!val) return NULL;
    return strdup(val);
}

bool envvar_set(const char *name, const char *value, char **err) {
    if (setenv(name, value, 1) != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "envvar_set: failed to set '%s'", name);
        *err = strdup(buf);
        return false;
    }
    return true;
}
