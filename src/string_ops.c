#include "string_ops.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Helper: treat NULL as "" */
static const char *safe_str(const char *s) {
    return s ? s : "";
}

bool lat_str_contains(const char *s, const char *substr) {
    s = safe_str(s);
    substr = safe_str(substr);
    return strstr(s, substr) != NULL;
}

char **lat_str_split(const char *s, const char *delim, size_t *out_count) {
    s = safe_str(s);
    delim = safe_str(delim);

    size_t slen = strlen(s);
    size_t dlen = strlen(delim);

    /* Empty delimiter: split into individual characters */
    if (dlen == 0) {
        size_t count = slen > 0 ? slen : 1;
        char **result = malloc(count * sizeof(char *));
        if (!result) {
            *out_count = 0;
            return NULL;
        }
        if (slen == 0) {
            result[0] = strdup("");
            *out_count = 1;
            return result;
        }
        for (size_t i = 0; i < slen; i++) {
            result[i] = malloc(2);
            if (result[i]) {
                result[i][0] = s[i];
                result[i][1] = '\0';
            }
        }
        *out_count = slen;
        return result;
    }

    /* Count occurrences to pre-allocate */
    size_t count = 1;
    const char *p = s;
    while ((p = strstr(p, delim)) != NULL) {
        count++;
        p += dlen;
    }

    char **result = malloc(count * sizeof(char *));
    if (!result) {
        *out_count = 0;
        return NULL;
    }

    size_t idx = 0;
    const char *start = s;
    p = s;
    while ((p = strstr(p, delim)) != NULL) {
        size_t seg_len = (size_t)(p - start);
        result[idx] = malloc(seg_len + 1);
        if (result[idx]) {
            memcpy(result[idx], start, seg_len);
            result[idx][seg_len] = '\0';
        }
        idx++;
        p += dlen;
        start = p;
    }

    /* Final segment */
    result[idx] = strdup(start);
    idx++;

    *out_count = idx;
    return result;
}

char *lat_str_trim(const char *s) {
    s = safe_str(s);
    size_t len = strlen(s);

    /* Find first non-whitespace */
    size_t start = 0;
    while (start < len && isspace((unsigned char)s[start])) {
        start++;
    }

    /* Find last non-whitespace */
    size_t end = len;
    while (end > start && isspace((unsigned char)s[end - 1])) {
        end--;
    }

    size_t trimmed_len = end - start;
    char *result = malloc(trimmed_len + 1);
    if (result) {
        memcpy(result, s + start, trimmed_len);
        result[trimmed_len] = '\0';
    }
    return result;
}

bool lat_str_starts_with(const char *s, const char *prefix) {
    s = safe_str(s);
    prefix = safe_str(prefix);
    size_t plen = strlen(prefix);
    if (plen > strlen(s)) {
        return false;
    }
    return memcmp(s, prefix, plen) == 0;
}

bool lat_str_ends_with(const char *s, const char *suffix) {
    s = safe_str(s);
    suffix = safe_str(suffix);
    size_t slen = strlen(s);
    size_t suflen = strlen(suffix);
    if (suflen > slen) {
        return false;
    }
    return memcmp(s + slen - suflen, suffix, suflen) == 0;
}

char *lat_str_replace(const char *s, const char *old_str, const char *new_str) {
    s = safe_str(s);
    old_str = safe_str(old_str);
    new_str = safe_str(new_str);

    size_t slen = strlen(s);
    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);

    /* Empty old_str: return copy of s */
    if (old_len == 0) {
        return strdup(s);
    }

    /* Count occurrences */
    size_t count = 0;
    const char *p = s;
    while ((p = strstr(p, old_str)) != NULL) {
        count++;
        p += old_len;
    }

    if (count == 0) {
        return strdup(s);
    }

    /* Allocate result */
    size_t result_len = slen + count * (new_len - old_len);
    char *result = malloc(result_len + 1);
    if (!result) {
        return NULL;
    }

    char *dst = result;
    const char *src = s;
    while ((p = strstr(src, old_str)) != NULL) {
        size_t seg_len = (size_t)(p - src);
        memcpy(dst, src, seg_len);
        dst += seg_len;
        memcpy(dst, new_str, new_len);
        dst += new_len;
        src = p + old_len;
    }

    /* Copy remaining */
    strcpy(dst, src);
    return result;
}

char *lat_str_to_upper(const char *s) {
    s = safe_str(s);
    size_t len = strlen(s);
    char *result = malloc(len + 1);
    if (result) {
        for (size_t i = 0; i < len; i++) {
            result[i] = (char)toupper((unsigned char)s[i]);
        }
        result[len] = '\0';
    }
    return result;
}

char *lat_str_to_lower(const char *s) {
    s = safe_str(s);
    size_t len = strlen(s);
    char *result = malloc(len + 1);
    if (result) {
        for (size_t i = 0; i < len; i++) {
            result[i] = (char)tolower((unsigned char)s[i]);
        }
        result[len] = '\0';
    }
    return result;
}

char *lat_str_substring(const char *s, int64_t start, int64_t end) {
    s = safe_str(s);
    int64_t len = (int64_t)strlen(s);

    /* Clamp to [0, len] */
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if (start > len) start = len;
    if (end > len) end = len;

    /* start >= end: return empty string */
    if (start >= end) {
        return strdup("");
    }

    size_t sub_len = (size_t)(end - start);
    char *result = malloc(sub_len + 1);
    if (result) {
        memcpy(result, s + start, sub_len);
        result[sub_len] = '\0';
    }
    return result;
}

int64_t lat_str_index_of(const char *s, const char *substr) {
    s = safe_str(s);
    substr = safe_str(substr);
    const char *p = strstr(s, substr);
    if (!p) {
        return -1;
    }
    return (int64_t)(p - s);
}

int64_t lat_str_char_code_at(const char *s, size_t idx) {
    s = safe_str(s);
    if (idx >= strlen(s)) {
        return -1;
    }
    return (int64_t)(unsigned char)s[idx];
}

char *lat_str_from_char_code(int64_t code) {
    if (code < 0 || code > 127) {
        return strdup("");
    }
    char *result = malloc(2);
    if (result) {
        result[0] = (char)code;
        result[1] = '\0';
    }
    return result;
}

char *lat_str_repeat(const char *s, size_t count) {
    s = safe_str(s);
    size_t len = strlen(s);

    if (count == 0 || len == 0) {
        return strdup("");
    }

    size_t total = len * count;
    char *result = malloc(total + 1);
    if (result) {
        char *dst = result;
        for (size_t i = 0; i < count; i++) {
            memcpy(dst, s, len);
            dst += len;
        }
        *dst = '\0';
    }
    return result;
}

char *lat_str_reverse(const char *s) {
    s = safe_str(s);
    size_t len = strlen(s);
    char *result = malloc(len + 1);
    if (result) {
        for (size_t i = 0; i < len; i++) {
            result[i] = s[len - 1 - i];
        }
        result[len] = '\0';
    }
    return result;
}
