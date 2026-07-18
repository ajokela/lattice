#ifndef FORMAT_OPS_H
#define FORMAT_OPS_H

#include "value.h"

/* Format a string with positional {} placeholders.
 * fmt: the format string with {} markers
 * args: array of LatValue pointers to substitute
 * argc: number of args
 * err: set on error (e.g., too few args for placeholders)
 * Returns heap-allocated result string. */
/* Returns a malloc'd buffer of *out_len bytes (NUL-terminated at [*out_len]); the length
 * must be carried by the caller (MBA-1336) — the buffer may contain embedded NULs when a
 * String argument does. out_len may be NULL if the caller cannot receive it. */
char *format_string(const char *fmt, const LatValue *args, size_t argc, size_t *out_len, char **err);

#endif /* FORMAT_OPS_H */
