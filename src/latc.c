#include "latc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Constant type tags for serialization ── */
#define TAG_INT     0
#define TAG_FLOAT   1
#define TAG_BOOL    2
#define TAG_STR     3
#define TAG_NIL     4
#define TAG_UNIT    5
#define TAG_CLOSURE 6

/* ── Growable byte buffer (writer) ── */

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} ByteBuf;

static void bb_init(ByteBuf *bb) {
    bb->len = 0;
    bb->cap = 1024;
    bb->data = malloc(bb->cap);
    if (!bb->data) {
        bb->cap = 0;
        return;
    }
}

static void bb_ensure(ByteBuf *bb, size_t need) {
    while (bb->len + need > bb->cap) {
        bb->cap *= 2;
        bb->data = realloc(bb->data, bb->cap);
    }
}

static void bb_write_bytes(ByteBuf *bb, const void *src, size_t n) {
    bb_ensure(bb, n);
    memcpy(bb->data + bb->len, src, n);
    bb->len += n;
}

static void bb_write_u8(ByteBuf *bb, uint8_t v) {
    bb_ensure(bb, 1);
    bb->data[bb->len++] = v;
}

static void bb_write_u16_le(ByteBuf *bb, uint16_t v) {
    uint8_t buf[2] = {(uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff)};
    bb_write_bytes(bb, buf, 2);
}

static void bb_write_u32_le(ByteBuf *bb, uint32_t v) {
    uint8_t buf[4] = {(uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff), (uint8_t)((v >> 16) & 0xff),
                      (uint8_t)((v >> 24) & 0xff)};
    bb_write_bytes(bb, buf, 4);
}

static void bb_write_i64_le(ByteBuf *bb, int64_t v) {
    uint64_t u = (uint64_t)v;
    uint8_t buf[8];
    for (int i = 0; i < 8; i++) buf[i] = (uint8_t)((u >> (i * 8)) & 0xff);
    bb_write_bytes(bb, buf, 8);
}

static void bb_write_f64_le(ByteBuf *bb, double v) {
    uint8_t buf[8];
    memcpy(buf, &v, 8);
    bb_write_bytes(bb, buf, 8);
}

/* ── Bounds-checked byte reader ── */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} ByteReader;

static bool br_read_u8(ByteReader *br, uint8_t *out) {
    if (br->pos + 1 > br->len) return false;
    *out = br->data[br->pos++];
    return true;
}

static bool br_read_u16_le(ByteReader *br, uint16_t *out) {
    if (br->pos + 2 > br->len) return false;
    *out = (uint16_t)br->data[br->pos] | ((uint16_t)br->data[br->pos + 1] << 8);
    br->pos += 2;
    return true;
}

static bool br_read_u32_le(ByteReader *br, uint32_t *out) {
    if (br->pos + 4 > br->len) return false;
    *out = (uint32_t)br->data[br->pos] | ((uint32_t)br->data[br->pos + 1] << 8) |
           ((uint32_t)br->data[br->pos + 2] << 16) | ((uint32_t)br->data[br->pos + 3] << 24);
    br->pos += 4;
    return true;
}

static bool br_read_i64_le(ByteReader *br, int64_t *out) {
    if (br->pos + 8 > br->len) return false;
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u |= (uint64_t)br->data[br->pos + i] << (i * 8);
    br->pos += 8;
    *out = (int64_t)u;
    return true;
}

static bool br_read_f64_le(ByteReader *br, double *out) {
    if (br->pos + 8 > br->len) return false;
    memcpy(out, br->data + br->pos, 8);
    br->pos += 8;
    return true;
}

static bool br_read_bytes(ByteReader *br, void *dst, size_t n) {
    if (br->pos + n > br->len) return false;
    memcpy(dst, br->data + br->pos, n);
    br->pos += n;
    return true;
}

/* ── Serialize a single chunk (recursive) ── */

