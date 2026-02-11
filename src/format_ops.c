#include "format_ops.h"

#include <stdlib.h>
#include <string.h>

char *format_string(const char *fmt, const LatValue *args, size_t argc, char **err) {
    size_t buf_cap = 128;
    size_t buf_len = 0;
    char *buf = malloc(buf_cap);
    if (!buf) { *err = strdup("format: out of memory"); return NULL; }

    size_t arg_idx = 0;
    const char *p = fmt;

    while (*p) {
        /* Escaped braces: {{ -> {, }} -> } */
        if (p[0] == '{' && p[1] == '{') {
            if (buf_len + 1 >= buf_cap) { buf_cap *= 2; buf = realloc(buf, buf_cap); }
            buf[buf_len++] = '{';
            p += 2;
            continue;
        }
        if (p[0] == '}' && p[1] == '}') {
            if (buf_len + 1 >= buf_cap) { buf_cap *= 2; buf = realloc(buf, buf_cap); }
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
            char *display = value_display(&args[arg_idx]);
            size_t dlen = strlen(display);
            while (buf_len + dlen >= buf_cap) { buf_cap *= 2; buf = realloc(buf, buf_cap); }
            memcpy(buf + buf_len, display, dlen);
            buf_len += dlen;
            free(display);
            arg_idx++;
            p += 2;
            continue;
        }

        /* Regular character */
        if (buf_len + 1 >= buf_cap) { buf_cap *= 2; buf = realloc(buf, buf_cap); }
        buf[buf_len++] = *p;
        p++;
    }

    buf[buf_len] = '\0';
    *err = NULL;
    return buf;
}
