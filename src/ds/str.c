#include "ds/str.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#define INITIAL_CAP 16

LatStr lat_str_new(void) {
    LatStr s;
    s.data = malloc(INITIAL_CAP);
    s.data[0] = '\0';
    s.len = 0;
    s.cap = INITIAL_CAP;
    return s;
}

LatStr lat_str_from(const char *cstr) {
    size_t len = strlen(cstr);
    return lat_str_from_len(cstr, len);
}

LatStr lat_str_from_len(const char *cstr, size_t len) {
    LatStr s;
    s.cap = len + 1;
    if (s.cap < INITIAL_CAP) s.cap = INITIAL_CAP;
    s.data = malloc(s.cap);
    memcpy(s.data, cstr, len);
    s.data[len] = '\0';
    s.len = len;
    return s;
}

LatStr lat_str_dup(const LatStr *s) {
    return lat_str_from_len(s->data, s->len);
}

void lat_str_free(LatStr *s) {
    free(s->data);
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

static void lat_str_grow(LatStr *s, size_t needed) {
    size_t new_cap = s->cap;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    if (new_cap != s->cap) {
        s->data = realloc(s->data, new_cap);
        s->cap = new_cap;
    }
}

void lat_str_push(LatStr *s, char c) {
    lat_str_grow(s, s->len + 2);
    s->data[s->len++] = c;
    s->data[s->len] = '\0';
}

void lat_str_append(LatStr *s, const char *cstr) {
    size_t add_len = strlen(cstr);
    lat_str_grow(s, s->len + add_len + 1);
    memcpy(s->data + s->len, cstr, add_len);
    s->len += add_len;
    s->data[s->len] = '\0';
}

void lat_str_append_str(LatStr *s, const LatStr *other) {
    lat_str_grow(s, s->len + other->len + 1);
    memcpy(s->data + s->len, other->data, other->len);
    s->len += other->len;
    s->data[s->len] = '\0';
}

void lat_str_appendf(LatStr *s, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list args2;
    va_copy(args2, args);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed > 0) {
        lat_str_grow(s, s->len + (size_t)needed + 1);
        vsnprintf(s->data + s->len, (size_t)needed + 1, fmt, args2);
        s->len += (size_t)needed;
    }
    va_end(args2);
}

const char *lat_str_cstr(const LatStr *s) {
    return s->data;
}

bool lat_str_eq(const LatStr *a, const LatStr *b) {
    if (a->len != b->len) return false;
    return memcmp(a->data, b->data, a->len) == 0;
}

bool lat_str_eq_cstr(const LatStr *s, const char *cstr) {
    return strcmp(s->data, cstr) == 0;
}

void lat_str_clear(LatStr *s) {
    s->len = 0;
    s->data[0] = '\0';
}