static void serialize_chunk(ByteBuf *bb, const Chunk *c) {
    /* Bytecode */
    bb_write_u32_le(bb, (uint32_t)c->code_len);
    bb_write_bytes(bb, c->code, c->code_len);

    /* Line numbers (parallel to code, same count) */
    bb_write_u32_le(bb, (uint32_t)c->lines_len);
    for (size_t i = 0; i < c->lines_len; i++) bb_write_u32_le(bb, (uint32_t)c->lines[i]);

    /* Constants */
    bb_write_u32_le(bb, (uint32_t)c->const_len);
    for (size_t i = 0; i < c->const_len; i++) {
        const LatValue *v = &c->constants[i];
        switch (v->type) {
            case VAL_INT:
                bb_write_u8(bb, TAG_INT);
                bb_write_i64_le(bb, v->as.int_val);
                break;
            case VAL_FLOAT:
                bb_write_u8(bb, TAG_FLOAT);
                bb_write_f64_le(bb, v->as.float_val);
                break;
            case VAL_BOOL:
                bb_write_u8(bb, TAG_BOOL);
                bb_write_u8(bb, v->as.bool_val ? 1 : 0);
                break;
            case VAL_STR: {
                bb_write_u8(bb, TAG_STR);
                uint32_t slen = (uint32_t)strlen(v->as.str_val);
                bb_write_u32_le(bb, slen);
                bb_write_bytes(bb, v->as.str_val, slen);
                break;
            }
            case VAL_NIL: bb_write_u8(bb, TAG_NIL); break;
            case VAL_UNIT: bb_write_u8(bb, TAG_UNIT); break;
            case VAL_CLOSURE:
                /* Compiled sub-chunk: body==NULL, native_fn holds Chunk* */
                if (v->as.closure.body == NULL && v->as.closure.native_fn != NULL) {
                    bb_write_u8(bb, TAG_CLOSURE);
                    bb_write_u32_le(bb, (uint32_t)v->as.closure.param_count);
                    bb_write_u8(bb, v->as.closure.has_variadic ? 1 : 0);
                    serialize_chunk(bb, (const Chunk *)v->as.closure.native_fn);
                } else {
                    /* Shouldn't appear in compiler output, treat as nil */
                    bb_write_u8(bb, TAG_NIL);
                }
                break;
            default:
                /* Unknown type in constant pool — write nil as fallback */
                bb_write_u8(bb, TAG_NIL);
                break;
        }
    }

    /* Local names (debug info) */
    bb_write_u32_le(bb, (uint32_t)c->local_name_cap);
    for (size_t i = 0; i < c->local_name_cap; i++) {
        if (c->local_names && c->local_names[i]) {
            bb_write_u8(bb, 1);
            uint32_t nlen = (uint32_t)strlen(c->local_names[i]);
            bb_write_u32_le(bb, nlen);
            bb_write_bytes(bb, c->local_names[i], nlen);
        } else {
            bb_write_u8(bb, 0);
        }
    }

    /* Chunk name (debug info for stack traces) */
    if (c->name) {
        bb_write_u8(bb, 1);
        uint32_t nlen = (uint32_t)strlen(c->name);
        bb_write_u32_le(bb, nlen);
        bb_write_bytes(bb, c->name, nlen);
    } else {
        bb_write_u8(bb, 0);
    }
}

/* ── Deserialize a single chunk (recursive) ── */

