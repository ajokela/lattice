#ifndef FORMATTER_H
#define FORMATTER_H

#include <stddef.h>
#include <stdbool.h>

/* Format a Lattice source string.
 * Returns a heap-allocated formatted string, or NULL on error.
 * If err is non-NULL, stores a heap-allocated error message on failure.
 * max_width: target line width (0 = use default of 100). */
char *lat_format(const char *source, int max_width, char **err);

/* Check whether source is already formatted.
 * Returns true if the source matches its formatted output.
 * max_width: target line width (0 = use default of 100). */
bool lat_format_check(const char *source, int max_width, char **err);

/* Format source read from stdin.
 * Returns a heap-allocated formatted string, or NULL on error.
 * max_width: target line width (0 = use default of 100). */
char *lat_format_stdin(int max_width, char **err);

#endif /* FORMATTER_H */
