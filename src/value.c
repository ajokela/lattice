#include "value.h"
#include "env.h"
#include "memory.h"
#include "channel.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

_Static_assert(sizeof(LatValue) <= LAT_MAP_INLINE_MAX,
               "LatValue must fit in LAT_MAP_INLINE_MAX bytes for inline hashmap storage");

/* ── Heap-tracked allocation wrappers ── */

#ifdef __EMSCRIPTEN__
static DualHeap *g_heap = NULL;
static CrystalRegion *g_arena = NULL;
#else
static _Thread_local DualHeap *g_heap = NULL;
static _Thread_local CrystalRegion *g_arena = NULL;
#endif

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
    LatValue val = { .type = VAL_INT, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    val.as.int_val = v;
    return val;
}

LatValue value_float(double v) {
    LatValue val = { .type = VAL_FLOAT, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    val.as.float_val = v;
    return val;
}

LatValue value_bool(bool v) {
    LatValue val = { .type = VAL_BOOL, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    val.as.bool_val = v;
    return val;
}

LatValue value_string(const char *s) {
    LatValue val = { .type = VAL_STR, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    val.as.str_val = lat_strdup(s);
    return val;
}

LatValue value_string_owned(char *s) {
    LatValue val = { .type = VAL_STR, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    val.as.str_val = s;
    return val;
}

LatValue value_array(LatValue *elems, size_t len) {
    LatValue val = { .type = VAL_ARRAY, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    size_t cap = len < 4 ? 4 : len;
    val.as.array.elems = lat_alloc(cap * sizeof(LatValue));
    if (len > 0) memcpy(val.as.array.elems, elems, len * sizeof(LatValue));
    val.as.array.len = len;
    val.as.array.cap = cap;
    return val;
}

LatValue value_struct(const char *name, char **field_names, LatValue *field_values, size_t count) {
    LatValue val = { .type = VAL_STRUCT, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    val.as.strct.name = lat_strdup(name);
    val.as.strct.field_names = lat_alloc(count * sizeof(char *));
    val.as.strct.field_values = lat_alloc(count * sizeof(LatValue));
    val.as.strct.field_phases = NULL;  /* lazy-allocated on first field freeze */
    for (size_t i = 0; i < count; i++) {
        val.as.strct.field_names[i] = lat_strdup(field_names[i]);
        val.as.strct.field_values[i] = field_values[i];
    }
    val.as.strct.field_count = count;
    return val;
}

LatValue value_struct_vm(const char *name, const char **field_names, LatValue *field_values, size_t count) {
    LatValue val = { .type = VAL_STRUCT, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    val.as.strct.name = lat_strdup(name);
    val.as.strct.field_names = lat_alloc(count * sizeof(char *));
    val.as.strct.field_values = lat_alloc(count * sizeof(LatValue));
    val.as.strct.field_phases = NULL;
    for (size_t i = 0; i < count; i++) {
        val.as.strct.field_names[i] = lat_strdup(field_names[i]);
        val.as.strct.field_values[i] = field_values[i];
    }
    val.as.strct.field_count = count;
    return val;
}

LatValue value_closure(char **param_names, size_t param_count, struct Expr *body, Env *captured,
                       struct Expr **default_values, bool has_variadic) {
    LatValue val = { .type = VAL_CLOSURE, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    val.as.closure.param_names = lat_alloc(param_count * sizeof(char *));
    for (size_t i = 0; i < param_count; i++) {
        val.as.closure.param_names[i] = lat_strdup(param_names[i]);
    }
    val.as.closure.param_count = param_count;
    val.as.closure.body = body;       /* borrowed reference */
    val.as.closure.captured_env = captured;
    val.as.closure.default_values = default_values;  /* borrowed from AST */
    val.as.closure.has_variadic = has_variadic;
    return val;
}

LatValue value_unit(void) {
    return (LatValue){ .type = VAL_UNIT, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
}

LatValue value_nil(void) {
    return (LatValue){ .type = VAL_NIL, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
}

LatValue value_range(int64_t start, int64_t end) {
    LatValue val = { .type = VAL_RANGE, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    val.as.range.start = start;
    val.as.range.end = end;
    return val;
}

LatValue value_map_new(void) {
    LatValue val = { .type = VAL_MAP, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    val.as.map.map = lat_alloc(sizeof(LatMap));
    *val.as.map.map = lat_map_new(sizeof(LatValue));
    val.as.map.key_phases = NULL;  /* lazy-allocated on first key freeze */
    return val;
}

LatValue value_channel(struct LatChannel *ch) {
    LatValue val = { .type = VAL_CHANNEL, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    channel_retain(ch);
    val.as.channel.ch = ch;
    return val;
}

LatValue value_enum(const char *enum_name, const char *variant_name,
                    LatValue *payload, size_t count) {
    LatValue val = { .type = VAL_ENUM, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    val.as.enm.enum_name = lat_strdup(enum_name);
    val.as.enm.variant_name = lat_strdup(variant_name);
    if (count > 0 && payload) {
        val.as.enm.payload = lat_alloc(count * sizeof(LatValue));
        for (size_t i = 0; i < count; i++)
            val.as.enm.payload[i] = value_deep_clone(&payload[i]);
        val.as.enm.payload_count = count;
    } else {
        val.as.enm.payload = NULL;
        val.as.enm.payload_count = 0;
    }
    return val;
}

LatValue value_set_new(void) {
    LatValue val = { .type = VAL_SET, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    val.as.set.map = lat_alloc(sizeof(LatMap));
    *val.as.set.map = lat_map_new(sizeof(LatValue));
    return val;
}

LatValue value_tuple(LatValue *elems, size_t len) {
    LatValue val = { .type = VAL_TUPLE, .phase = VTAG_CRYSTAL, .region_id = (size_t)-1 };
    val.as.tuple.elems = lat_alloc(len * sizeof(LatValue));
    for (size_t i = 0; i < len; i++) {
        val.as.tuple.elems[i] = value_deep_clone(&elems[i]);
    }
    val.as.tuple.len = len;
    return val;
}

LatValue value_buffer(const uint8_t *data, size_t len) {
    LatValue val = { .type = VAL_BUFFER, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    size_t cap = len < 8 ? 8 : len;
    val.as.buffer.data = lat_alloc(cap);
    if (len > 0 && data) memcpy(val.as.buffer.data, data, len);
    val.as.buffer.len = len;
    val.as.buffer.cap = cap;
    return val;
}

LatValue value_buffer_alloc(size_t size) {
    LatValue val = { .type = VAL_BUFFER, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    size_t cap = size < 8 ? 8 : size;
    val.as.buffer.data = lat_calloc(cap, 1);
    val.as.buffer.len = size;
    val.as.buffer.cap = cap;
    return val;
}

LatValue value_ref(LatValue inner) {
    LatValue val = { .type = VAL_REF, .phase = VTAG_UNPHASED, .region_id = (size_t)-1 };
    LatRef *r = malloc(sizeof(LatRef));
    r->value = value_deep_clone(&inner);
    r->refcount = 1;
    val.as.ref.ref = r;
    return val;
}

void ref_retain(LatRef *r) {
    if (r) r->refcount++;
}

void ref_release(LatRef *r) {
    if (!r) return;
    if (--r->refcount == 0) {
        value_free(&r->value);
        free(r);
    }
}

/* ── Phase helpers ── */

bool value_is_fluid(const LatValue *v) { return v->phase == VTAG_FLUID; }
bool value_is_crystal(const LatValue *v) { return v->phase == VTAG_CRYSTAL; }

/* ── Deep clone ── */

LatValue value_deep_clone(const LatValue *v) {
    LatValue out = { .type = v->type, .phase = v->phase, .region_id = (size_t)-1 };

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
            if (v->as.strct.field_phases) {
                out.as.strct.field_phases = lat_alloc(fc * sizeof(PhaseTag));
                memcpy(out.as.strct.field_phases, v->as.strct.field_phases, fc * sizeof(PhaseTag));
            } else {
                out.as.strct.field_phases = NULL;
            }
            break;
        }
        case VAL_CLOSURE: {
            size_t pc = v->as.closure.param_count;
            if (v->as.closure.param_names) {
                out.as.closure.param_names = lat_alloc(pc * sizeof(char *));
                for (size_t i = 0; i < pc; i++) {
                    out.as.closure.param_names[i] = lat_strdup(v->as.closure.param_names[i]);
                }
            } else {
                out.as.closure.param_names = NULL;
            }
            out.as.closure.param_count = pc;
            out.as.closure.body = v->as.closure.body;  /* borrowed */
            /* Compiled bytecode closures store ObjUpvalue** in captured_env (not Env*).
             * Shallow-copy the pointer; the VM manages upvalue lifetime. */
            if (v->as.closure.body == NULL && v->as.closure.native_fn != NULL) {
                out.as.closure.captured_env = v->as.closure.captured_env;
            } else {
                /* Tree-walk closures: share the env (refcounted) so mutations
                 * inside the closure persist across calls.
                 * Exception: when an arena is active (during freeze), we must
                 * clone the env into the arena so GC can safely skip crystal
                 * values without traversing their fluid-heap env pointers. */
                if (value_get_arena()) {
                    out.as.closure.captured_env = env_clone(v->as.closure.captured_env);
                } else {
                    out.as.closure.captured_env = v->as.closure.captured_env;
                    env_retain(v->as.closure.captured_env);
                }
            }
            out.as.closure.default_values = v->as.closure.default_values;  /* borrowed */
            out.as.closure.has_variadic = v->as.closure.has_variadic;
            out.as.closure.native_fn = v->as.closure.native_fn;  /* shared, not owned */
            /* Compiled bytecode closures store upvalue count in region_id */
            if (v->as.closure.body == NULL && v->as.closure.native_fn != NULL)
                out.region_id = v->region_id;
            break;
        }
        case VAL_UNIT: break;
        case VAL_NIL: break;
        case VAL_RANGE:
            out.as.range.start = v->as.range.start;
            out.as.range.end = v->as.range.end;
            break;
        case VAL_CHANNEL:
            channel_retain(v->as.channel.ch);
            out.as.channel.ch = v->as.channel.ch;
            break;
        case VAL_ENUM: {
            out.as.enm.enum_name = lat_strdup(v->as.enm.enum_name);
            out.as.enm.variant_name = lat_strdup(v->as.enm.variant_name);
            if (v->as.enm.payload_count > 0) {
                out.as.enm.payload = lat_alloc(v->as.enm.payload_count * sizeof(LatValue));
                for (size_t i = 0; i < v->as.enm.payload_count; i++)
                    out.as.enm.payload[i] = value_deep_clone(&v->as.enm.payload[i]);
                out.as.enm.payload_count = v->as.enm.payload_count;
            } else {
                out.as.enm.payload = NULL;
                out.as.enm.payload_count = 0;
            }
            break;
        }
        case VAL_MAP: {
            LatMap *src = v->as.map.map;
            if (g_arena) {
                /* Arena mode: build map internals through lat_alloc/lat_calloc
                 * so everything goes into the arena. No rehashing possible. */
                LatMap *dst = lat_alloc(sizeof(LatMap));
                dst->value_size = src->value_size;
                dst->cap = src->cap;
                dst->count = src->count;  /* preserve tombstone count for probe chains */
                dst->live = src->live;
                dst->entries = lat_calloc(src->cap, sizeof(LatMapEntry));
                for (size_t i = 0; i < src->cap; i++) {
                    dst->entries[i].value = dst->entries[i]._ibuf;
                    if (src->entries[i].state == MAP_OCCUPIED) {
                        dst->entries[i].state = MAP_OCCUPIED;
                        dst->entries[i].key = lat_strdup(src->entries[i].key);
                        LatValue *sv = (LatValue *)src->entries[i].value;
                        LatValue cloned = value_deep_clone(sv);
                        *(LatValue *)dst->entries[i].value = cloned;
                    } else if (src->entries[i].state == MAP_TOMBSTONE) {
                        dst->entries[i].state = MAP_TOMBSTONE;
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
            /* Clone per-key phases if present */
            if (v->as.map.key_phases) {
                LatMap *ksrc = v->as.map.key_phases;
                out.as.map.key_phases = lat_alloc(sizeof(LatMap));
                *out.as.map.key_phases = lat_map_new(sizeof(PhaseTag));
                for (size_t i = 0; i < ksrc->cap; i++) {
                    if (ksrc->entries[i].state == MAP_OCCUPIED) {
                        lat_map_set(out.as.map.key_phases, ksrc->entries[i].key,
                                    ksrc->entries[i].value);
                    }
                }
            } else {
                out.as.map.key_phases = NULL;
            }
            break;
        }
        case VAL_SET: {
            LatMap *src = v->as.set.map;
            if (g_arena) {
                LatMap *dst = lat_alloc(sizeof(LatMap));
                dst->value_size = src->value_size;
                dst->cap = src->cap;
                dst->count = src->count;  /* preserve tombstone count for probe chains */
                dst->live = src->live;
                dst->entries = lat_calloc(src->cap, sizeof(LatMapEntry));
                for (size_t i = 0; i < src->cap; i++) {
                    dst->entries[i].value = dst->entries[i]._ibuf;
                    if (src->entries[i].state == MAP_OCCUPIED) {
                        dst->entries[i].state = MAP_OCCUPIED;
                        dst->entries[i].key = lat_strdup(src->entries[i].key);
                        LatValue *sv = (LatValue *)src->entries[i].value;
                        LatValue cloned = value_deep_clone(sv);
                        *(LatValue *)dst->entries[i].value = cloned;
                    } else if (src->entries[i].state == MAP_TOMBSTONE) {
                        dst->entries[i].state = MAP_TOMBSTONE;
                    }
                }
                out.as.set.map = dst;
            } else {
                out.as.set.map = lat_alloc(sizeof(LatMap));
                *out.as.set.map = lat_map_new(sizeof(LatValue));
                for (size_t i = 0; i < src->cap; i++) {
                    if (src->entries[i].state == MAP_OCCUPIED) {
                        LatValue *sv = (LatValue *)src->entries[i].value;
                        LatValue cloned = value_deep_clone(sv);
                        lat_map_set(out.as.set.map, src->entries[i].key, &cloned);
                    }
                }
            }
            break;
        }
        case VAL_TUPLE: {
            out.as.tuple.elems = lat_alloc(v->as.tuple.len * sizeof(LatValue));
            for (size_t i = 0; i < v->as.tuple.len; i++) {
                out.as.tuple.elems[i] = value_deep_clone(&v->as.tuple.elems[i]);
            }
            out.as.tuple.len = v->as.tuple.len;
            break;
        }
        case VAL_BUFFER: {
            size_t cap = v->as.buffer.cap;
            out.as.buffer.data = lat_alloc(cap);
            memcpy(out.as.buffer.data, v->as.buffer.data, v->as.buffer.len);
            out.as.buffer.len = v->as.buffer.len;
            out.as.buffer.cap = cap;
            break;
        }
        case VAL_REF:
            ref_retain(v->as.ref.ref);
            out.as.ref.ref = v->as.ref.ref;
            break;
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
        /* Update field-level phases */
        if (v->as.strct.field_phases) {
            if (phase == VTAG_CRYSTAL) {
                for (size_t i = 0; i < v->as.strct.field_count; i++)
                    v->as.strct.field_phases[i] = VTAG_CRYSTAL;
            } else {
                /* Thawing: clear per-field phases */
                lat_free(v->as.strct.field_phases);
                v->as.strct.field_phases = NULL;
            }
        }
    } else if (v->type == VAL_MAP) {
        for (size_t i = 0; i < v->as.map.map->cap; i++) {
            if (v->as.map.map->entries[i].state == MAP_OCCUPIED) {
                LatValue *mv = (LatValue *)v->as.map.map->entries[i].value;
                set_phase_recursive(mv, phase);
            }
        }
    } else if (v->type == VAL_ENUM) {
        for (size_t i = 0; i < v->as.enm.payload_count; i++)
            set_phase_recursive(&v->as.enm.payload[i], phase);
    } else if (v->type == VAL_SET) {
        for (size_t i = 0; i < v->as.set.map->cap; i++) {
            if (v->as.set.map->entries[i].state == MAP_OCCUPIED) {
                LatValue *sv = (LatValue *)v->as.set.map->entries[i].value;
                set_phase_recursive(sv, phase);
            }
        }
    } else if (v->type == VAL_TUPLE) {
        for (size_t i = 0; i < v->as.tuple.len; i++) {
            set_phase_recursive(&v->as.tuple.elems[i], phase);
        }
    }
    /* VAL_BUFFER: just set phase tag (no nested values) */
    else if (v->type == VAL_REF) {
        set_phase_recursive(&v->as.ref.ref->value, phase);
    }
}

LatValue value_freeze(LatValue v) {
    set_phase_recursive(&v, VTAG_CRYSTAL);
    return v;
}

/* ── Thaw ── */

LatValue value_thaw(const LatValue *v) {
    if (v->type == VAL_REF) {
        /* Thaw breaks sharing: new LatRef with deep-cloned inner */
        LatValue inner_clone = value_deep_clone(&v->as.ref.ref->value);
        set_phase_recursive(&inner_clone, VTAG_FLUID);
        LatValue result = value_ref(inner_clone);
        value_free(&inner_clone);
        result.phase = VTAG_FLUID;
        return result;
    }
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
            if (v->as.closure.param_names) {
                for (size_t i = 0; i < v->as.closure.param_count; i++) {
                    if (i > 0) { buf[pos++] = ','; buf[pos++] = ' '; }
                    const char *pn = v->as.closure.param_names[i];
                    size_t nl = strlen(pn);
                    while (pos + nl + 4 > cap) { cap *= 2; buf = realloc(buf, cap); }
                    memcpy(buf + pos, pn, nl);
                    pos += nl;
                }
            }
            buf[pos++] = '|';
            buf[pos++] = '>';
            buf[pos] = '\0';
            break;
        }
        case VAL_UNIT:
            buf = strdup("()");
            break;
        case VAL_NIL:
            buf = strdup("nil");
            break;
        case VAL_RANGE:
            (void)asprintf(&buf, "%lld..%lld", (long long)v->as.range.start, (long long)v->as.range.end);
            break;
        case VAL_CHANNEL:
            buf = strdup("<Channel>");
            break;
        case VAL_ENUM: {
            if (v->as.enm.payload_count == 0) {
                (void)asprintf(&buf, "%s::%s", v->as.enm.enum_name, v->as.enm.variant_name);
            } else {
                size_t ecap = 64;
                buf = malloc(ecap);
                size_t epos = 0;
                size_t enlen = strlen(v->as.enm.enum_name);
                size_t vnlen = strlen(v->as.enm.variant_name);
                while (epos + enlen + vnlen + 8 > ecap) { ecap *= 2; buf = realloc(buf, ecap); }
                memcpy(buf + epos, v->as.enm.enum_name, enlen); epos += enlen;
                buf[epos++] = ':'; buf[epos++] = ':';
                memcpy(buf + epos, v->as.enm.variant_name, vnlen); epos += vnlen;
                buf[epos++] = '(';
                for (size_t i = 0; i < v->as.enm.payload_count; i++) {
                    if (i > 0) { buf[epos++] = ','; buf[epos++] = ' '; }
                    char *elem = value_display(&v->as.enm.payload[i]);
                    size_t elen = strlen(elem);
                    while (epos + elen + 4 > ecap) { ecap *= 2; buf = realloc(buf, ecap); }
                    memcpy(buf + epos, elem, elen); epos += elen;
                    free(elem);
                }
                buf[epos++] = ')';
                buf[epos] = '\0';
            }
            break;
        }
        case VAL_SET: {
            size_t cap2 = 64;
            buf = malloc(cap2);
            size_t pos2 = 0;
            memcpy(buf, "Set{", 4); pos2 = 4;
            bool sfirst = true;
            for (size_t i = 0; i < v->as.set.map->cap; i++) {
                if (v->as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                if (!sfirst) { while (pos2 + 3 > cap2) { cap2 *= 2; buf = realloc(buf, cap2); } buf[pos2++] = ','; buf[pos2++] = ' '; }
                sfirst = false;
                LatValue *sv = (LatValue *)v->as.set.map->entries[i].value;
                char *elem = value_display(sv);
                size_t elen = strlen(elem);
                while (pos2 + elen + 4 > cap2) { cap2 *= 2; buf = realloc(buf, cap2); }
                memcpy(buf + pos2, elem, elen); pos2 += elen;
                free(elem);
            }
            while (pos2 + 2 > cap2) { cap2 *= 2; buf = realloc(buf, cap2); }
            buf[pos2++] = '}';
            buf[pos2] = '\0';
            break;
        }
        case VAL_TUPLE: {
            size_t tcap = 64;
            buf = malloc(tcap);
            size_t tpos = 0;
            buf[tpos++] = '(';
            for (size_t i = 0; i < v->as.tuple.len; i++) {
                if (i > 0) {
                    while (tpos + 3 > tcap) { tcap *= 2; buf = realloc(buf, tcap); }
                    buf[tpos++] = ','; buf[tpos++] = ' ';
                }
                char *elem = value_display(&v->as.tuple.elems[i]);
                size_t elen = strlen(elem);
                while (tpos + elen + 4 > tcap) { tcap *= 2; buf = realloc(buf, tcap); }
                memcpy(buf + tpos, elem, elen); tpos += elen;
                free(elem);
            }
            if (v->as.tuple.len == 1) {
                while (tpos + 3 > tcap) { tcap *= 2; buf = realloc(buf, tcap); }
                buf[tpos++] = ',';
            }
            while (tpos + 2 > tcap) { tcap *= 2; buf = realloc(buf, tcap); }
            buf[tpos++] = ')';
            buf[tpos] = '\0';
            break;
        }
        case VAL_BUFFER:
            (void)asprintf(&buf, "Buffer<%zu bytes>", v->as.buffer.len);
            break;
        case VAL_REF:
            (void)asprintf(&buf, "Ref<%s>", value_type_name(&v->as.ref.ref->value));
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

char *value_repr(const LatValue *v) {
    if (v->type == VAL_STR) {
        /* Wrap strings in double quotes */
        size_t slen = strlen(v->as.str_val);
        char *buf = malloc(slen + 3);
        buf[0] = '"';
        memcpy(buf + 1, v->as.str_val, slen);
        buf[slen + 1] = '"';
        buf[slen + 2] = '\0';
        return buf;
    }
    if (v->type == VAL_BUFFER) {
        /* Buffer<N bytes: XX XX XX ...> (show first 8 bytes hex) */
        size_t show = v->as.buffer.len < 8 ? v->as.buffer.len : 8;
        size_t cap = 64 + show * 3;
        char *buf = malloc(cap);
        int pos = snprintf(buf, cap, "Buffer<%zu bytes:", v->as.buffer.len);
        for (size_t i = 0; i < show; i++)
            pos += snprintf(buf + pos, cap - (size_t)pos, " %02x", v->as.buffer.data[i]);
        if (v->as.buffer.len > 8)
            pos += snprintf(buf + pos, cap - (size_t)pos, " ...");
        snprintf(buf + pos, cap - (size_t)pos, ">");
        return buf;
    }
    /* Everything else uses standard display */
    return value_display(v);
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
        case VAL_NIL:     return "Nil";
        case VAL_RANGE:   return "Range";
        case VAL_MAP:     return "Map";
        case VAL_CHANNEL: return "Channel";
        case VAL_ENUM:    return "Enum";
        case VAL_SET:     return "Set";
        case VAL_TUPLE:   return "Tuple";
        case VAL_BUFFER:  return "Buffer";
        case VAL_REF:     return "Ref";
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
        case VAL_NIL:   return true;
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
        case VAL_CHANNEL:
            return a->as.channel.ch == b->as.channel.ch;
        case VAL_ENUM:
            if (strcmp(a->as.enm.enum_name, b->as.enm.enum_name) != 0) return false;
            if (strcmp(a->as.enm.variant_name, b->as.enm.variant_name) != 0) return false;
            if (a->as.enm.payload_count != b->as.enm.payload_count) return false;
            for (size_t i = 0; i < a->as.enm.payload_count; i++) {
                if (!value_eq(&a->as.enm.payload[i], &b->as.enm.payload[i])) return false;
            }
            return true;
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
        case VAL_SET: {
            if (lat_map_len(a->as.set.map) != lat_map_len(b->as.set.map)) return false;
            for (size_t i = 0; i < a->as.set.map->cap; i++) {
                if (a->as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                const char *key = a->as.set.map->entries[i].key;
                if (!lat_map_contains(b->as.set.map, key)) return false;
            }
            return true;
        }
        case VAL_TUPLE:
            if (a->as.tuple.len != b->as.tuple.len) return false;
            for (size_t i = 0; i < a->as.tuple.len; i++) {
                if (!value_eq(&a->as.tuple.elems[i], &b->as.tuple.elems[i]))
                    return false;
            }
            return true;
        case VAL_BUFFER:
            if (a->as.buffer.len != b->as.buffer.len) return false;
            return memcmp(a->as.buffer.data, b->as.buffer.data, a->as.buffer.len) == 0;
        case VAL_REF:
            return a->as.ref.ref == b->as.ref.ref;
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
            val_dealloc(v, v->as.strct.field_phases);
            break;
        case VAL_CLOSURE:
            if (v->as.closure.param_names) {
                for (size_t i = 0; i < v->as.closure.param_count; i++)
                    val_dealloc(v, v->as.closure.param_names[i]);
                val_dealloc(v, v->as.closure.param_names);
            }
            /* Don't free compiled bytecode closures' env — they store ObjUpvalue**,
             * not Env*. The VM manages upvalue lifetime. */
            if (v->as.closure.captured_env &&
                !(v->as.closure.body == NULL && v->as.closure.native_fn != NULL))
                env_release(v->as.closure.captured_env);
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
            if (v->as.map.key_phases) {
                lat_map_free(v->as.map.key_phases);
                val_dealloc(v, v->as.map.key_phases);
            }
            break;
        case VAL_CHANNEL:
            if (v->as.channel.ch)
                channel_release(v->as.channel.ch);
            break;
        case VAL_ENUM:
            val_dealloc(v, v->as.enm.enum_name);
            val_dealloc(v, v->as.enm.variant_name);
            if (v->as.enm.payload) {
                for (size_t i = 0; i < v->as.enm.payload_count; i++)
                    value_free(&v->as.enm.payload[i]);
                val_dealloc(v, v->as.enm.payload);
            }
            break;
        case VAL_SET:
            if (v->as.set.map) {
                for (size_t i = 0; i < v->as.set.map->cap; i++) {
                    if (v->as.set.map->entries[i].state == MAP_OCCUPIED) {
                        LatValue *sv = (LatValue *)v->as.set.map->entries[i].value;
                        value_free(sv);
                    }
                }
                lat_map_free(v->as.set.map);
                val_dealloc(v, v->as.set.map);
            }
            break;
        case VAL_TUPLE:
            for (size_t i = 0; i < v->as.tuple.len; i++)
                value_free(&v->as.tuple.elems[i]);
            val_dealloc(v, v->as.tuple.elems);
            break;
        case VAL_BUFFER:
            val_dealloc(v, v->as.buffer.data);
            break;
        case VAL_REF:
            ref_release(v->as.ref.ref);
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
        case VAL_NIL:   return false;
        case VAL_MAP:     return lat_map_len(v->as.map.map) > 0;
        case VAL_SET:     return lat_map_len(v->as.set.map) > 0;
        case VAL_TUPLE:   return v->as.tuple.len > 0;
        case VAL_CHANNEL: return true;
        case VAL_BUFFER:  return v->as.buffer.len > 0;
        case VAL_REF:     return true;
        default:          return true;
    }
}