static Chunk *deserialize_chunk(ByteReader *br, char **err) {
    uint32_t code_len;
    if (!br_read_u32_le(br, &code_len)) {
        *err = strdup("truncated: missing code_len");
        return NULL;
    }

    Chunk *c = chunk_new();

    /* Bytecode */
    if (code_len > 0) {
        if (c->code_cap < code_len) {
            c->code_cap = code_len;
            c->code = realloc(c->code, c->code_cap);
        }
        if (!br_read_bytes(br, c->code, code_len)) {
            *err = strdup("truncated: incomplete bytecode");
            chunk_free(c);
            return NULL;
        }
    }
    c->code_len = code_len;

    /* Line numbers */
    uint32_t line_count;
    if (!br_read_u32_le(br, &line_count)) {
        *err = strdup("truncated: missing line_count");
        chunk_free(c);
        return NULL;
    }
    if (c->lines_cap < line_count) {
        c->lines_cap = line_count;
        c->lines = realloc(c->lines, c->lines_cap * sizeof(int));
    }
    for (uint32_t i = 0; i < line_count; i++) {
        uint32_t line_val;
        if (!br_read_u32_le(br, &line_val)) {
            *err = strdup("truncated: incomplete line data");
            chunk_free(c);
            return NULL;
        }
        c->lines[i] = (int)line_val;
    }
    c->lines_len = line_count;

    /* Constants */
    uint32_t const_count;
    if (!br_read_u32_le(br, &const_count)) {
        *err = strdup("truncated: missing const_count");
        chunk_free(c);
        return NULL;
    }
    for (uint32_t i = 0; i < const_count; i++) {
        uint8_t tag;
        if (!br_read_u8(br, &tag)) {
            *err = strdup("truncated: missing constant type tag");
            chunk_free(c);
            return NULL;
        }
        switch (tag) {
            case TAG_INT: {
                int64_t val;
                if (!br_read_i64_le(br, &val)) {
                    *err = strdup("truncated: incomplete int constant");
                    chunk_free(c);
                    return NULL;
                }
                chunk_add_constant_nodupe(c, value_int(val));
                break;
            }
            case TAG_FLOAT: {
                double val;
                if (!br_read_f64_le(br, &val)) {
                    *err = strdup("truncated: incomplete float constant");
                    chunk_free(c);
                    return NULL;
                }
                chunk_add_constant_nodupe(c, value_float(val));
                break;
            }
            case TAG_BOOL: {
                uint8_t val;
                if (!br_read_u8(br, &val)) {
                    *err = strdup("truncated: incomplete bool constant");
                    chunk_free(c);
                    return NULL;
                }
                chunk_add_constant_nodupe(c, value_bool(val != 0));
                break;
            }
            case TAG_STR: {
                uint32_t slen;
                if (!br_read_u32_le(br, &slen)) {
                    *err = strdup("truncated: incomplete string length");
                    chunk_free(c);
                    return NULL;
                }
                char *s = malloc(slen + 1);
                if (!s) return NULL;
                if (!br_read_bytes(br, s, slen)) {
                    free(s);
                    *err = strdup("truncated: incomplete string data");
                    chunk_free(c);
                    return NULL;
                }
                s[slen] = '\0';
                chunk_add_constant_nodupe(c, value_string_owned(s));
                break;
            }
            case TAG_NIL: chunk_add_constant_nodupe(c, value_nil()); break;
            case TAG_UNIT: chunk_add_constant_nodupe(c, value_unit()); break;
            case TAG_CLOSURE: {
                uint32_t param_count;
                uint8_t has_variadic;
                if (!br_read_u32_le(br, &param_count)) {
                    *err = strdup("truncated: incomplete closure param_count");
                    chunk_free(c);
                    return NULL;
                }
                if (!br_read_u8(br, &has_variadic)) {
                    *err = strdup("truncated: incomplete closure has_variadic");
                    chunk_free(c);
                    return NULL;
                }
                Chunk *sub = deserialize_chunk(br, err);
                if (!sub) {
                    chunk_free(c);
                    return NULL;
                }
                LatValue fn_val;
                memset(&fn_val, 0, sizeof(fn_val));
                fn_val.type = VAL_CLOSURE;
                fn_val.phase = VTAG_UNPHASED;
                fn_val.region_id = (size_t)-1;
                fn_val.as.closure.param_names = NULL;
                fn_val.as.closure.param_count = (size_t)param_count;
                fn_val.as.closure.body = NULL;
                fn_val.as.closure.captured_env = NULL;
                fn_val.as.closure.default_values = NULL;
                fn_val.as.closure.has_variadic = (has_variadic != 0);
                fn_val.as.closure.native_fn = sub;
                chunk_add_constant_nodupe(c, fn_val);
                break;
            }
            default: {
                char msg[64];
                snprintf(msg, sizeof(msg), "unknown constant type tag: %d", tag);
                *err = strdup(msg);
                chunk_free(c);
                return NULL;
            }
        }
    }

    /* Local names */
    uint32_t local_name_count;
    if (!br_read_u32_le(br, &local_name_count)) {
        *err = strdup("truncated: missing local_name_count");
        chunk_free(c);
        return NULL;
    }
    for (uint32_t i = 0; i < local_name_count; i++) {
        uint8_t present;
        if (!br_read_u8(br, &present)) {
            *err = strdup("truncated: incomplete local name flag");
            chunk_free(c);
            return NULL;
        }
        if (present) {
            uint32_t nlen;
            if (!br_read_u32_le(br, &nlen)) {
                *err = strdup("truncated: incomplete local name length");
                chunk_free(c);
                return NULL;
            }
            char *name = malloc(nlen + 1);
            if (!name) return NULL;
            if (!br_read_bytes(br, name, nlen)) {
                free(name);
                *err = strdup("truncated: incomplete local name data");
                chunk_free(c);
                return NULL;
            }
            name[nlen] = '\0';
            chunk_set_local_name(c, (size_t)i, name);
            free(name);
        }
    }

    /* Chunk name (debug info for stack traces) */
    {
        uint8_t has_name;
        if (br_read_u8(br, &has_name) && has_name) {
            uint32_t nlen;
            if (!br_read_u32_le(br, &nlen)) {
                *err = strdup("truncated: incomplete chunk name length");
                chunk_free(c);
                return NULL;
            }
            char *cname = malloc(nlen + 1);
            if (!cname) {
                chunk_free(c);
                return NULL;
            }
            if (!br_read_bytes(br, cname, nlen)) {
                free(cname);
                *err = strdup("truncated: incomplete chunk name data");
                chunk_free(c);
                return NULL;
            }
            cname[nlen] = '\0';
            c->name = cname;
        }
        /* If br_read_u8 fails, chunk name is optional — just skip */
    }

    return c;
}

