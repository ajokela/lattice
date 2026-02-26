#ifndef REGEX_OPS_H
#define REGEX_OPS_H

#include "value.h"

/* Parse a flags string (e.g. "im") into POSIX regex flags to OR with REG_EXTENDED.
 * Supported: 'i' (REG_ICASE), 'm' (REG_NEWLINE).
 * Returns 0 on success, sets *err on invalid flag character. */
int parse_regex_flags(const char *flags, int *out_flags, char **err);

/* regex_match: returns Bool (true if pattern matches anywhere in str).
 * extra_flags is OR'd with REG_EXTENDED (pass 0 for default behavior).
 * Sets *err on regex compilation failure. */
LatValue regex_match(const char *pattern, const char *str, int extra_flags, char **err);

/* regex_find_all: returns Array of matched Strings.
 * extra_flags is OR'd with REG_EXTENDED (pass 0 for default behavior).
 * Sets *err on regex compilation failure. */
LatValue regex_find_all(const char *pattern, const char *str, int extra_flags, char **err);

/* regex_replace: replaces all occurrences of pattern in str with replacement.
 * extra_flags is OR'd with REG_EXTENDED (pass 0 for default behavior).
 * Returns heap-allocated result string.
 * Sets *err on regex compilation failure (returns NULL). */
char *regex_replace(const char *pattern, const char *str, const char *replacement, int extra_flags, char **err);

#endif
