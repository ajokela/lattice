#include "value.h"
#include "env.h"
#include "memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Heap-tracked allocation wrappers ── */

static DualHeap *g_heap = NULL;
static CrystalRegion *g_arena = NULL;

void value_set_heap(DualHeap *heap) { g_heap = heap; }
void value_set_arena(CrystalRegion *region) { g_arena = region; }
CrystalRegion *value_get_arena(void) { return g_arena; }

static void *lat_alloc(size_t size) {
    if (g_arena) return arena_alloc(g_arena, size);
    if (g_heap) return fluid_alloc(g_heap->fluid, size);
    return malloc(size);
}

static void *lat_calloc(size_t count, size_t size) {
    if (count > 0 && size > SIZE_MAX / count) return NULL;
    if (g_arena) return arena_calloc(g_arena, count, size);
    if (g_heap) {
        size_t total = count * size;
        void *ptr = fluid_alloc(g_heap->fluid, total);
        memset(ptr, 0, total);
        return ptr;
    }
    return calloc(count, size);
}

static char *lat_strdup(const char *s) {
    if (g_arena) return arena_strdup(g_arena, s);
    size_t len = strlen(s) + 1;
    char *p = lat_alloc(len);
    memcpy(p, s, len);
    return p;
}

static void lat_free(void *ptr) {
    if (!ptr) return;
    if (g_arena) return;  /* no-op during arena clone */
    if (g_heap && fluid_dealloc(g_heap->fluid, ptr)) return;
    free(ptr);
}

/* ── Arena-routed allocation (public, for env.c) ── */

void *lat_alloc_routed(size_t size) { return lat_alloc(size); }
void *lat_calloc_routed(size_t count, size_t size) { return lat_calloc(count, size); }
char *lat_strdup_routed(const char *s) { return lat_strdup(s); }

/* ── Constructors ── */

LatValue value_int(int64_t v) {
    LatValue val;
    memset(&val, 0, sizeof(val));
    val.type = VAL_INT;
    val.phase = VTAG_UNPHASED;
    val.region_id = (size_t)-1;
    val.as.int_val = v;
    return val;
}

LatValue value_float(double v) {
    LatValue val;
    memset(&val, 0, sizeof(val));
    val.type = VAL_FLOAT;
    val.phase = VTAG_UNPHASED;
    val.region_id = (size_t)-1;
    val.as.float_val = v;
    return val;
}

LatValue value_bool(bool v) {
    LatValue val;
    memset(&val, 0, sizeof(val));
    val.type = VAL_BOOL;
    val.phase = VTAG_UNPHASED;
    val.region_id = (size_t)-1;
    val.as.bool_val = v;
    return val;
}

LatValue value_string(const char *s) {
    LatValue val;
    memset(&val, 0, sizeof(val));
    val.type = VAL_STR;
    val.phase = VTAG_UNPHASED;
    val.region_id = (size_t)-1;
    val.as.str_val = lat_strdup(s);
    return val;
}

LatValue value_string_owned(char *s) {
    LatValue val;
    memset(&val, 0, sizeof(val));
    val.type = VAL_STR;
    val.phase = VTAG_UNPHASED;
    val.region_id = (size_t)-1;
    val.as.str_val = s;
    return val;
}

LatValue value_array(LatValue *elems, size_t len) {
    LatValue val;
    memset(&val, 0, sizeof(val));
    val.type = VAL_ARRAY;
    val.phase = VTAG_UNPHASED;
    val.region_id = (size_t)-1;
    size_t cap = len < 4 ? 4 : len;
    val.as.array.elems = lat_alloc(cap * sizeof(LatValue));
    memcpy(val.as.array.elems, elems, len * sizeof(LatValue));
    val.as.array.len = len;
    val.as.array.cap = cap;
    return val;
}

LatValue value_struct(const char *name, char **field_names, LatValue *field_values, size_t count) {
    LatValue val;
    memset(&val, 0, sizeof(val));
    val.type = VAL_STRUCT;
    val.phase = VTAG_UNPHASED;
    val.region_id = (size_t)-1;
    val.as.strct.name = lat_strdup(name);
    val.as.strct.field_names = lat_alloc(count * sizeof(char *));
    val.as.strct.field_values = lat_alloc(count * sizeof(LatValue));
    for (size_t i = 0; i < count; i++) {
        val.as.strct.field_names[i] = lat_strdup(field_names[i]);
        val.as.strct.field_values[i] = field_values[i];
    }
    val.as.strct.field_count = count;
    return val;
}