/* ── Public API ── */

uint8_t *chunk_serialize(const Chunk *c, size_t *out_len) {
    ByteBuf bb;
    bb_init(&bb);

    /* Header */
    bb_write_bytes(&bb, LATC_MAGIC, 4);
    bb_write_u16_le(&bb, LATC_FORMAT);
    bb_write_u16_le(&bb, 0); /* reserved */

    serialize_chunk(&bb, c);

    *out_len = bb.len;
    return bb.data;
}

Chunk *chunk_deserialize(const uint8_t *data, size_t len, char **err) {
    ByteReader br = {data, len, 0};

    /* Validate header */
    if (len < 8) {
        *err = strdup("file too small for .latc header");
        return NULL;
    }
    if (memcmp(data, LATC_MAGIC, 4) != 0) {
        *err = strdup("invalid magic: not a .latc file");
        return NULL;
    }
    br.pos = 4;

    uint16_t version;
    if (!br_read_u16_le(&br, &version)) {
        *err = strdup("truncated: missing format version");
        return NULL;
    }
    if (version != LATC_FORMAT) {
        char msg[64];
        snprintf(msg, sizeof(msg), "unsupported .latc format version: %u", version);
        *err = strdup(msg);
        return NULL;
    }

    uint16_t reserved;
    if (!br_read_u16_le(&br, &reserved)) {
        *err = strdup("truncated: missing reserved field");
        return NULL;
    }

    return deserialize_chunk(&br, err);
}

int chunk_save(const Chunk *c, const char *path) {
    size_t len;
    uint8_t *data = chunk_serialize(c, &len);
    if (!data) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(data);
        return -1;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    free(data);
    return (written == len) ? 0 : -1;
}

Chunk *chunk_load(const char *path, char **err) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot open '%s'", path);
        *err = strdup(msg);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (flen < 0) {
        fclose(f);
        *err = strdup("cannot determine file size");
        return NULL;
    }

    size_t len = (size_t)flen;
    uint8_t *data = malloc(len);
    if (!data) return NULL;
    size_t n = fread(data, 1, len, f);
    fclose(f);

    if (n != len) {
        free(data);
        *err = strdup("failed to read file completely");
        return NULL;
    }

    Chunk *c = chunk_deserialize(data, len, err);
    free(data);
    return c;
}

