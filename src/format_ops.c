#include "format_ops.h"

#include <stdlib.h>
#include <string.h>

char *format_string(const char *fmt, const LatValue *args, size_t argc, char **err) {
    size_t buf_cap = 128;
    size_t buf_len = 0;
    char *buf = malloc(buf_cap);
    if (!buf) {
        *err = strdup("format: out of memory");
        return NULL;
    }

    size_t arg_idx = 0;
    const char *p = fmt;

    while (*p) {
        /* Escaped braces: {{ -> {, }} -> } */
        if (p[0] == '{' && p[1] == '{') {
            if (buf_len + 1 >= buf_cap) {
                buf_cap *= 2;
                buf = realloc(buf, buf_cap);
            }
            buf[buf_len++] = '{';
            p += 2;
            continue;
        }
        if (p[0] == '}' && p[1] == '}') {
            if (buf_len + 1 >= buf_cap) {
                buf_cap *= 2;
                buf = realloc(buf, buf_cap);
            }
            buf[buf_len++] = '}';
            p += 2;
            continue;
        }

        /* Placeholder: {} */
        if (p[0] == '{' && p[1] == '}') {
            if (arg_idx >= argc) {
                free(buf);
                *err = strdup("format: not enough arguments for placeholders");
                return NULL;
            }
            /* MBA-1336: copy String args by byte length (may contain NULs);
             * value_display's char* return would lose the length. */
            const LatValue *arg = &args[arg_idx];
            char *display = arg->type == VAL_STR ? NULL : value_display(arg);
            const char *src = display ? display : arg->as.str_val;
            size_t dlen = display ? strlen(display) : value_string_length(arg);
            while (buf_len + dlen >= buf_cap) {
                buf_cap *= 2;
                buf = realloc(buf, buf_cap);
            }
            memcpy(buf + buf_len, src, dlen);
            buf_len += dlen;
            free(display);
            arg_idx++;
            p += 2;
            continue;
        }

        /* Regular character */
        if (buf_len + 1 >= buf_cap) {
            buf_cap *= 2;
            buf = realloc(buf, buf_cap);
        }
        buf[buf_len++] = *p;
        p++;
    }

    buf[buf_len] = '\0';
    *err = NULL;
    return buf;
}
