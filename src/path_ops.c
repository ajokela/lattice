#include "path_ops.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

char *path_join(const char **parts, size_t count) {
    if (count == 0) return strdup("");

    /* Calculate total length needed */
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += strlen(parts[i]);
    }
    /* Room for separators between each part */
    total += count - 1;

    char *result = malloc(total + 1);
    if (!result) return strdup("");

    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        const char *part = parts[i];
        size_t plen = strlen(part);

        if (i > 0) {
            /* Avoid double slashes: skip separator if previous ends with '/'
             * or current starts with '/' */
            bool prev_slash = (pos > 0 && result[pos - 1] == '/');
            bool curr_slash = (plen > 0 && part[0] == '/');

            if (prev_slash && curr_slash) {
                /* Skip the leading slash of the current part */
                part++;
                plen--;
            } else if (!prev_slash && !curr_slash) {
                result[pos++] = '/';
            }
            /* If exactly one has a slash, no separator needed */
        }

        memcpy(result + pos, part, plen);
        pos += plen;
    }

    result[pos] = '\0';
    return result;
}

char *path_dir(const char *path) {
    if (!path || path[0] == '\0') return strdup(".");

    size_t len = strlen(path);

    /* Find last '/' */
    const char *last_slash = NULL;
    for (size_t i = len; i > 0; i--) {
        if (path[i - 1] == '/') {
            last_slash = &path[i - 1];
            break;
        }
    }

    if (!last_slash) return strdup(".");

    /* Path is just "/" */
    if (last_slash == path) return strdup("/");

    /* Return everything before the last '/' */
    size_t dir_len = (size_t)(last_slash - path);
    char *result = malloc(dir_len + 1);
    if (!result) return strdup(".");
    memcpy(result, path, dir_len);
    result[dir_len] = '\0';
    return result;
}

char *path_base(const char *path) {
    if (!path || path[0] == '\0') return strdup("");

    size_t len = strlen(path);

    /* If path ends with '/', return "" */
    if (path[len - 1] == '/') return strdup("");

    /* Find last '/' */
    const char *last_slash = NULL;
    for (size_t i = len; i > 0; i--) {
        if (path[i - 1] == '/') {
            last_slash = &path[i - 1];
            break;
        }
    }

    if (!last_slash) return strdup(path);

    return strdup(last_slash + 1);
}

char *path_ext(const char *path) {
    if (!path || path[0] == '\0') return strdup("");

    /* Get the basename portion first */
    size_t len = strlen(path);

    /* Find last '/' to get basename start */
    const char *base_start = path;
    for (size_t i = len; i > 0; i--) {
        if (path[i - 1] == '/') {
            base_start = &path[i];
            break;
        }
    }

    size_t base_len = strlen(base_start);
    if (base_len == 0) return strdup("");

    /* Find last '.' in the basename */
    const char *last_dot = NULL;
    for (size_t i = base_len; i > 0; i--) {
        if (base_start[i - 1] == '.') {
            last_dot = &base_start[i - 1];
            break;
        }
    }

    /* No dot, or dot is the first char of basename (hidden file like .hidden) */
    if (!last_dot || last_dot == base_start) return strdup("");

    return strdup(last_dot);
}
