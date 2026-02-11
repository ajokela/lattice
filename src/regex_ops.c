#include "regex_ops.h"
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Helper: compile a POSIX extended regex.
 * Returns 0 on success. On failure, sets *err and returns non-zero. */
static int compile_regex(regex_t *re, const char *pattern, char **err) {
    int rc = regcomp(re, pattern, REG_EXTENDED);
    if (rc != 0) {
        size_t needed = regerror(rc, re, NULL, 0);
        char *buf = malloc(needed);
        regerror(rc, re, buf, needed);
        char *msg = NULL;
        (void)asprintf(&msg, "regex error: %s", buf);
        free(buf);
        *err = msg;
        return rc;
    }
    return 0;
}

/* ── regex_match ── */

LatValue regex_match(const char *pattern, const char *str, char **err) {
    regex_t re;
    if (compile_regex(&re, pattern, err) != 0) {
        return value_unit();
    }
    int result = regexec(&re, str, 0, NULL, 0);
    regfree(&re);
    return value_bool(result == 0);
}

/* ── regex_find_all ── */

LatValue regex_find_all(const char *pattern, const char *str, char **err) {
    regex_t re;
    if (compile_regex(&re, pattern, err) != 0) {
        return value_unit();
    }

    /* Collect matches into a dynamic array */
    size_t cap = 8;
    size_t len = 0;
    LatValue *elems = malloc(cap * sizeof(LatValue));

    regmatch_t match;
    const char *cursor = str;

    while (regexec(&re, cursor, 1, &match, 0) == 0) {
        /* Guard against zero-length matches to avoid infinite loop */
        if (match.rm_so == match.rm_eo) {
            if (cursor[match.rm_eo] == '\0') break;
            cursor += match.rm_eo + 1;
            continue;
        }

        size_t match_len = (size_t)(match.rm_eo - match.rm_so);
        char *substr = malloc(match_len + 1);
        memcpy(substr, cursor + match.rm_so, match_len);
        substr[match_len] = '\0';

        if (len >= cap) {
            cap *= 2;
            elems = realloc(elems, cap * sizeof(LatValue));
        }
        elems[len++] = value_string_owned(substr);

        cursor += match.rm_eo;
    }

    regfree(&re);
    return value_array(elems, len);
}

/* ── regex_replace ── */

char *regex_replace(const char *pattern, const char *str, const char *replacement, char **err) {
    regex_t re;
    if (compile_regex(&re, pattern, err) != 0) {
        return NULL;
    }

    size_t repl_len = strlen(replacement);
    size_t result_cap = strlen(str) + 64;
    size_t result_len = 0;
    char *result = malloc(result_cap);

    regmatch_t match;
    const char *cursor = str;

    while (regexec(&re, cursor, 1, &match, 0) == 0) {
        /* Guard against zero-length matches */
        if (match.rm_so == match.rm_eo) {
            if (cursor[match.rm_eo] == '\0') break;
            /* Copy the character at the match position and advance */
            size_t needed = result_len + 1;
            if (needed >= result_cap) {
                result_cap = needed * 2;
                result = realloc(result, result_cap);
            }
            result[result_len++] = cursor[match.rm_eo];
            cursor += match.rm_eo + 1;
            continue;
        }

        /* Copy prefix (before match) */
        size_t prefix_len = (size_t)match.rm_so;
        size_t needed = result_len + prefix_len + repl_len + 1;
        if (needed >= result_cap) {
            result_cap = needed * 2;
            result = realloc(result, result_cap);
        }
        memcpy(result + result_len, cursor, prefix_len);
        result_len += prefix_len;

        /* Copy replacement */
        memcpy(result + result_len, replacement, repl_len);
        result_len += repl_len;

        cursor += match.rm_eo;
    }

    /* Copy remainder */
    size_t tail_len = strlen(cursor);
    size_t needed = result_len + tail_len + 1;
    if (needed >= result_cap) {
        result_cap = needed;
        result = realloc(result, result_cap);
    }
    memcpy(result + result_len, cursor, tail_len);
    result_len += tail_len;
    result[result_len] = '\0';

    regfree(&re);
    return result;
}
