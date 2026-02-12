#include "env_ops.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef __EMSCRIPTEN__

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

extern char **environ;

void envvar_keys(char ***out_keys, size_t *out_count) {
    size_t count = 0;
    if (environ) {
        for (char **e = environ; *e; e++)
            count++;
    }

    char **keys = malloc(count * sizeof(char *));
    if (!keys) {
        *out_keys = NULL;
        *out_count = 0;
        return;
    }

    for (size_t i = 0; i < count; i++) {
        const char *eq = strchr(environ[i], '=');
        if (eq) {
            size_t klen = (size_t)(eq - environ[i]);
            keys[i] = malloc(klen + 1);
            memcpy(keys[i], environ[i], klen);
            keys[i][klen] = '\0';
        } else {
            keys[i] = strdup(environ[i]);
        }
    }

    *out_keys = keys;
    *out_count = count;
}

#else /* __EMSCRIPTEN__ */

char *envvar_get(const char *name) {
    (void)name;
    return NULL;
}

bool envvar_set(const char *name, const char *value, char **err) {
    (void)name; (void)value;
    *err = strdup("env_set: not available in browser");
    return false;
}

void envvar_keys(char ***out_keys, size_t *out_count) {
    *out_keys = NULL;
    *out_count = 0;
}

#endif /* __EMSCRIPTEN__ */