/* ═══════════════════════════════════════════════════════
 * Register VM bytecode serialization (.rlatc)
 *
 * Format: RLATC_MAGIC(4) + version(u16) + reserved(u16)
 *         + serialize_regchunk(...)
 *
 * RegChunk layout:
 *   code_len(u32) + code(u32[] LE)
 *   lines_len(u32) + lines(u32[])
 *   const_len(u32) + tagged constants
 *   local_name_cap(u32) + local names
 * ═══════════════════════════════════════════════════════ */

static void serialize_regchunk(ByteBuf *bb, const RegChunk *c) {
    /* Instructions (fixed-width u32) */
    bb_write_u32_le(bb, (uint32_t)c->code_len);
    for (size_t i = 0; i < c->code_len; i++) bb_write_u32_le(bb, c->code[i]);

    /* Line numbers */
    bb_write_u32_le(bb, (uint32_t)c->lines_len);
    for (size_t i = 0; i < c->lines_len; i++) bb_write_u32_le(bb, (uint32_t)c->lines[i]);

    /* Constants — same tagging as stack VM, plus upvalue count for closures */
    bb_write_u32_le(bb, (uint32_t)c->const_len);
    for (size_t i = 0; i < c->const_len; i++) {
        const LatValue *v = &c->constants[i];
        switch (v->type) {
            case VAL_INT:
                bb_write_u8(bb, TAG_INT);
                bb_write_i64_le(bb, v->as.int_val);
                break;
            case VAL_FLOAT:
                bb_write_u8(bb, TAG_FLOAT);
                bb_write_f64_le(bb, v->as.float_val);
                break;
            case VAL_BOOL:
                bb_write_u8(bb, TAG_BOOL);
                bb_write_u8(bb, v->as.bool_val ? 1 : 0);
                break;
            case VAL_STR: {
                bb_write_u8(bb, TAG_STR);
                uint32_t slen = (uint32_t)strlen(v->as.str_val);
                bb_write_u32_le(bb, slen);
                bb_write_bytes(bb, v->as.str_val, slen);
                break;
            }
            case VAL_NIL: bb_write_u8(bb, TAG_NIL); break;
            case VAL_UNIT: bb_write_u8(bb, TAG_UNIT); break;
            case VAL_CLOSURE:
                /* Compiled sub-chunk: body==NULL, native_fn holds RegChunk* */
                if (v->as.closure.body == NULL && v->as.closure.native_fn != NULL) {
                    bb_write_u8(bb, TAG_CLOSURE);
                    bb_write_u32_le(bb, (uint32_t)v->as.closure.param_count);
                    bb_write_u8(bb, v->as.closure.has_variadic ? 1 : 0);
                    /* RegVM stores upvalue count in region_id */
                    bb_write_u32_le(bb, (uint32_t)v->region_id);
                    serialize_regchunk(bb, (const RegChunk *)v->as.closure.native_fn);
                } else {
                    bb_write_u8(bb, TAG_NIL);
                }
                break;
            default: bb_write_u8(bb, TAG_NIL); break;
        }
    }

    /* Local names */
    bb_write_u32_le(bb, (uint32_t)c->local_name_cap);
    for (size_t i = 0; i < c->local_name_cap; i++) {
        if (c->local_names && c->local_names[i]) {
            bb_write_u8(bb, 1);
            uint32_t nlen = (uint32_t)strlen(c->local_names[i]);
            bb_write_u32_le(bb, nlen);
            bb_write_bytes(bb, c->local_names[i], nlen);
        } else {
            bb_write_u8(bb, 0);
        }
    }

    /* max_reg (high-water register count for bounded init/cleanup) */
    bb_write_u8(bb, c->max_reg);
}

