#include "json.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

/* ========================================================================
 * Internal: JSON Parser (recursive descent)
 * ======================================================================== */

typedef struct {
    const char *src;
    size_t pos;
    char *err;
    int depth; /* current container nesting depth */
} JsonParser;

/* Bound recursion so deeply nested input cannot overflow the C stack. */
#define JSON_MAX_DEPTH 1000

static void jp_skip_ws(JsonParser *p) {
    while (p->src[p->pos] == ' ' || p->src[p->pos] == '\t' || p->src[p->pos] == '\n' || p->src[p->pos] == '\r') {
        p->pos++;
    }
}

static char jp_peek(JsonParser *p) { return p->src[p->pos]; }

static void jp_error(JsonParser *p, const char *msg) {
    if (!p->err) {
        size_t len = strlen(msg) + 64;
        p->err = malloc(len);
        if (!p->err) {
            p->err = strdup("json_parse error: out of memory");
            return;
        }
        snprintf(p->err, len, "json_parse error at position %zu: %s", p->pos, msg);
    }
}

/* Forward declaration */
static LatValue jp_parse_value(JsonParser *p);

/* ── Parse string ── */

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Read exactly 4 hex digits at p->pos, returning the value (0..0xFFFF) and
 * advancing pos past them. Returns -1 (and leaves pos wherever it stopped) if a
 * non-hex digit is encountered. */
static int jp_parse_hex4(JsonParser *p) {
    int cp = 0;
    for (int i = 0; i < 4; i++) {
        int d = hex_digit(p->src[p->pos]);
        if (d < 0) return -1;
        cp = (cp << 4) | d;
        p->pos++;
    }
    return cp;
}

static LatValue jp_parse_string(JsonParser *p) {
    /* Opening " already verified by caller; consume it */
    p->pos++; /* skip '"' */

    size_t cap = 64;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        jp_error(p, "out of memory");
        return value_unit();
    }

    while (p->src[p->pos] != '\0') {
        char c = p->src[p->pos];
        if (c == '"') {
            p->pos++; /* consume closing quote */
            buf[len] = '\0';
            /* Transfer ownership of buf and carry the explicit length so that an
             * embedded NUL (from a \u0000 escape) is preserved instead of being
             * truncated by a strlen-based string constructor. */
            return value_string_owned_len(buf, len);
        }
        if (c == '\\') {
            p->pos++;
            char esc = p->src[p->pos];
            if (esc == '\0') {
                jp_error(p, "unexpected end of string");
                free(buf);
                return value_unit();
            }
            p->pos++;
            switch (esc) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    /* \uXXXX - parse 4 hex digits into a UTF-16 code unit. */
                    int codepoint = jp_parse_hex4(p);
                    if (codepoint < 0) {
                        jp_error(p, "invalid \\uXXXX escape");
                        free(buf);
                        return value_unit();
                    }
                    /* Combine UTF-16 surrogate pairs into a single code point so
                     * non-BMP characters become one valid 4-byte UTF-8 sequence
                     * instead of two lone-surrogate (WTF-8) sequences. */
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        /* High surrogate: look for a following \uDCxx low surrogate. */
                        if (p->src[p->pos] == '\\' && p->src[p->pos + 1] == 'u') {
                            size_t save = p->pos;
                            p->pos += 2;
                            int lo = jp_parse_hex4(p);
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                /* Not a low surrogate: rewind and emit U+FFFD. */
                                p->pos = save;
                                codepoint = 0xFFFD;
                            }
                        } else {
                            codepoint = 0xFFFD; /* lone high surrogate */
                        }
                    } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                        codepoint = 0xFFFD; /* lone low surrogate */
                    }
                    /* Encode the (possibly combined) code point as UTF-8. NOTE: a
                     * \u0000 escape inserts a real NUL byte here; the string is
                     * built with an explicit length (value_string_owned_len) so it
                     * is preserved rather than truncated. */
                    if (codepoint < 0x80) {
                        if (len + 1 >= cap) {
                            cap *= 2;
                            char *nb = realloc(buf, cap);
                            if (!nb) {
                                jp_error(p, "out of memory");
                                free(buf);
                                return value_unit();
                            }
                            buf = nb;
                        }
                        buf[len++] = (char)codepoint;
                    } else if (codepoint < 0x800) {
                        if (len + 2 >= cap) {
                            cap *= 2;
                            char *nb = realloc(buf, cap);
                            if (!nb) {
                                jp_error(p, "out of memory");
                                free(buf);
                                return value_unit();
                            }
                            buf = nb;
                        }
                        buf[len++] = (char)(0xC0 | (codepoint >> 6));
                        buf[len++] = (char)(0x80 | (codepoint & 0x3F));
                    } else if (codepoint < 0x10000) {
                        if (len + 3 >= cap) {
                            cap *= 2;
                            char *nb = realloc(buf, cap);
                            if (!nb) {
                                jp_error(p, "out of memory");
                                free(buf);
                                return value_unit();
                            }
                            buf = nb;
                        }
                        buf[len++] = (char)(0xE0 | (codepoint >> 12));
                        buf[len++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        buf[len++] = (char)(0x80 | (codepoint & 0x3F));
                    } else {
                        if (len + 4 >= cap) {
                            cap *= 2;
                            char *nb = realloc(buf, cap);
                            if (!nb) {
                                jp_error(p, "out of memory");
                                free(buf);
                                return value_unit();
                            }
                            buf = nb;
                        }
                        buf[len++] = (char)(0xF0 | (codepoint >> 18));
                        buf[len++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
                        buf[len++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        buf[len++] = (char)(0x80 | (codepoint & 0x3F));
                    }
                    continue; /* don't fall through to the single-char append below */
                }
                default:
                    jp_error(p, "invalid escape sequence");
                    free(buf);
                    return value_unit();
            }
        } else {
            p->pos++;
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                jp_error(p, "out of memory");
                free(buf);
                return value_unit();
            }
            buf = nb;
        }
        buf[len++] = c;
    }

    jp_error(p, "unterminated string");
    free(buf);
    return value_unit();
}