LatValue value_closure(char **param_names, size_t param_count, struct Expr *body, Env *captured) {
    LatValue val;
    memset(&val, 0, sizeof(val));
    val.type = VAL_CLOSURE;
    val.phase = VTAG_UNPHASED;
    val.region_id = (size_t)-1;
    val.as.closure.param_names = lat_alloc(param_count * sizeof(char *));
    for (size_t i = 0; i < param_count; i++) {
        val.as.closure.param_names[i] = lat_strdup(param_names[i]);
    }
    val.as.closure.param_count = param_count;
    val.as.closure.body = body;       /* borrowed reference */
    val.as.closure.captured_env = captured;
    return val;
}

LatValue value_unit(void) {
    LatValue val;
    memset(&val, 0, sizeof(val));
    val.type = VAL_UNIT;
    val.phase = VTAG_UNPHASED;
    val.region_id = (size_t)-1;
    return val;
}

LatValue value_range(int64_t start, int64_t end) {
    LatValue val;
    memset(&val, 0, sizeof(val));
    val.type = VAL_RANGE;
    val.phase = VTAG_UNPHASED;
    val.region_id = (size_t)-1;
    val.as.range.start = start;
    val.as.range.end = end;
    return val;
}

LatValue value_map_new(void) {
    LatValue val;
    memset(&val, 0, sizeof(val));
    val.type = VAL_MAP;
    val.phase = VTAG_UNPHASED;
    val.region_id = (size_t)-1;
    val.as.map.map = lat_alloc(sizeof(LatMap));
    *val.as.map.map = lat_map_new(sizeof(LatValue));
    return val;
}

/* ── Phase helpers ── */

bool value_is_fluid(const LatValue *v) { return v->phase == VTAG_FLUID; }
bool value_is_crystal(const LatValue *v) { return v->phase == VTAG_CRYSTAL; }

/* ── Deep clone ── */

LatValue value_deep_clone(const LatValue *v) {
    LatValue out;
    memset(&out, 0, sizeof(out));
    out.type = v->type;
    out.phase = v->phase;
    out.region_id = (size_t)-1;  /* clone is a new value, not in any region */

    switch (v->type) {
        case VAL_INT:   out.as.int_val = v->as.int_val; break;
        case VAL_FLOAT: out.as.float_val = v->as.float_val; break;
        case VAL_BOOL:  out.as.bool_val = v->as.bool_val; break;
        case VAL_STR:   out.as.str_val = lat_strdup(v->as.str_val); break;
        case VAL_ARRAY: {
            size_t len = v->as.array.len;
            size_t cap = v->as.array.cap;
            out.as.array.elems = lat_alloc(cap * sizeof(LatValue));
            for (size_t i = 0; i < len; i++) {
                out.as.array.elems[i] = value_deep_clone(&v->as.array.elems[i]);
            }
            out.as.array.len = len;
            out.as.array.cap = cap;
            break;
        }
        case VAL_STRUCT: {
            size_t fc = v->as.strct.field_count;
            out.as.strct.name = lat_strdup(v->as.strct.name);
            out.as.strct.field_names = lat_alloc(fc * sizeof(char *));
            out.as.strct.field_values = lat_alloc(fc * sizeof(LatValue));
            for (size_t i = 0; i < fc; i++) {
                out.as.strct.field_names[i] = lat_strdup(v->as.strct.field_names[i]);
                out.as.strct.field_values[i] = value_deep_clone(&v->as.strct.field_values[i]);
            }
            out.as.strct.field_count = fc;
            break;
        }
        case VAL_CLOSURE: {
            size_t pc = v->as.closure.param_count;
            out.as.closure.param_names = lat_alloc(pc * sizeof(char *));
            for (size_t i = 0; i < pc; i++) {
                out.as.closure.param_names[i] = lat_strdup(v->as.closure.param_names[i]);
            }
            out.as.closure.param_count = pc;
            out.as.closure.body = v->as.closure.body;  /* borrowed */
            out.as.closure.captured_env = env_clone(v->as.closure.captured_env);
            break;
        }
        case VAL_UNIT: break;
        case VAL_RANGE:
            out.as.range.start = v->as.range.start;
            out.as.range.end = v->as.range.end;
            break;
        case VAL_MAP: {
            LatMap *src = v->as.map.map;
            if (g_arena) {
                /* Arena mode: build map internals through lat_alloc/lat_calloc
                 * so everything goes into the arena. No rehashing possible. */
                LatMap *dst = lat_alloc(sizeof(LatMap));
                dst->value_size = src->value_size;
                dst->cap = src->cap;
                dst->count = src->live;  /* only OCCUPIED entries are copied */
                dst->live = src->live;
                dst->entries = lat_calloc(src->cap, sizeof(LatMapEntry));
                for (size_t i = 0; i < src->cap; i++) {
                    if (src->entries[i].state == MAP_OCCUPIED) {
                        dst->entries[i].state = MAP_OCCUPIED;
                        dst->entries[i].key = lat_strdup(src->entries[i].key);
                        LatValue *sv = (LatValue *)src->entries[i].value;
                        LatValue cloned = value_deep_clone(sv);
                        LatValue *dv = lat_alloc(sizeof(LatValue));
                        *dv = cloned;
                        dst->entries[i].value = dv;
                    }
                }
                out.as.map.map = dst;
            } else {
                /* Normal path: lat_map_new + lat_map_set */
                out.as.map.map = lat_alloc(sizeof(LatMap));
                *out.as.map.map = lat_map_new(sizeof(LatValue));
                for (size_t i = 0; i < src->cap; i++) {
                    if (src->entries[i].state == MAP_OCCUPIED) {
                        LatValue *sv = (LatValue *)src->entries[i].value;
                        LatValue cloned = value_deep_clone(sv);
                        lat_map_set(out.as.map.map, src->entries[i].key, &cloned);
                    }
                }
            }
            break;
        }
    }
    return out;
}

