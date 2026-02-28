#include "toml_ops.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#ifdef _WIN32
#include "win32_compat.h"
#endif

/* ========================================================================
 * Internal: TOML Parser
 * ======================================================================== */

typedef struct {
    const char *src;
    size_t pos;
    char *err;
} TomlParser;

static void tp_skip_ws(TomlParser *p) {
    while (p->src[p->pos] == ' ' || p->src[p->pos] == '\t') p->pos++;
}

static void tp_skip_ws_and_newlines(TomlParser *p) {
    while (p->src[p->pos] == ' ' || p->src[p->pos] == '\t' || p->src[p->pos] == '\n' || p->src[p->pos] == '\r')
        p->pos++;
}

static void tp_skip_comment(TomlParser *p) {
    if (p->src[p->pos] == '#') {
        while (p->src[p->pos] && p->src[p->pos] != '\n') p->pos++;
    }
}

static void tp_skip_line_rest(TomlParser *p) {
    tp_skip_ws(p);
    tp_skip_comment(p);
    if (p->src[p->pos] == '\n') p->pos++;
    else if (p->src[p->pos] == '\r') {
        p->pos++;
        if (p->src[p->pos] == '\n') p->pos++;
    }
}

static void tp_error(TomlParser *p, const char *msg) {
    if (!p->err) {
        size_t len = strlen(msg) + 64;
        p->err = malloc(len);
        if (!p->err) {
            p->err = strdup("toml_parse error: out of memory");
            return;
        }
        snprintf(p->err, len, "toml_parse error at position %zu: %s", p->pos, msg);
    }
}

/* Forward declarations */
static LatValue tp_parse_value(TomlParser *p);

/* ── Parse bare key ── */
static char *tp_parse_bare_key(TomlParser *p) {
    size_t start = p->pos;
    while (isalnum((unsigned char)p->src[p->pos]) || p->src[p->pos] == '_' || p->src[p->pos] == '-') p->pos++;
    if (p->pos == start) {
        tp_error(p, "expected key");
        return NULL;
    }
    return strndup(p->src + start, p->pos - start);
}

/* ── Parse quoted string ── */
static char *tp_parse_quoted_string(TomlParser *p) {
    char quote = p->src[p->pos];
    p->pos++;

    size_t cap = 64;
    char *buf = malloc(cap);
    if (!buf) {
        tp_error(p, "out of memory");
        return NULL;
    }
    size_t len = 0;

    if (quote == '"') {
        while (p->src[p->pos] && p->src[p->pos] != '"') {
            if (p->src[p->pos] == '\\') {
                p->pos++;
                char c = p->src[p->pos++];
                char esc;
                switch (c) {
                    case 'n': esc = '\n'; break;
                    case 't': esc = '\t'; break;
                    case 'r': esc = '\r'; break;
                    case '\\': esc = '\\'; break;
                    case '"': esc = '"'; break;
                    default:
                        tp_error(p, "invalid escape sequence");
                        free(buf);
                        return NULL;
                }
                if (len + 1 >= cap) {
                    cap *= 2;
                    buf = realloc(buf, cap);
                }
                buf[len++] = esc;
            } else {
                if (len + 1 >= cap) {
                    cap *= 2;
                    buf = realloc(buf, cap);
                }
                buf[len++] = p->src[p->pos++];
            }
        }
    } else {
        while (p->src[p->pos] && p->src[p->pos] != '\'') {
            if (len + 1 >= cap) {
                cap *= 2;
                buf = realloc(buf, cap);
            }
            buf[len++] = p->src[p->pos++];
        }
    }

    if (p->src[p->pos] != quote) {
        tp_error(p, "unterminated string");
        free(buf);
        return NULL;
    }
    p->pos++;
    buf[len] = '\0';
    return buf;
}

/* ── Parse a key (bare or quoted) ── */
static char *tp_parse_key(TomlParser *p) {
    if (p->src[p->pos] == '"' || p->src[p->pos] == '\'') return tp_parse_quoted_string(p);
    return tp_parse_bare_key(p);
}

/* ── Parse string value ── */
static LatValue tp_parse_string_value(TomlParser *p) {
    char *s = tp_parse_quoted_string(p);
    if (!s) return value_unit();
    LatValue v = value_string(s);
    free(s);
    return v;
}