/* ── Parse number ── */

static LatValue jp_parse_number(JsonParser *p) {
    const char *start = p->src + p->pos;
    bool is_float = false;

    /* Optional leading minus */
    if (p->src[p->pos] == '-') p->pos++;

    /* Integer part */
    if (p->src[p->pos] == '0') {
        p->pos++;
    } else if (p->src[p->pos] >= '1' && p->src[p->pos] <= '9') {
        while (p->src[p->pos] >= '0' && p->src[p->pos] <= '9') p->pos++;
    } else {
        jp_error(p, "invalid number");
        return value_unit();
    }

    /* Fractional part */
    if (p->src[p->pos] == '.') {
        is_float = true;
        p->pos++;
        if (!(p->src[p->pos] >= '0' && p->src[p->pos] <= '9')) {
            jp_error(p, "invalid number: expected digit after '.'");
            return value_unit();
        }
        while (p->src[p->pos] >= '0' && p->src[p->pos] <= '9') p->pos++;
    }

    /* Exponent part */
    if (p->src[p->pos] == 'e' || p->src[p->pos] == 'E') {
        is_float = true;
        p->pos++;
        if (p->src[p->pos] == '+' || p->src[p->pos] == '-') p->pos++;
        if (!(p->src[p->pos] >= '0' && p->src[p->pos] <= '9')) {
            jp_error(p, "invalid number: expected digit in exponent");
            return value_unit();
        }
        while (p->src[p->pos] >= '0' && p->src[p->pos] <= '9') p->pos++;
    }

    /* Extract the substring and convert */
    size_t numlen = (size_t)((p->src + p->pos) - start);
    char *numstr = malloc(numlen + 1);
    if (!numstr) {
        jp_error(p, "out of memory");
        return value_unit();
    }
    memcpy(numstr, start, numlen);
    numstr[numlen] = '\0';

    LatValue result;
    if (is_float) {
        double d = strtod(numstr, NULL);
        result = value_float(d);
    } else {
        char *end = NULL;
        errno = 0;
        intmax_t i = strtoimax(numstr, &end, 10);
        if (errno == ERANGE || end == numstr || *end != '\0' || i < INT64_MIN || i > INT64_MAX) {
            jp_error(p, "integer out of signed 64-bit range");
            free(numstr);
            return value_unit();
        }
        result = value_int((int64_t)i);
    }
    free(numstr);
    return result;
}