/* ── Freeze ── */

static void set_phase_recursive(LatValue *v, PhaseTag phase) {
    v->phase = phase;
    if (v->type == VAL_ARRAY) {
        for (size_t i = 0; i < v->as.array.len; i++) {
            set_phase_recursive(&v->as.array.elems[i], phase);
        }
    } else if (v->type == VAL_STRUCT) {
        for (size_t i = 0; i < v->as.strct.field_count; i++) {
            set_phase_recursive(&v->as.strct.field_values[i], phase);
        }
    } else if (v->type == VAL_MAP) {
        for (size_t i = 0; i < v->as.map.map->cap; i++) {
            if (v->as.map.map->entries[i].state == MAP_OCCUPIED) {
                LatValue *mv = (LatValue *)v->as.map.map->entries[i].value;
                set_phase_recursive(mv, phase);
            }
        }
    }
}

LatValue value_freeze(LatValue v) {
    set_phase_recursive(&v, VTAG_CRYSTAL);
    return v;
}

/* ── Thaw ── */

LatValue value_thaw(const LatValue *v) {
    LatValue cloned = value_deep_clone(v);
    set_phase_recursive(&cloned, VTAG_FLUID);
    return cloned;
}

/* ── Display ── */

void value_print(const LatValue *v, FILE *out) {
    char *s = value_display(v);
    fprintf(out, "%s", s);
    free(s);
}

