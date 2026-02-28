#include "yaml_ops.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#ifdef _WIN32
#include "win32_compat.h"
#endif

/* ========================================================================
 * Internal: YAML Parser (indentation-based)
 * ======================================================================== */

typedef struct {
    const char *src;
    size_t pos;
    char *err;
} YamlParser;

/* ── Scalar auto-detection ── */
static LatValue yaml_detect_scalar(const char *s) {
    if (!s || *s == '\0') return value_string("");

    if (strcmp(s, "true") == 0 || strcmp(s, "True") == 0 || strcmp(s, "TRUE") == 0 || strcmp(s, "yes") == 0 ||
        strcmp(s, "Yes") == 0 || strcmp(s, "YES") == 0)
        return value_bool(true);
    if (strcmp(s, "false") == 0 || strcmp(s, "False") == 0 || strcmp(s, "FALSE") == 0 || strcmp(s, "no") == 0 ||
        strcmp(s, "No") == 0 || strcmp(s, "NO") == 0)
        return value_bool(false);
    if (strcmp(s, "null") == 0 || strcmp(s, "Null") == 0 || strcmp(s, "NULL") == 0 || strcmp(s, "~") == 0)
        return value_nil();

    /* Integer */
    const char *p = s;
    if (*p == '-' || *p == '+') p++;
    if (*p != '\0' && isdigit((unsigned char)*p)) {
        bool all_digits = true;
        while (*p) {
            if (!isdigit((unsigned char)*p)) {
                all_digits = false;
                break;
            }
            p++;
        }
        if (all_digits) return value_int(strtoll(s, NULL, 10));
    }

    /* Float */
    char *endptr;
    double fval = strtod(s, &endptr);
    if (*endptr == '\0' && endptr != s && strchr(s, '.')) return value_float(fval);

    return value_string(s);
}

/* ── Strip quotes from a string ── */
static char *yaml_strip_quotes(const char *s) {
    size_t len = strlen(s);
    if (len >= 2 && ((s[0] == '"' && s[len - 1] == '"') || (s[0] == '\'' && s[len - 1] == '\'')))
        return strndup(s + 1, len - 2);
    return strdup(s);
}

/* ── Trim trailing whitespace ── */
static char *yaml_trim_end(const char *s, size_t len) {
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) len--;
    return strndup(s, len);
}

/* ── Count indentation at position ── */
static int yaml_indent_at(const char *src, size_t pos) {
    int indent = 0;
    while (src[pos + (size_t)indent] == ' ') indent++;
    return indent;
}

/* ── Read rest of line as trimmed value ── */
static char *yaml_read_line_value(YamlParser *p) {
    size_t start = p->pos;
    while (p->src[p->pos] && p->src[p->pos] != '\n' && p->src[p->pos] != '\r') {
        if (p->src[p->pos] == '#' && p->pos > start && p->src[p->pos - 1] == ' ') break;
        p->pos++;
    }
    char *val = yaml_trim_end(p->src + start, p->pos - start);
    if (p->src[p->pos] == '\r') p->pos++;
    if (p->src[p->pos] == '\n') p->pos++;
    return val;
}

/* ── Skip blank and comment-only lines ── */
static void yaml_skip_blanks(YamlParser *p) {
    while (p->src[p->pos]) {
        size_t saved = p->pos;
        while (p->src[p->pos] == ' ' || p->src[p->pos] == '\t') p->pos++;
        if (p->src[p->pos] == '\n' || p->src[p->pos] == '\r' || p->src[p->pos] == '#') {
            while (p->src[p->pos] && p->src[p->pos] != '\n') p->pos++;
            if (p->src[p->pos] == '\n') p->pos++;
            continue;
        }
        p->pos = saved;
        break;
    }
}

/* Forward declarations */
static LatValue yaml_parse_node(YamlParser *p, int min_indent);
static LatValue yaml_parse_flow_value(YamlParser *p);