/* ── Parse array ── */

static LatValue jp_parse_array(JsonParser *p) {
    p->pos++; /* skip '[' */

    /* Build a dynamically-growing array */
    size_t cap = 8;
    size_t len = 0;
    LatValue *elems = malloc(cap * sizeof(LatValue));
    if (!elems) {
        jp_error(p, "out of memory");
        return value_unit();
    }

    jp_skip_ws(p);
    if (jp_peek(p) == ']') {
        p->pos++;
        LatValue arr = value_array(elems, 0);
        free(elems);
        return arr;
    }

    for (;;) {
        if (p->err) {
            free(elems);
            return value_unit();
        }

        LatValue elem = jp_parse_value(p);
        if (p->err) {
            value_free(&elem);
            for (size_t i = 0; i < len; i++) value_free(&elems[i]);
            free(elems);
            return value_unit();
        }

        if (len >= cap) {
            cap *= 2;
            LatValue *ne = realloc(elems, cap * sizeof(LatValue));
            if (!ne) {
                jp_error(p, "out of memory");
                value_free(&elem);
                for (size_t i = 0; i < len; i++) value_free(&elems[i]);
                free(elems);
                return value_unit();
            }
            elems = ne;
        }
        elems[len++] = elem;

        jp_skip_ws(p);
        if (jp_peek(p) == ',') {
            p->pos++;
            continue;
        }
        if (jp_peek(p) == ']') {
            p->pos++;
            break;
        }
        jp_error(p, "expected ',' or ']' in array");
        for (size_t i = 0; i < len; i++) value_free(&elems[i]);
        free(elems);
        return value_unit();
    }

    LatValue arr = value_array(elems, len);
    free(elems);
    return arr;
}

/* ── Parse object ── */

static LatValue jp_parse_object(JsonParser *p) {
    p->pos++; /* skip '{' */

    LatValue map = value_map_new();

    jp_skip_ws(p);
    if (jp_peek(p) == '}') {
        p->pos++;
        return map;
    }

    for (;;) {
        if (p->err) {
            value_free(&map);
            return value_unit();
        }

        jp_skip_ws(p);
        if (jp_peek(p) != '"') {
            jp_error(p, "expected string key in object");
            value_free(&map);
            return value_unit();
        }

        /* Parse key as a string value, then extract */
        LatValue key_val = jp_parse_string(p);
        if (p->err) {
            value_free(&key_val);
            value_free(&map);
            return value_unit();
        }

        /* LatMap keys are NUL-terminated C strings.  Reject an embedded NUL
         * rather than silently truncating the decoded JSON member name. */
        if (memchr(key_val.as.str_val, '\0', key_val.as.str_len) != NULL) {
            jp_error(p, "object key contains an embedded NUL");
            value_free(&key_val);
            value_free(&map);
            return value_unit();
        }
        char *key = strdup(key_val.as.str_val);
        value_free(&key_val);
        if (!key) {
            jp_error(p, "out of memory");
            value_free(&map);
            return value_unit();
        }

        /* JSON member names are compared after escape decoding.  Accepting a
         * duplicate and overwriting its earlier value makes signed or strict
         * protocol documents ambiguous, so reject it at every nesting level. */
        if (lat_map_get(map.as.map.map, key) != NULL) {
            jp_error(p, "duplicate object key");
            free(key);
            value_free(&map);
            return value_unit();
        }

        jp_skip_ws(p);
        if (jp_peek(p) != ':') {
            jp_error(p, "expected ':' after object key");
            free(key);
            value_free(&map);
            return value_unit();
        }
        p->pos++; /* skip ':' */

        LatValue val = jp_parse_value(p);
        if (p->err) {
            value_free(&val);
            free(key);
            value_free(&map);
            return value_unit();
        }

        lat_map_set(map.as.map.map, key, &val);
        free(key);

        jp_skip_ws(p);
        if (jp_peek(p) == ',') {
            p->pos++;
            continue;
        }
        if (jp_peek(p) == '}') {
            p->pos++;
            break;
        }
        jp_error(p, "expected ',' or '}' in object");
        value_free(&map);
        return value_unit();
    }

    return map;
}

