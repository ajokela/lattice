#ifndef FORMATTER_H
#define FORMATTER_H

#include <stddef.h>
#include <stdbool.h>

/* Format a Lattice source string.
 * Returns a heap-allocated formatted string, or NULL on error.
 * If err is non-NULL, stores a heap-allocated error message on failure. */
char *lat_format(const char *source, char **err);

/* Check whether source is already formatted.
 * Returns true if the source matches its formatted output. */
bool lat_format_check(const char *source, char **err);

/* Format source read from stdin.
 * Returns a heap-allocated formatted string, or NULL on error. */
char *lat_format_stdin(char **err);

#endif /* FORMATTER_H */