/* ── Parse flow sequence [a, b, c] ── */
static LatValue yaml_parse_flow_seq(YamlParser *p) {
    p->pos++; /* skip [ */
    size_t cap = 4, len = 0;
    LatValue *elems = malloc(cap * sizeof(LatValue));
    if (!elems) return value_array(NULL, 0);

    while (p->src[p->pos] && p->src[p->pos] != ']') {
        while (p->src[p->pos] == ' ' || p->src[p->pos] == '\t' || p->src[p->pos] == '\n' || p->src[p->pos] == '\r')
            p->pos++;
        if (p->src[p->pos] == ']') break;

        LatValue elem = yaml_parse_flow_value(p);
        if (p->err) {
            value_free(&elem);
            for (size_t i = 0; i < len; i++) value_free(&elems[i]);
            free(elems);
            return value_unit();
        }
        if (len >= cap) {
            cap *= 2;
            elems = realloc(elems, cap * sizeof(LatValue));
        }
        elems[len++] = elem;

        while (p->src[p->pos] == ' ') p->pos++;
        if (p->src[p->pos] == ',') p->pos++;
    }
    if (p->src[p->pos] == ']') p->pos++;
    LatValue arr = value_array(elems, len);
    free(elems);
    return arr;
}

/* ── Parse flow mapping {a: b, c: d} ── */
static LatValue yaml_parse_flow_map(YamlParser *p) {
    p->pos++; /* skip { */
    LatValue map = value_map_new();

    while (p->src[p->pos] && p->src[p->pos] != '}') {
        while (p->src[p->pos] == ' ' || p->src[p->pos] == '\t') p->pos++;
        if (p->src[p->pos] == '}') break;

        size_t kstart = p->pos;
        while (p->src[p->pos] && p->src[p->pos] != ':' && p->src[p->pos] != '}') p->pos++;
        char *key = yaml_trim_end(p->src + kstart, p->pos - kstart);
        char *stripped_key = yaml_strip_quotes(key);
        free(key);

        if (p->src[p->pos] == ':') {
            p->pos++;
            while (p->src[p->pos] == ' ') p->pos++;
        }

        LatValue val = yaml_parse_flow_value(p);
        if (p->err) {
            free(stripped_key);
            value_free(&val);
            value_free(&map);
            return value_unit();
        }
        lat_map_set(map.as.map.map, stripped_key, &val);
        free(stripped_key);

        while (p->src[p->pos] == ' ') p->pos++;
        if (p->src[p->pos] == ',') p->pos++;
    }
    if (p->src[p->pos] == '}') p->pos++;
    return map;
}

/* ── Parse a flow value ── */
static LatValue yaml_parse_flow_value(YamlParser *p) {
    while (p->src[p->pos] == ' ') p->pos++;
    if (p->src[p->pos] == '[') return yaml_parse_flow_seq(p);
    if (p->src[p->pos] == '{') return yaml_parse_flow_map(p);

    /* Quoted string */
    if (p->src[p->pos] == '"' || p->src[p->pos] == '\'') {
        char quote = p->src[p->pos++];
        size_t start = p->pos;
        while (p->src[p->pos] && p->src[p->pos] != quote) {
            if (p->src[p->pos] == '\\') p->pos++;
            p->pos++;
        }
        char *s = strndup(p->src + start, p->pos - start);
        if (p->src[p->pos] == quote) p->pos++;
        LatValue v = value_string(s);
        free(s);
        return v;
    }

    /* Unquoted scalar */
    size_t start = p->pos;
    while (p->src[p->pos] && p->src[p->pos] != ',' && p->src[p->pos] != ']' && p->src[p->pos] != '}' &&
           p->src[p->pos] != '\n' && p->src[p->pos] != '\r')
        p->pos++;
    char *raw = yaml_trim_end(p->src + start, p->pos - start);
    LatValue v = yaml_detect_scalar(raw);
    free(raw);
    return v;
}