/* ── Parse value (top-level dispatch) ── */

static LatValue jp_parse_value(JsonParser *p) {
    jp_skip_ws(p);
    char c = jp_peek(p);

    if (c == '"') return jp_parse_string(p);
    if (c == '{') {
        if (p->depth >= JSON_MAX_DEPTH) {
            jp_error(p, "maximum nesting depth exceeded");
            return value_unit();
        }
        p->depth++;
        LatValue r = jp_parse_object(p);
        p->depth--;
        return r;
    }
    if (c == '[') {
        if (p->depth >= JSON_MAX_DEPTH) {
            jp_error(p, "maximum nesting depth exceeded");
            return value_unit();
        }
        p->depth++;
        LatValue r = jp_parse_array(p);
        p->depth--;
        return r;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return jp_parse_number(p);

    /* true */
    if (strncmp(p->src + p->pos, "true", 4) == 0 && !isalnum((unsigned char)p->src[p->pos + 4])) {
        p->pos += 4;
        return value_bool(true);
    }
    /* false */
    if (strncmp(p->src + p->pos, "false", 5) == 0 && !isalnum((unsigned char)p->src[p->pos + 5])) {
        p->pos += 5;
        return value_bool(false);
    }
    /* null */
    if (strncmp(p->src + p->pos, "null", 4) == 0 && !isalnum((unsigned char)p->src[p->pos + 4])) {
        p->pos += 4;
        return value_nil();
    }

    jp_error(p, "unexpected character");
    return value_unit();
}

/* ── Public API: json_parse ── */

LatValue json_parse(const char *json, char **err) {
    *err = NULL;
    JsonParser p = {.src = json, .pos = 0, .err = NULL};

    LatValue result = jp_parse_value(&p);
    if (p.err) {
        value_free(&result);
        *err = p.err;
        return value_unit();
    }

    /* Verify no trailing non-whitespace */
    jp_skip_ws(&p);
    if (p.src[p.pos] != '\0') {
        value_free(&result);
        jp_error(&p, "unexpected trailing content");
        *err = p.err;
        return value_unit();
    }

    return result;
}

/* ========================================================================
 * Internal: JSON Serializer
 * ======================================================================== */

/* Dynamic string buffer for serialization */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} JsonBuf;

static void jb_init(JsonBuf *b) {
    b->cap = 128;
    b->len = 0;
    b->buf = malloc(b->cap);
    /* buf checked lazily by caller via jb_serialize error path */
}

static void jb_ensure(JsonBuf *b, size_t extra) {
    if (!b->buf) return;
    while (b->len + extra >= b->cap) {
        b->cap *= 2;
        b->buf = realloc(b->buf, b->cap);
    }
}

static void jb_append(JsonBuf *b, const char *s, size_t slen) {
    jb_ensure(b, slen + 1);
    memcpy(b->buf + b->len, s, slen);
    b->len += slen;
}

static void jb_append_str(JsonBuf *b, const char *s) { jb_append(b, s, strlen(s)); }

static void jb_append_char(JsonBuf *b, char c) {
    jb_ensure(b, 2);
    b->buf[b->len++] = c;
}

