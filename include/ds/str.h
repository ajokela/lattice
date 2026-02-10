#ifndef LAT_STR_H
#define LAT_STR_H

#include <stddef.h>
#include <stdbool.h>

/* Dynamic string */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} LatStr;

/* Create an empty string */
LatStr lat_str_new(void);

/* Create from C string */
LatStr lat_str_from(const char *s);

/* Create from C string with explicit length */
LatStr lat_str_from_len(const char *s, size_t len);

/* Duplicate a LatStr */
LatStr lat_str_dup(const LatStr *s);

/* Free a LatStr */
void lat_str_free(LatStr *s);

/* Append a C string */
void lat_str_append(LatStr *s, const char *cstr);

/* Append a single character */
void lat_str_push(LatStr *s, char c);

/* Append another LatStr */
void lat_str_append_str(LatStr *s, const LatStr *other);

/* Append a formatted string */
void lat_str_appendf(LatStr *s, const char *fmt, ...);

/* Get C string (null-terminated) */
const char *lat_str_cstr(const LatStr *s);

/* Compare two LatStr */
bool lat_str_eq(const LatStr *a, const LatStr *b);

/* Compare LatStr with C string */
bool lat_str_eq_cstr(const LatStr *s, const char *cstr);

/* Clear contents (keep capacity) */
void lat_str_clear(LatStr *s);

#endif /* LAT_STR_H */