/* ── Parse a YAML node at given indentation level ── */
static LatValue yaml_parse_node(YamlParser *p, int min_indent) {
    yaml_skip_blanks(p);
    if (p->src[p->pos] == '\0') return value_unit();

    int cur_indent = yaml_indent_at(p->src, p->pos);
    if (cur_indent < min_indent) return value_unit();

    size_t line_start = p->pos;
    p->pos += (size_t)cur_indent;

    /* Flow values */
    if (p->src[p->pos] == '[') return yaml_parse_flow_seq(p);
    if (p->src[p->pos] == '{') return yaml_parse_flow_map(p);

    /* Detect sequence (starts with "- ") */
    if (p->src[p->pos] == '-' && (p->src[p->pos + 1] == ' ' || p->src[p->pos + 1] == '\n' ||
                                  p->src[p->pos + 1] == '\r' || p->src[p->pos + 1] == '\0')) {
        p->pos = line_start;
        size_t arr_cap = 4, arr_len = 0;
        LatValue *arr_elems = malloc(arr_cap * sizeof(LatValue));
        if (!arr_elems) return value_array(NULL, 0);

        while (p->src[p->pos]) {
            yaml_skip_blanks(p);
            if (p->src[p->pos] == '\0') break;

            int indent = yaml_indent_at(p->src, p->pos);
            if (indent != cur_indent) break;
            if (p->src[p->pos + (size_t)indent] != '-') break;

            p->pos += (size_t)indent + 1; /* skip indent + '-' */
            if (p->src[p->pos] == ' ') p->pos++;

            LatValue elem;
            if (p->src[p->pos] == '\n' || p->src[p->pos] == '\r' || p->src[p->pos] == '\0') {
                if (p->src[p->pos] == '\r') p->pos++;
                if (p->src[p->pos] == '\n') p->pos++;
                elem = yaml_parse_node(p, cur_indent + 2);
            } else if (p->src[p->pos] == '[' || p->src[p->pos] == '{') {
                elem = yaml_parse_flow_value(p);
                while (p->src[p->pos] && p->src[p->pos] != '\n') p->pos++;
                if (p->src[p->pos] == '\n') p->pos++;
            } else {
                /* Check for inline key: value (mapping element) */
                size_t scan = p->pos;
                bool is_mapping = false;
                while (p->src[scan] && p->src[scan] != '\n' && p->src[scan] != '\r') {
                    if (p->src[scan] == ':' && (p->src[scan + 1] == ' ' || p->src[scan + 1] == '\n' ||
                                                p->src[scan + 1] == '\r' || p->src[scan + 1] == '\0')) {
                        is_mapping = true;
                        break;
                    }
                    if (p->src[scan] == '"' || p->src[scan] == '\'') break;
                    scan++;
                }

                if (is_mapping) {
                    /* Parse mapping starting at this position */
                    int item_indent = (int)(p->pos - line_start);
                    /* Parse first key */
                    size_t kstart = p->pos;
                    while (p->src[p->pos] && p->src[p->pos] != ':') p->pos++;
                    char *key = yaml_trim_end(p->src + kstart, p->pos - kstart);
                    char *stripped_key = yaml_strip_quotes(key);
                    free(key);

                    p->pos++; /* skip : */
                    if (p->src[p->pos] == ' ') p->pos++;

                    LatValue map_elem = value_map_new();

                    if (p->src[p->pos] == '\n' || p->src[p->pos] == '\r' || p->src[p->pos] == '\0') {
                        if (p->src[p->pos] == '\r') p->pos++;
                        if (p->src[p->pos] == '\n') p->pos++;
                        LatValue val = yaml_parse_node(p, cur_indent + 2);
                        lat_map_set(map_elem.as.map.map, stripped_key, &val);
                    } else {
                        char *raw = yaml_read_line_value(p);
                        char *stripped = yaml_strip_quotes(raw);
                        LatValue val = yaml_detect_scalar(stripped);
                        free(stripped);
                        free(raw);
                        lat_map_set(map_elem.as.map.map, stripped_key, &val);
                    }
                    free(stripped_key);

                    /* Continue parsing additional keys for this map element */
                    yaml_skip_blanks(p);
                    while (p->src[p->pos]) {
                        int next_indent = yaml_indent_at(p->src, p->pos);
                        if (next_indent <= cur_indent) break;
                        if (next_indent < item_indent) break;
                        if (p->src[p->pos + (size_t)next_indent] == '-') break;

                        p->pos += (size_t)next_indent;
                        size_t kstart2 = p->pos;
                        while (p->src[p->pos] && p->src[p->pos] != ':' && p->src[p->pos] != '\n') p->pos++;
                        if (p->src[p->pos] != ':') break;
                        char *key2 = yaml_trim_end(p->src + kstart2, p->pos - kstart2);
                        char *stripped_key2 = yaml_strip_quotes(key2);
                        free(key2);
                        p->pos++;
                        if (p->src[p->pos] == ' ') p->pos++;

                        if (p->src[p->pos] == '\n' || p->src[p->pos] == '\r' || p->src[p->pos] == '\0') {
                            if (p->src[p->pos] == '\r') p->pos++;
                            if (p->src[p->pos] == '\n') p->pos++;
                            LatValue val = yaml_parse_node(p, next_indent + 1);
                            lat_map_set(map_elem.as.map.map, stripped_key2, &val);
                        } else {
                            char *raw2 = yaml_read_line_value(p);
                            char *stripped2 = yaml_strip_quotes(raw2);
                            LatValue val = yaml_detect_scalar(stripped2);
                            free(stripped2);
                            free(raw2);
                            lat_map_set(map_elem.as.map.map, stripped_key2, &val);
                        }
                        free(stripped_key2);
                        yaml_skip_blanks(p);
                    }

                    elem = map_elem;
                } else {
                    char *raw = yaml_read_line_value(p);
                    char *stripped = yaml_strip_quotes(raw);
                    elem = yaml_detect_scalar(stripped);
                    free(stripped);
                    free(raw);
                }
            }

            if (arr_len >= arr_cap) {
                arr_cap *= 2;
                arr_elems = realloc(arr_elems, arr_cap * sizeof(LatValue));
            }
            arr_elems[arr_len++] = elem;

            /* Update line_start for next iteration */
            line_start = p->pos;
        }
        LatValue arr = value_array(arr_elems, arr_len);
        free(arr_elems);
        return arr;
    }

    /* Detect mapping (key: value) */
    {
        size_t scan = p->pos;
        while (p->src[scan] && p->src[scan] != '\n' && p->src[scan] != '\r') {
            if (p->src[scan] == ':' && (p->src[scan + 1] == ' ' || p->src[scan + 1] == '\n' ||
                                        p->src[scan + 1] == '\r' || p->src[scan + 1] == '\0')) {
                /* This is a mapping */
                p->pos = line_start;
                LatValue map = value_map_new();

                while (p->src[p->pos]) {
                    yaml_skip_blanks(p);
                    if (p->src[p->pos] == '\0') break;

                    int indent = yaml_indent_at(p->src, p->pos);
                    if (indent != cur_indent) break;

                    if (p->src[p->pos + (size_t)indent] == '-' &&
                        (p->src[p->pos + (size_t)indent + 1] == ' ' || p->src[p->pos + (size_t)indent + 1] == '\n'))
                        break;

                    p->pos += (size_t)indent;

                    size_t kstart = p->pos;
                    while (p->src[p->pos] && p->src[p->pos] != ':' && p->src[p->pos] != '\n') p->pos++;
                    if (p->src[p->pos] != ':') {
                        p->pos = kstart;
                        break;
                    }
                    char *key = yaml_trim_end(p->src + kstart, p->pos - kstart);
                    char *stripped_key = yaml_strip_quotes(key);
                    free(key);

                    p->pos++; /* skip : */
                    if (p->src[p->pos] == ' ') p->pos++;

                    if (p->src[p->pos] == '\n' || p->src[p->pos] == '\r' || p->src[p->pos] == '\0') {
                        if (p->src[p->pos] == '\r') p->pos++;
                        if (p->src[p->pos] == '\n') p->pos++;
                        LatValue val = yaml_parse_node(p, cur_indent + 1);
                        lat_map_set(map.as.map.map, stripped_key, &val);
                    } else if (p->src[p->pos] == '[' || p->src[p->pos] == '{') {
                        LatValue val = yaml_parse_flow_value(p);
                        lat_map_set(map.as.map.map, stripped_key, &val);
                        while (p->src[p->pos] && p->src[p->pos] != '\n') p->pos++;
                        if (p->src[p->pos] == '\n') p->pos++;
                    } else {
                        char *raw = yaml_read_line_value(p);
                        char *stripped = yaml_strip_quotes(raw);
                        LatValue val = yaml_detect_scalar(stripped);
                        free(stripped);
                        free(raw);
                        lat_map_set(map.as.map.map, stripped_key, &val);
                    }
                    free(stripped_key);
                }
                return map;
            }
            if (p->src[scan] == '"' || p->src[scan] == '\'') {
                char q = p->src[scan++];
                while (p->src[scan] && p->src[scan] != q) {
                    if (p->src[scan] == '\\') scan++;
                    scan++;
                }
                if (p->src[scan]) scan++;
                continue;
            }
            scan++;
        }
    }

    /* Plain scalar */
    char *raw = yaml_read_line_value(p);
    char *stripped = yaml_strip_quotes(raw);
    LatValue v = yaml_detect_scalar(stripped);
    free(stripped);
    free(raw);
    return v;
}

