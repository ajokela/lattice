#ifndef REGEX_OPS_H
#define REGEX_OPS_H

#include "value.h"

/* regex_match: returns Bool (true if pattern matches anywhere in str).
 * Sets *err on regex compilation failure. */
LatValue regex_match(const char *pattern, const char *str, char **err);

/* regex_find_all: returns Array of matched Strings.
 * Sets *err on regex compilation failure. */
LatValue regex_find_all(const char *pattern, const char *str, char **err);

/* regex_replace: replaces all occurrences of pattern in str with replacement.
 * Returns heap-allocated result string.
 * Sets *err on regex compilation failure (returns NULL). */
char *regex_replace(const char *pattern, const char *str, const char *replacement, char **err);

#endif