/* Append a JSON-escaped string (with surrounding quotes) */
static void jb_append_escaped_string(JsonBuf *b, const char *s) {
    jb_append_char(b, '"');
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"': jb_append_str(b, "\\\""); break;
            case '\\': jb_append_str(b, "\\\\"); break;
            case '\b': jb_append_str(b, "\\b"); break;
            case '\f': jb_append_str(b, "\\f"); break;
            case '\n': jb_append_str(b, "\\n"); break;
            case '\r': jb_append_str(b, "\\r"); break;
            case '\t': jb_append_str(b, "\\t"); break;
            default:
                if (c < 0x20) {
                    /* Control character: encode as \u00XX */
                    char esc[7];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    jb_append_str(b, esc);
                } else {
                    jb_append_char(b, (char)c);
                }
                break;
        }
    }
    jb_append_char(b, '"');
}

/* Forward declaration */
static bool jb_serialize(JsonBuf *b, const LatValue *val, char **err);

static bool jb_serialize(JsonBuf *b, const LatValue *val, char **err) {
    switch (val->type) {
        case VAL_INT: {
            char num[32];
            snprintf(num, sizeof(num), "%lld", (long long)val->as.int_val);
            jb_append_str(b, num);
            return true;
        }
        case VAL_FLOAT: {
            char num[64];
            double d = val->as.float_val;
            if (isinf(d) || isnan(d)) {
                jb_append_str(b, "null"); /* JSON has no Inf/NaN */
            } else {
                snprintf(num, sizeof(num), "%.17g", d);
                jb_append_str(b, num);
            }
            return true;
        }
        case VAL_BOOL: jb_append_str(b, val->as.bool_val ? "true" : "false"); return true;
        case VAL_STR: jb_append_escaped_string(b, val->as.str_val); return true;
        case VAL_UNIT:
        case VAL_NIL: jb_append_str(b, "null"); return true;
        case VAL_ARRAY: {
            jb_append_char(b, '[');
            for (size_t i = 0; i < val->as.array.len; i++) {
                if (i > 0) jb_append_char(b, ',');
                if (!jb_serialize(b, &val->as.array.elems[i], err)) return false;
            }
            jb_append_char(b, ']');
            return true;
        }
        case VAL_MAP: {
            jb_append_char(b, '{');
            LatMap *m = val->as.map.map;
            bool first = true;
            for (size_t i = 0; i < m->cap; i++) {
                if (m->entries[i].state != MAP_OCCUPIED) continue;
                if (!first) jb_append_char(b, ',');
                first = false;
                jb_append_escaped_string(b, m->entries[i].key);
                jb_append_char(b, ':');
                LatValue *mv = (LatValue *)m->entries[i].value;
                if (!jb_serialize(b, mv, err)) return false;
            }
            jb_append_char(b, '}');
            return true;
        }
        case VAL_TUPLE: {
            jb_append_char(b, '[');
            for (size_t i = 0; i < val->as.tuple.len; i++) {
                if (i > 0) jb_append_char(b, ',');
                if (!jb_serialize(b, &val->as.tuple.elems[i], err)) return false;
            }
            jb_append_char(b, ']');
            return true;
        }
        case VAL_BUFFER: {
            jb_append_char(b, '[');
            for (size_t i = 0; i < val->as.buffer.len; i++) {
                if (i > 0) jb_append_char(b, ',');
                char num[8];
                snprintf(num, sizeof(num), "%u", val->as.buffer.data[i]);
                jb_append_str(b, num);
            }
            jb_append_char(b, ']');
            return true;
        }
        case VAL_REF: return jb_serialize(b, &val->as.ref.ref->value, err);
        case VAL_STRUCT:
        case VAL_CLOSURE:
        case VAL_RANGE:
        case VAL_CHANNEL:
        case VAL_ENUM:
        case VAL_SET:
        case VAL_ITERATOR: *err = strdup("json_stringify: unsupported value type"); return false;
    }
    *err = strdup("json_stringify: unknown value type");
    return false;
}

/* ── Public API: json_stringify ── */

char *json_stringify(const LatValue *val, char **err) {
    *err = NULL;
    JsonBuf b;
    jb_init(&b);

    if (!jb_serialize(&b, val, err)) {
        free(b.buf);
        return NULL;
    }

    jb_append_char(&b, '\0');
    return b.buf;
}
