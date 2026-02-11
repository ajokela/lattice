#ifndef FORMAT_OPS_H
#define FORMAT_OPS_H

#include "value.h"

/* Format a string with positional {} placeholders.
 * fmt: the format string with {} markers
 * args: array of LatValue pointers to substitute
 * argc: number of args
 * err: set on error (e.g., too few args for placeholders)
 * Returns heap-allocated result string. */
char *format_string(const char *fmt, const LatValue *args, size_t argc, char **err);

#endif /* FORMAT_OPS_H */
