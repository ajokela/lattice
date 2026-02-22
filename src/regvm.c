#include "regvm.h"
#include "regopcode.h"
#include "value.h"
#include "env.h"
#include "vm.h"   /* For ObjUpvalue */
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "builtins.h"
#include "memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#ifndef __EMSCRIPTEN__
#include <pthread.h>
#endif

/* Native function marker — same sentinel as stack VM (defined in vm.c) */
#define VM_NATIVE_MARKER ((struct Expr **)(uintptr_t)0x1)

/* ── RegChunk implementation ── */

RegChunk *regchunk_new(void) {
    RegChunk *c = calloc(1, sizeof(RegChunk));
    c->code_cap = 128;
    c->code = malloc(c->code_cap * sizeof(RegInstr));
    c->const_cap = 32;
    c->constants = malloc(c->const_cap * sizeof(LatValue));
    c->lines_cap = 128;
    c->lines = malloc(c->lines_cap * sizeof(int));
    c->local_name_cap = 0;
    c->local_names = NULL;
    c->name = NULL;
    return c;
}

void regchunk_free(RegChunk *c) {
    if (!c) return;
    /* Free sub-chunks stored in closure constants */
    for (size_t i = 0; i < c->const_len; i++) {
        LatValue *v = &c->constants[i];
        if (v->type == VAL_CLOSURE && v->as.closure.body == NULL &&
            v->as.closure.native_fn != NULL &&
            v->as.closure.default_values != VM_NATIVE_MARKER) {
            regchunk_free((RegChunk *)v->as.closure.native_fn);
            v->as.closure.native_fn = NULL;
        } else {
            value_free(v);
        }
    }
    free(c->constants);
    free(c->code);
    free(c->lines);
    if (c->local_names) {
        for (size_t i = 0; i < c->local_name_cap; i++)
            free(c->local_names[i]);
        free(c->local_names);
    }
    free(c->name);
    free(c);
}

size_t regchunk_write(RegChunk *c, RegInstr instr, int line) {
    if (c->code_len >= c->code_cap) {
        c->code_cap *= 2;
        c->code = realloc(c->code, c->code_cap * sizeof(RegInstr));
    }
    if (c->lines_len >= c->lines_cap) {
        c->lines_cap *= 2;
        c->lines = realloc(c->lines, c->lines_cap * sizeof(int));
    }
    size_t offset = c->code_len;
    c->code[c->code_len++] = instr;
    c->lines[c->lines_len++] = line;
    return offset;
}

size_t regchunk_add_constant(RegChunk *c, LatValue val) {
    if (c->const_len >= c->const_cap) {
        c->const_cap *= 2;
        c->constants = realloc(c->constants, c->const_cap * sizeof(LatValue));
    }
    c->constants[c->const_len] = val;
    return c->const_len++;
}

void regchunk_set_local_name(RegChunk *c, size_t reg, const char *name) {
    if (reg >= c->local_name_cap) {
        size_t old_cap = c->local_name_cap;
        c->local_name_cap = reg + 16;
        c->local_names = realloc(c->local_names, c->local_name_cap * sizeof(char *));
        for (size_t i = old_cap; i < c->local_name_cap; i++)
            c->local_names[i] = NULL;
    }
    free(c->local_names[reg]);
    c->local_names[reg] = name ? strdup(name) : NULL;
}

/* ── Register VM ── */

void regvm_init(RegVM *vm) {
    memset(vm, 0, sizeof(RegVM));
    vm->env = env_new();
    vm->struct_meta = env_new();
    vm->fn_chunk_cap = 16;
    vm->fn_chunks = malloc(vm->fn_chunk_cap * sizeof(RegChunk *));
    vm->module_cache = NULL;
    vm->script_dir = NULL;
    vm->ephemeral = bump_arena_new();
    /* Initialize register stack to nil */
    for (size_t i = 0; i < REGVM_REG_MAX * REGVM_FRAMES_MAX; i++) {
        vm->reg_stack[i] = value_nil();
    }
}

void regvm_free(RegVM *vm) {
    env_free(vm->env);
    env_free(vm->struct_meta);
    for (size_t i = 0; i < vm->fn_chunk_count; i++)
        regchunk_free(vm->fn_chunks[i]);
    free(vm->fn_chunks);
    free(vm->error);
    free(vm->script_dir);
    if (vm->module_cache) {
        for (size_t i = 0; i < vm->module_cache->cap; i++) {
            if (vm->module_cache->entries[i].state == MAP_OCCUPIED) {
                LatValue *v = (LatValue *)vm->module_cache->entries[i].value;
                value_free(v);
            }
        }
        lat_map_free(vm->module_cache);
        free(vm->module_cache);
        vm->module_cache = NULL;
    }
    if (vm->ephemeral) {
        bump_arena_free(vm->ephemeral);
        vm->ephemeral = NULL;
    }
    /* Free register values */
    for (size_t i = 0; i < vm->reg_stack_top; i++)
        value_free_inline(&vm->reg_stack[i]);
    /* Free open upvalues */
    ObjUpvalue *uv = vm->open_upvalues;
    while (uv) {
        ObjUpvalue *next = uv->next;
        value_free(&uv->closed);
        free(uv);
        uv = next;
    }
}

void regvm_track_chunk(RegVM *vm, RegChunk *ch) {
    if (vm->fn_chunk_count >= vm->fn_chunk_cap) {
        vm->fn_chunk_cap *= 2;
        vm->fn_chunks = realloc(vm->fn_chunks, vm->fn_chunk_cap * sizeof(RegChunk *));
    }
    vm->fn_chunks[vm->fn_chunk_count++] = ch;
}

/* ── Value cloning (same fast-path as stack VM) ── */

static inline LatValue rvm_clone(const LatValue *src) {
    switch (src->type) {
        case VAL_INT: case VAL_FLOAT: case VAL_BOOL:
        case VAL_UNIT: case VAL_NIL: case VAL_RANGE: {
            LatValue v = *src;
            v.region_id = REGION_NONE;
            return v;
        }
        case VAL_STR: {
            LatValue v = *src;
            v.as.str_val = strdup(src->as.str_val);
            v.region_id = REGION_NONE;
            return v;
        }
        case VAL_CLOSURE: {
            if (src->as.closure.body == NULL && src->as.closure.native_fn != NULL &&
                src->as.closure.default_values != VM_NATIVE_MARKER) {
                LatValue v = *src;
                if (src->as.closure.param_names) {
                    v.as.closure.param_names = malloc(src->as.closure.param_count * sizeof(char *));
                    for (size_t i = 0; i < src->as.closure.param_count; i++)
                        v.as.closure.param_names[i] = strdup(src->as.closure.param_names[i]);
                }
                return v;
            }
            return value_deep_clone(src);
        }
        case VAL_ARRAY: {
            LatValue v = *src;
            size_t len = src->as.array.len;
            size_t cap = src->as.array.cap > 0 ? src->as.array.cap : (len > 0 ? len : 1);
            v.as.array.elems = malloc(cap * sizeof(LatValue));
            v.as.array.cap = cap;
            for (size_t i = 0; i < len; i++)
                v.as.array.elems[i] = rvm_clone(&src->as.array.elems[i]);
            v.region_id = REGION_NONE;
            return v;
        }
        default:
            return value_deep_clone(src);
    }
}

/* ── Runtime error ── */

/* Basic error (no exception handler check, for use outside dispatch loop) */
static RegVMResult rvm_error(RegVM *vm, const char *fmt, ...) {
    char *msg = NULL;
    va_list args;
    va_start(args, fmt);
    (void)vasprintf(&msg, fmt, args);
    va_end(args);

    RegCallFrame *f = &vm->frames[vm->frame_count - 1];
    int line = 0;
    if (f->ip > f->chunk->code) {
        size_t offset = (size_t)(f->ip - f->chunk->code - 1);
        if (offset < f->chunk->lines_len)
            line = f->chunk->lines[offset];
    }
    if (line > 0) {
        char *full = NULL;
        (void)asprintf(&full, "[line %d] %s", line, msg);
        free(msg);
        vm->error = full;
    } else {
        vm->error = msg;
    }
    return REGVM_RUNTIME_ERROR;
}

/* Forward declaration needed by rvm_handle_error */
static inline void reg_set(LatValue *r, LatValue val);

/* Error handler that routes through exception handlers when available.
 * Used inside the dispatch loop via RVM_ERROR macro.
 * Returns REGVM_OK if handled (execution continues), error otherwise. */
static RegVMResult rvm_handle_error(RegVM *vm, RegCallFrame **frame_ptr,
                                     LatValue **R_ptr, const char *fmt, ...) {
    char *inner = NULL;
    va_list args;
    va_start(args, fmt);
    (void)vasprintf(&inner, fmt, args);
    va_end(args);

    /* Prepend line info */
    RegCallFrame *f = &vm->frames[vm->frame_count - 1];
    int line = 0;
    if (f->ip > f->chunk->code) {
        size_t offset = (size_t)(f->ip - f->chunk->code - 1);
        if (offset < f->chunk->lines_len)
            line = f->chunk->lines[offset];
    }
    char *msg;
    if (line > 0) {
        (void)asprintf(&msg, "[line %d] %s", line, inner);
        free(inner);
    } else {
        msg = inner;
    }

    if (vm->handler_count > 0) {
        RegHandler h = vm->handlers[--vm->handler_count];

        /* Unwind frames */
        while (vm->frame_count - 1 > (int)h.frame_index) {
            RegCallFrame *uf = &vm->frames[vm->frame_count - 1];
            for (int i = 0; i < REGVM_REG_MAX; i++)
                value_free_inline(&uf->regs[i]);
            vm->frame_count--;
            vm->reg_stack_top -= REGVM_REG_MAX;
        }

        *frame_ptr = &vm->frames[vm->frame_count - 1];
        *R_ptr = (*frame_ptr)->regs;
        (*frame_ptr)->ip = h.ip;
        reg_set(&(*R_ptr)[h.error_reg], value_string_owned(msg));
        return REGVM_OK;
    }

    vm->error = msg;
    return REGVM_RUNTIME_ERROR;
}

/* ── Set a register (free old value, then assign) ── */

static inline void reg_set(LatValue *r, LatValue val) {
    value_free_inline(r);
    *r = val;
}

/* Forward declarations for recursive closure calls */
static RegVMResult regvm_dispatch(RegVM *vm, int base_frame, LatValue *result);
static LatValue regvm_call_closure(RegVM *vm, LatValue *closure, LatValue *args, int argc);

/* Run a sub-chunk within the current VM (pushes a new frame, doesn't reset state) */
static RegVMResult regvm_run_sub(RegVM *vm, RegChunk *chunk, LatValue *result) {
    if (vm->frame_count >= REGVM_FRAMES_MAX)
        return rvm_error(vm, "call stack overflow");
    size_t new_base = vm->reg_stack_top;
    if (new_base + REGVM_REG_MAX > REGVM_REG_MAX * REGVM_FRAMES_MAX)
        return rvm_error(vm, "register stack overflow");
    LatValue *new_regs = &vm->reg_stack[new_base];
    vm->reg_stack_top += REGVM_REG_MAX;
    for (int i = 0; i < REGVM_REG_MAX; i++)
        new_regs[i] = value_nil();

    int saved_base = vm->frame_count;
    RegCallFrame *new_frame = &vm->frames[vm->frame_count++];
    new_frame->chunk = chunk;
    new_frame->ip = chunk->code;
    new_frame->regs = new_regs;
    new_frame->reg_count = REGVM_REG_MAX;
    new_frame->upvalues = NULL;
    new_frame->upvalue_count = 0;
    new_frame->caller_result_reg = 0;

    return regvm_dispatch(vm, saved_base, result);
}

/* ── Invoke builtin method ── */
/* Returns true if handled, false if not a builtin */