char *value_display(const LatValue *v) {
    char *buf = NULL;
    switch (v->type) {
        case VAL_INT:
            (void)asprintf(&buf, "%lld", (long long)v->as.int_val);
            break;
        case VAL_FLOAT: {
            (void)asprintf(&buf, "%g", v->as.float_val);
            break;
        }
        case VAL_BOOL:
            buf = strdup(v->as.bool_val ? "true" : "false");
            break;
        case VAL_STR:
            buf = strdup(v->as.str_val);
            break;
        case VAL_ARRAY: {
            size_t cap = 64;
            buf = malloc(cap);
            size_t pos = 0;
            buf[pos++] = '[';
            for (size_t i = 0; i < v->as.array.len; i++) {
                if (i > 0) { buf[pos++] = ','; buf[pos++] = ' '; }
                char *elem = value_display(&v->as.array.elems[i]);
                size_t elen = strlen(elem);
                while (pos + elen + 4 > cap) { cap *= 2; buf = realloc(buf, cap); }
                memcpy(buf + pos, elem, elen);
                pos += elen;
                free(elem);
            }
            buf[pos++] = ']';
            buf[pos] = '\0';
            break;
        }
        case VAL_STRUCT: {
            size_t cap = 64;
            buf = malloc(cap);
            size_t pos = 0;
            size_t nlen = strlen(v->as.strct.name);
            while (pos + nlen + 8 > cap) { cap *= 2; buf = realloc(buf, cap); }
            memcpy(buf + pos, v->as.strct.name, nlen);
            pos += nlen;
            buf[pos++] = ' ';
            buf[pos++] = '{';
            buf[pos++] = ' ';
            for (size_t i = 0; i < v->as.strct.field_count; i++) {
                if (i > 0) { buf[pos++] = ','; buf[pos++] = ' '; }
                const char *fname = v->as.strct.field_names[i];
                size_t flen = strlen(fname);
                char *fval = value_display(&v->as.strct.field_values[i]);
                size_t vlen = strlen(fval);
                while (pos + flen + vlen + 8 > cap) { cap *= 2; buf = realloc(buf, cap); }
                memcpy(buf + pos, fname, flen);
                pos += flen;
                buf[pos++] = ':';
                buf[pos++] = ' ';
                memcpy(buf + pos, fval, vlen);
                pos += vlen;
                free(fval);
            }
            buf[pos++] = ' ';
            buf[pos++] = '}';
            buf[pos] = '\0';
            break;
        }
        case VAL_CLOSURE: {
            size_t cap = 64;
            buf = malloc(cap);
            size_t pos = 0;
            const char *prefix = "<closure|";
            size_t plen = strlen(prefix);
            memcpy(buf, prefix, plen);
            pos = plen;
            for (size_t i = 0; i < v->as.closure.param_count; i++) {
                if (i > 0) { buf[pos++] = ','; buf[pos++] = ' '; }
                const char *pn = v->as.closure.param_names[i];
                size_t nl = strlen(pn);
                while (pos + nl + 4 > cap) { cap *= 2; buf = realloc(buf, cap); }
                memcpy(buf + pos, pn, nl);
                pos += nl;
            }
            buf[pos++] = '|';
            buf[pos++] = '>';
            buf[pos] = '\0';
            break;
        }
        case VAL_UNIT:
            buf = strdup("()");
            break;
        case VAL_RANGE:
            (void)asprintf(&buf, "%lld..%lld", (long long)v->as.range.start, (long long)v->as.range.end);
            break;
        case VAL_MAP: {
            size_t cap2 = 64;
            buf = malloc(cap2);
            size_t pos2 = 0;
            buf[pos2++] = '{';
            bool first = true;
            for (size_t i = 0; i < v->as.map.map->cap; i++) {
                if (v->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                if (!first) { buf[pos2++] = ','; buf[pos2++] = ' '; }
                first = false;
                const char *key = v->as.map.map->entries[i].key;
                LatValue *mval = (LatValue *)v->as.map.map->entries[i].value;
                char *vstr = value_display(mval);
                size_t klen = strlen(key);
                size_t vlen = strlen(vstr);
                while (pos2 + klen + vlen + 8 > cap2) { cap2 *= 2; buf = realloc(buf, cap2); }
                buf[pos2++] = '"';
                memcpy(buf + pos2, key, klen); pos2 += klen;
                buf[pos2++] = '"';
                buf[pos2++] = ':';
                buf[pos2++] = ' ';
                memcpy(buf + pos2, vstr, vlen); pos2 += vlen;
                free(vstr);
            }
            while (pos2 + 2 > cap2) { cap2 *= 2; buf = realloc(buf, cap2); }
            buf[pos2++] = '}';
            buf[pos2] = '\0';
            break;
        }
    }
    return buf;
}

/* ── Type name ── */

const char *value_type_name(const LatValue *v) {
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

/* ── Equality ── */

bool value_eq(const LatValue *a, const LatValue *b) {
    if (a->type != b->type) return false;
    switch (a->type) {
        case VAL_INT:   return a->as.int_val == b->as.int_val;
        case VAL_FLOAT: return a->as.float_val == b->as.float_val;
        case VAL_BOOL:  return a->as.bool_val == b->as.bool_val;
        case VAL_STR:   return strcmp(a->as.str_val, b->as.str_val) == 0;
        case VAL_UNIT:  return true;
        case VAL_RANGE: return a->as.range.start == b->as.range.start &&
                               a->as.range.end == b->as.range.end;
        case VAL_ARRAY:
            if (a->as.array.len != b->as.array.len) return false;
            for (size_t i = 0; i < a->as.array.len; i++) {
                if (!value_eq(&a->as.array.elems[i], &b->as.array.elems[i]))
                    return false;
            }
            return true;
        case VAL_STRUCT:
            if (strcmp(a->as.strct.name, b->as.strct.name) != 0) return false;
            if (a->as.strct.field_count != b->as.strct.field_count) return false;
            for (size_t i = 0; i < a->as.strct.field_count; i++) {
                if (strcmp(a->as.strct.field_names[i], b->as.strct.field_names[i]) != 0)
                    return false;
                if (!value_eq(&a->as.strct.field_values[i], &b->as.strct.field_values[i]))
                    return false;
            }
            return true;
        case VAL_CLOSURE: return false;
        case VAL_MAP: {
            if (lat_map_len(a->as.map.map) != lat_map_len(b->as.map.map)) return false;
            for (size_t i = 0; i < a->as.map.map->cap; i++) {
                if (a->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                const char *key = a->as.map.map->entries[i].key;
                LatValue *av = (LatValue *)a->as.map.map->entries[i].value;
                LatValue *bv = (LatValue *)lat_map_get(b->as.map.map, key);
                if (!bv || !value_eq(av, bv)) return false;
            }
            return true;
        }
    }
    return false;
}

/* ── Free ── */

static void val_dealloc(LatValue *v, void *ptr) {
    if (!ptr) return;
    if (v->region_id != (size_t)-1) return;  /* arena-backed: no-op */
    lat_free(ptr);
}

void value_free(LatValue *v) {
    if (v->region_id != (size_t)-1) {
        memset(v, 0, sizeof(*v));  /* arena owns everything */
        return;
    }
    switch (v->type) {
        case VAL_STR:
            val_dealloc(v, v->as.str_val);
            break;
        case VAL_ARRAY:
            for (size_t i = 0; i < v->as.array.len; i++)
                value_free(&v->as.array.elems[i]);
            val_dealloc(v, v->as.array.elems);
            break;
        case VAL_STRUCT:
            val_dealloc(v, v->as.strct.name);
            for (size_t i = 0; i < v->as.strct.field_count; i++) {
                val_dealloc(v, v->as.strct.field_names[i]);
                value_free(&v->as.strct.field_values[i]);
            }
            val_dealloc(v, v->as.strct.field_names);
            val_dealloc(v, v->as.strct.field_values);
            break;
        case VAL_CLOSURE:
            for (size_t i = 0; i < v->as.closure.param_count; i++)
                val_dealloc(v, v->as.closure.param_names[i]);
            val_dealloc(v, v->as.closure.param_names);
            if (v->as.closure.captured_env)
                env_free(v->as.closure.captured_env);
            break;
        case VAL_MAP:
            if (v->as.map.map) {
                /* Free each stored LatValue before freeing the map */
                for (size_t i = 0; i < v->as.map.map->cap; i++) {
                    if (v->as.map.map->entries[i].state == MAP_OCCUPIED) {
                        LatValue *mv = (LatValue *)v->as.map.map->entries[i].value;
                        value_free(mv);
                    }
                }
                lat_map_free(v->as.map.map);
                val_dealloc(v, v->as.map.map);
            }
            break;
        default:
            break;
    }
    memset(v, 0, sizeof(*v));
}

/* ── Truthiness ── */

bool value_is_truthy(const LatValue *v) {
    switch (v->type) {
        case VAL_BOOL:  return v->as.bool_val;
        case VAL_INT:   return v->as.int_val != 0;
        case VAL_FLOAT: return v->as.float_val != 0.0;
        case VAL_STR:   return v->as.str_val[0] != '\0';
        case VAL_UNIT:  return false;
        case VAL_MAP:   return lat_map_len(v->as.map.map) > 0;
        default:        return true;
    }
}
