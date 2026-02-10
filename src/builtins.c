#include "builtins.h"
#include "value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <editline/readline.h>

/* ── builtin_input ── */

char *builtin_input(const char *prompt) {
    char *line = readline(prompt ? prompt : "");
    if (line == NULL) return NULL;
    if (line[0] != '\0') add_history(line);
    return line;
}

/* ── builtin_read_file ── */

char *builtin_read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)len + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, (size_t)len, f);
    buf[read] = '\0';
    fclose(f);

    return buf;
}

/* ── builtin_write_file ── */

bool builtin_write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return false;
    }

    fputs(content, f);
    fclose(f);

    return true;
}

/* ── builtin_typeof_str ── */

const char *builtin_typeof_str(const LatValue *v) {
    switch (v->type) {
        case VAL_INT:     return "Int";
        case VAL_FLOAT:   return "Float";
        case VAL_BOOL:    return "Bool";
        case VAL_STR:     return "String";
        case VAL_ARRAY:   return "Array";
        case VAL_STRUCT:  return "Struct";
        case VAL_CLOSURE: return "Closure";
        case VAL_UNIT:    return "Unit";
        case VAL_RANGE:   return "Range";
        case VAL_MAP:     return "Map";
    }
    return "?";
}

/* ── builtin_phase_of_str ── */

const char *builtin_phase_of_str(const LatValue *v) {
    switch (v->phase) {
        case VTAG_FLUID:    return "fluid";
        case VTAG_CRYSTAL:  return "crystal";
        case VTAG_UNPHASED: return "unphased";
    }
    return "?";
}

/* ── builtin_to_string ── */

char *builtin_to_string(const LatValue *v) {
    return value_display(v);
}

/* ── builtin_ord ── */

int64_t builtin_ord(const char *s) {
    if (s[0] == '\0') {
        return -1;
    }
    return (int64_t)(unsigned char)s[0];
}

/* ── builtin_chr ── */

char *builtin_chr(int64_t code) {
    if (code >= 0 && code <= 127) {
        char buf[2];
        buf[0] = (char)code;
        buf[1] = '\0';
        return strdup(buf);
    }
    return strdup("?");
}

/* ── builtin_parse_int ── */

int64_t builtin_parse_int(const char *s, bool *ok) {
    char *endptr;
    errno = 0;
    long long val = strtoll(s, &endptr, 10);
    if (errno != 0 || endptr == s || *endptr != '\0') {
        *ok = false;
        return 0;
    }
    *ok = true;
    return (int64_t)val;
}

/* ── builtin_parse_float ── */

double builtin_parse_float(const char *s, bool *ok) {
    char *endptr;
    errno = 0;
    double val = strtod(s, &endptr);
    if (errno != 0 || endptr == s || *endptr != '\0') {
        *ok = false;
        return 0.0;
    }
    *ok = true;
    return val;
}
