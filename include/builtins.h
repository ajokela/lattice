#ifndef BUILTINS_H
#define BUILTINS_H

#include "value.h"
#include <stdio.h>

/* Read a line from stdin. If prompt is non-NULL, print it first.
 * Returns heap-allocated string (without trailing newline), or NULL on EOF. */
char *builtin_input(const char *prompt);

/* Read entire file contents. Returns heap-allocated string, or NULL on error. */
char *builtin_read_file(const char *path);

/* Write content to file. Returns true on success. */
bool builtin_write_file(const char *path, const char *content);

/* Return the type name as a static string: "Int", "Float", "Bool", "String",
 * "Array", "Struct", "Closure", "Unit", "Range" */
const char *builtin_typeof_str(const LatValue *v);

/* Return the phase name as a static string: "fluid", "crystal", "unphased" */
const char *builtin_phase_of_str(const LatValue *v);

/* Convert a value to its string representation. Returns heap-allocated string.
 * This is essentially value_display(). */
char *builtin_to_string(const LatValue *v);

/* Get the char code (ASCII/byte value) of the first character. Returns -1 for empty string. */
int64_t builtin_ord(const char *s);

/* Create a single-character string from a char code. Returns heap-allocated string. */
char *builtin_chr(int64_t code);

/* Parse a string to int. Sets *ok to true on success, false on error.
 * Returns the parsed value (0 on error). */
int64_t builtin_parse_int(const char *s, bool *ok);

/* Parse a string to float. Sets *ok to true on success, false on error.
 * Returns the parsed value (0.0 on error). */
double builtin_parse_float(const char *s, bool *ok);

#endif /* BUILTINS_H */