static RegChunk *deserialize_regchunk(ByteReader *br, char **err) {
    uint32_t code_len;
    if (!br_read_u32_le(br, &code_len)) {
        *err = strdup("truncated: missing code_len");
        return NULL;
    }

    RegChunk *c = regchunk_new();

    /* Instructions */
    for (uint32_t i = 0; i < code_len; i++) {
        uint32_t instr;
        if (!br_read_u32_le(br, &instr)) {
            *err = strdup("truncated: incomplete instructions");
            regchunk_free(c);
            return NULL;
        }
        regchunk_write(c, instr, 0);
    }

    /* Line numbers — overwrite the zeros written by regchunk_write */
    uint32_t line_count;
    if (!br_read_u32_le(br, &line_count)) {
        *err = strdup("truncated: missing line_count");
        regchunk_free(c);
        return NULL;
    }
    /* Ensure lines array is large enough, then read all line entries.
     * We must always consume the serialized bytes to keep the reader
     * position correct for subsequent fields (constants, local names). */
    if (line_count > c->lines_cap) {
        c->lines_cap = line_count;
        c->lines = realloc(c->lines, c->lines_cap * sizeof(int));
    }
    for (uint32_t i = 0; i < line_count; i++) {
        uint32_t line_val;
        if (!br_read_u32_le(br, &line_val)) {
            *err = strdup("truncated: incomplete line data");
            regchunk_free(c);
            return NULL;
        }
        c->lines[i] = (int)line_val;
    }
    c->lines_len = line_count;

    /* Constants */
    uint32_t const_count;
    if (!br_read_u32_le(br, &const_count)) {
        *err = strdup("truncated: missing const_count");
        regchunk_free(c);
        return NULL;
    }
    for (uint32_t i = 0; i < const_count; i++) {
        uint8_t tag;
        if (!br_read_u8(br, &tag)) {
            *err = strdup("truncated: missing constant type tag");
            regchunk_free(c);
            return NULL;
        }
        switch (tag) {
            case TAG_INT: {
                int64_t val;
                if (!br_read_i64_le(br, &val)) {
                    *err = strdup("truncated int");
                    regchunk_free(c);
                    return NULL;
                }
                regchunk_add_constant(c, value_int(val));
                break;
            }
            case TAG_FLOAT: {
                double val;
                if (!br_read_f64_le(br, &val)) {
                    *err = strdup("truncated float");
                    regchunk_free(c);
                    return NULL;
                }
                regchunk_add_constant(c, value_float(val));
                break;
            }
            case TAG_BOOL: {
                uint8_t val;
                if (!br_read_u8(br, &val)) {
                    *err = strdup("truncated bool");
                    regchunk_free(c);
                    return NULL;
                }
                regchunk_add_constant(c, value_bool(val != 0));
                break;
            }
            case TAG_STR: {
                uint32_t slen;
                if (!br_read_u32_le(br, &slen)) {
                    *err = strdup("truncated string len");
                    regchunk_free(c);
                    return NULL;
                }
                char *s = malloc(slen + 1);
                if (!br_read_bytes(br, s, slen)) {
                    free(s);
                    *err = strdup("truncated string data");
                    regchunk_free(c);
                    return NULL;
                }
                s[slen] = '\0';
                regchunk_add_constant(c, value_string_owned(s));
                break;
            }
            case TAG_NIL: regchunk_add_constant(c, value_nil()); break;
            case TAG_UNIT: regchunk_add_constant(c, value_unit()); break;
            case TAG_CLOSURE: {
                uint32_t param_count;
                uint8_t has_variadic;
                uint32_t upvalue_count;
                if (!br_read_u32_le(br, &param_count)) {
                    *err = strdup("truncated closure");
                    regchunk_free(c);
                    return NULL;
                }
                if (!br_read_u8(br, &has_variadic)) {
                    *err = strdup("truncated closure");
                    regchunk_free(c);
                    return NULL;
                }
                /* RegVM stores upvalue count in region_id */
                if (!br_read_u32_le(br, &upvalue_count)) {
                    *err = strdup("truncated closure upvalue count");
                    regchunk_free(c);
                    return NULL;
                }
                RegChunk *sub = deserialize_regchunk(br, err);
                if (!sub) {
                    regchunk_free(c);
                    return NULL;
                }
                LatValue fn_val;
                memset(&fn_val, 0, sizeof(fn_val));
                fn_val.type = VAL_CLOSURE;
                fn_val.phase = VTAG_UNPHASED;
                fn_val.region_id = (size_t)upvalue_count;
                fn_val.as.closure.param_names = NULL;
                fn_val.as.closure.param_count = (size_t)param_count;
                fn_val.as.closure.body = NULL;
                fn_val.as.closure.captured_env = NULL;
                fn_val.as.closure.default_values = NULL;
                fn_val.as.closure.has_variadic = (has_variadic != 0);
                fn_val.as.closure.native_fn = sub;
                regchunk_add_constant(c, fn_val);
                break;
            }
            default: {
                char msg[64];
                snprintf(msg, sizeof(msg), "unknown constant type tag: %d", tag);
                *err = strdup(msg);
                regchunk_free(c);
                return NULL;
            }
        }
    }

    /* Local names */
    uint32_t local_name_count;
    if (!br_read_u32_le(br, &local_name_count)) {
        *err = strdup("truncated: missing local_name_count");
        regchunk_free(c);
        return NULL;
    }
    for (uint32_t i = 0; i < local_name_count; i++) {
        uint8_t present;
        if (!br_read_u8(br, &present)) {
            *err = strdup("truncated local name");
            regchunk_free(c);
            return NULL;
        }
        if (present) {
            uint32_t nlen;
            if (!br_read_u32_le(br, &nlen)) {
                *err = strdup("truncated local name len");
                regchunk_free(c);
                return NULL;
            }
            char *name = malloc(nlen + 1);
            if (!br_read_bytes(br, name, nlen)) {
                free(name);
                *err = strdup("truncated local name data");
                regchunk_free(c);
                return NULL;
            }
            name[nlen] = '\0';
            regchunk_set_local_name(c, (size_t)i, name);
            free(name);
        }
    }

    /* max_reg (high-water register count) */
    uint8_t max_reg;
    if (!br_read_u8(br, &max_reg)) {
        *err = strdup("truncated: missing max_reg");
        regchunk_free(c);
        return NULL;
    }
    c->max_reg = max_reg;

    return c;
}