/* ── Main YAML parse function ── */
LatValue yaml_ops_parse(const char *yaml_str, char **err) {
    YamlParser p = {.src = yaml_str, .pos = 0, .err = NULL};

    yaml_skip_blanks(&p);
    if (strncmp(p.src + p.pos, "---", 3) == 0) {
        p.pos += 3;
        while (p.src[p.pos] && p.src[p.pos] != '\n') p.pos++;
        if (p.src[p.pos] == '\n') p.pos++;
    }

    LatValue result = yaml_parse_node(&p, 0);

    if (p.err) {
        value_free(&result);
        *err = p.err;
        return value_unit();
    }
    return result;
}

/* ========================================================================
 * YAML Stringify
 * ======================================================================== */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} YamlBuf;

static void yb_init(YamlBuf *b) {
    b->cap = 256;
    b->buf = malloc(b->cap);
    b->len = 0;
    if (b->buf) b->buf[0] = '\0';
}

static void yb_append(YamlBuf *b, const char *s) {
    size_t slen = strlen(s);
    while (b->len + slen + 1 > b->cap) {
        b->cap *= 2;
        b->buf = realloc(b->buf, b->cap);
    }
    memcpy(b->buf + b->len, s, slen);
    b->len += slen;
    b->buf[b->len] = '\0';
}