/* ── Parse number ── */
static LatValue tp_parse_number(TomlParser *p) {
    size_t start = p->pos;
    if (p->src[p->pos] == '-' || p->src[p->pos] == '+') p->pos++;
    bool is_float = false;

    while (isdigit((unsigned char)p->src[p->pos]) || p->src[p->pos] == '_') p->pos++;
    if (p->src[p->pos] == '.') {
        is_float = true;
        p->pos++;
        while (isdigit((unsigned char)p->src[p->pos]) || p->src[p->pos] == '_') p->pos++;
    }
    if (p->src[p->pos] == 'e' || p->src[p->pos] == 'E') {
        is_float = true;
        p->pos++;
        if (p->src[p->pos] == '+' || p->src[p->pos] == '-') p->pos++;
        while (isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }

    size_t nlen = p->pos - start;
    char *numstr = malloc(nlen + 1);
    if (!numstr) {
        tp_error(p, "out of memory");
        return value_unit();
    }
    size_t j = 0;
    for (size_t i = start; i < p->pos; i++) {
        if (p->src[i] != '_') numstr[j++] = p->src[i];
    }
    numstr[j] = '\0';

    LatValue result;
    if (is_float) {
        result = value_float(strtod(numstr, NULL));
    } else {
        result = value_int(strtoll(numstr, NULL, 10));
    }
    free(numstr);
    return result;
}

/* ── Array push helper ── */
static void arr_push(LatValue **elems, size_t *len, size_t *cap, LatValue v) {
    if (*len >= *cap) {
        *cap = *cap < 4 ? 4 : *cap * 2;
        *elems = realloc(*elems, *cap * sizeof(LatValue));
    }
    (*elems)[(*len)++] = v;
}

/* ── Parse inline array ── */
static LatValue tp_parse_array(TomlParser *p) {
    p->pos++; /* skip [ */
    size_t cap = 4, len = 0;
    LatValue *elems = malloc(cap * sizeof(LatValue));
    if (!elems) {
        tp_error(p, "out of memory");
        return value_unit();
    }

    tp_skip_ws_and_newlines(p);
    tp_skip_comment(p);
    tp_skip_ws_and_newlines(p);

    while (p->src[p->pos] != ']' && p->src[p->pos] != '\0') {
        if (p->err) {
            for (size_t i = 0; i < len; i++) value_free(&elems[i]);
            free(elems);
            return value_unit();
        }

        LatValue elem = tp_parse_value(p);
        if (p->err) {
            value_free(&elem);
            for (size_t i = 0; i < len; i++) value_free(&elems[i]);
            free(elems);
            return value_unit();
        }

        arr_push(&elems, &len, &cap, elem);

        tp_skip_ws_and_newlines(p);
        tp_skip_comment(p);
        tp_skip_ws_and_newlines(p);
        if (p->src[p->pos] == ',') {
            p->pos++;
            tp_skip_ws_and_newlines(p);
            tp_skip_comment(p);
            tp_skip_ws_and_newlines(p);
        }
    }

    if (p->src[p->pos] != ']') {
        tp_error(p, "unterminated array");
        for (size_t i = 0; i < len; i++) value_free(&elems[i]);
        free(elems);
        return value_unit();
    }
    p->pos++;
    LatValue arr = value_array(elems, len);
    free(elems);
    return arr;
}

/* ── Parse inline table ── */
static LatValue tp_parse_inline_table(TomlParser *p) {
    p->pos++; /* skip { */
    LatValue map = value_map_new();

    tp_skip_ws(p);
    while (p->src[p->pos] != '}' && p->src[p->pos] != '\0') {
        if (p->err) {
            value_free(&map);
            return value_unit();
        }

        char *key = tp_parse_key(p);
        if (!key || p->err) {
            free(key);
            value_free(&map);
            return value_unit();
        }

        tp_skip_ws(p);
        if (p->src[p->pos] != '=') {
            tp_error(p, "expected '=' after key");
            free(key);
            value_free(&map);
            return value_unit();
        }
        p->pos++;
        tp_skip_ws(p);

        LatValue val = tp_parse_value(p);
        if (p->err) {
            free(key);
            value_free(&val);
            value_free(&map);
            return value_unit();
        }

        lat_map_set(map.as.map.map, key, &val);
        free(key);

        tp_skip_ws(p);
        if (p->src[p->pos] == ',') {
            p->pos++;
            tp_skip_ws(p);
        }
    }

    if (p->src[p->pos] != '}') {
        tp_error(p, "unterminated inline table");
        value_free(&map);
        return value_unit();
    }
    p->pos++;
    return map;
}

/* ── Parse a value ── */
static LatValue tp_parse_value(TomlParser *p) {
    char c = p->src[p->pos];
    if (c == '"' || c == '\'') return tp_parse_string_value(p);
    if (c == '[') return tp_parse_array(p);
    if (c == '{') return tp_parse_inline_table(p);
    if (c == 't' && strncmp(p->src + p->pos, "true", 4) == 0 && !isalnum((unsigned char)p->src[p->pos + 4])) {
        p->pos += 4;
        return value_bool(true);
    }
    if (c == 'f' && strncmp(p->src + p->pos, "false", 5) == 0 && !isalnum((unsigned char)p->src[p->pos + 5])) {
        p->pos += 5;
        return value_bool(false);
    }
    if (isdigit((unsigned char)c) || c == '-' || c == '+') return tp_parse_number(p);

    tp_error(p, "unexpected character");
    return value_unit();
}

/* ── Get or create nested map from dotted key path ── */
static LatMap *tp_ensure_table(LatValue *root, char **parts, size_t count) {
    LatMap *cur = root->as.map.map;
    for (size_t i = 0; i < count; i++) {
        LatValue *existing = (LatValue *)lat_map_get(cur, parts[i]);
        if (existing && existing->type == VAL_MAP) {
            cur = existing->as.map.map;
        } else {
            LatValue sub = value_map_new();
            lat_map_set(cur, parts[i], &sub);
            LatValue *inserted = (LatValue *)lat_map_get(cur, parts[i]);
            cur = inserted->as.map.map;
        }
    }
    return cur;
}

/* ── Parse dotted key into parts ── */
static size_t tp_parse_dotted_key(TomlParser *p, char **parts, size_t max_parts) {
    size_t count = 0;
    parts[count] = tp_parse_key(p);
    if (!parts[count] || p->err) return 0;
    count++;

    while (p->src[p->pos] == '.' && count < max_parts) {
        p->pos++;
        parts[count] = tp_parse_key(p);
        if (!parts[count] || p->err) {
            for (size_t i = 0; i < count; i++) free(parts[i]);
            return 0;
        }
        count++;
    }
    return count;
}

/* ── Main TOML parse function ── */
LatValue toml_ops_parse(const char *toml_str, char **err) {
    TomlParser p = {.src = toml_str, .pos = 0, .err = NULL};
    LatValue root = value_map_new();
    LatMap *current_table = root.as.map.map;

    while (p.src[p.pos] != '\0') {
        tp_skip_ws_and_newlines(&p);
        tp_skip_comment(&p);
        if (p.src[p.pos] == '\0') break;
        if (p.src[p.pos] == '\n' || p.src[p.pos] == '\r') {
            p.pos++;
            continue;
        }

        if (p.err) break;

        /* Table header: [key] or [[key]] */
        if (p.src[p.pos] == '[') {
            bool is_array_table = false;
            p.pos++;
            if (p.src[p.pos] == '[') {
                is_array_table = true;
                p.pos++;
            }
            tp_skip_ws(&p);

            char *parts[32];
            size_t count = tp_parse_dotted_key(&p, parts, 32);
            if (count == 0 || p.err) {
                value_free(&root);
                *err = p.err ? p.err : strdup("toml_parse: invalid table header");
                return value_unit();
            }

            tp_skip_ws(&p);

            if (is_array_table) {
                if (p.src[p.pos] != ']' || p.src[p.pos + 1] != ']') {
                    for (size_t i = 0; i < count; i++) free(parts[i]);
                    tp_error(&p, "expected ']]'");
                    break;
                }
                p.pos += 2;

                /* Navigate to parent */
                LatMap *parent = root.as.map.map;
                for (size_t i = 0; i + 1 < count; i++) {
                    LatValue *existing = (LatValue *)lat_map_get(parent, parts[i]);
                    if (existing && existing->type == VAL_MAP) {
                        parent = existing->as.map.map;
                    } else {
                        LatValue sub = value_map_new();
                        lat_map_set(parent, parts[i], &sub);
                        LatValue *ins = (LatValue *)lat_map_get(parent, parts[i]);
                        parent = ins->as.map.map;
                    }
                }
                char *arr_key = parts[count - 1];
                LatValue *arr_val = (LatValue *)lat_map_get(parent, arr_key);
                if (!arr_val || arr_val->type != VAL_ARRAY) {
                    LatValue empty_arr = value_array(NULL, 0);
                    lat_map_set(parent, arr_key, &empty_arr);
                    arr_val = (LatValue *)lat_map_get(parent, arr_key);
                }
                /* Push a new map entry to this array */
                LatValue new_entry = value_map_new();
                if (arr_val->as.array.len >= arr_val->as.array.cap) {
                    size_t old_cap = arr_val->as.array.cap;
                    arr_val->as.array.cap = old_cap < 4 ? 4 : old_cap * 2;
                    arr_val->as.array.elems =
                        realloc(arr_val->as.array.elems, arr_val->as.array.cap * sizeof(LatValue));
                }
                arr_val->as.array.elems[arr_val->as.array.len] = new_entry;
                current_table = arr_val->as.array.elems[arr_val->as.array.len].as.map.map;
                arr_val->as.array.len++;
            } else {
                if (p.src[p.pos] != ']') {
                    for (size_t i = 0; i < count; i++) free(parts[i]);
                    tp_error(&p, "expected ']'");
                    break;
                }
                p.pos++;
                current_table = tp_ensure_table(&root, parts, count);
            }

            for (size_t i = 0; i < count; i++) free(parts[i]);
            tp_skip_line_rest(&p);
            continue;
        }

        /* Key = Value */
        if (isalnum((unsigned char)p.src[p.pos]) || p.src[p.pos] == '"' || p.src[p.pos] == '\'' ||
            p.src[p.pos] == '_' || p.src[p.pos] == '-') {

            char *parts[32];
            size_t count = tp_parse_dotted_key(&p, parts, 32);
            if (count == 0 || p.err) {
                value_free(&root);
                *err = p.err ? p.err : strdup("toml_parse: invalid key");
                return value_unit();
            }

            tp_skip_ws(&p);
            if (p.src[p.pos] != '=') {
                for (size_t i = 0; i < count; i++) free(parts[i]);
                tp_error(&p, "expected '='");
                break;
            }
            p.pos++;
            tp_skip_ws(&p);

            LatValue val = tp_parse_value(&p);
            if (p.err) {
                for (size_t i = 0; i < count; i++) free(parts[i]);
                value_free(&val);
                break;
            }

            /* Navigate dotted key path */
            LatMap *target = current_table;
            for (size_t i = 0; i + 1 < count; i++) {
                LatValue *existing = (LatValue *)lat_map_get(target, parts[i]);
                if (existing && existing->type == VAL_MAP) {
                    target = existing->as.map.map;
                } else {
                    LatValue sub = value_map_new();
                    lat_map_set(target, parts[i], &sub);
                    LatValue *ins = (LatValue *)lat_map_get(target, parts[i]);
                    target = ins->as.map.map;
                }
            }
            lat_map_set(target, parts[count - 1], &val);

            for (size_t i = 0; i < count; i++) free(parts[i]);
            tp_skip_line_rest(&p);
            continue;
        }

        if (p.src[p.pos] == '#') {
            tp_skip_comment(&p);
            continue;
        }
        if (p.src[p.pos] == '\n' || p.src[p.pos] == '\r') {
            p.pos++;
            continue;
        }

        tp_error(&p, "unexpected character");
        break;
    }

    if (p.err) {
        value_free(&root);
        *err = p.err;
        return value_unit();
    }

    return root;
}

/* ========================================================================
 * TOML Stringify
 * ======================================================================== */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} TomlBuf;

