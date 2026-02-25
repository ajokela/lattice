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

/* Case transforms. All return new malloc'd string. */
char *lat_str_capitalize(const char *s);
char *lat_str_title_case(const char *s);
char *lat_str_snake_case(const char *s);
char *lat_str_camel_case(const char *s);
char *lat_str_kebab_case(const char *s);

/* ── Spellcheck / similarity helpers ── */

/* Compute Levenshtein edit distance between two strings.
 * Returns the minimum number of single-character edits
 * (insertions, deletions, substitutions) to transform a into b. */
int lat_levenshtein(const char *a, const char *b);

/* Search a NULL-terminated array of candidate names and return the best
 * match within max_distance. Returns NULL if no match found. */
const char *lat_find_similar(const char *name, const char **candidates, int max_distance);

/* Check if a type name is a known built-in type (Int, Float, String, etc.) */
bool lat_is_known_type(const char *name);

/* Search built-in type names (Int, Float, String, ...) for a close match.
 * Also accepts optional NULL-terminated arrays of user-defined struct and
 * enum names. Returns the best match within edit distance 2, or NULL. */
const char *lat_find_similar_type(const char *name,
                                  const char **struct_names,
                                  const char **enum_names);

/* Search Lattice keyword list for a close match to the given identifier.
 * Returns the best match within edit distance 2, or NULL. */
const char *lat_find_similar_keyword(const char *name);

#endif /* STRING_OPS_H */