static bool rvm_invoke_builtin(RegVM *vm, LatValue *obj, const char *method,
                                LatValue *args, int arg_count, LatValue *result) {
    (void)vm;
    if (obj->type == VAL_ARRAY) {
        if (strcmp(method, "len") == 0 && arg_count == 0) {
            *result = value_int((int64_t)obj->as.array.len);
            return true;
        }
        if (strcmp(method, "push") == 0 && arg_count == 1) {
            LatValue val = rvm_clone(&args[0]);
            if (obj->as.array.len >= obj->as.array.cap) {
                obj->as.array.cap = obj->as.array.cap ? obj->as.array.cap * 2 : 4;
                obj->as.array.elems = realloc(obj->as.array.elems,
                    obj->as.array.cap * sizeof(LatValue));
            }
            obj->as.array.elems[obj->as.array.len++] = val;
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "pop") == 0 && arg_count == 0) {
            if (obj->as.array.len == 0) {
                *result = value_nil();
            } else {
                *result = obj->as.array.elems[--obj->as.array.len];
            }
            return true;
        }
        if (strcmp(method, "contains") == 0 && arg_count == 1) {
            bool found = false;
            for (size_t i = 0; i < obj->as.array.len; i++) {
                if (value_eq(&obj->as.array.elems[i], &args[0])) {
                    found = true;
                    break;
                }
            }
            *result = value_bool(found);
            return true;
        }
        if (strcmp(method, "reverse") == 0 && arg_count == 0) {
            LatValue *elems = malloc(obj->as.array.len * sizeof(LatValue));
            for (size_t i = 0; i < obj->as.array.len; i++)
                elems[i] = rvm_clone(&obj->as.array.elems[obj->as.array.len - 1 - i]);
            *result = value_array(elems, obj->as.array.len);
            free(elems);
            return true;
        }
        if (strcmp(method, "map") == 0 && arg_count == 1) {
            LatValue *closure = &args[0];
            size_t len = obj->as.array.len;
            LatValue *elems = malloc(len * sizeof(LatValue));
            for (size_t i = 0; i < len; i++) {
                LatValue arg = rvm_clone(&obj->as.array.elems[i]);
                elems[i] = regvm_call_closure(vm, closure, &arg, 1);
                value_free(&arg);
            }
            *result = value_array(elems, len);
            free(elems);
            return true;
        }
        if (strcmp(method, "filter") == 0 && arg_count == 1) {
            LatValue *closure = &args[0];
            size_t len = obj->as.array.len;
            size_t cap = len > 0 ? len : 1;
            LatValue *elems = malloc(cap * sizeof(LatValue));
            size_t out_len = 0;
            for (size_t i = 0; i < len; i++) {
                LatValue arg = rvm_clone(&obj->as.array.elems[i]);
                LatValue pred = regvm_call_closure(vm, closure, &arg, 1);
                bool keep = (pred.type == VAL_BOOL && pred.as.bool_val);
                value_free(&pred);
                if (keep) {
                    elems[out_len++] = arg;
                } else {
                    value_free(&arg);
                }
            }
            *result = value_array(elems, out_len);
            free(elems);
            return true;
        }
        if (strcmp(method, "join") == 0 && arg_count == 1) {
            const char *sep = (args[0].type == VAL_STR) ? args[0].as.str_val : "";
            size_t sep_len = strlen(sep);
            size_t n = obj->as.array.len;
            char **parts = malloc(n * sizeof(char *));
            size_t *lens = malloc(n * sizeof(size_t));
            size_t total = 0;
            for (size_t i = 0; i < n; i++) {
                parts[i] = value_display(&obj->as.array.elems[i]);
                lens[i] = strlen(parts[i]);
                total += lens[i];
            }
            if (n > 1) total += sep_len * (n - 1);
            char *buf = malloc(total + 1);
            size_t pos = 0;
            for (size_t i = 0; i < n; i++) {
                if (i > 0) { memcpy(buf + pos, sep, sep_len); pos += sep_len; }
                memcpy(buf + pos, parts[i], lens[i]); pos += lens[i];
                free(parts[i]);
            }
            buf[pos] = '\0';
            free(parts); free(lens);
            *result = value_string_owned(buf);
            return true;
        }
    }

    if (obj->type == VAL_STR) {
        if (strcmp(method, "len") == 0 && arg_count == 0) {
            *result = value_int((int64_t)strlen(obj->as.str_val));
            return true;
        }
        if (strcmp(method, "contains") == 0 && arg_count == 1) {
            if (args[0].type == VAL_STR) {
                *result = value_bool(strstr(obj->as.str_val, args[0].as.str_val) != NULL);
            } else {
                *result = value_bool(false);
            }
            return true;
        }
    }

    if (obj->type == VAL_MAP) {
        if (strcmp(method, "len") == 0 && arg_count == 0) {
            size_t count = 0;
            for (size_t i = 0; i < obj->as.map.map->cap; i++) {
                if (obj->as.map.map->entries[i].state == MAP_OCCUPIED)
                    count++;
            }
            *result = value_int((int64_t)count);
            return true;
        }
        if (strcmp(method, "keys") == 0 && arg_count == 0) {
            size_t cap = obj->as.map.map->cap;
            LatValue *keys = malloc(cap * sizeof(LatValue));
            size_t count = 0;
            for (size_t i = 0; i < cap; i++) {
                if (obj->as.map.map->entries[i].state == MAP_OCCUPIED)
                    keys[count++] = value_string(obj->as.map.map->entries[i].key);
            }
            *result = value_array(keys, count);
            free(keys);
            return true;
        }
        if (strcmp(method, "values") == 0 && arg_count == 0) {
            size_t cap = obj->as.map.map->cap;
            LatValue *vals = malloc(cap * sizeof(LatValue));
            size_t count = 0;
            for (size_t i = 0; i < cap; i++) {
                if (obj->as.map.map->entries[i].state == MAP_OCCUPIED)
                    vals[count++] = rvm_clone((LatValue *)obj->as.map.map->entries[i].value);
            }
            *result = value_array(vals, count);
            free(vals);
            return true;
        }
        if (strcmp(method, "get") == 0 && arg_count == 1) {
            if (args[0].type == VAL_STR) {
                LatValue *val = lat_map_get(obj->as.map.map, args[0].as.str_val);
                *result = val ? rvm_clone(val) : value_nil();
            } else {
                *result = value_nil();
            }
            return true;
        }
        if (strcmp(method, "set") == 0 && arg_count == 2) {
            if (args[0].type == VAL_STR) {
                LatValue cloned = rvm_clone(&args[1]);
                lat_map_set(obj->as.map.map, args[0].as.str_val, &cloned);
            }
            *result = value_unit();
            return true;
        }
        if ((strcmp(method, "has") == 0 || strcmp(method, "contains") == 0) && arg_count == 1) {
            if (args[0].type == VAL_STR)
                *result = value_bool(lat_map_get(obj->as.map.map, args[0].as.str_val) != NULL);
            else
                *result = value_bool(false);
            return true;
        }
        if (strcmp(method, "entries") == 0 && arg_count == 0) {
            size_t cap = obj->as.map.map->cap;
            LatValue *entries = malloc(cap * sizeof(LatValue));
            size_t count = 0;
            for (size_t i = 0; i < cap; i++) {
                if (obj->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                LatValue pair[2];
                pair[0] = value_string(obj->as.map.map->entries[i].key);
                pair[1] = rvm_clone((LatValue *)obj->as.map.map->entries[i].value);
                entries[count++] = value_array(pair, 2);
            }
            *result = value_array(entries, count);
            free(entries);
            return true;
        }
        if (strcmp(method, "merge") == 0 && arg_count == 1) {
            if (args[0].type == VAL_MAP) {
                LatValue merged = rvm_clone(obj);
                for (size_t i = 0; i < args[0].as.map.map->cap; i++) {
                    if (args[0].as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                    LatValue v = rvm_clone((LatValue *)args[0].as.map.map->entries[i].value);
                    lat_map_set(merged.as.map.map, args[0].as.map.map->entries[i].key, &v);
                }
                *result = merged;
            } else {
                *result = rvm_clone(obj);
            }
            return true;
        }
        if (strcmp(method, "for_each") == 0 && arg_count == 1) {
            LatValue *closure = &args[0];
            for (size_t i = 0; i < obj->as.map.map->cap; i++) {
                if (obj->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                LatValue cb_args[2];
                cb_args[0] = value_string(obj->as.map.map->entries[i].key);
                cb_args[1] = rvm_clone((LatValue *)obj->as.map.map->entries[i].value);
                LatValue ret = regvm_call_closure(vm, closure, cb_args, 2);
                value_free(&cb_args[0]);
                value_free(&cb_args[1]);
                value_free(&ret);
            }
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "filter") == 0 && arg_count == 1) {
            LatValue *closure = &args[0];
            LatValue filtered = value_map_new();
            for (size_t i = 0; i < obj->as.map.map->cap; i++) {
                if (obj->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                LatValue cb_args[2];
                cb_args[0] = value_string(obj->as.map.map->entries[i].key);
                cb_args[1] = rvm_clone((LatValue *)obj->as.map.map->entries[i].value);
                LatValue pred = regvm_call_closure(vm, closure, cb_args, 2);
                if (pred.type == VAL_BOOL && pred.as.bool_val) {
                    LatValue v = rvm_clone((LatValue *)obj->as.map.map->entries[i].value);
                    lat_map_set(filtered.as.map.map, obj->as.map.map->entries[i].key, &v);
                }
                value_free(&cb_args[0]);
                value_free(&cb_args[1]);
                value_free(&pred);
            }
            *result = filtered;
            return true;
        }
    }

    /* ── Array additional methods ── */
    if (obj->type == VAL_ARRAY) {
        if (strcmp(method, "enumerate") == 0 && arg_count == 0) {
            size_t len = obj->as.array.len;
            LatValue *elems = malloc(len * sizeof(LatValue));
            for (size_t i = 0; i < len; i++) {
                LatValue pair[2];
                pair[0] = value_int((int64_t)i);
                pair[1] = rvm_clone(&obj->as.array.elems[i]);
                elems[i] = value_array(pair, 2);
            }
            *result = value_array(elems, len);
            free(elems);
            return true;
        }
        if (strcmp(method, "reduce") == 0 && (arg_count == 1 || arg_count == 2)) {
            LatValue *closure = &args[0];
            LatValue acc;
            size_t start = 0;
            if (arg_count == 2) {
                acc = rvm_clone(&args[1]);
            } else if (obj->as.array.len > 0) {
                acc = rvm_clone(&obj->as.array.elems[0]);
                start = 1;
            } else {
                *result = value_nil();
                return true;
            }
            for (size_t i = start; i < obj->as.array.len; i++) {
                LatValue cb_args[2];
                cb_args[0] = acc;
                cb_args[1] = rvm_clone(&obj->as.array.elems[i]);
                acc = regvm_call_closure(vm, closure, cb_args, 2);
                value_free(&cb_args[0]);
                value_free(&cb_args[1]);
            }
            *result = acc;
            return true;
        }
        if ((strcmp(method, "each") == 0 || strcmp(method, "for_each") == 0) && arg_count == 1) {
            LatValue *closure = &args[0];
            for (size_t i = 0; i < obj->as.array.len; i++) {
                LatValue arg = rvm_clone(&obj->as.array.elems[i]);
                LatValue ret = regvm_call_closure(vm, closure, &arg, 1);
                value_free(&arg);
                value_free(&ret);
            }
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "sort") == 0 && arg_count <= 1) {
            size_t len = obj->as.array.len;
            LatValue *sorted = malloc(len * sizeof(LatValue));
            for (size_t i = 0; i < len; i++)
                sorted[i] = rvm_clone(&obj->as.array.elems[i]);
            /* Insertion sort */
            for (size_t i = 1; i < len; i++) {
                LatValue key = sorted[i];
                int64_t j = (int64_t)i - 1;
                while (j >= 0) {
                    bool swap = false;
                    if (arg_count == 1) {
                        LatValue cb_args[2] = { rvm_clone(&sorted[j]), rvm_clone(&key) };
                        LatValue cmp = regvm_call_closure(vm, &args[0], cb_args, 2);
                        swap = (cmp.type == VAL_INT && cmp.as.int_val > 0) ||
                               (cmp.type == VAL_FLOAT && cmp.as.float_val > 0);
                        value_free(&cmp);
                        value_free(&cb_args[0]);
                        value_free(&cb_args[1]);
                    } else {
                        if (sorted[j].type == VAL_INT && key.type == VAL_INT)
                            swap = sorted[j].as.int_val > key.as.int_val;
                        else if (sorted[j].type == VAL_FLOAT || key.type == VAL_FLOAT) {
                            double a = sorted[j].type == VAL_FLOAT ? sorted[j].as.float_val : (double)sorted[j].as.int_val;
                            double b = key.type == VAL_FLOAT ? key.as.float_val : (double)key.as.int_val;
                            swap = a > b;
                        } else if (sorted[j].type == VAL_STR && key.type == VAL_STR) {
                            swap = strcmp(sorted[j].as.str_val, key.as.str_val) > 0;
                        }
                    }
                    if (!swap) break;
                    sorted[j + 1] = sorted[j];
                    j--;
                }
                sorted[j + 1] = key;
            }
            *result = value_array(sorted, len);
            free(sorted);
            return true;
        }
        if (strcmp(method, "sort_by") == 0 && arg_count == 1) {
            LatValue *closure = &args[0];
            size_t len = obj->as.array.len;
            LatValue *sorted = malloc(len * sizeof(LatValue));
            LatValue *keys = malloc(len * sizeof(LatValue));
            for (size_t i = 0; i < len; i++) {
                sorted[i] = rvm_clone(&obj->as.array.elems[i]);
                LatValue arg = rvm_clone(&obj->as.array.elems[i]);
                keys[i] = regvm_call_closure(vm, closure, &arg, 1);
                value_free(&arg);
            }
            for (size_t i = 1; i < len; i++) {
                LatValue key_val = keys[i];
                LatValue elem = sorted[i];
                int64_t j = (int64_t)i - 1;
                while (j >= 0) {
                    bool swap = false;
                    if (keys[j].type == VAL_INT && key_val.type == VAL_INT)
                        swap = keys[j].as.int_val > key_val.as.int_val;
                    else if (keys[j].type == VAL_STR && key_val.type == VAL_STR)
                        swap = strcmp(keys[j].as.str_val, key_val.as.str_val) > 0;
                    if (!swap) break;
                    sorted[j + 1] = sorted[j];
                    keys[j + 1] = keys[j];
                    j--;
                }
                sorted[j + 1] = elem;
                keys[j + 1] = key_val;
            }
            *result = value_array(sorted, len);
            for (size_t i = 0; i < len; i++)
                value_free(&keys[i]);
            free(keys);
            free(sorted);
            return true;
        }
        if (strcmp(method, "find") == 0 && arg_count == 1) {
            LatValue *closure = &args[0];
            for (size_t i = 0; i < obj->as.array.len; i++) {
                LatValue arg = rvm_clone(&obj->as.array.elems[i]);
                LatValue pred = regvm_call_closure(vm, closure, &arg, 1);
                bool found = pred.type == VAL_BOOL && pred.as.bool_val;
                value_free(&pred);
                if (found) { *result = arg; return true; }
                value_free(&arg);
            }
            *result = value_nil();
            return true;
        }
        if (strcmp(method, "any") == 0 && arg_count == 1) {
            LatValue *closure = &args[0];
            for (size_t i = 0; i < obj->as.array.len; i++) {
                LatValue arg = rvm_clone(&obj->as.array.elems[i]);
                LatValue pred = regvm_call_closure(vm, closure, &arg, 1);
                bool yes = pred.type == VAL_BOOL && pred.as.bool_val;
                value_free(&pred);
                value_free(&arg);
                if (yes) { *result = value_bool(true); return true; }
            }
            *result = value_bool(false);
            return true;
        }
        if (strcmp(method, "all") == 0 && arg_count == 1) {
            LatValue *closure = &args[0];
            for (size_t i = 0; i < obj->as.array.len; i++) {
                LatValue arg = rvm_clone(&obj->as.array.elems[i]);
                LatValue pred = regvm_call_closure(vm, closure, &arg, 1);
                bool yes = pred.type == VAL_BOOL && pred.as.bool_val;
                value_free(&pred);
                value_free(&arg);
                if (!yes) { *result = value_bool(false); return true; }
            }
            *result = value_bool(true);
            return true;
        }
        if (strcmp(method, "flat_map") == 0 && arg_count == 1) {
            LatValue *closure = &args[0];
            size_t cap = obj->as.array.len * 2;
            LatValue *elems = malloc(cap * sizeof(LatValue));
            size_t out = 0;
            for (size_t i = 0; i < obj->as.array.len; i++) {
                LatValue arg = rvm_clone(&obj->as.array.elems[i]);
                LatValue mapped = regvm_call_closure(vm, closure, &arg, 1);
                value_free(&arg);
                if (mapped.type == VAL_ARRAY) {
                    for (size_t j = 0; j < mapped.as.array.len; j++) {
                        if (out >= cap) { cap *= 2; elems = realloc(elems, cap * sizeof(LatValue)); }
                        elems[out++] = rvm_clone(&mapped.as.array.elems[j]);
                    }
                    value_free(&mapped);
                } else {
                    if (out >= cap) { cap *= 2; elems = realloc(elems, cap * sizeof(LatValue)); }
                    elems[out++] = mapped;
                }
            }
            *result = value_array(elems, out);
            free(elems);
            return true;
        }
        if (strcmp(method, "unique") == 0 && arg_count == 0) {
            size_t len = obj->as.array.len;
            LatValue *elems = malloc(len * sizeof(LatValue));
            size_t out = 0;
            for (size_t i = 0; i < len; i++) {
                bool dup = false;
                for (size_t j = 0; j < out; j++) {
                    if (value_eq(&obj->as.array.elems[i], &elems[j])) { dup = true; break; }
                }
                if (!dup) elems[out++] = rvm_clone(&obj->as.array.elems[i]);
            }
            *result = value_array(elems, out);
            free(elems);
            return true;
        }
        if (strcmp(method, "index_of") == 0 && arg_count == 1) {
            for (size_t i = 0; i < obj->as.array.len; i++) {
                if (value_eq(&obj->as.array.elems[i], &args[0])) {
                    *result = value_int((int64_t)i);
                    return true;
                }
            }
            *result = value_int(-1);
            return true;
        }
        if (strcmp(method, "first") == 0 && arg_count == 0) {
            *result = obj->as.array.len > 0 ? rvm_clone(&obj->as.array.elems[0]) : value_nil();
            return true;
        }
        if (strcmp(method, "last") == 0 && arg_count == 0) {
            *result = obj->as.array.len > 0 ? rvm_clone(&obj->as.array.elems[obj->as.array.len - 1]) : value_nil();
            return true;
        }
        if (strcmp(method, "slice") == 0 && (arg_count == 1 || arg_count == 2)) {
            int64_t start = args[0].type == VAL_INT ? args[0].as.int_val : 0;
            int64_t end = arg_count == 2 && args[1].type == VAL_INT ? args[1].as.int_val : (int64_t)obj->as.array.len;
            if (start < 0) start += (int64_t)obj->as.array.len;
            if (end < 0) end += (int64_t)obj->as.array.len;
            if (start < 0) start = 0;
            if (end > (int64_t)obj->as.array.len) end = (int64_t)obj->as.array.len;
            if (start >= end) { *result = value_array(NULL, 0); return true; }
            size_t count = (size_t)(end - start);
            LatValue *elems = malloc(count * sizeof(LatValue));
            for (size_t i = 0; i < count; i++)
                elems[i] = rvm_clone(&obj->as.array.elems[start + (int64_t)i]);
            *result = value_array(elems, count);
            free(elems);
            return true;
        }
        if (strcmp(method, "take") == 0 && arg_count == 1) {
            int64_t n = args[0].type == VAL_INT ? args[0].as.int_val : 0;
            if (n < 0) n = 0;
            if (n > (int64_t)obj->as.array.len) n = (int64_t)obj->as.array.len;
            LatValue *elems = malloc((size_t)n * sizeof(LatValue));
            for (int64_t i = 0; i < n; i++)
                elems[i] = rvm_clone(&obj->as.array.elems[i]);
            *result = value_array(elems, (size_t)n);
            free(elems);
            return true;
        }
        if (strcmp(method, "drop") == 0 && arg_count == 1) {
            int64_t n = args[0].type == VAL_INT ? args[0].as.int_val : 0;
            if (n < 0) n = 0;
            if (n > (int64_t)obj->as.array.len) n = (int64_t)obj->as.array.len;
            size_t count = obj->as.array.len - (size_t)n;
            LatValue *elems = malloc(count * sizeof(LatValue));
            for (size_t i = 0; i < count; i++)
                elems[i] = rvm_clone(&obj->as.array.elems[n + (int64_t)i]);
            *result = value_array(elems, count);
            free(elems);
            return true;
        }
        if (strcmp(method, "flatten") == 0 && arg_count == 0) {
            size_t cap = obj->as.array.len * 2;
            LatValue *elems = malloc(cap * sizeof(LatValue));
            size_t out = 0;
            for (size_t i = 0; i < obj->as.array.len; i++) {
                if (obj->as.array.elems[i].type == VAL_ARRAY) {
                    LatValue *inner = &obj->as.array.elems[i];
                    for (size_t j = 0; j < inner->as.array.len; j++) {
                        if (out >= cap) { cap *= 2; elems = realloc(elems, cap * sizeof(LatValue)); }
                        elems[out++] = rvm_clone(&inner->as.array.elems[j]);
                    }
                } else {
                    if (out >= cap) { cap *= 2; elems = realloc(elems, cap * sizeof(LatValue)); }
                    elems[out++] = rvm_clone(&obj->as.array.elems[i]);
                }
            }
            *result = value_array(elems, out);
            free(elems);
            return true;
        }
        if (strcmp(method, "zip") == 0 && arg_count == 1) {
            if (args[0].type != VAL_ARRAY) { *result = value_array(NULL, 0); return true; }
            size_t len = obj->as.array.len < args[0].as.array.len ? obj->as.array.len : args[0].as.array.len;
            LatValue *elems = malloc(len * sizeof(LatValue));
            for (size_t i = 0; i < len; i++) {
                LatValue pair[2];
                pair[0] = rvm_clone(&obj->as.array.elems[i]);
                pair[1] = rvm_clone(&args[0].as.array.elems[i]);
                elems[i] = value_array(pair, 2);
            }
            *result = value_array(elems, len);
            free(elems);
            return true;
        }
        if (strcmp(method, "sum") == 0 && arg_count == 0) {
            int64_t isum = 0; double fsum = 0; bool has_float = false;
            for (size_t i = 0; i < obj->as.array.len; i++) {
                if (obj->as.array.elems[i].type == VAL_FLOAT) { has_float = true; fsum += obj->as.array.elems[i].as.float_val; }
                else if (obj->as.array.elems[i].type == VAL_INT) { isum += obj->as.array.elems[i].as.int_val; fsum += (double)obj->as.array.elems[i].as.int_val; }
            }
            *result = has_float ? value_float(fsum) : value_int(isum);
            return true;
        }
        if (strcmp(method, "min") == 0 && arg_count == 0) {
            if (obj->as.array.len == 0) { *result = value_nil(); return true; }
            LatValue min = obj->as.array.elems[0];
            for (size_t i = 1; i < obj->as.array.len; i++) {
                if (obj->as.array.elems[i].type == VAL_INT && min.type == VAL_INT) {
                    if (obj->as.array.elems[i].as.int_val < min.as.int_val) min = obj->as.array.elems[i];
                } else if (obj->as.array.elems[i].type == VAL_FLOAT || min.type == VAL_FLOAT) {
                    double a = obj->as.array.elems[i].type == VAL_FLOAT ? obj->as.array.elems[i].as.float_val : (double)obj->as.array.elems[i].as.int_val;
                    double b = min.type == VAL_FLOAT ? min.as.float_val : (double)min.as.int_val;
                    if (a < b) min = obj->as.array.elems[i];
                }
            }
            *result = rvm_clone(&min);
            return true;
        }
        if (strcmp(method, "max") == 0 && arg_count == 0) {
            if (obj->as.array.len == 0) { *result = value_nil(); return true; }
            LatValue max = obj->as.array.elems[0];
            for (size_t i = 1; i < obj->as.array.len; i++) {
                if (obj->as.array.elems[i].type == VAL_INT && max.type == VAL_INT) {
                    if (obj->as.array.elems[i].as.int_val > max.as.int_val) max = obj->as.array.elems[i];
                } else if (obj->as.array.elems[i].type == VAL_FLOAT || max.type == VAL_FLOAT) {
                    double a = obj->as.array.elems[i].type == VAL_FLOAT ? obj->as.array.elems[i].as.float_val : (double)obj->as.array.elems[i].as.int_val;
                    double b = max.type == VAL_FLOAT ? max.as.float_val : (double)max.as.int_val;
                    if (a > b) max = obj->as.array.elems[i];
                }
            }
            *result = rvm_clone(&max);
            return true;
        }
        if (strcmp(method, "insert") == 0 && arg_count == 2) {
            if (args[0].type != VAL_INT) { *result = value_unit(); return true; }
            int64_t idx = args[0].as.int_val;
            if (idx < 0) idx += (int64_t)obj->as.array.len;
            if (idx < 0) idx = 0;
            if (idx > (int64_t)obj->as.array.len) idx = (int64_t)obj->as.array.len;
            if (obj->as.array.len >= obj->as.array.cap) {
                obj->as.array.cap = obj->as.array.cap ? obj->as.array.cap * 2 : 4;
                obj->as.array.elems = realloc(obj->as.array.elems, obj->as.array.cap * sizeof(LatValue));
            }
            memmove(&obj->as.array.elems[idx + 1], &obj->as.array.elems[idx],
                    (obj->as.array.len - (size_t)idx) * sizeof(LatValue));
            obj->as.array.elems[idx] = rvm_clone(&args[1]);
            obj->as.array.len++;
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "remove_at") == 0 && arg_count == 1) {
            if (args[0].type != VAL_INT) { *result = value_nil(); return true; }
            int64_t idx = args[0].as.int_val;
            if (idx < 0) idx += (int64_t)obj->as.array.len;
            if (idx < 0 || (size_t)idx >= obj->as.array.len) { *result = value_nil(); return true; }
            *result = obj->as.array.elems[idx];
            memmove(&obj->as.array.elems[idx], &obj->as.array.elems[idx + 1],
                    (obj->as.array.len - (size_t)idx - 1) * sizeof(LatValue));
            obj->as.array.len--;
            return true;
        }
        if (strcmp(method, "chunk") == 0 && arg_count == 1) {
            if (args[0].type != VAL_INT || args[0].as.int_val <= 0) { *result = value_array(NULL, 0); return true; }
            int64_t sz = args[0].as.int_val;
            size_t len = obj->as.array.len;
            size_t chunks = (len + (size_t)sz - 1) / (size_t)sz;
            LatValue *elems = malloc(chunks * sizeof(LatValue));
            for (size_t i = 0; i < chunks; i++) {
                size_t start = i * (size_t)sz;
                size_t end = start + (size_t)sz;
                if (end > len) end = len;
                size_t count = end - start;
                LatValue *chunk_elems = malloc(count * sizeof(LatValue));
                for (size_t j = 0; j < count; j++)
                    chunk_elems[j] = rvm_clone(&obj->as.array.elems[start + j]);
                elems[i] = value_array(chunk_elems, count);
                free(chunk_elems);
            }
            *result = value_array(elems, chunks);
            free(elems);
            return true;
        }
        if (strcmp(method, "group_by") == 0 && arg_count == 1) {
            LatValue *closure = &args[0];
            LatValue map = value_map_new();
            for (size_t i = 0; i < obj->as.array.len; i++) {
                LatValue arg = rvm_clone(&obj->as.array.elems[i]);
                LatValue key = regvm_call_closure(vm, closure, &arg, 1);
                char *key_str = value_display(&key);
                LatValue *existing = lat_map_get(map.as.map.map, key_str);
                if (existing && existing->type == VAL_ARRAY) {
                    if (existing->as.array.len >= existing->as.array.cap) {
                        existing->as.array.cap = existing->as.array.cap ? existing->as.array.cap * 2 : 4;
                        existing->as.array.elems = realloc(existing->as.array.elems, existing->as.array.cap * sizeof(LatValue));
                    }
                    existing->as.array.elems[existing->as.array.len++] = arg;
                } else {
                    LatValue arr = value_array(&arg, 1);
                    lat_map_set(map.as.map.map, key_str, &arr);
                }
                value_free(&key);
                free(key_str);
            }
            *result = map;
            return true;
        }
    }

    /* ── String additional methods ── */
    if (obj->type == VAL_STR) {
        if (strcmp(method, "split") == 0 && arg_count == 1) {
            if (args[0].type != VAL_STR) { *result = value_array(NULL, 0); return true; }
            const char *s = obj->as.str_val;
            const char *sep = args[0].as.str_val;
            size_t sep_len = strlen(sep);
            size_t cap = 8;
            LatValue *parts = malloc(cap * sizeof(LatValue));
            size_t count = 0;
            if (sep_len == 0) {
                for (size_t i = 0; s[i]; i++) {
                    if (count >= cap) { cap *= 2; parts = realloc(parts, cap * sizeof(LatValue)); }
                    char c[2] = { s[i], '\0' };
                    parts[count++] = value_string(c);
                }
            } else {
                const char *p = s;
                while (*p) {
                    const char *found = strstr(p, sep);
                    if (!found) { if (count >= cap) { cap *= 2; parts = realloc(parts, cap * sizeof(LatValue)); } parts[count++] = value_string(p); break; }
                    if (count >= cap) { cap *= 2; parts = realloc(parts, cap * sizeof(LatValue)); }
                    char *part = strndup(p, (size_t)(found - p));
                    parts[count++] = value_string_owned(part);
                    p = found + sep_len;
                }
            }
            *result = value_array(parts, count);
            free(parts);
            return true;
        }
        if (strcmp(method, "trim") == 0 && arg_count == 0) {
            const char *s = obj->as.str_val;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            const char *e = obj->as.str_val + strlen(obj->as.str_val);
            while (e > s && (*(e-1) == ' ' || *(e-1) == '\t' || *(e-1) == '\n' || *(e-1) == '\r')) e--;
            *result = value_string_owned(strndup(s, (size_t)(e - s)));
            return true;
        }
        if (strcmp(method, "trim_start") == 0 && arg_count == 0) {
            const char *s = obj->as.str_val;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            *result = value_string(s);
            return true;
        }
        if (strcmp(method, "trim_end") == 0 && arg_count == 0) {
            size_t len = strlen(obj->as.str_val);
            const char *e = obj->as.str_val + len;
            while (e > obj->as.str_val && (*(e-1) == ' ' || *(e-1) == '\t' || *(e-1) == '\n' || *(e-1) == '\r')) e--;
            *result = value_string_owned(strndup(obj->as.str_val, (size_t)(e - obj->as.str_val)));
            return true;
        }
        if (strcmp(method, "to_upper") == 0 && arg_count == 0) {
            char *s = strdup(obj->as.str_val);
            for (char *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
            *result = value_string_owned(s);
            return true;
        }
        if (strcmp(method, "to_lower") == 0 && arg_count == 0) {
            char *s = strdup(obj->as.str_val);
            for (char *p = s; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
            *result = value_string_owned(s);
            return true;
        }
        if (strcmp(method, "starts_with") == 0 && arg_count == 1) {
            if (args[0].type == VAL_STR)
                *result = value_bool(strncmp(obj->as.str_val, args[0].as.str_val, strlen(args[0].as.str_val)) == 0);
            else
                *result = value_bool(false);
            return true;
        }
        if (strcmp(method, "ends_with") == 0 && arg_count == 1) {
            if (args[0].type == VAL_STR) {
                size_t slen = strlen(obj->as.str_val);
                size_t plen = strlen(args[0].as.str_val);
                *result = value_bool(plen <= slen && strcmp(obj->as.str_val + slen - plen, args[0].as.str_val) == 0);
            } else {
                *result = value_bool(false);
            }
            return true;
        }
        if (strcmp(method, "replace") == 0 && arg_count == 2) {
            if (args[0].type != VAL_STR || args[1].type != VAL_STR) { *result = rvm_clone(obj); return true; }
            const char *s = obj->as.str_val;
            const char *from = args[0].as.str_val;
            const char *to = args[1].as.str_val;
            size_t from_len = strlen(from), to_len = strlen(to);
            if (from_len == 0) { *result = rvm_clone(obj); return true; }
            size_t cap = strlen(s) + 64;
            char *buf = malloc(cap);
            size_t pos = 0;
            while (*s) {
                if (strncmp(s, from, from_len) == 0) {
                    while (pos + to_len >= cap) { cap *= 2; buf = realloc(buf, cap); }
                    memcpy(buf + pos, to, to_len); pos += to_len; s += from_len;
                } else {
                    if (pos + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                    buf[pos++] = *s++;
                }
            }
            buf[pos] = '\0';
            *result = value_string_owned(buf);
            return true;
        }
        if (strcmp(method, "index_of") == 0 && arg_count == 1) {
            if (args[0].type == VAL_STR) {
                const char *found = strstr(obj->as.str_val, args[0].as.str_val);
                *result = found ? value_int((int64_t)(found - obj->as.str_val)) : value_int(-1);
            } else {
                *result = value_int(-1);
            }
            return true;
        }
        if (strcmp(method, "substring") == 0 && (arg_count == 1 || arg_count == 2)) {
            size_t slen = strlen(obj->as.str_val);
            int64_t start = args[0].type == VAL_INT ? args[0].as.int_val : 0;
            int64_t end = arg_count == 2 && args[1].type == VAL_INT ? args[1].as.int_val : (int64_t)slen;
            if (start < 0) start += (int64_t)slen;
            if (end < 0) end += (int64_t)slen;
            if (start < 0) start = 0;
            if (end > (int64_t)slen) end = (int64_t)slen;
            if (start >= end) { *result = value_string(""); return true; }
            *result = value_string_owned(strndup(obj->as.str_val + start, (size_t)(end - start)));
            return true;
        }
        if (strcmp(method, "repeat") == 0 && arg_count == 1) {
            if (args[0].type != VAL_INT || args[0].as.int_val < 0) { *result = value_string(""); return true; }
            int64_t n = args[0].as.int_val;
            size_t slen = strlen(obj->as.str_val);
            char *buf = malloc(slen * (size_t)n + 1);
            for (int64_t i = 0; i < n; i++)
                memcpy(buf + i * (int64_t)slen, obj->as.str_val, slen);
            buf[slen * (size_t)n] = '\0';
            *result = value_string_owned(buf);
            return true;
        }
        if (strcmp(method, "chars") == 0 && arg_count == 0) {
            size_t len = strlen(obj->as.str_val);
            LatValue *elems = malloc(len * sizeof(LatValue));
            for (size_t i = 0; i < len; i++) {
                char c[2] = { obj->as.str_val[i], '\0' };
                elems[i] = value_string(c);
            }
            *result = value_array(elems, len);
            free(elems);
            return true;
        }
        if (strcmp(method, "bytes") == 0 && arg_count == 0) {
            size_t len = strlen(obj->as.str_val);
            LatValue *elems = malloc(len * sizeof(LatValue));
            for (size_t i = 0; i < len; i++)
                elems[i] = value_int((int64_t)(unsigned char)obj->as.str_val[i]);
            *result = value_array(elems, len);
            free(elems);
            return true;
        }
        if (strcmp(method, "reverse") == 0 && arg_count == 0) {
            size_t len = strlen(obj->as.str_val);
            char *buf = malloc(len + 1);
            for (size_t i = 0; i < len; i++) buf[i] = obj->as.str_val[len - 1 - i];
            buf[len] = '\0';
            *result = value_string_owned(buf);
            return true;
        }
        if (strcmp(method, "pad_left") == 0 && (arg_count == 1 || arg_count == 2)) {
            int64_t n = args[0].type == VAL_INT ? args[0].as.int_val : 0;
            char pad = (arg_count == 2 && args[1].type == VAL_STR && args[1].as.str_val[0]) ? args[1].as.str_val[0] : ' ';
            size_t slen = strlen(obj->as.str_val);
            if ((int64_t)slen >= n) { *result = rvm_clone(obj); return true; }
            size_t plen = (size_t)n - slen;
            char *buf = malloc((size_t)n + 1);
            memset(buf, pad, plen);
            memcpy(buf + plen, obj->as.str_val, slen);
            buf[(size_t)n] = '\0';
            *result = value_string_owned(buf);
            return true;
        }
        if (strcmp(method, "pad_right") == 0 && (arg_count == 1 || arg_count == 2)) {
            int64_t n = args[0].type == VAL_INT ? args[0].as.int_val : 0;
            char pad = (arg_count == 2 && args[1].type == VAL_STR && args[1].as.str_val[0]) ? args[1].as.str_val[0] : ' ';
            size_t slen = strlen(obj->as.str_val);
            if ((int64_t)slen >= n) { *result = rvm_clone(obj); return true; }
            char *buf = malloc((size_t)n + 1);
            memcpy(buf, obj->as.str_val, slen);
            memset(buf + slen, pad, (size_t)n - slen);
            buf[(size_t)n] = '\0';
            *result = value_string_owned(buf);
            return true;
        }
    }

    /* ── Enum methods ── */
    if (obj->type == VAL_ENUM) {
        if (strcmp(method, "tag") == 0 || strcmp(method, "variant_name") == 0) {
            *result = value_string(obj->as.enm.variant_name);
            return true;
        }
        if (strcmp(method, "enum_name") == 0) {
            *result = value_string(obj->as.enm.enum_name);
            return true;
        }
        if (strcmp(method, "payload") == 0) {
            if (obj->as.enm.payload_count > 0)
                *result = rvm_clone(&obj->as.enm.payload[0]);
            else
                *result = value_nil();
            return true;
        }
        if (strcmp(method, "is_variant") == 0 && arg_count == 1) {
            if (args[0].type == VAL_STR)
                *result = value_bool(strcmp(obj->as.enm.variant_name, args[0].as.str_val) == 0);
            else
                *result = value_bool(false);
            return true;
        }
    }

    /* ── Tuple methods ── */
    if (obj->type == VAL_TUPLE) {
        if (strcmp(method, "len") == 0 && arg_count == 0) {
            *result = value_int((int64_t)obj->as.tuple.len);
            return true;
        }
    }

    /* ── Range methods ── */
    if (obj->type == VAL_RANGE) {
        if (strcmp(method, "len") == 0 && arg_count == 0) {
            int64_t len = obj->as.range.end - obj->as.range.start;
            *result = value_int(len > 0 ? len : 0);
            return true;
        }
        if (strcmp(method, "contains") == 0 && arg_count == 1) {
            if (args[0].type == VAL_INT) {
                int64_t v = args[0].as.int_val;
                *result = value_bool(v >= obj->as.range.start && v < obj->as.range.end);
            } else {
                *result = value_bool(false);
            }
            return true;
        }
    }

    /* ── Set methods ── */
    if (obj->type == VAL_SET) {
        if (strcmp(method, "len") == 0 && arg_count == 0) {
            *result = value_int((int64_t)lat_map_len(obj->as.set.map));
            return true;
        }
        if (strcmp(method, "has") == 0 && arg_count == 1) {
            char *key = value_display(&args[0]);
            bool found = lat_map_contains(obj->as.set.map, key);
            free(key);
            *result = value_bool(found);
            return true;
        }
        if (strcmp(method, "to_array") == 0 && arg_count == 0) {
            size_t len = lat_map_len(obj->as.set.map);
            LatValue *elems = malloc((len > 0 ? len : 1) * sizeof(LatValue));
            size_t idx = 0;
            for (size_t i = 0; i < obj->as.set.map->cap; i++) {
                if (obj->as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                LatValue *v = (LatValue *)obj->as.set.map->entries[i].value;
                elems[idx++] = rvm_clone(v);
            }
            *result = value_array(elems, idx);
            free(elems);
            return true;
        }
    }

    return false;
}

/* ── VM Dispatch Loop ── */

/* Native function type (same as stack VM) */
typedef LatValue (*VMNativeFn)(LatValue *args, int arg_count);

/* Call a closure from within a builtin handler (map, filter, etc.). */
static LatValue regvm_call_closure(RegVM *vm, LatValue *closure, LatValue *args, int argc) {
    if (closure->type != VAL_CLOSURE) return value_nil();

    /* Check for native C function */
    if (closure->as.closure.default_values == VM_NATIVE_MARKER) {
        VMNativeFn native = (VMNativeFn)closure->as.closure.native_fn;
        return native(args, argc);
    }

    RegChunk *fn_chunk = (RegChunk *)closure->as.closure.native_fn;
    if (!fn_chunk) return value_nil();

    if (vm->frame_count >= REGVM_FRAMES_MAX) return value_nil();

    size_t new_base = vm->reg_stack_top;
    if (new_base + REGVM_REG_MAX > REGVM_REG_MAX * REGVM_FRAMES_MAX)
        return value_nil();

    LatValue *new_regs = &vm->reg_stack[new_base];
    vm->reg_stack_top += REGVM_REG_MAX;
    for (int i = 0; i < REGVM_REG_MAX; i++)
        new_regs[i] = value_nil();

    new_regs[0] = value_unit();
    for (int i = 0; i < argc; i++) {
        value_free(&new_regs[1 + i]);
        new_regs[1 + i] = rvm_clone(&args[i]);
    }

    ObjUpvalue **upvals = (ObjUpvalue **)closure->as.closure.captured_env;
    size_t uv_count = closure->region_id != (size_t)-1 ? closure->region_id : 0;

    int saved_base = vm->frame_count;
    RegCallFrame *new_frame = &vm->frames[vm->frame_count++];
    new_frame->chunk = fn_chunk;
    new_frame->ip = fn_chunk->code;
    new_frame->regs = new_regs;
    new_frame->reg_count = REGVM_REG_MAX;
    new_frame->upvalues = upvals;
    new_frame->upvalue_count = uv_count;
    new_frame->caller_result_reg = 0;

    LatValue ret;
    RegVMResult res = regvm_dispatch(vm, saved_base, &ret);
    if (res != REGVM_OK) return value_nil();
    return ret;
}

static RegVMResult regvm_dispatch(RegVM *vm, int base_frame, LatValue *result) {
    RegCallFrame *frame = &vm->frames[vm->frame_count - 1];

    LatValue *R = frame->regs;  /* Register base pointer */

/* Route runtime errors through exception handlers when possible */
#define RVM_ERROR(...) do { \
    RegVMResult _err = rvm_handle_error(vm, &frame, &R, __VA_ARGS__); \
    if (_err != REGVM_OK) return _err; \
    DISPATCH(); \
} while(0)

#define READ_INSTR()  (*frame->ip++)
#define REGS          R

#ifdef VM_USE_COMPUTED_GOTO
    /* Computed goto dispatch table */
    static void *dispatch_table[ROP_COUNT] = {
        [ROP_MOVE]         = &&L_MOVE,
        [ROP_LOADK]        = &&L_LOADK,
        [ROP_LOADI]        = &&L_LOADI,
        [ROP_LOADNIL]      = &&L_LOADNIL,
        [ROP_LOADTRUE]     = &&L_LOADTRUE,
        [ROP_LOADFALSE]    = &&L_LOADFALSE,
        [ROP_LOADUNIT]     = &&L_LOADUNIT,
        [ROP_ADD]          = &&L_ADD,
        [ROP_SUB]          = &&L_SUB,
        [ROP_MUL]          = &&L_MUL,
        [ROP_DIV]          = &&L_DIV,
        [ROP_MOD]          = &&L_MOD,
        [ROP_NEG]          = &&L_NEG,
        [ROP_ADDI]         = &&L_ADDI,
        [ROP_CONCAT]       = &&L_CONCAT,
        [ROP_EQ]           = &&L_EQ,
        [ROP_NEQ]          = &&L_NEQ,
        [ROP_LT]           = &&L_LT,
        [ROP_LTEQ]         = &&L_LTEQ,
        [ROP_GT]           = &&L_GT,
        [ROP_GTEQ]         = &&L_GTEQ,
        [ROP_NOT]          = &&L_NOT,
        [ROP_JMP]          = &&L_JMP,
        [ROP_JMPFALSE]     = &&L_JMPFALSE,
        [ROP_JMPTRUE]      = &&L_JMPTRUE,
        [ROP_GETGLOBAL]    = &&L_GETGLOBAL,
        [ROP_SETGLOBAL]    = &&L_SETGLOBAL,
        [ROP_DEFINEGLOBAL] = &&L_DEFINEGLOBAL,
        [ROP_GETFIELD]     = &&L_GETFIELD,
        [ROP_SETFIELD]     = &&L_SETFIELD,
        [ROP_GETINDEX]     = &&L_GETINDEX,
        [ROP_SETINDEX]     = &&L_SETINDEX,
        [ROP_GETUPVALUE]   = &&L_GETUPVALUE,
        [ROP_SETUPVALUE]   = &&L_SETUPVALUE,
        [ROP_CLOSEUPVALUE] = &&L_CLOSEUPVALUE,
        [ROP_CALL]         = &&L_CALL,
        [ROP_RETURN]       = &&L_RETURN,
        [ROP_CLOSURE]      = &&L_CLOSURE,
        [ROP_NEWARRAY]     = &&L_NEWARRAY,
        [ROP_NEWSTRUCT]    = &&L_NEWSTRUCT,
        [ROP_BUILDRANGE]   = &&L_BUILDRANGE,
        [ROP_LEN]          = &&L_LEN,
        [ROP_PRINT]        = &&L_PRINT,
        [ROP_INVOKE]       = &&L_INVOKE,
        [ROP_FREEZE]       = &&L_FREEZE,
        [ROP_THAW]         = &&L_THAW,
        [ROP_CLONE]        = &&L_CLONE,
        [ROP_ITERINIT]     = &&L_ITERINIT,
        [ROP_ITERNEXT]     = &&L_ITERNEXT,
        [ROP_MARKFLUID]    = &&L_MARKFLUID,
        /* Bitwise */
        [ROP_BIT_AND]      = &&L_BIT_AND,
        [ROP_BIT_OR]       = &&L_BIT_OR,
        [ROP_BIT_XOR]      = &&L_BIT_XOR,
        [ROP_BIT_NOT]      = &&L_BIT_NOT,
        [ROP_LSHIFT]       = &&L_LSHIFT,
        [ROP_RSHIFT]       = &&L_RSHIFT,
        /* Tuple */
        [ROP_NEWTUPLE]     = &&L_NEWTUPLE,
        /* Spread/Flatten */
        [ROP_ARRAY_FLATTEN] = &&L_ARRAY_FLATTEN,
        /* Enum */
        [ROP_NEWENUM]      = &&L_NEWENUM,
        /* Optional chaining */
        [ROP_JMPNOTNIL]    = &&L_JMPNOTNIL,
        /* Exception handling */
        [ROP_PUSH_HANDLER] = &&L_PUSH_HANDLER,
        [ROP_POP_HANDLER]  = &&L_POP_HANDLER,
        [ROP_THROW]        = &&L_THROW,
        [ROP_TRY_UNWRAP]   = &&L_TRY_UNWRAP,
        /* Defer */
        [ROP_DEFER_PUSH]   = &&L_DEFER_PUSH,
        [ROP_DEFER_RUN]    = &&L_DEFER_RUN,
        /* Variadic */
        [ROP_COLLECT_VARARGS] = &&L_COLLECT_VARARGS,
        /* Advanced phase */
        [ROP_FREEZE_VAR]   = &&L_FREEZE_VAR,
        [ROP_THAW_VAR]     = &&L_THAW_VAR,
        [ROP_SUBLIMATE_VAR] = &&L_SUBLIMATE_VAR,
        [ROP_REACT]        = &&L_REACT,
        [ROP_UNREACT]      = &&L_UNREACT,
        [ROP_BOND]         = &&L_BOND,
        [ROP_UNBOND]       = &&L_UNBOND,
        [ROP_SEED]         = &&L_SEED,
        [ROP_UNSEED]       = &&L_UNSEED,
        /* Module/Import */
        [ROP_IMPORT]       = &&L_IMPORT,
        /* Concurrency */
        [ROP_SCOPE]        = &&L_SCOPE,
        [ROP_SELECT]       = &&L_SELECT,
        /* Ephemeral arena */
        [ROP_RESET_EPHEMERAL] = &&L_RESET_EPHEMERAL,
        /* Optimization */
        [ROP_ADD_INT]      = &&L_ADD_INT,
        [ROP_SUB_INT]      = &&L_SUB_INT,
        [ROP_MUL_INT]      = &&L_MUL_INT,
        [ROP_LT_INT]       = &&L_LT_INT,
        [ROP_LTEQ_INT]     = &&L_LTEQ_INT,
        [ROP_INC_REG]      = &&L_INC_REG,
        [ROP_DEC_REG]      = &&L_DEC_REG,
        [ROP_SETINDEX_LOCAL] = &&L_SETINDEX_LOCAL,
        [ROP_INVOKE_GLOBAL] = &&L_INVOKE_GLOBAL,
        /* Misc */
        [ROP_HALT]         = &&L_HALT,
    };

#define DISPATCH() do { \
    RegInstr _i = READ_INSTR(); \
    goto *dispatch_table[REG_GET_OP(_i)]; \
    } while(0)

    /* We need the instruction available after goto. Use a local. */
#undef DISPATCH
#define DISPATCH() do { \
    instr = READ_INSTR(); \
    goto *dispatch_table[REG_GET_OP(instr)]; \
    } while(0)

    RegInstr instr;
    DISPATCH();

#define CASE(label) L_##label:

#else
    /* Switch-based dispatch */
    for (;;) {
        RegInstr instr = READ_INSTR();
        switch (REG_GET_OP(instr)) {

#define CASE(label) case ROP_##label:
#define DISPATCH() continue

#endif

    CASE(MOVE) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        reg_set(&R[a], rvm_clone(&R[b]));
        DISPATCH();
    }

    CASE(LOADK) {
        uint8_t a = REG_GET_A(instr);
        uint16_t bx = REG_GET_Bx(instr);
        reg_set(&R[a], rvm_clone(&frame->chunk->constants[bx]));
        DISPATCH();
    }

    CASE(LOADI) {
        uint8_t a = REG_GET_A(instr);
        int16_t sbx = REG_GET_sBx(instr);
        reg_set(&R[a], value_int((int64_t)sbx));
        DISPATCH();
    }

    CASE(LOADNIL) {
        uint8_t a = REG_GET_A(instr);
        reg_set(&R[a], value_nil());
        DISPATCH();
    }

    CASE(LOADTRUE) {
        uint8_t a = REG_GET_A(instr);
        reg_set(&R[a], value_bool(true));
        DISPATCH();
    }

    CASE(LOADFALSE) {
        uint8_t a = REG_GET_A(instr);
        reg_set(&R[a], value_bool(false));
        DISPATCH();
    }

    CASE(LOADUNIT) {
        uint8_t a = REG_GET_A(instr);
        reg_set(&R[a], value_unit());
        DISPATCH();
    }

    CASE(ADD) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type == VAL_INT && R[c].type == VAL_INT) {
            reg_set(&R[a], value_int(R[b].as.int_val + R[c].as.int_val));
        } else if (R[b].type == VAL_FLOAT || R[c].type == VAL_FLOAT) {
            double lv = R[b].type == VAL_FLOAT ? R[b].as.float_val : (double)R[b].as.int_val;
            double rv = R[c].type == VAL_FLOAT ? R[c].as.float_val : (double)R[c].as.int_val;
            reg_set(&R[a], value_float(lv + rv));
        } else if (R[b].type == VAL_STR && R[c].type == VAL_STR) {
            size_t lb = strlen(R[b].as.str_val);
            size_t lc = strlen(R[c].as.str_val);
            char *buf = malloc(lb + lc + 1);
            memcpy(buf, R[b].as.str_val, lb);
            memcpy(buf + lb, R[c].as.str_val, lc);
            buf[lb + lc] = '\0';
            reg_set(&R[a], value_string_owned(buf));
        } else {
            RVM_ERROR("cannot add %s and %s",
                value_type_name(&R[b]), value_type_name(&R[c]));
        }
        DISPATCH();
    }

    CASE(SUB) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type == VAL_INT && R[c].type == VAL_INT) {
            reg_set(&R[a], value_int(R[b].as.int_val - R[c].as.int_val));
        } else if (R[b].type == VAL_FLOAT || R[c].type == VAL_FLOAT) {
            double lv = R[b].type == VAL_FLOAT ? R[b].as.float_val : (double)R[b].as.int_val;
            double rv = R[c].type == VAL_FLOAT ? R[c].as.float_val : (double)R[c].as.int_val;
            reg_set(&R[a], value_float(lv - rv));
        } else {
            RVM_ERROR("cannot subtract %s from %s",
                value_type_name(&R[c]), value_type_name(&R[b]));
        }
        DISPATCH();
    }

    CASE(MUL) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type == VAL_INT && R[c].type == VAL_INT) {
            reg_set(&R[a], value_int(R[b].as.int_val * R[c].as.int_val));
        } else if (R[b].type == VAL_FLOAT || R[c].type == VAL_FLOAT) {
            double lv = R[b].type == VAL_FLOAT ? R[b].as.float_val : (double)R[b].as.int_val;
            double rv = R[c].type == VAL_FLOAT ? R[c].as.float_val : (double)R[c].as.int_val;
            reg_set(&R[a], value_float(lv * rv));
        } else {
            RVM_ERROR("cannot multiply %s and %s",
                value_type_name(&R[b]), value_type_name(&R[c]));
        }
        DISPATCH();
    }

    CASE(DIV) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type == VAL_INT && R[c].type == VAL_INT) {
            if (R[c].as.int_val == 0)
                RVM_ERROR("division by zero");
            reg_set(&R[a], value_int(R[b].as.int_val / R[c].as.int_val));
        } else if (R[b].type == VAL_FLOAT || R[c].type == VAL_FLOAT) {
            double rv = R[c].type == VAL_FLOAT ? R[c].as.float_val : (double)R[c].as.int_val;
            if (rv == 0.0)
                RVM_ERROR("division by zero");
            double lv = R[b].type == VAL_FLOAT ? R[b].as.float_val : (double)R[b].as.int_val;
            reg_set(&R[a], value_float(lv / rv));
        } else {
            RVM_ERROR("cannot divide %s by %s",
                value_type_name(&R[b]), value_type_name(&R[c]));
        }
        DISPATCH();
    }

    CASE(MOD) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type == VAL_INT && R[c].type == VAL_INT) {
            if (R[c].as.int_val == 0)
                RVM_ERROR("modulo by zero");
            reg_set(&R[a], value_int(R[b].as.int_val % R[c].as.int_val));
        } else {
            RVM_ERROR("cannot modulo %s by %s",
                value_type_name(&R[b]), value_type_name(&R[c]));
        }
        DISPATCH();
    }

    CASE(NEG) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        if (R[b].type == VAL_INT) {
            reg_set(&R[a], value_int(-R[b].as.int_val));
        } else if (R[b].type == VAL_FLOAT) {
            reg_set(&R[a], value_float(-R[b].as.float_val));
        } else {
            RVM_ERROR("cannot negate %s", value_type_name(&R[b]));
        }
        DISPATCH();
    }

    CASE(ADDI) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        int8_t  c = (int8_t)REG_GET_C(instr);
        if (R[b].type == VAL_INT) {
            reg_set(&R[a], value_int(R[b].as.int_val + c));
        } else if (R[b].type == VAL_FLOAT) {
            reg_set(&R[a], value_float(R[b].as.float_val + c));
        } else {
            RVM_ERROR("cannot add immediate to %s", value_type_name(&R[b]));
        }
        DISPATCH();
    }

    CASE(CONCAT) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        char *ls = value_display(&R[b]);
        char *rs = value_display(&R[c]);
        size_t ll = strlen(ls), rl = strlen(rs);
        char *buf = malloc(ll + rl + 1);
        memcpy(buf, ls, ll);
        memcpy(buf + ll, rs, rl);
        buf[ll + rl] = '\0';
        free(ls); free(rs);
        reg_set(&R[a], value_string_owned(buf));
        DISPATCH();
    }

    CASE(EQ) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        reg_set(&R[a], value_bool(value_eq(&R[b], &R[c])));
        DISPATCH();
    }

    CASE(NEQ) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        reg_set(&R[a], value_bool(!value_eq(&R[b], &R[c])));
        DISPATCH();
    }

    CASE(LT) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type == VAL_INT && R[c].type == VAL_INT) {
            reg_set(&R[a], value_bool(R[b].as.int_val < R[c].as.int_val));
        } else if (R[b].type == VAL_FLOAT || R[c].type == VAL_FLOAT) {
            double lv = R[b].type == VAL_FLOAT ? R[b].as.float_val : (double)R[b].as.int_val;
            double rv = R[c].type == VAL_FLOAT ? R[c].as.float_val : (double)R[c].as.int_val;
            reg_set(&R[a], value_bool(lv < rv));
        } else {
            RVM_ERROR("cannot compare %s < %s",
                value_type_name(&R[b]), value_type_name(&R[c]));
        }
        DISPATCH();
    }

    CASE(LTEQ) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type == VAL_INT && R[c].type == VAL_INT) {
            reg_set(&R[a], value_bool(R[b].as.int_val <= R[c].as.int_val));
        } else if (R[b].type == VAL_FLOAT || R[c].type == VAL_FLOAT) {
            double lv = R[b].type == VAL_FLOAT ? R[b].as.float_val : (double)R[b].as.int_val;
            double rv = R[c].type == VAL_FLOAT ? R[c].as.float_val : (double)R[c].as.int_val;
            reg_set(&R[a], value_bool(lv <= rv));
        } else {
            RVM_ERROR("cannot compare %s <= %s",
                value_type_name(&R[b]), value_type_name(&R[c]));
        }
        DISPATCH();
    }

    CASE(GT) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type == VAL_INT && R[c].type == VAL_INT) {
            reg_set(&R[a], value_bool(R[b].as.int_val > R[c].as.int_val));
        } else if (R[b].type == VAL_FLOAT || R[c].type == VAL_FLOAT) {
            double lv = R[b].type == VAL_FLOAT ? R[b].as.float_val : (double)R[b].as.int_val;
            double rv = R[c].type == VAL_FLOAT ? R[c].as.float_val : (double)R[c].as.int_val;
            reg_set(&R[a], value_bool(lv > rv));
        } else {
            RVM_ERROR("cannot compare %s > %s",
                value_type_name(&R[b]), value_type_name(&R[c]));
        }
        DISPATCH();
    }

    CASE(GTEQ) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type == VAL_INT && R[c].type == VAL_INT) {
            reg_set(&R[a], value_bool(R[b].as.int_val >= R[c].as.int_val));
        } else if (R[b].type == VAL_FLOAT || R[c].type == VAL_FLOAT) {
            double lv = R[b].type == VAL_FLOAT ? R[b].as.float_val : (double)R[b].as.int_val;
            double rv = R[c].type == VAL_FLOAT ? R[c].as.float_val : (double)R[c].as.int_val;
            reg_set(&R[a], value_bool(lv >= rv));
        } else {
            RVM_ERROR("cannot compare %s >= %s",
                value_type_name(&R[b]), value_type_name(&R[c]));
        }
        DISPATCH();
    }

    CASE(NOT) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        reg_set(&R[a], value_bool(!value_is_truthy(&R[b])));
        DISPATCH();
    }

    CASE(JMP) {
        int32_t offset = REG_GET_sBx24(instr);
        frame->ip += offset;
        DISPATCH();
    }

    CASE(JMPFALSE) {
        uint8_t a = REG_GET_A(instr);
        int16_t offset = REG_GET_sBx(instr);
        if (!value_is_truthy(&R[a]))
            frame->ip += offset;
        DISPATCH();
    }

    CASE(JMPTRUE) {
        uint8_t a = REG_GET_A(instr);
        int16_t offset = REG_GET_sBx(instr);
        if (value_is_truthy(&R[a]))
            frame->ip += offset;
        DISPATCH();
    }

    CASE(GETGLOBAL) {
        uint8_t a = REG_GET_A(instr);
        uint16_t bx = REG_GET_Bx(instr);
        const char *name = frame->chunk->constants[bx].as.str_val;
        LatValue val;
        if (!env_get(vm->env, name, &val))
            RVM_ERROR("undefined variable '%s'", name);
        reg_set(&R[a], rvm_clone(&val));
        DISPATCH();
    }

    CASE(SETGLOBAL) {
        uint8_t a = REG_GET_A(instr);
        uint16_t bx = REG_GET_Bx(instr);
        const char *name = frame->chunk->constants[bx].as.str_val;
        if (!env_set(vm->env, name, rvm_clone(&R[a])))
            RVM_ERROR("undefined variable '%s'", name);
        DISPATCH();
    }

    CASE(DEFINEGLOBAL) {
        uint8_t a = REG_GET_A(instr);
        uint16_t bx = REG_GET_Bx(instr);
        const char *name = frame->chunk->constants[bx].as.str_val;
        env_define(vm->env, name, rvm_clone(&R[a]));
        DISPATCH();
    }

    CASE(GETFIELD) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        const char *field_name = frame->chunk->constants[c].as.str_val;

        if (R[b].type == VAL_STRUCT) {
            bool found = false;
            for (size_t i = 0; i < R[b].as.strct.field_count; i++) {
                if (strcmp(R[b].as.strct.field_names[i], field_name) == 0) {
                    reg_set(&R[a], rvm_clone(&R[b].as.strct.field_values[i]));
                    found = true;
                    break;
                }
            }
            if (!found)
                RVM_ERROR("struct '%s' has no field '%s'",
                    R[b].as.strct.name, field_name);
        } else if (R[b].type == VAL_MAP) {
            LatValue *val = lat_map_get(R[b].as.map.map, field_name);
            if (val)
                reg_set(&R[a], rvm_clone(val));
            else
                reg_set(&R[a], value_nil());
        } else {
            RVM_ERROR("cannot access field '%s' on %s",
                field_name, value_type_name(&R[b]));
        }
        DISPATCH();
    }

    CASE(SETFIELD) {
        uint8_t a = REG_GET_A(instr);  /* object reg */
        uint8_t b = REG_GET_B(instr);  /* field name constant */
        uint8_t c = REG_GET_C(instr);  /* value reg */
        const char *field_name = frame->chunk->constants[b].as.str_val;

        if (R[a].type == VAL_STRUCT) {
            for (size_t i = 0; i < R[a].as.strct.field_count; i++) {
                if (strcmp(R[a].as.strct.field_names[i], field_name) == 0) {
                    value_free(&R[a].as.strct.field_values[i]);
                    R[a].as.strct.field_values[i] = rvm_clone(&R[c]);
                    break;
                }
            }
        } else if (R[a].type == VAL_MAP) {
            LatValue cloned = rvm_clone(&R[c]);
            lat_map_set(R[a].as.map.map, field_name, &cloned);
        }
        DISPATCH();
    }

    CASE(GETINDEX) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);

        if (R[b].type == VAL_ARRAY) {
            if (R[c].type != VAL_INT)
                RVM_ERROR("array index must be an integer");
            int64_t idx = R[c].as.int_val;
            if (idx < 0) idx += (int64_t)R[b].as.array.len;
            if (idx < 0 || (size_t)idx >= R[b].as.array.len)
                RVM_ERROR("array index %lld out of bounds (len %zu)",
                    (long long)R[c].as.int_val, R[b].as.array.len);
            reg_set(&R[a], rvm_clone(&R[b].as.array.elems[idx]));
        } else if (R[b].type == VAL_MAP) {
            if (R[c].type != VAL_STR)
                RVM_ERROR("map key must be a string");
            LatValue *val = lat_map_get(R[b].as.map.map, R[c].as.str_val);
            if (val)
                reg_set(&R[a], rvm_clone(val));
            else
                reg_set(&R[a], value_nil());
        } else if (R[b].type == VAL_STR) {
            if (R[c].type != VAL_INT)
                RVM_ERROR("string index must be an integer");
            int64_t idx = R[c].as.int_val;
            size_t len = strlen(R[b].as.str_val);
            if (idx < 0) idx += (int64_t)len;
            if (idx < 0 || (size_t)idx >= len)
                RVM_ERROR("string index out of bounds");
            char buf[2] = { R[b].as.str_val[idx], '\0' };
            reg_set(&R[a], value_string(buf));
        } else {
            RVM_ERROR("cannot index %s", value_type_name(&R[b]));
        }
        DISPATCH();
    }

    CASE(SETINDEX) {
        uint8_t a = REG_GET_A(instr);  /* object */
        uint8_t b = REG_GET_B(instr);  /* index */
        uint8_t c = REG_GET_C(instr);  /* value */

        if (R[a].type == VAL_ARRAY) {
            if (R[b].type != VAL_INT)
                RVM_ERROR("array index must be an integer");
            int64_t idx = R[b].as.int_val;
            if (idx < 0) idx += (int64_t)R[a].as.array.len;
            if (idx < 0 || (size_t)idx >= R[a].as.array.len)
                RVM_ERROR("array index out of bounds");
            value_free(&R[a].as.array.elems[idx]);
            R[a].as.array.elems[idx] = rvm_clone(&R[c]);
        } else if (R[a].type == VAL_MAP) {
            if (R[b].type != VAL_STR)
                RVM_ERROR("map key must be a string");
            LatValue cloned = rvm_clone(&R[c]);
            lat_map_set(R[a].as.map.map, R[b].as.str_val, &cloned);
        } else {
            RVM_ERROR("cannot set index on %s", value_type_name(&R[a]));
        }
        DISPATCH();
    }

    CASE(GETUPVALUE) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        if (frame->upvalues && b < frame->upvalue_count) {
            reg_set(&R[a], rvm_clone(frame->upvalues[b]->location));
        }
        DISPATCH();
    }

    CASE(SETUPVALUE) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        if (frame->upvalues && b < frame->upvalue_count) {
            value_free(frame->upvalues[b]->location);
            *frame->upvalues[b]->location = rvm_clone(&R[a]);
        }
        DISPATCH();
    }

    CASE(CLOSEUPVALUE) {
        /* Close upvalue at register A */
        uint8_t a = REG_GET_A(instr);
        ObjUpvalue *prev = NULL;
        ObjUpvalue *uv = vm->open_upvalues;
        while (uv) {
            if (uv->location == &R[a]) {
                uv->closed = rvm_clone(&R[a]);
                uv->location = &uv->closed;
                if (prev) prev->next = uv->next;
                else vm->open_upvalues = uv->next;
                break;
            }
            prev = uv;
            uv = uv->next;
        }
        DISPATCH();
    }

    CASE(CALL) {
        uint8_t a = REG_GET_A(instr);     /* func register */
        uint8_t b = REG_GET_B(instr);     /* arg count */
        uint8_t c = REG_GET_C(instr);     /* return count (1 for now) */
        (void)c;

        LatValue *func = &R[a];

        if (func->type != VAL_CLOSURE)
            RVM_ERROR("attempt to call a non-function (%s)",
                value_type_name(func));

        /* Check for native function */
        if (func->as.closure.default_values == VM_NATIVE_MARKER) {
            VMNativeFn native = (VMNativeFn)func->as.closure.native_fn;
            /* Collect args */
            LatValue args[16];
            for (int i = 0; i < b; i++)
                args[i] = rvm_clone(&R[a + 1 + i]);
            LatValue ret = native(args, b);
            for (int i = 0; i < b; i++)
                value_free(&args[i]);
            reg_set(&R[a], ret);
            DISPATCH();
        }

        /* Compiled function call */
        RegChunk *fn_chunk = (RegChunk *)func->as.closure.native_fn;
        if (!fn_chunk)
            RVM_ERROR("attempt to call a closure with NULL chunk");

        if (vm->frame_count >= REGVM_FRAMES_MAX)
            RVM_ERROR("call stack overflow");

        /* Allocate new register window */
        size_t new_base = vm->reg_stack_top;
        if (new_base + REGVM_REG_MAX > REGVM_REG_MAX * REGVM_FRAMES_MAX)
            RVM_ERROR("register stack overflow");

        LatValue *new_regs = &vm->reg_stack[new_base];
        vm->reg_stack_top += REGVM_REG_MAX;

        /* Initialize new registers to nil */
        for (int i = 0; i < REGVM_REG_MAX; i++)
            new_regs[i] = value_nil();

        /* Copy arguments: R[0] = reserved, R[1..n] = args */
        new_regs[0] = value_unit();  /* Reserved slot */
        for (int i = 0; i < b; i++) {
            value_free(&new_regs[1 + i]);
            new_regs[1 + i] = rvm_clone(&R[a + 1 + i]);
        }

        /* Set up upvalues */
        ObjUpvalue **upvals = (ObjUpvalue **)func->as.closure.captured_env;
        size_t uv_count = func->region_id != (size_t)-1 ? func->region_id : 0;

        /* Push new frame */
        RegCallFrame *new_frame = &vm->frames[vm->frame_count++];
        new_frame->chunk = fn_chunk;
        new_frame->ip = fn_chunk->code;
        new_frame->regs = new_regs;
        new_frame->reg_count = REGVM_REG_MAX;
        new_frame->upvalues = upvals;
        new_frame->upvalue_count = uv_count;
        new_frame->caller_result_reg = a;  /* RETURN puts result here */
        frame = new_frame;
        R = new_regs;
        DISPATCH();
    }

    CASE(RETURN) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);

        LatValue ret_val = (b > 0) ? rvm_clone(&R[a]) : value_unit();
        uint8_t dest_reg = frame->caller_result_reg;

        /* Close any open upvalues that point into this frame's registers */
        {
            LatValue *frame_base = frame->regs;
            LatValue *frame_end = frame_base + REGVM_REG_MAX;
            ObjUpvalue **prev = &vm->open_upvalues;
            while (*prev) {
                ObjUpvalue *uv = *prev;
                if (uv->location >= frame_base && uv->location < frame_end) {
                    /* Close this upvalue: move value to uv->closed */
                    uv->closed = *uv->location;
                    *uv->location = value_nil(); /* prevent double-free */
                    uv->location = &uv->closed;
                    *prev = uv->next;
                } else {
                    prev = &uv->next;
                }
            }
        }

        /* Clean up current frame's registers */
        for (int i = 0; i < REGVM_REG_MAX; i++)
            value_free_inline(&frame->regs[i]);

        vm->frame_count--;
        vm->reg_stack_top -= REGVM_REG_MAX;

        if (vm->frame_count == base_frame) {
            /* Return to caller (or top-level) */
            *result = ret_val;
            return REGVM_OK;
        }

        /* Restore caller frame */
        frame = &vm->frames[vm->frame_count - 1];
        R = frame->regs;

        reg_set(&R[dest_reg], ret_val);
        DISPATCH();
    }

    CASE(CLOSURE) {
        uint8_t a = REG_GET_A(instr);
        uint16_t bx = REG_GET_Bx(instr);
        LatValue fn_proto = frame->chunk->constants[bx];

        /* Create closure from prototype */
        LatValue closure;
        memset(&closure, 0, sizeof(closure));
        closure.type = VAL_CLOSURE;
        closure.phase = VTAG_UNPHASED;
        closure.region_id = (size_t)-1;
        closure.as.closure.body = NULL;
        closure.as.closure.native_fn = fn_proto.as.closure.native_fn;
        closure.as.closure.param_count = fn_proto.as.closure.param_count;
        closure.as.closure.param_names = NULL;
        closure.as.closure.default_values = NULL;
        closure.as.closure.has_variadic = false;
        closure.as.closure.captured_env = NULL;

        /* Process upvalue descriptors that follow the CLOSURE instruction */
        /* Each upvalue descriptor is encoded as a MOVE instruction:
         * A=1 means local, A=0 means upvalue; B=index */
        size_t uv_count = 0;
        ObjUpvalue **upvals = NULL;

        /* Count upvalue descriptors by peeking ahead */
        RegInstr *peek = frame->ip;
        while (peek < frame->chunk->code + frame->chunk->code_len) {
            RegInstr desc = *peek;
            uint8_t desc_op = REG_GET_OP(desc);
            if (desc_op != ROP_MOVE) break;
            uint8_t desc_a = REG_GET_A(desc);
            if (desc_a > 1) break;  /* Not an upvalue descriptor */
            uv_count++;
            peek++;
        }

        if (uv_count > 0) {
            upvals = malloc(uv_count * sizeof(ObjUpvalue *));
            for (size_t i = 0; i < uv_count; i++) {
                RegInstr desc = READ_INSTR();
                uint8_t is_local = REG_GET_A(desc);
                uint8_t index = REG_GET_B(desc);

                if (is_local) {
                    /* Capture from current frame's register */
                    ObjUpvalue *uv = malloc(sizeof(ObjUpvalue));
                    uv->location = &R[index];
                    uv->closed = value_nil();
                    uv->next = vm->open_upvalues;
                    vm->open_upvalues = uv;
                    upvals[i] = uv;
                } else {
                    /* Capture from enclosing upvalue */
                    if (frame->upvalues && index < frame->upvalue_count)
                        upvals[i] = frame->upvalues[index];
                    else
                        upvals[i] = NULL;
                }
            }
            closure.as.closure.captured_env = (Env *)upvals;
            closure.region_id = uv_count;
        }

        reg_set(&R[a], closure);
        DISPATCH();
    }

    CASE(NEWARRAY) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);  /* base register */
        uint8_t c = REG_GET_C(instr);  /* count */

        if (c == 0) {
            reg_set(&R[a], value_array(NULL, 0));
        } else {
            LatValue *elems = malloc(c * sizeof(LatValue));
            for (int i = 0; i < c; i++)
                elems[i] = rvm_clone(&R[b + i]);
            reg_set(&R[a], value_array(elems, c));
            free(elems);
        }
        DISPATCH();
    }

    CASE(NEWSTRUCT) {
        uint8_t a = REG_GET_A(instr);
        /* b is unused; name constant comes from follow-up LOADK instruction */
        uint8_t c = REG_GET_C(instr);  /* field count */

        /* Read the follow-up LOADK instruction to get the full constant index */
        RegInstr name_instr = READ_INSTR();
        uint16_t name_ki = REG_GET_Bx(name_instr);
        const char *struct_name = frame->chunk->constants[name_ki].as.str_val;

        /* Look up field names from struct metadata */
        char meta_name[256];
        snprintf(meta_name, sizeof(meta_name), "__struct_%s", struct_name);
        LatValue meta;
        if (!env_get(vm->env, meta_name, &meta)) {
            RVM_ERROR("unknown struct '%s'", struct_name);
        }

        if (meta.type != VAL_ARRAY || (int)meta.as.array.len != c) {
            RVM_ERROR("struct '%s' field count mismatch", struct_name);
        }

        /* Build field names array */
        char **field_names = malloc(c * sizeof(char *));
        LatValue *field_values = malloc(c * sizeof(LatValue));
        for (int i = 0; i < c; i++) {
            field_names[i] = strdup(meta.as.array.elems[i].as.str_val);
            /* Field values are in registers a+1..a+c */
            /* Actually, the compiler puts them starting at base (which is free'd by the compiler,
             * but the values are still in registers we can read) */
            /* The register where fields were compiled: they're before `a` in the temp window.
             * But we don't know the exact base. Let's use the meta field names to find them
             * from the struct literal compilation. Actually, let me revisit the compiler... */
            /* The compiler compiles fields into base..base+c-1, then emits NEWSTRUCT into dst(a).
             * So fields are in R[a+1..a+c] if base = a+1... But the compiler uses alloc_reg
             * for base, so base = next_reg. The fields are contiguous starting at base.
             * After NEWSTRUCT, the compiler calls free_regs_to(base). But we haven't freed yet.
             * Actually, base was allocated, then fields at base..base+c-1.
             * After NEWSTRUCT is emitted, the LOADK follow-up uses base (which was the first alloc'd reg).
             * So the fields are at the register given by REG_GET_A(name_instr) and following. */
            /* Wait, looking at the compiler: base = alloc_reg(), fields compiled into base..base+c-1.
             * Then NEWSTRUCT A=dst, then LOADK A=base. So base is in REG_GET_A(name_instr). */
        }
        uint8_t field_base = REG_GET_A(name_instr);
        for (int i = 0; i < c; i++) {
            field_values[i] = rvm_clone(&R[field_base + i]);
        }

        LatValue strct = value_struct(struct_name, field_names, field_values, c);
        for (int i = 0; i < c; i++)
            free(field_names[i]);
        free(field_names);
        free(field_values);
        reg_set(&R[a], strct);
        DISPATCH();
    }

    CASE(BUILDRANGE) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type != VAL_INT || R[c].type != VAL_INT)
            RVM_ERROR("range bounds must be integers");
        reg_set(&R[a], value_range(R[b].as.int_val, R[c].as.int_val));
        DISPATCH();
    }

    CASE(LEN) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        if (R[b].type == VAL_ARRAY) {
            reg_set(&R[a], value_int((int64_t)R[b].as.array.len));
        } else if (R[b].type == VAL_STR) {
            reg_set(&R[a], value_int((int64_t)strlen(R[b].as.str_val)));
        } else if (R[b].type == VAL_RANGE) {
            int64_t len = R[b].as.range.end - R[b].as.range.start;
            if (len < 0) len = 0;
            reg_set(&R[a], value_int(len));
        } else {
            RVM_ERROR("cannot get length of %s", value_type_name(&R[b]));
        }
        DISPATCH();
    }

    CASE(PRINT) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);  /* count */
        for (int i = 0; i < b; i++) {
            if (i > 0) printf(" ");
            value_print(&R[a + i], stdout);
        }
        printf("\n");
        DISPATCH();
    }

    CASE(INVOKE) {
        /* Two-instruction sequence:
         *   INVOKE A=dst, B=method_ki, C=argc
         *   data:  A=obj_reg, B=args_base, C=0
         * Object is mutated in-place at R[obj_reg] (for push/pop).
         * Return value goes into R[dst]. */
        uint8_t dst = REG_GET_A(instr);
        uint8_t method_ki = REG_GET_B(instr);
        uint8_t argc = REG_GET_C(instr);

        /* Read data word */
        RegInstr data = *frame->ip++;
        uint8_t obj_reg = REG_GET_A(data);
        uint8_t args_base = REG_GET_B(data);

        const char *method_name = frame->chunk->constants[method_ki].as.str_val;

        /* Try builtin */
        LatValue invoke_result;
        LatValue *invoke_args = (argc > 0) ? &R[args_base] : NULL;
        if (rvm_invoke_builtin(vm, &R[obj_reg], method_name, invoke_args, argc, &invoke_result)) {
            if (vm->error)
                return REGVM_RUNTIME_ERROR;
            /* Object was mutated in-place at R[obj_reg]; result goes to R[dst] */
            reg_set(&R[dst], invoke_result);
            DISPATCH();
        }

        /* Check for callable closure field in struct */
        if (R[obj_reg].type == VAL_STRUCT) {
            for (size_t fi = 0; fi < R[obj_reg].as.strct.field_count; fi++) {
                if (strcmp(R[obj_reg].as.strct.field_names[fi], method_name) != 0) continue;
                LatValue *field = &R[obj_reg].as.strct.field_values[fi];
                if (field->type == VAL_CLOSURE && field->as.closure.native_fn) {
                    RegChunk *fn_chunk = (RegChunk *)field->as.closure.native_fn;
                    if (vm->frame_count >= REGVM_FRAMES_MAX)
                        RVM_ERROR("call stack overflow");

                    size_t new_base = vm->reg_stack_top;
                    LatValue *new_regs = &vm->reg_stack[new_base];
                    vm->reg_stack_top += REGVM_REG_MAX;
                    for (int i = 0; i < REGVM_REG_MAX; i++)
                        new_regs[i] = value_nil();

                    /* Slot 0 = reserved, slot 1 = self, slots 2+ = args */
                    new_regs[0] = value_unit();
                    new_regs[1] = rvm_clone(&R[obj_reg]);  /* self = first param */
                    for (int i = 0; i < argc; i++) {
                        value_free(&new_regs[2 + i]);
                        new_regs[2 + i] = rvm_clone(&R[args_base + i]);
                    }

                    ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                    size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;

                    RegCallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->chunk = fn_chunk;
                    new_frame->ip = fn_chunk->code;
                    new_frame->regs = new_regs;
                    new_frame->reg_count = REGVM_REG_MAX;
                    new_frame->upvalues = upvals;
                    new_frame->upvalue_count = uv_count;
                    new_frame->caller_result_reg = dst;
                    frame = new_frame;
                    R = new_regs;
                    DISPATCH();
                }
            }
        }

        /* Check for impl method (TypeName::method) */
        if (R[obj_reg].type == VAL_STRUCT) {
            char key[256];
            snprintf(key, sizeof(key), "%s::%s", R[obj_reg].as.strct.name, method_name);
            LatValue impl_fn;
            if (env_get(vm->env, key, &impl_fn) && impl_fn.type == VAL_CLOSURE) {
                RegChunk *fn_chunk = (RegChunk *)impl_fn.as.closure.native_fn;
                if (!fn_chunk) goto invoke_fail;

                if (vm->frame_count >= REGVM_FRAMES_MAX)
                    RVM_ERROR("call stack overflow");

                size_t new_base = vm->reg_stack_top;
                LatValue *new_regs = &vm->reg_stack[new_base];
                vm->reg_stack_top += REGVM_REG_MAX;
                for (int i = 0; i < REGVM_REG_MAX; i++)
                    new_regs[i] = value_nil();

                /* Slot 0 = reserved, slot 1 = self, slots 2+ = args */
                new_regs[0] = value_unit();
                new_regs[1] = rvm_clone(&R[obj_reg]);  /* self = first param */
                for (int i = 0; i < argc; i++) {
                    value_free(&new_regs[2 + i]);
                    new_regs[2 + i] = rvm_clone(&R[args_base + i]);
                }

                ObjUpvalue **upvals = (ObjUpvalue **)impl_fn.as.closure.captured_env;
                size_t uv_count = impl_fn.region_id != (size_t)-1 ? impl_fn.region_id : 0;

                RegCallFrame *new_frame = &vm->frames[vm->frame_count++];
                new_frame->chunk = fn_chunk;
                new_frame->ip = fn_chunk->code;
                new_frame->regs = new_regs;
                new_frame->reg_count = REGVM_REG_MAX;
                new_frame->upvalues = upvals;
                new_frame->upvalue_count = uv_count;
                new_frame->caller_result_reg = dst;
                frame = new_frame;
                R = new_regs;
                DISPATCH();
            }
        }

        invoke_fail:
        RVM_ERROR("no method '%s' on %s", method_name, value_type_name(&R[obj_reg]));
    }

    CASE(FREEZE) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        LatValue frozen = value_freeze(rvm_clone(&R[b]));
        reg_set(&R[a], frozen);
        DISPATCH();
    }

    CASE(THAW) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        LatValue thawed = value_thaw(&R[b]);
        reg_set(&R[a], thawed);
        DISPATCH();
    }

    CASE(CLONE) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        reg_set(&R[a], value_deep_clone(&R[b]));
        DISPATCH();
    }

    CASE(ITERINIT) {
        /* A = destination (collection stays in A), B = source */
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        if (a != b)
            reg_set(&R[a], rvm_clone(&R[b]));
        /* The collection stays in R[a]. Index starts at 0 (set by compiler). */
        DISPATCH();
    }

    CASE(ITERNEXT) {
        /* A = result (loop var), B = collection, C = index register */
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);

        if (R[b].type == VAL_RANGE) {
            int64_t idx = R[c].as.int_val;
            int64_t start = R[b].as.range.start;
            int64_t end = R[b].as.range.end;
            int64_t current_val = start + idx;
            if (current_val >= end) {
                /* Done — set result to nil (falsy, triggers JMPFALSE) */
                reg_set(&R[a], value_nil());
            } else {
                reg_set(&R[a], value_int(current_val));
            }
        } else if (R[b].type == VAL_ARRAY) {
            int64_t idx = R[c].as.int_val;
            if ((size_t)idx >= R[b].as.array.len) {
                reg_set(&R[a], value_nil());
            } else {
                reg_set(&R[a], rvm_clone(&R[b].as.array.elems[idx]));
            }
        } else {
            RVM_ERROR("cannot iterate over %s", value_type_name(&R[b]));
        }
        DISPATCH();
    }

    CASE(MARKFLUID) {
        uint8_t a = REG_GET_A(instr);
        R[a].phase = VTAG_FLUID;
        DISPATCH();
    }

    /* ── Bitwise operations ── */

    CASE(BIT_AND) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type != VAL_INT || R[c].type != VAL_INT)
            RVM_ERROR("bitwise AND requires integers");
        reg_set(&R[a], value_int(R[b].as.int_val & R[c].as.int_val));
        DISPATCH();
    }

    CASE(BIT_OR) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type != VAL_INT || R[c].type != VAL_INT)
            RVM_ERROR("bitwise OR requires integers");
        reg_set(&R[a], value_int(R[b].as.int_val | R[c].as.int_val));
        DISPATCH();
    }

    CASE(BIT_XOR) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type != VAL_INT || R[c].type != VAL_INT)
            RVM_ERROR("bitwise XOR requires integers");
        reg_set(&R[a], value_int(R[b].as.int_val ^ R[c].as.int_val));
        DISPATCH();
    }

    CASE(BIT_NOT) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        if (R[b].type != VAL_INT)
            RVM_ERROR("bitwise NOT requires integer");
        reg_set(&R[a], value_int(~R[b].as.int_val));
        DISPATCH();
    }

    CASE(LSHIFT) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type != VAL_INT || R[c].type != VAL_INT)
            RVM_ERROR("left shift requires integers");
        reg_set(&R[a], value_int(R[b].as.int_val << R[c].as.int_val));
        DISPATCH();
    }

    CASE(RSHIFT) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type != VAL_INT || R[c].type != VAL_INT)
            RVM_ERROR("right shift requires integers");
        reg_set(&R[a], value_int(R[b].as.int_val >> R[c].as.int_val));
        DISPATCH();
    }

    /* ── Tuple ── */

    CASE(NEWTUPLE) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);  /* count */
        LatValue *elems = c > 0 ? malloc(c * sizeof(LatValue)) : NULL;
        for (int i = 0; i < c; i++)
            elems[i] = rvm_clone(&R[b + i]);
        LatValue tup;
        tup.type = VAL_TUPLE;
        tup.phase = VTAG_UNPHASED;
        tup.region_id = REGION_NONE;
        tup.as.tuple.elems = elems;
        tup.as.tuple.len = c;
        reg_set(&R[a], tup);
        DISPATCH();
    }

    /* ── Spread/Flatten ── */

    CASE(ARRAY_FLATTEN) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        if (R[b].type != VAL_ARRAY) {
            reg_set(&R[a], rvm_clone(&R[b]));
            DISPATCH();
        }
        /* One-level flatten */
        size_t cap = R[b].as.array.len * 2;
        if (cap == 0) cap = 1;
        LatValue *elems = malloc(cap * sizeof(LatValue));
        size_t out = 0;
        for (size_t i = 0; i < R[b].as.array.len; i++) {
            if (R[b].as.array.elems[i].type == VAL_ARRAY) {
                LatValue *inner = &R[b].as.array.elems[i];
                for (size_t j = 0; j < inner->as.array.len; j++) {
                    if (out >= cap) { cap *= 2; elems = realloc(elems, cap * sizeof(LatValue)); }
                    elems[out++] = rvm_clone(&inner->as.array.elems[j]);
                }
            } else {
                if (out >= cap) { cap *= 2; elems = realloc(elems, cap * sizeof(LatValue)); }
                elems[out++] = rvm_clone(&R[b].as.array.elems[i]);
            }
        }
        reg_set(&R[a], value_array(elems, out));
        free(elems);
        DISPATCH();
    }

    /* ── Enum with payload ── */

    CASE(NEWENUM) {
        uint8_t dst = REG_GET_A(instr);
        uint8_t name_ki_lo = REG_GET_B(instr);
        uint8_t argc = REG_GET_C(instr);

        /* Read data word: A=base, B=variant_ki, C=name_ki_hi */
        RegInstr data = READ_INSTR();
        uint8_t base = REG_GET_A(data);
        uint8_t var_ki = REG_GET_B(data);
        uint8_t name_ki_hi = REG_GET_C(data);

        uint16_t name_ki = (uint16_t)name_ki_lo | ((uint16_t)name_ki_hi << 8);
        const char *enum_name = frame->chunk->constants[name_ki].as.str_val;
        const char *variant_name = frame->chunk->constants[var_ki].as.str_val;

        if (argc == 0) {
            reg_set(&R[dst], value_enum(enum_name, variant_name, NULL, 0));
        } else {
            LatValue *payload = malloc(argc * sizeof(LatValue));
            for (int i = 0; i < argc; i++)
                payload[i] = rvm_clone(&R[base + i]);
            reg_set(&R[dst], value_enum(enum_name, variant_name, payload, argc));
            free(payload);
        }
        DISPATCH();
    }

    /* ── Optional chaining ── */

    CASE(JMPNOTNIL) {
        uint8_t a = REG_GET_A(instr);
        int16_t offset = REG_GET_sBx(instr);
        if (R[a].type != VAL_NIL)
            frame->ip += offset;
        DISPATCH();
    }

    /* ── Exception handling ── */

    CASE(PUSH_HANDLER) {
        uint8_t a = REG_GET_A(instr);
        int16_t offset = REG_GET_sBx(instr);
        if (vm->handler_count >= REGVM_HANDLER_MAX)
            RVM_ERROR("exception handler stack overflow");
        RegHandler *h = &vm->handlers[vm->handler_count++];
        h->ip = frame->ip + offset;
        h->chunk = frame->chunk;
        h->frame_index = (size_t)(vm->frame_count - 1);
        h->reg_stack_top = vm->reg_stack_top;
        h->error_reg = a;
        DISPATCH();
    }

    CASE(POP_HANDLER) {
        if (vm->handler_count > 0)
            vm->handler_count--;
        DISPATCH();
    }

    CASE(THROW) {
        uint8_t a = REG_GET_A(instr);
        LatValue thrown = rvm_clone(&R[a]);

        if (vm->handler_count == 0) {
            char *msg = value_display(&thrown);
            RegVMResult err = rvm_error(vm, "unhandled exception: %s", msg);
            free(msg);
            value_free(&thrown);
            return err;
        }

        /* Unwind to handler */
        RegHandler h = vm->handlers[--vm->handler_count];

        /* Clean up frames between current and handler frame */
        while (vm->frame_count - 1 > (int)h.frame_index) {
            RegCallFrame *f = &vm->frames[vm->frame_count - 1];
            for (int i = 0; i < REGVM_REG_MAX; i++)
                value_free_inline(&f->regs[i]);
            vm->frame_count--;
            vm->reg_stack_top -= REGVM_REG_MAX;
        }

        frame = &vm->frames[vm->frame_count - 1];
        R = frame->regs;
        frame->ip = h.ip;

        reg_set(&R[h.error_reg], thrown);
        DISPATCH();
    }

    CASE(TRY_UNWRAP) {
        uint8_t a = REG_GET_A(instr);
        /* R[a] should be a Result map: {tag: "ok", value: ...} or {tag: "err", value: ...} */
        if (R[a].type == VAL_MAP) {
            LatValue *tag = lat_map_get(R[a].as.map.map, "tag");
            if (tag && tag->type == VAL_STR) {
                if (strcmp(tag->as.str_val, "ok") == 0) {
                    LatValue *val = lat_map_get(R[a].as.map.map, "value");
                    LatValue unwrapped = val ? rvm_clone(val) : value_nil();
                    reg_set(&R[a], unwrapped);
                    DISPATCH();
                } else if (strcmp(tag->as.str_val, "err") == 0) {
                    /* Propagate error: return the error map */
                    LatValue err_val = rvm_clone(&R[a]);
                    uint8_t dest_reg = frame->caller_result_reg;

                    for (int i = 0; i < REGVM_REG_MAX; i++)
                        value_free_inline(&frame->regs[i]);
                    vm->frame_count--;
                    vm->reg_stack_top -= REGVM_REG_MAX;

                    if (vm->frame_count == base_frame) {
                        *result = err_val;
                        return REGVM_OK;
                    }
                    frame = &vm->frames[vm->frame_count - 1];
                    R = frame->regs;
                    reg_set(&R[dest_reg], err_val);
                    DISPATCH();
                }
            }
        }
        /* If it's an enum Result */
        if (R[a].type == VAL_ENUM) {
            if (strcmp(R[a].as.enm.variant_name, "Ok") == 0) {
                LatValue unwrapped = R[a].as.enm.payload_count > 0
                    ? rvm_clone(&R[a].as.enm.payload[0]) : value_nil();
                reg_set(&R[a], unwrapped);
                DISPATCH();
            } else if (strcmp(R[a].as.enm.variant_name, "Err") == 0) {
                LatValue err_val = rvm_clone(&R[a]);
                uint8_t dest_reg = frame->caller_result_reg;
                for (int i = 0; i < REGVM_REG_MAX; i++)
                    value_free_inline(&frame->regs[i]);
                vm->frame_count--;
                vm->reg_stack_top -= REGVM_REG_MAX;
                if (vm->frame_count == base_frame) {
                    *result = err_val;
                    return REGVM_OK;
                }
                frame = &vm->frames[vm->frame_count - 1];
                R = frame->regs;
                reg_set(&R[dest_reg], err_val);
                DISPATCH();
            }
        }
        /* Not a Result — pass through */
        DISPATCH();
    }

    /* ── Defer ── */

    CASE(DEFER_PUSH) {
        /* sBx = offset to jump past the defer body */
        int32_t offset = REG_GET_sBx24(instr);
        if (vm->defer_count >= REGVM_DEFER_MAX)
            RVM_ERROR("defer stack overflow");
        RegDefer *d = &vm->defers[vm->defer_count++];
        d->ip = frame->ip;       /* Points to start of defer body */
        d->chunk = frame->chunk;
        d->frame_index = (size_t)(vm->frame_count - 1);
        d->regs = frame->regs;
        /* Skip past the defer body */
        frame->ip += offset;
        DISPATCH();
    }

    CASE(DEFER_RUN) {
        /* Execute all defers for the current frame in LIFO order.
         * Each defer body runs in a NEW frame with copies of the current
         * frame's registers, so RETURN in the defer body pops cleanly. */
        size_t frame_idx = (size_t)(vm->frame_count - 1);
        while (vm->defer_count > 0) {
            RegDefer *d = &vm->defers[vm->defer_count - 1];
            if (d->frame_index != frame_idx) break;
            vm->defer_count--;

            /* Push a new frame for the defer body */
            if (vm->frame_count >= REGVM_FRAMES_MAX ||
                vm->reg_stack_top + REGVM_REG_MAX > REGVM_REG_MAX * REGVM_FRAMES_MAX) {
                continue; /* Skip defer if stack is full */
            }

            LatValue *new_regs = &vm->reg_stack[vm->reg_stack_top];
            vm->reg_stack_top += REGVM_REG_MAX;

            /* Copy current registers so defer body can access locals */
            for (int i = 0; i < REGVM_REG_MAX; i++)
                new_regs[i] = rvm_clone(&R[i]);

            RegCallFrame *df = &vm->frames[vm->frame_count++];
            df->chunk = d->chunk;
            df->ip = d->ip;
            df->regs = new_regs;
            df->reg_count = REGVM_REG_MAX;
            df->upvalues = frame->upvalues;
            df->upvalue_count = frame->upvalue_count;
            df->caller_result_reg = 0;

            LatValue defer_result;
            RegVMResult dr = regvm_dispatch(vm, (int)(vm->frame_count - 1), &defer_result);
            value_free(&defer_result);
            (void)dr;

            /* Restore frame state */
            frame = &vm->frames[vm->frame_count - 1];
            R = frame->regs;
        }
        DISPATCH();
    }

    /* ── Variadic ── */

    CASE(COLLECT_VARARGS) {
        uint8_t a = REG_GET_A(instr);  /* destination register */
        uint8_t b = REG_GET_B(instr);  /* start position (declared_arity + 1) */
        /* Collect excess args into an array */
        size_t cap = 8;
        LatValue *elems = malloc(cap * sizeof(LatValue));
        size_t count = 0;
        for (int i = b; i < REGVM_REG_MAX; i++) {
            if (R[i].type == VAL_NIL || R[i].type == VAL_UNIT) break;
            if (count >= cap) { cap *= 2; elems = realloc(elems, cap * sizeof(LatValue)); }
            elems[count++] = rvm_clone(&R[i]);
        }
        reg_set(&R[a], value_array(elems, count));
        free(elems);
        DISPATCH();
    }

    /* ── Advanced phase system ── */

    CASE(FREEZE_VAR) {
        /* A=name constant index, B=loc_type (0=local, 1=upvalue, 2=global), C=slot */
        uint8_t name_ki = REG_GET_A(instr);
        uint8_t loc_type = REG_GET_B(instr);
        uint8_t slot = REG_GET_C(instr);
        (void)name_ki;

        LatValue *target = NULL;
        if (loc_type == 0) {
            target = &R[slot];
        } else if (loc_type == 1 && frame->upvalues && slot < frame->upvalue_count) {
            target = frame->upvalues[slot]->location;
        } else if (loc_type == 2) {
            const char *name = frame->chunk->constants[name_ki].as.str_val;
            LatValue gval;
            if (env_get(vm->env, name, &gval)) {
                LatValue frozen = value_freeze(rvm_clone(&gval));
                env_set(vm->env, name, frozen);
            }
            DISPATCH();
        }
        if (target) {
            LatValue frozen = value_freeze(rvm_clone(target));
            value_free(target);
            *target = frozen;
        }
        DISPATCH();
    }

    CASE(THAW_VAR) {
        uint8_t name_ki = REG_GET_A(instr);
        uint8_t loc_type = REG_GET_B(instr);
        uint8_t slot = REG_GET_C(instr);
        (void)name_ki;

        LatValue *target = NULL;
        if (loc_type == 0) {
            target = &R[slot];
        } else if (loc_type == 1 && frame->upvalues && slot < frame->upvalue_count) {
            target = frame->upvalues[slot]->location;
        } else if (loc_type == 2) {
            const char *name = frame->chunk->constants[name_ki].as.str_val;
            LatValue gval;
            if (env_get(vm->env, name, &gval)) {
                LatValue thawed = value_thaw(&gval);
                env_set(vm->env, name, thawed);
            }
            DISPATCH();
        }
        if (target) {
            LatValue thawed = value_thaw(target);
            value_free(target);
            *target = thawed;
        }
        DISPATCH();
    }

    CASE(SUBLIMATE_VAR) {
        uint8_t name_ki = REG_GET_A(instr);
        uint8_t loc_type = REG_GET_B(instr);
        uint8_t slot = REG_GET_C(instr);
        (void)name_ki;

        LatValue *target = NULL;
        if (loc_type == 0) {
            target = &R[slot];
        } else if (loc_type == 1 && frame->upvalues && slot < frame->upvalue_count) {
            target = frame->upvalues[slot]->location;
        } else if (loc_type == 2) {
            const char *name = frame->chunk->constants[name_ki].as.str_val;
            LatValue gval;
            if (env_get(vm->env, name, &gval)) {
                gval.phase = VTAG_SUBLIMATED;
                env_set(vm->env, name, gval);
            }
            DISPATCH();
        }
        if (target) {
            target->phase = VTAG_SUBLIMATED;
        }
        DISPATCH();
    }

    CASE(REACT) {
        /* A=closure_reg, B=target_reg: register reaction callback */
        /* Placeholder: reactions are not yet implemented in regvm */
        (void)REG_GET_A(instr);
        (void)REG_GET_B(instr);
        DISPATCH();
    }

    CASE(UNREACT) {
        (void)REG_GET_A(instr);
        DISPATCH();
    }

    CASE(BOND) {
        (void)REG_GET_A(instr);
        (void)REG_GET_B(instr);
        DISPATCH();
    }

    CASE(UNBOND) {
        (void)REG_GET_A(instr);
        DISPATCH();
    }

    CASE(SEED) {
        (void)REG_GET_A(instr);
        (void)REG_GET_B(instr);
        DISPATCH();
    }

    CASE(UNSEED) {
        (void)REG_GET_A(instr);
        DISPATCH();
    }

    /* ── Module/Import ── */

    CASE(IMPORT) {
        uint8_t a = REG_GET_A(instr);
        uint16_t bx = REG_GET_Bx(instr);
        const char *raw_path = frame->chunk->constants[bx].as.str_val;

        /* Resolve file path: append .lat if not present */
        size_t plen = strlen(raw_path);
        char *file_path;
        if (plen >= 4 && strcmp(raw_path + plen - 4, ".lat") == 0) {
            file_path = strdup(raw_path);
        } else {
            file_path = malloc(plen + 5);
            memcpy(file_path, raw_path, plen);
            memcpy(file_path + plen, ".lat", 5);
        }

        /* Resolve to absolute path */
        char resolved[PATH_MAX];
        if (!realpath(file_path, resolved)) {
            free(file_path);
            RVM_ERROR("import: cannot find '%s'", raw_path);
        }
        free(file_path);

        /* Check module cache */
        if (vm->module_cache) {
            LatValue *cached = lat_map_get(vm->module_cache, resolved);
            if (cached) {
                reg_set(&R[a], rvm_clone(cached));
                DISPATCH();
            }
        }

        /* Read the file */
        char *source = builtin_read_file(resolved);
        if (!source)
            RVM_ERROR("import: cannot read '%s'", resolved);

        /* Lex */
        Lexer mod_lex = lexer_new(source);
        char *lex_err = NULL;
        LatVec mod_toks = lexer_tokenize(&mod_lex, &lex_err);
        free(source);
        if (lex_err) {
            char errmsg[1024];
            snprintf(errmsg, sizeof(errmsg), "import '%s': %s", resolved, lex_err);
            free(lex_err);
            lat_vec_free(&mod_toks);
            RVM_ERROR("%s", errmsg);
        }

        /* Parse */
        Parser mod_parser = parser_new(&mod_toks);
        char *parse_err = NULL;
        Program mod_prog = parser_parse(&mod_parser, &parse_err);
        if (parse_err) {
            char errmsg[1024];
            snprintf(errmsg, sizeof(errmsg), "import '%s': %s", resolved, parse_err);
            free(parse_err);
            program_free(&mod_prog);
            for (size_t ti = 0; ti < mod_toks.len; ti++)
                token_free(lat_vec_get(&mod_toks, ti));
            lat_vec_free(&mod_toks);
            RVM_ERROR("%s", errmsg);
        }

        /* Compile as module */
        char *comp_err = NULL;
        RegChunk *mod_chunk = reg_compile_module(&mod_prog, &comp_err);

        /* Free parse artifacts */
        program_free(&mod_prog);
        for (size_t ti = 0; ti < mod_toks.len; ti++)
            token_free(lat_vec_get(&mod_toks, ti));
        lat_vec_free(&mod_toks);

        if (!mod_chunk) {
            char errmsg[1024];
            snprintf(errmsg, sizeof(errmsg), "import '%s': %s", resolved,
                     comp_err ? comp_err : "compile error");
            free(comp_err);
            RVM_ERROR("%s", errmsg);
        }

        /* Track chunk */
        regvm_track_chunk(vm, mod_chunk);

        /* Push module scope */
        env_push_scope(vm->env);

        /* Run module by pushing a new frame */
        LatValue mod_result;
        RegVMResult mod_r = regvm_run_sub(vm, mod_chunk, &mod_result);
        /* Restore frame/R pointers after dispatch */
        frame = &vm->frames[vm->frame_count - 1];
        R = frame->regs;

        if (mod_r != REGVM_OK) {
            env_pop_scope(vm->env);
            reg_set(&R[a], value_nil());
            DISPATCH();
        }
        value_free(&mod_result);

        /* Build module Map from the module scope */
        LatValue module_map = value_map_new();
        Scope *mod_scope = &vm->env->scopes[vm->env->count - 1];
        for (size_t mi = 0; mi < mod_scope->cap; mi++) {
            if (mod_scope->entries[mi].state != MAP_OCCUPIED) continue;
            const char *name = mod_scope->entries[mi].key;
            LatValue *val_ptr = (LatValue *)mod_scope->entries[mi].value;

            /* Copy all module bindings to base scope for closures */
            env_define_at(vm->env, 0, name, value_deep_clone(val_ptr));

            /* Skip internal metadata */
            if ((name[0] == '_' && name[1] == '_') || strchr(name, ':'))
                continue;

            LatValue exported = rvm_clone(val_ptr);
            lat_map_set(module_map.as.map.map, name, &exported);
        }

        env_pop_scope(vm->env);

        /* Cache */
        if (!vm->module_cache) {
            vm->module_cache = malloc(sizeof(LatMap));
            *vm->module_cache = lat_map_new(sizeof(LatValue));
        }
        LatValue cache_copy = value_deep_clone(&module_map);
        lat_map_set(vm->module_cache, resolved, &cache_copy);

        reg_set(&R[a], module_map);
        DISPATCH();
    }

    /* ── Concurrency ── */

    CASE(SCOPE) {
        /* Variable-length: read spawn_count, sync_idx, spawn_indices */
        RegInstr data1 = READ_INSTR();
        uint8_t spawn_count = REG_GET_A(data1);
        uint8_t sync_idx = REG_GET_B(data1);
        uint8_t spawn_indices[256];
        /* Read spawn indices from follow-up data words */
        for (uint8_t i = 0; i < spawn_count; i += 4) {
            RegInstr sp = READ_INSTR();
            spawn_indices[i] = REG_GET_A(sp);
            if (i + 1 < spawn_count) spawn_indices[i + 1] = REG_GET_B(sp);
            if (i + 2 < spawn_count) spawn_indices[i + 2] = REG_GET_C(sp);
            /* 4th byte would need another encoding — keep it simple for now */
        }

        /* Export locals to env for sub-chunk access */
        env_push_scope(vm->env);
        for (size_t fi2 = 0; fi2 < (size_t)vm->frame_count; fi2++) {
            RegCallFrame *f2 = &vm->frames[fi2];
            if (!f2->chunk) continue;
            for (size_t sl = 0; sl < f2->chunk->local_name_cap; sl++) {
                if (f2->chunk->local_names[sl])
                    env_define(vm->env, f2->chunk->local_names[sl],
                               rvm_clone(&f2->regs[sl]));
            }
        }

        /* Run sync body */
        if (sync_idx != 0xFF) {
            RegChunk *sync_body = (RegChunk *)frame->chunk->constants[sync_idx].as.closure.native_fn;
            if (sync_body) {
                LatValue scope_result;
                RegVMResult sr = regvm_run_sub(vm, sync_body, &scope_result);
                /* Restore frame/R pointers */
                frame = &vm->frames[vm->frame_count - 1];
                R = frame->regs;
                if (sr != REGVM_OK) {
                    env_pop_scope(vm->env);
                    RVM_ERROR("%s", vm->error ? vm->error : "scope error");
                }
                value_free(&scope_result);
            }
        }

        /* Run spawns synchronously (threading TBD) */
        for (uint8_t i = 0; i < spawn_count; i++) {
            RegChunk *sp_chunk = (RegChunk *)frame->chunk->constants[spawn_indices[i]].as.closure.native_fn;
            if (sp_chunk) {
                LatValue sp_result;
                RegVMResult sr = regvm_run_sub(vm, sp_chunk, &sp_result);
                /* Restore frame/R pointers */
                frame = &vm->frames[vm->frame_count - 1];
                R = frame->regs;
                value_free(&sp_result);
                (void)sr;
            }
        }

        env_pop_scope(vm->env);
        DISPATCH();
    }

    CASE(SELECT) {
        /* Variable-length: arm_count, per-arm data */
        RegInstr data1 = READ_INSTR();
        uint8_t arm_count = REG_GET_A(data1);

        /* Read arm descriptors */
        for (uint8_t i = 0; i < arm_count; i++) {
            RegInstr arm_data = READ_INSTR();
            (void)arm_data;  /* flags, chan_idx, body_idx, binding_idx */
        }

        /* Placeholder: select not yet fully implemented */
        /* Just produce unit for now */
        DISPATCH();
    }

    /* ── Ephemeral arena ── */

    CASE(RESET_EPHEMERAL) {
        if (vm->ephemeral)
            bump_arena_reset(vm->ephemeral);
        DISPATCH();
    }

    /* ── Optimization opcodes ── */

    CASE(ADD_INT) {
        uint8_t a = REG_GET_A(instr), b = REG_GET_B(instr), c = REG_GET_C(instr);
        R[a].type = VAL_INT;
        R[a].as.int_val = R[b].as.int_val + R[c].as.int_val;
        DISPATCH();
    }

    CASE(SUB_INT) {
        uint8_t a = REG_GET_A(instr), b = REG_GET_B(instr), c = REG_GET_C(instr);
        R[a].type = VAL_INT;
        R[a].as.int_val = R[b].as.int_val - R[c].as.int_val;
        DISPATCH();
    }

    CASE(MUL_INT) {
        uint8_t a = REG_GET_A(instr), b = REG_GET_B(instr), c = REG_GET_C(instr);
        R[a].type = VAL_INT;
        R[a].as.int_val = R[b].as.int_val * R[c].as.int_val;
        DISPATCH();
    }

    CASE(LT_INT) {
        uint8_t a = REG_GET_A(instr), b = REG_GET_B(instr), c = REG_GET_C(instr);
        R[a].type = VAL_BOOL;
        R[a].as.bool_val = R[b].as.int_val < R[c].as.int_val;
        DISPATCH();
    }

    CASE(LTEQ_INT) {
        uint8_t a = REG_GET_A(instr), b = REG_GET_B(instr), c = REG_GET_C(instr);
        R[a].type = VAL_BOOL;
        R[a].as.bool_val = R[b].as.int_val <= R[c].as.int_val;
        DISPATCH();
    }

    CASE(INC_REG) {
        uint8_t a = REG_GET_A(instr);
        R[a].as.int_val++;
        DISPATCH();
    }

    CASE(DEC_REG) {
        uint8_t a = REG_GET_A(instr);
        R[a].as.int_val--;
        DISPATCH();
    }

    CASE(SETINDEX_LOCAL) {
        /* R[A][R[B]] = R[C] — in-place array/map mutation */
        uint8_t a = REG_GET_A(instr), b = REG_GET_B(instr), c = REG_GET_C(instr);
        if (R[a].type == VAL_ARRAY) {
            if (R[b].type == VAL_INT) {
                int64_t idx = R[b].as.int_val;
                if (idx < 0) idx += (int64_t)R[a].as.array.len;
                if (idx >= 0 && (size_t)idx < R[a].as.array.len) {
                    value_free(&R[a].as.array.elems[idx]);
                    R[a].as.array.elems[idx] = rvm_clone(&R[c]);
                }
            }
        } else if (R[a].type == VAL_MAP) {
            if (R[b].type == VAL_STR) {
                LatValue cloned = rvm_clone(&R[c]);
                lat_map_set(R[a].as.map.map, R[b].as.str_val, &cloned);
            }
        }
        DISPATCH();
    }

    CASE(INVOKE_GLOBAL) {
        /* Two-instruction sequence:
         *   INVOKE_GLOBAL dst, name_ki, argc
         *   data:         method_ki, args_base, 0
         * Mutates the global value in-place (for push/pop/etc). */
        uint8_t dst = REG_GET_A(instr);
        uint8_t name_ki = REG_GET_B(instr);
        uint8_t argc = REG_GET_C(instr);

        RegInstr data = *frame->ip++;
        uint8_t method_ki = REG_GET_A(data);
        uint8_t args_base = REG_GET_B(data);

        const char *global_name = frame->chunk->constants[name_ki].as.str_val;
        const char *method_name = frame->chunk->constants[method_ki].as.str_val;

        /* Get a direct reference to the global value */
        LatValue *obj_ref = env_get_ref(vm->env, global_name);
        if (!obj_ref) {
            RVM_ERROR("undefined variable '%s'", global_name);
            DISPATCH();
        }

        /* Try builtin — mutates obj_ref in-place */
        LatValue invoke_result;
        LatValue *invoke_args = (argc > 0) ? &R[args_base] : NULL;
        if (rvm_invoke_builtin(vm, obj_ref, method_name, invoke_args, argc, &invoke_result)) {
            if (vm->error)
                return REGVM_RUNTIME_ERROR;
            reg_set(&R[dst], invoke_result);
            DISPATCH();
        }

        /* Check for callable closure field in struct */
        if (obj_ref->type == VAL_STRUCT) {
            for (size_t fi = 0; fi < obj_ref->as.strct.field_count; fi++) {
                if (strcmp(obj_ref->as.strct.field_names[fi], method_name) != 0)
                    continue;
                LatValue *field = &obj_ref->as.strct.field_values[fi];
                if (field->type == VAL_CLOSURE) {
                    /* Copy object into a temp register, then proceed like normal INVOKE */
                    uint8_t tmp = args_base > 0 ? args_base - 1 : dst;
                    reg_set(&R[tmp], rvm_clone(obj_ref));
                    /* Fall through to closure call logic similar to INVOKE */
                    LatValue closure = rvm_clone(field);
                    if (closure.as.closure.body == NULL &&
                        closure.as.closure.native_fn != NULL &&
                        closure.as.closure.default_values != VM_NATIVE_MARKER) {
                        /* Compiled closure */
                        RegChunk *fn_chunk = (RegChunk *)closure.as.closure.native_fn;
                        if (vm->frame_count >= REGVM_FRAMES_MAX) {
                            value_free(&closure);
                            RVM_ERROR("stack overflow");
                            DISPATCH();
                        }
                        LatValue *new_regs = &vm->reg_stack[vm->reg_stack_top];
                        vm->reg_stack_top += REGVM_REG_MAX;
                        for (int ri = 0; ri < REGVM_REG_MAX; ri++)
                            new_regs[ri] = value_nil();

                        /* Slot 0 = reserved, slot 1 = self, slots 2+ = args */
                        new_regs[0] = value_unit();
                        new_regs[1] = rvm_clone(obj_ref);  /* self = first param */
                        value_free(&closure);
                        /* Copy args into param slots */
                        for (int ai = 0; ai < argc && ai + 2 < REGVM_REG_MAX; ai++)
                            new_regs[ai + 2] = rvm_clone(&R[args_base + ai]);

                        RegCallFrame *nf = &vm->frames[vm->frame_count++];
                        nf->chunk = fn_chunk;
                        nf->ip = fn_chunk->code;
                        nf->regs = new_regs;
                        nf->reg_count = REGVM_REG_MAX;
                        nf->caller_result_reg = dst;

                        ObjUpvalue **upvals = (ObjUpvalue **)closure.as.closure.captured_env;
                        size_t uv_count = closure.region_id;
                        nf->upvalues = upvals;
                        nf->upvalue_count = uv_count;

                        frame = nf;
                        R = frame->regs;
                        DISPATCH();
                    }
                    value_free(&closure);
                }
                break;
            }
        }

        /* Fallback: copy global into temp, do regular invoke */
        {
            LatValue obj_copy = rvm_clone(obj_ref);
            LatValue fb_result;
            LatValue *fb_args = (argc > 0) ? &R[args_base] : NULL;
            if (rvm_invoke_builtin(vm, &obj_copy, method_name, fb_args, argc, &fb_result)) {
                /* Write back the mutated copy to the global */
                value_free(obj_ref);
                *obj_ref = obj_copy;
                reg_set(&R[dst], fb_result);
            } else {
                value_free(&obj_copy);
                reg_set(&R[dst], value_nil());
            }
        }
        DISPATCH();
    }

    CASE(HALT) {
        *result = value_unit();
        return REGVM_OK;
    }

