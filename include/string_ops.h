#ifndef STRING_OPS_H
#define STRING_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Returns true if s contains substr */
bool lat_str_contains(const char *s, const char *substr);

/* Split s by delimiter. Returns malloc'd array of malloc'd strings. Sets *out_count. Caller frees all. */
char **lat_str_split(const char *s, const char *delim, size_t *out_count);

/* Returns new trimmed string (leading/trailing whitespace removed) */
char *lat_str_trim(const char *s);

/* Prefix/suffix checks */
bool lat_str_starts_with(const char *s, const char *prefix);
bool lat_str_ends_with(const char *s, const char *suffix);

/* Replace all occurrences of old_str with new_str. Returns new string. */
char *lat_str_replace(const char *s, const char *old_str, const char *new_str);

/* Case conversion. Returns new string. */
char *lat_str_to_upper(const char *s);
char *lat_str_to_lower(const char *s);

/* Substring from start to end (exclusive). Returns new string. Clamps to bounds. */
char *lat_str_substring(const char *s, int64_t start, int64_t end);

/* Find first occurrence of substr. Returns index or -1 if not found. */
int64_t lat_str_index_of(const char *s, const char *substr);

/* Get char code at index. Returns -1 if out of bounds. */
int64_t lat_str_char_code_at(const char *s, size_t idx);

/* Create single-char string from char code. Returns malloc'd string. */
char *lat_str_from_char_code(int64_t code);

/* Repeat string n times. Returns new string. */
char *lat_str_repeat(const char *s, size_t count);

/* Reverse string. Returns new string. */
char *lat_str_reverse(const char *s);

#endif /* STRING_OPS_H */