uint8_t *regchunk_serialize(const RegChunk *c, size_t *out_len) {
    ByteBuf bb;
    bb_init(&bb);
    bb_write_bytes(&bb, RLATC_MAGIC, 4);
    bb_write_u16_le(&bb, RLATC_FORMAT);
    bb_write_u16_le(&bb, 0);
    serialize_regchunk(&bb, c);
    *out_len = bb.len;
    return bb.data;
}

RegChunk *regchunk_deserialize(const uint8_t *data, size_t len, char **err) {
    ByteReader br = {data, len, 0};
    if (len < 8) {
        *err = strdup("file too small for .rlatc header");
        return NULL;
    }
    if (memcmp(data, RLATC_MAGIC, 4) != 0) {
        *err = strdup("invalid magic: not a .rlatc file");
        return NULL;
    }
    br.pos = 4;
    uint16_t version;
    if (!br_read_u16_le(&br, &version)) {
        *err = strdup("truncated version");
        return NULL;
    }
    if (version != RLATC_FORMAT) {
        *err = strdup("unsupported .rlatc format version");
        return NULL;
    }
    uint16_t reserved;
    br_read_u16_le(&br, &reserved);
    return deserialize_regchunk(&br, err);
}

int regchunk_save(const RegChunk *c, const char *path) {
    size_t len;
    uint8_t *data = regchunk_serialize(c, &len);
    if (!data) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(data);
        return -1;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    free(data);
    return (written == len) ? 0 : -1;
}

RegChunk *regchunk_load(const char *path, char **err) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot open '%s'", path);
        *err = strdup(msg);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen < 0) {
        fclose(f);
        *err = strdup("cannot determine file size");
        return NULL;
    }
    size_t len = (size_t)flen;
    uint8_t *data = malloc(len);
    if (!data) return NULL;
    size_t n = fread(data, 1, len, f);
    fclose(f);
    if (n != len) {
        free(data);
        *err = strdup("failed to read file");
        return NULL;
    }
    RegChunk *c = regchunk_deserialize(data, len, err);
    free(data);
    return c;
}