#ifdef VM_USE_COMPUTED_GOTO
    /* End of computed goto block - should never reach here */
    RVM_ERROR("unreachable: fell through computed goto dispatch");
#else
        default:
            RVM_ERROR("unknown register opcode %d", REG_GET_OP(instr));
        } /* end switch */
    } /* end for */
#endif
}

RegVMResult regvm_run(RegVM *vm, RegChunk *chunk, LatValue *result) {
    /* Set up initial frame */
    RegCallFrame *frame = &vm->frames[0];
    frame->chunk = chunk;
    frame->ip = chunk->code;
    frame->regs = vm->reg_stack;
    frame->reg_count = REGVM_REG_MAX;
    frame->upvalues = NULL;
    frame->upvalue_count = 0;
    frame->caller_result_reg = 0;
    vm->frame_count = 1;
    vm->reg_stack_top = REGVM_REG_MAX;

    return regvm_dispatch(vm, 0, result);
}

/* REPL variant: reuses existing frame 0 registers (preserves globals/locals) */
RegVMResult regvm_run_repl(RegVM *vm, RegChunk *chunk, LatValue *result) {
    RegCallFrame *frame = &vm->frames[0];
    frame->chunk = chunk;
    frame->ip = chunk->code;
    /* Reuse existing regs — don't reset reg_stack */
    frame->regs = vm->reg_stack;
    frame->reg_count = REGVM_REG_MAX;
    /* Preserve upvalues from previous iterations */
    frame->caller_result_reg = 0;
    vm->frame_count = 1;
    vm->reg_stack_top = REGVM_REG_MAX;

    return regvm_dispatch(vm, 0, result);
}