static void yb_appendf(YamlBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void yb_appendf(YamlBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[256];
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    yb_append(b, tmp);
}

static void yb_indent(YamlBuf *b, int level) {
    for (int i = 0; i < level * 2; i++) yb_append(b, " ");
}

static bool yaml_needs_quoting(const char *s) {
    if (s[0] == '\0') return true;
    if (strcmp(s, "true") == 0 || strcmp(s, "false") == 0 || strcmp(s, "null") == 0 || strcmp(s, "~") == 0 ||
        strcmp(s, "yes") == 0 || strcmp(s, "no") == 0 || strcmp(s, "True") == 0 || strcmp(s, "False") == 0 ||
        strcmp(s, "Yes") == 0 || strcmp(s, "No") == 0)
        return true;
    for (const char *c = s; *c; c++) {
        if (*c == ':' || *c == '#' || *c == '[' || *c == ']' || *c == '{' || *c == '}' || *c == ',' || *c == '\n' ||
            *c == '"' || *c == '\'' || *c == '|' || *c == '>')
            return true;
    }
    if (s[0] == '-' || s[0] == '?' || s[0] == '*' || s[0] == '&') return true;
    /* Looks like a number */
    char *endptr;
    strtod(s, &endptr);
    if (*endptr == '\0' && endptr != s) return true;
    return false;
}

static void yaml_stringify_value(YamlBuf *b, const LatValue *val, int indent);

static void yaml_stringify_value(YamlBuf *b, const LatValue *val, int indent) {
    switch (val->type) {
        case VAL_STR:
            if (yaml_needs_quoting(val->as.str_val)) {
                yb_append(b, "\"");
                for (const char *s = val->as.str_val; *s; s++) {
                    switch (*s) {
                        case '"': yb_append(b, "\\\""); break;
                        case '\\': yb_append(b, "\\\\"); break;
                        case '\n': yb_append(b, "\\n"); break;
                        case '\t': yb_append(b, "\\t"); break;
                        default: {
                            char c[2] = {*s, '\0'};
                            yb_append(b, c);
                        }
                    }
                }
                yb_append(b, "\"");
            } else {
                yb_append(b, val->as.str_val);
            }
            break;
        case VAL_INT: yb_appendf(b, "%lld", (long long)val->as.int_val); break;
        case VAL_FLOAT: yb_appendf(b, "%g", val->as.float_val); break;
        case VAL_BOOL: yb_append(b, val->as.bool_val ? "true" : "false"); break;
        case VAL_UNIT:
        case VAL_NIL: yb_append(b, "null"); break;
        case VAL_ARRAY: {
            if (val->as.array.len == 0) {
                yb_append(b, "[]");
            } else {
                yb_append(b, "\n");
                for (size_t i = 0; i < val->as.array.len; i++) {
                    yb_indent(b, indent);
                    yb_append(b, "- ");
                    yaml_stringify_value(b, &val->as.array.elems[i], indent + 1);
                    if (val->as.array.elems[i].type != VAL_MAP && val->as.array.elems[i].type != VAL_ARRAY)
                        yb_append(b, "\n");
                }
            }
            break;
        }
        case VAL_MAP: {
            size_t count = lat_map_len(val->as.map.map);
            if (count == 0) {
                yb_append(b, "{}");
            } else {
                yb_append(b, "\n");
                for (size_t i = 0; i < val->as.map.map->cap; i++) {
                    if (val->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                    yb_indent(b, indent);
                    yb_append(b, val->as.map.map->entries[i].key);
                    yb_append(b, ": ");
                    LatValue *v = (LatValue *)val->as.map.map->entries[i].value;
                    yaml_stringify_value(b, v, indent + 1);
                    if (v->type != VAL_MAP && v->type != VAL_ARRAY) yb_append(b, "\n");
                }
            }
            break;
        }
        case VAL_TUPLE: {
            if (val->as.tuple.len == 0) {
                yb_append(b, "[]");
            } else {
                yb_append(b, "\n");
                for (size_t i = 0; i < val->as.tuple.len; i++) {
                    yb_indent(b, indent);
                    yb_append(b, "- ");
                    yaml_stringify_value(b, &val->as.tuple.elems[i], indent + 1);
                    if (val->as.tuple.elems[i].type != VAL_MAP && val->as.tuple.elems[i].type != VAL_ARRAY)
                        yb_append(b, "\n");
                }
            }
            break;
        }
        default: yb_append(b, "null"); break;
    }
}

char *yaml_ops_stringify(const LatValue *val, char **err) {
    (void)err;
    YamlBuf b;
    yb_init(&b);
    yaml_stringify_value(&b, val, 0);
    if (b.len > 0 && b.buf[b.len - 1] != '\n') yb_append(&b, "\n");
    return b.buf;
}