static void tb_init(TomlBuf *b) {
    b->cap = 256;
    b->buf = malloc(b->cap);
    b->len = 0;
    if (b->buf) b->buf[0] = '\0';
}

static void tb_append(TomlBuf *b, const char *s) {
    size_t slen = strlen(s);
    while (b->len + slen + 1 > b->cap) {
        b->cap *= 2;
        b->buf = realloc(b->buf, b->cap);
    }
    memcpy(b->buf + b->len, s, slen);
    b->len += slen;
    b->buf[b->len] = '\0';
}

static void tb_appendf(TomlBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tb_appendf(TomlBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[256];
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    tb_append(b, tmp);
}

static void tb_append_escaped(TomlBuf *b, const char *s) {
    tb_append(b, "\"");
    while (*s) {
        switch (*s) {
            case '"': tb_append(b, "\\\""); break;
            case '\\': tb_append(b, "\\\\"); break;
            case '\n': tb_append(b, "\\n"); break;
            case '\t': tb_append(b, "\\t"); break;
            case '\r': tb_append(b, "\\r"); break;
            default: {
                char c[2] = {*s, '\0'};
                tb_append(b, c);
            }
        }
        s++;
    }
    tb_append(b, "\"");
}

static void toml_stringify_value(TomlBuf *b, const LatValue *val);

static void toml_stringify_value(TomlBuf *b, const LatValue *val) {
    switch (val->type) {
        case VAL_STR: tb_append_escaped(b, val->as.str_val); break;
        case VAL_INT: tb_appendf(b, "%lld", (long long)val->as.int_val); break;
        case VAL_FLOAT: tb_appendf(b, "%g", val->as.float_val); break;
        case VAL_BOOL: tb_append(b, val->as.bool_val ? "true" : "false"); break;
        case VAL_ARRAY: {
            tb_append(b, "[");
            for (size_t i = 0; i < val->as.array.len; i++) {
                if (i > 0) tb_append(b, ", ");
                toml_stringify_value(b, &val->as.array.elems[i]);
            }
            tb_append(b, "]");
            break;
        }
        case VAL_MAP: {
            tb_append(b, "{");
            bool first = true;
            for (size_t i = 0; i < val->as.map.map->cap; i++) {
                if (val->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                if (!first) tb_append(b, ", ");
                first = false;
                tb_append(b, val->as.map.map->entries[i].key);
                tb_append(b, " = ");
                toml_stringify_value(b, (LatValue *)val->as.map.map->entries[i].value);
            }
            tb_append(b, "}");
            break;
        }
        default: tb_append(b, "\"\""); break;
    }
}

static void toml_stringify_table(TomlBuf *b, const LatValue *val, const char *prefix) {
    if (val->type != VAL_MAP) return;

    /* First pass: simple key = value */
    for (size_t i = 0; i < val->as.map.map->cap; i++) {
        if (val->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
        LatValue *v = (LatValue *)val->as.map.map->entries[i].value;
        if (v->type == VAL_MAP) continue;
        /* Skip arrays of all-maps (array of tables) */
        if (v->type == VAL_ARRAY && v->as.array.len > 0) {
            bool all_maps = true;
            for (size_t j = 0; j < v->as.array.len; j++) {
                if (v->as.array.elems[j].type != VAL_MAP) {
                    all_maps = false;
                    break;
                }
            }
            if (all_maps) continue;
        }
        tb_append(b, val->as.map.map->entries[i].key);
        tb_append(b, " = ");
        toml_stringify_value(b, v);
        tb_append(b, "\n");
    }

    /* Second pass: sub-tables */
    for (size_t i = 0; i < val->as.map.map->cap; i++) {
        if (val->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
        LatValue *v = (LatValue *)val->as.map.map->entries[i].value;
        const char *key = val->as.map.map->entries[i].key;

        if (v->type == VAL_MAP) {
            char *full_key;
            if (prefix[0] != '\0') {
                size_t flen = strlen(prefix) + strlen(key) + 2;
                full_key = malloc(flen);
                if (!full_key) continue;
                snprintf(full_key, flen, "%s.%s", prefix, key);
            } else {
                full_key = strdup(key);
            }
            tb_append(b, "\n[");
            tb_append(b, full_key);
            tb_append(b, "]\n");
            toml_stringify_table(b, v, full_key);
            free(full_key);
        } else if (v->type == VAL_ARRAY && v->as.array.len > 0) {
            bool all_maps = true;
            for (size_t j = 0; j < v->as.array.len; j++) {
                if (v->as.array.elems[j].type != VAL_MAP) {
                    all_maps = false;
                    break;
                }
            }
            if (all_maps) {
                char *full_key;
                if (prefix[0] != '\0') {
                    size_t flen = strlen(prefix) + strlen(key) + 2;
                    full_key = malloc(flen);
                    if (!full_key) continue;
                    snprintf(full_key, flen, "%s.%s", prefix, key);
                } else {
                    full_key = strdup(key);
                }
                for (size_t j = 0; j < v->as.array.len; j++) {
                    tb_append(b, "\n[[");
                    tb_append(b, full_key);
                    tb_append(b, "]]\n");
                    toml_stringify_table(b, &v->as.array.elems[j], full_key);
                }
                free(full_key);
            }
        }
    }
}

char *toml_ops_stringify(const LatValue *val, char **err) {
    if (val->type != VAL_MAP) {
        *err = strdup("toml_stringify: value must be a Map");
        return NULL;
    }
    TomlBuf b;
    tb_init(&b);
    toml_stringify_table(&b, val, "");
    return b.buf;
}
