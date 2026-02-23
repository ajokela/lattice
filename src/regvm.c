#include "regvm.h"
#include "regopcode.h"
#include "runtime.h"
#include "value.h"
#include "env.h"
#include "stackvm.h"   /* For ObjUpvalue */
#include "channel.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "builtins.h"
#include "ext.h"
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
/* Extension function marker — same sentinel as stack VM */
#define VM_EXT_MARKER    ((struct Expr **)(uintptr_t)0x2)

/* ── RegChunk implementation ── */

RegChunk *regchunk_new(void) {
    RegChunk *c = calloc(1, sizeof(RegChunk));
    c->magic = REGCHUNK_MAGIC;
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
            v->as.closure.default_values != VM_NATIVE_MARKER &&
            v->as.closure.default_values != VM_EXT_MARKER) {
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
    free(c->param_phases);
    if (c->export_names) {
        for (size_t i = 0; i < c->export_count; i++)
            free(c->export_names[i]);
        free(c->export_names);
    }
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

void regvm_init(RegVM *vm, LatRuntime *rt) {
    memset(vm, 0, sizeof(RegVM));
    vm->fn_chunk_cap = 16;
    vm->fn_chunks = malloc(vm->fn_chunk_cap * sizeof(RegChunk *));
    vm->module_cache = NULL;
    vm->ephemeral = bump_arena_new();
    vm->rt = rt;
    vm->env = rt->env;
    vm->struct_meta = rt->struct_meta;
    /* Initialize register stack to nil */
    for (size_t i = 0; i < REGVM_REG_MAX * REGVM_FRAMES_MAX; i++) {
        vm->reg_stack[i] = value_nil();
    }
}

void regvm_free(RegVM *vm) {
    /* Don't free env/struct_meta — runtime owns them */
    for (size_t i = 0; i < vm->fn_chunk_count; i++)
        regchunk_free(vm->fn_chunks[i]);
    free(vm->fn_chunks);
    free(vm->error);
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
    /* Reactions, bonds, seeds are owned by LatRuntime — not freed here */
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
                src->as.closure.default_values != VM_NATIVE_MARKER &&
                src->as.closure.default_values != VM_EXT_MARKER) {
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

    /* If there's an active handler, pass raw message to catch variable */
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
        reg_set(&(*R_ptr)[h.error_reg], value_string_owned(inner));
        return REGVM_OK;
    }

    /* Uncaught error: prepend line info */
    RegCallFrame *f = &vm->frames[vm->frame_count - 1];
    int line = 0;
    if (f->ip > f->chunk->code) {
        size_t offset = (size_t)(f->ip - f->chunk->code - 1);
        if (offset < f->chunk->lines_len)
            line = f->chunk->lines[offset];
    }
    if (line > 0) {
        char *msg;
        (void)asprintf(&msg, "[line %d] %s", line, inner);
        free(inner);
        vm->error = msg;
    } else {
        vm->error = inner;
    }
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

    RegVMResult res = regvm_dispatch(vm, saved_base, result);

    /* Clean up any frames left by HALT (which doesn't pop the frame) */
    while (vm->frame_count > saved_base) {
        RegCallFrame *f = &vm->frames[vm->frame_count - 1];
        for (int i = 0; i < REGVM_REG_MAX; i++)
            value_free_inline(&f->regs[i]);
        vm->frame_count--;
        vm->reg_stack_top -= REGVM_REG_MAX;
    }

    return res;
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
            if (value_is_crystal(obj)) {
                vm->error = strdup("cannot push to a crystal array");
                *result = value_unit();
                return true;
            }
            if (obj->phase == VTAG_SUBLIMATED) {
                vm->error = strdup("cannot push to a sublimated array");
                *result = value_unit();
                return true;
            }
            /* Check pressure constraints */
            {
                if (vm->rt && vm->rt->pressure_count > 0) {
                    /* Find variable name for this object by checking register names */
                    RegCallFrame *cf = &vm->frames[vm->frame_count - 1];
                    if (cf->chunk && cf->chunk->local_names) {
                        for (size_t r = 0; r < cf->chunk->local_name_cap; r++) {
                            if (&cf->regs[r] == obj && cf->chunk->local_names[r] && cf->chunk->local_names[r][0]) {
                                for (size_t pi = 0; pi < vm->rt->pressure_count; pi++) {
                                    if (strcmp(vm->rt->pressures[pi].name, cf->chunk->local_names[r]) == 0) {
                                        const char *mode = vm->rt->pressures[pi].mode;
                                        if (strcmp(mode, "no_grow") == 0 || strcmp(mode, "no_resize") == 0) {
                                            (void)asprintf(&vm->error, "pressurized (%s): cannot push to '%s'",
                                                           mode, cf->chunk->local_names[r]);
                                            *result = value_unit();
                                            return true;
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
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
            if (value_is_crystal(obj)) {
                vm->error = strdup("cannot pop from a crystal array");
                *result = value_unit();
                return true;
            }
            if (obj->phase == VTAG_SUBLIMATED) {
                vm->error = strdup("cannot pop from a sublimated array");
                *result = value_unit();
                return true;
            }
            /* Check pressure constraints */
            {
                if (vm->rt && vm->rt->pressure_count > 0) {
                    RegCallFrame *cf = &vm->frames[vm->frame_count - 1];
                    if (cf->chunk && cf->chunk->local_names) {
                        for (size_t r = 0; r < cf->chunk->local_name_cap; r++) {
                            if (&cf->regs[r] == obj && cf->chunk->local_names[r] && cf->chunk->local_names[r][0]) {
                                for (size_t pi = 0; pi < vm->rt->pressure_count; pi++) {
                                    if (strcmp(vm->rt->pressures[pi].name, cf->chunk->local_names[r]) == 0) {
                                        const char *mode = vm->rt->pressures[pi].mode;
                                        if (strcmp(mode, "no_shrink") == 0 || strcmp(mode, "no_resize") == 0) {
                                            (void)asprintf(&vm->error, "pressurized (%s): cannot pop from '%s'",
                                                           mode, cf->chunk->local_names[r]);
                                            *result = value_unit();
                                            return true;
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
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
                /* Mutate obj in place (like stack VM) */
                for (size_t i = 0; i < args[0].as.map.map->cap; i++) {
                    if (args[0].as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                    LatValue v = rvm_clone((LatValue *)args[0].as.map.map->entries[i].value);
                    lat_map_set(obj->as.map.map, args[0].as.map.map->entries[i].key, &v);
                }
            }
            *result = value_unit();
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
                        else if ((sorted[j].type == VAL_FLOAT || sorted[j].type == VAL_INT) &&
                                 (key.type == VAL_FLOAT || key.type == VAL_INT)) {
                            double a = sorted[j].type == VAL_FLOAT ? sorted[j].as.float_val : (double)sorted[j].as.int_val;
                            double b = key.type == VAL_FLOAT ? key.as.float_val : (double)key.as.int_val;
                            swap = a > b;
                        } else if (sorted[j].type == VAL_STR && key.type == VAL_STR) {
                            swap = strcmp(sorted[j].as.str_val, key.as.str_val) > 0;
                        } else {
                            for (size_t k = 0; k < len; k++) value_free(&sorted[k]);
                            free(sorted);
                            vm->error = strdup("sort: cannot compare values of different types");
                            *result = value_unit();
                            return true;
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
            LatValue *buf = malloc((len > 0 ? len : 1) * sizeof(LatValue));
            for (size_t i = 0; i < len; i++)
                buf[i] = rvm_clone(&obj->as.array.elems[i]);
            /* Insertion sort using comparator: closure(a, b) < 0 means a < b */
            for (size_t i = 1; i < len; i++) {
                LatValue key = buf[i];
                size_t j = i;
                while (j > 0) {
                    LatValue ca[2];
                    ca[0] = rvm_clone(&key);
                    ca[1] = rvm_clone(&buf[j - 1]);
                    LatValue cmp = regvm_call_closure(vm, closure, ca, 2);
                    value_free(&ca[0]); value_free(&ca[1]);
                    if (cmp.type != VAL_INT || cmp.as.int_val >= 0) { value_free(&cmp); break; }
                    value_free(&cmp);
                    buf[j] = buf[j - 1]; j--;
                }
                buf[j] = key;
            }
            *result = value_array(buf, len);
            free(buf);
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
            *result = value_unit();
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
            *result = obj->as.array.len > 0 ? rvm_clone(&obj->as.array.elems[0]) : value_unit();
            return true;
        }
        if (strcmp(method, "last") == 0 && arg_count == 0) {
            *result = obj->as.array.len > 0 ? rvm_clone(&obj->as.array.elems[obj->as.array.len - 1]) : value_unit();
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
            if (obj->as.array.len == 0) {
                vm->error = strdup("min() called on empty array");
                *result = value_unit();
                return true;
            }
            LatValue min_val = obj->as.array.elems[0];
            for (size_t i = 1; i < obj->as.array.len; i++) {
                if (obj->as.array.elems[i].type == VAL_INT && min_val.type == VAL_INT) {
                    if (obj->as.array.elems[i].as.int_val < min_val.as.int_val) min_val = obj->as.array.elems[i];
                } else if (obj->as.array.elems[i].type == VAL_FLOAT || min_val.type == VAL_FLOAT) {
                    double a = obj->as.array.elems[i].type == VAL_FLOAT ? obj->as.array.elems[i].as.float_val : (double)obj->as.array.elems[i].as.int_val;
                    double b = min_val.type == VAL_FLOAT ? min_val.as.float_val : (double)min_val.as.int_val;
                    if (a < b) min_val = obj->as.array.elems[i];
                }
            }
            *result = rvm_clone(&min_val);
            return true;
        }
        if (strcmp(method, "max") == 0 && arg_count == 0) {
            if (obj->as.array.len == 0) {
                vm->error = strdup("max() called on empty array");
                *result = value_unit();
                return true;
            }
            LatValue max_val = obj->as.array.elems[0];
            for (size_t i = 1; i < obj->as.array.len; i++) {
                if (obj->as.array.elems[i].type == VAL_INT && max_val.type == VAL_INT) {
                    if (obj->as.array.elems[i].as.int_val > max_val.as.int_val) max_val = obj->as.array.elems[i];
                } else if (obj->as.array.elems[i].type == VAL_FLOAT || max_val.type == VAL_FLOAT) {
                    double a = obj->as.array.elems[i].type == VAL_FLOAT ? obj->as.array.elems[i].as.float_val : (double)obj->as.array.elems[i].as.int_val;
                    double b = max_val.type == VAL_FLOAT ? max_val.as.float_val : (double)max_val.as.int_val;
                    if (a > b) max_val = obj->as.array.elems[i];
                }
            }
            *result = rvm_clone(&max_val);
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

    /* ── Array additional methods ── */
    if (obj->type == VAL_ARRAY) {
        if (strcmp(method, "flat") == 0 && arg_count == 0) {
            size_t cap = obj->as.array.len * 2;
            if (cap == 0) cap = 1;
            LatValue *elems = malloc(cap * sizeof(LatValue));
            size_t out = 0;
            for (size_t i = 0; i < obj->as.array.len; i++) {
                LatValue *el = &obj->as.array.elems[i];
                if (el->type == VAL_ARRAY) {
                    for (size_t j = 0; j < el->as.array.len; j++) {
                        if (out >= cap) { cap *= 2; elems = realloc(elems, cap * sizeof(LatValue)); }
                        elems[out++] = rvm_clone(&el->as.array.elems[j]);
                    }
                } else {
                    if (out >= cap) { cap *= 2; elems = realloc(elems, cap * sizeof(LatValue)); }
                    elems[out++] = rvm_clone(el);
                }
            }
            *result = value_array(elems, out);
            free(elems);
            return true;
        }
        if (strcmp(method, "first") == 0 && arg_count == 0) {
            *result = (obj->as.array.len > 0) ? rvm_clone(&obj->as.array.elems[0]) : value_unit();
            return true;
        }
        if (strcmp(method, "last") == 0 && arg_count == 0) {
            *result = (obj->as.array.len > 0) ? rvm_clone(&obj->as.array.elems[obj->as.array.len - 1]) : value_unit();
            return true;
        }
        if (strcmp(method, "min") == 0 && arg_count == 0) {
            if (obj->as.array.len == 0) { *result = value_unit(); return true; }
            LatValue best = rvm_clone(&obj->as.array.elems[0]);
            for (size_t i = 1; i < obj->as.array.len; i++) {
                LatValue *el = &obj->as.array.elems[i];
                bool less = false;
                if (el->type == VAL_INT && best.type == VAL_INT)
                    less = el->as.int_val < best.as.int_val;
                else if (el->type == VAL_FLOAT || best.type == VAL_FLOAT) {
                    double a = el->type == VAL_FLOAT ? el->as.float_val : (double)el->as.int_val;
                    double b = best.type == VAL_FLOAT ? best.as.float_val : (double)best.as.int_val;
                    less = a < b;
                }
                if (less) { value_free(&best); best = rvm_clone(el); }
            }
            *result = best;
            return true;
        }
        if (strcmp(method, "max") == 0 && arg_count == 0) {
            if (obj->as.array.len == 0) { *result = value_unit(); return true; }
            LatValue best = rvm_clone(&obj->as.array.elems[0]);
            for (size_t i = 1; i < obj->as.array.len; i++) {
                LatValue *el = &obj->as.array.elems[i];
                bool greater = false;
                if (el->type == VAL_INT && best.type == VAL_INT)
                    greater = el->as.int_val > best.as.int_val;
                else if (el->type == VAL_FLOAT || best.type == VAL_FLOAT) {
                    double a = el->type == VAL_FLOAT ? el->as.float_val : (double)el->as.int_val;
                    double b = best.type == VAL_FLOAT ? best.as.float_val : (double)best.as.int_val;
                    greater = a > b;
                }
                if (greater) { value_free(&best); best = rvm_clone(el); }
            }
            *result = best;
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
            if (obj->as.enm.payload_count > 0) {
                LatValue *elems = malloc(obj->as.enm.payload_count * sizeof(LatValue));
                for (size_t pi = 0; pi < obj->as.enm.payload_count; pi++)
                    elems[pi] = rvm_clone(&obj->as.enm.payload[pi]);
                *result = value_array(elems, obj->as.enm.payload_count);
                free(elems);
            } else {
                *result = value_array(NULL, 0);
            }
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
        if (strcmp(method, "add") == 0 && arg_count == 1) {
            char *key = value_display(&args[0]);
            LatValue val = rvm_clone(&args[0]);
            lat_map_set(obj->as.set.map, key, &val);
            free(key);
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "remove") == 0 && arg_count == 1) {
            char *key = value_display(&args[0]);
            lat_map_remove(obj->as.set.map, key);
            free(key);
            *result = value_unit();
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
        if (strcmp(method, "union") == 0 && arg_count == 1 && args[0].type == VAL_SET) {
            LatValue result_set = value_set_new();
            for (size_t i = 0; i < obj->as.set.map->cap; i++) {
                if (obj->as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                LatValue v = rvm_clone((LatValue *)obj->as.set.map->entries[i].value);
                lat_map_set(result_set.as.set.map, obj->as.set.map->entries[i].key, &v);
            }
            for (size_t i = 0; i < args[0].as.set.map->cap; i++) {
                if (args[0].as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                LatValue v = rvm_clone((LatValue *)args[0].as.set.map->entries[i].value);
                lat_map_set(result_set.as.set.map, args[0].as.set.map->entries[i].key, &v);
            }
            *result = result_set;
            return true;
        }
        if (strcmp(method, "intersection") == 0 && arg_count == 1 && args[0].type == VAL_SET) {
            LatValue result_set = value_set_new();
            for (size_t i = 0; i < obj->as.set.map->cap; i++) {
                if (obj->as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                if (lat_map_contains(args[0].as.set.map, obj->as.set.map->entries[i].key)) {
                    LatValue v = rvm_clone((LatValue *)obj->as.set.map->entries[i].value);
                    lat_map_set(result_set.as.set.map, obj->as.set.map->entries[i].key, &v);
                }
            }
            *result = result_set;
            return true;
        }
        if (strcmp(method, "difference") == 0 && arg_count == 1 && args[0].type == VAL_SET) {
            LatValue result_set = value_set_new();
            for (size_t i = 0; i < obj->as.set.map->cap; i++) {
                if (obj->as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                if (!lat_map_contains(args[0].as.set.map, obj->as.set.map->entries[i].key)) {
                    LatValue v = rvm_clone((LatValue *)obj->as.set.map->entries[i].value);
                    lat_map_set(result_set.as.set.map, obj->as.set.map->entries[i].key, &v);
                }
            }
            *result = result_set;
            return true;
        }
        if (strcmp(method, "is_subset") == 0 && arg_count == 1 && args[0].type == VAL_SET) {
            bool is = true;
            for (size_t i = 0; i < obj->as.set.map->cap; i++) {
                if (obj->as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                if (!lat_map_contains(args[0].as.set.map, obj->as.set.map->entries[i].key)) {
                    is = false; break;
                }
            }
            *result = value_bool(is);
            return true;
        }
        if (strcmp(method, "is_superset") == 0 && arg_count == 1 && args[0].type == VAL_SET) {
            bool is = true;
            for (size_t i = 0; i < args[0].as.set.map->cap; i++) {
                if (args[0].as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                if (!lat_map_contains(obj->as.set.map, args[0].as.set.map->entries[i].key)) {
                    is = false; break;
                }
            }
            *result = value_bool(is);
            return true;
        }
    }

    /* ── String additional: count, is_empty ── */
    if (obj->type == VAL_STR) {
        if (strcmp(method, "count") == 0 && arg_count == 1) {
            int64_t cnt = 0;
            if (args[0].type == VAL_STR && args[0].as.str_val[0]) {
                const char *p = obj->as.str_val;
                size_t nlen = strlen(args[0].as.str_val);
                while ((p = strstr(p, args[0].as.str_val)) != NULL) { cnt++; p += nlen; }
            }
            *result = value_int(cnt);
            return true;
        }
        if (strcmp(method, "is_empty") == 0 && arg_count == 0) {
            *result = value_bool(obj->as.str_val[0] == '\0');
            return true;
        }
    }

    /* ── Map additional: remove/delete ── */
    if (obj->type == VAL_MAP) {
        if ((strcmp(method, "remove") == 0 || strcmp(method, "delete") == 0) && arg_count == 1) {
            if (args[0].type == VAL_STR)
                lat_map_remove(obj->as.map.map, args[0].as.str_val);
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "map") == 0 && arg_count == 1) {
            LatValue *closure = &args[0];
            LatValue mapped = value_map_new();
            for (size_t i = 0; i < obj->as.map.map->cap; i++) {
                if (obj->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                LatValue cb_args[2];
                cb_args[0] = value_string(obj->as.map.map->entries[i].key);
                cb_args[1] = rvm_clone((LatValue *)obj->as.map.map->entries[i].value);
                LatValue ret = regvm_call_closure(vm, closure, cb_args, 2);
                lat_map_set(mapped.as.map.map, obj->as.map.map->entries[i].key, &ret);
                value_free(&cb_args[0]);
                value_free(&cb_args[1]);
            }
            *result = mapped;
            return true;
        }
    }

    /* ── Buffer methods ── */
    if (obj->type == VAL_BUFFER) {
        if (strcmp(method, "len") == 0 && arg_count == 0) {
            *result = value_int((int64_t)obj->as.buffer.len);
            return true;
        }
        if (strcmp(method, "capacity") == 0 && arg_count == 0) {
            *result = value_int((int64_t)obj->as.buffer.cap);
            return true;
        }
        if (strcmp(method, "push") == 0 && arg_count == 1) {
            if (args[0].type == VAL_INT) {
                if (obj->as.buffer.len >= obj->as.buffer.cap) {
                    obj->as.buffer.cap = obj->as.buffer.cap ? obj->as.buffer.cap * 2 : 8;
                    obj->as.buffer.data = realloc(obj->as.buffer.data, obj->as.buffer.cap);
                }
                obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)args[0].as.int_val;
            }
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "push_u16") == 0 && arg_count == 1) {
            if (args[0].type == VAL_INT) {
                uint16_t v = (uint16_t)args[0].as.int_val;
                while (obj->as.buffer.len + 2 > obj->as.buffer.cap) {
                    obj->as.buffer.cap = obj->as.buffer.cap ? obj->as.buffer.cap * 2 : 8;
                    obj->as.buffer.data = realloc(obj->as.buffer.data, obj->as.buffer.cap);
                }
                obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)(v & 0xFF);
                obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)((v >> 8) & 0xFF);
            }
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "push_u32") == 0 && arg_count == 1) {
            if (args[0].type == VAL_INT) {
                uint32_t v = (uint32_t)args[0].as.int_val;
                while (obj->as.buffer.len + 4 > obj->as.buffer.cap) {
                    obj->as.buffer.cap = obj->as.buffer.cap ? obj->as.buffer.cap * 2 : 8;
                    obj->as.buffer.data = realloc(obj->as.buffer.data, obj->as.buffer.cap);
                }
                obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)(v & 0xFF);
                obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)((v >> 8) & 0xFF);
                obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)((v >> 16) & 0xFF);
                obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)((v >> 24) & 0xFF);
            }
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "read_u8") == 0 && arg_count == 1) {
            if (args[0].type == VAL_INT) {
                int64_t idx = args[0].as.int_val;
                if (idx < 0 || (size_t)idx >= obj->as.buffer.len) {
                    *result = value_nil();
                } else {
                    *result = value_int((int64_t)obj->as.buffer.data[idx]);
                }
            } else {
                *result = value_nil();
            }
            return true;
        }
        if (strcmp(method, "write_u8") == 0 && arg_count == 2) {
            if (args[0].type == VAL_INT && args[1].type == VAL_INT) {
                int64_t idx = args[0].as.int_val;
                if (idx >= 0 && (size_t)idx < obj->as.buffer.len)
                    obj->as.buffer.data[idx] = (uint8_t)args[1].as.int_val;
            }
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "read_u16") == 0 && arg_count == 1) {
            if (args[0].type == VAL_INT) {
                int64_t idx = args[0].as.int_val;
                if (idx < 0 || (size_t)idx + 2 > obj->as.buffer.len) {
                    *result = value_nil();
                } else {
                    uint16_t v = (uint16_t)(obj->as.buffer.data[idx]) |
                                 ((uint16_t)(obj->as.buffer.data[idx + 1]) << 8);
                    *result = value_int((int64_t)v);
                }
            } else {
                *result = value_nil();
            }
            return true;
        }
        if (strcmp(method, "write_u16") == 0 && arg_count == 2) {
            if (args[0].type == VAL_INT && args[1].type == VAL_INT) {
                int64_t idx = args[0].as.int_val;
                uint16_t v = (uint16_t)args[1].as.int_val;
                if (idx >= 0 && (size_t)idx + 1 < obj->as.buffer.len) {
                    obj->as.buffer.data[idx] = (uint8_t)(v & 0xFF);
                    obj->as.buffer.data[idx + 1] = (uint8_t)(v >> 8);
                }
            }
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "read_u32") == 0 && arg_count == 1) {
            if (args[0].type == VAL_INT) {
                int64_t idx = args[0].as.int_val;
                if (idx < 0 || (size_t)idx + 4 > obj->as.buffer.len) {
                    *result = value_nil();
                } else {
                    uint32_t v = (uint32_t)(obj->as.buffer.data[idx]) |
                                 ((uint32_t)(obj->as.buffer.data[idx + 1]) << 8) |
                                 ((uint32_t)(obj->as.buffer.data[idx + 2]) << 16) |
                                 ((uint32_t)(obj->as.buffer.data[idx + 3]) << 24);
                    *result = value_int((int64_t)v);
                }
            } else {
                *result = value_nil();
            }
            return true;
        }
        if (strcmp(method, "write_u32") == 0 && arg_count == 2) {
            if (args[0].type == VAL_INT && args[1].type == VAL_INT) {
                int64_t idx = args[0].as.int_val;
                uint32_t v = (uint32_t)args[1].as.int_val;
                if (idx >= 0 && (size_t)idx + 3 < obj->as.buffer.len) {
                    obj->as.buffer.data[idx] = (uint8_t)(v & 0xFF);
                    obj->as.buffer.data[idx + 1] = (uint8_t)((v >> 8) & 0xFF);
                    obj->as.buffer.data[idx + 2] = (uint8_t)((v >> 16) & 0xFF);
                    obj->as.buffer.data[idx + 3] = (uint8_t)((v >> 24) & 0xFF);
                }
            }
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "slice") == 0 && (arg_count == 1 || arg_count == 2)) {
            int64_t start = args[0].type == VAL_INT ? args[0].as.int_val : 0;
            int64_t end = arg_count == 2 && args[1].type == VAL_INT ? args[1].as.int_val : (int64_t)obj->as.buffer.len;
            if (start < 0) start = 0;
            if (end > (int64_t)obj->as.buffer.len) end = (int64_t)obj->as.buffer.len;
            if (start >= end) { *result = value_buffer(NULL, 0); return true; }
            *result = value_buffer(obj->as.buffer.data + start, (size_t)(end - start));
            return true;
        }
        if (strcmp(method, "clear") == 0 && arg_count == 0) {
            obj->as.buffer.len = 0;
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "fill") == 0 && arg_count == 1) {
            if (args[0].type == VAL_INT)
                memset(obj->as.buffer.data, (uint8_t)args[0].as.int_val, obj->as.buffer.len);
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "resize") == 0 && arg_count == 1) {
            if (args[0].type == VAL_INT && args[0].as.int_val >= 0) {
                size_t new_len = (size_t)args[0].as.int_val;
                if (new_len > obj->as.buffer.cap) {
                    obj->as.buffer.cap = new_len;
                    obj->as.buffer.data = realloc(obj->as.buffer.data, new_len);
                }
                if (new_len > obj->as.buffer.len)
                    memset(obj->as.buffer.data + obj->as.buffer.len, 0, new_len - obj->as.buffer.len);
                obj->as.buffer.len = new_len;
            }
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "to_string") == 0 && arg_count == 0) {
            char *s = malloc(obj->as.buffer.len + 1);
            memcpy(s, obj->as.buffer.data, obj->as.buffer.len);
            s[obj->as.buffer.len] = '\0';
            *result = value_string_owned(s);
            return true;
        }
        if (strcmp(method, "to_array") == 0 && arg_count == 0) {
            LatValue *elems = malloc((obj->as.buffer.len > 0 ? obj->as.buffer.len : 1) * sizeof(LatValue));
            for (size_t i = 0; i < obj->as.buffer.len; i++)
                elems[i] = value_int((int64_t)obj->as.buffer.data[i]);
            *result = value_array(elems, obj->as.buffer.len);
            free(elems);
            return true;
        }
        if (strcmp(method, "to_hex") == 0 && arg_count == 0) {
            char *hex = malloc(obj->as.buffer.len * 2 + 1);
            for (size_t i = 0; i < obj->as.buffer.len; i++)
                snprintf(hex + i * 2, 3, "%02x", obj->as.buffer.data[i]);
            hex[obj->as.buffer.len * 2] = '\0';
            *result = value_string_owned(hex);
            return true;
        }
    }

    /* ── Ref methods ── */
    if (obj->type == VAL_REF) {
        LatRef *ref = obj->as.ref.ref;
        if ((strcmp(method, "get") == 0 || strcmp(method, "deref") == 0) && arg_count == 0) {
            *result = value_deep_clone(&ref->value);
            return true;
        }
        if (strcmp(method, "set") == 0 && arg_count == 1 && arg_count == 1) {
            if (obj->phase == VTAG_CRYSTAL) {
                *result = value_unit();
                return true;
            }
            value_free(&ref->value);
            ref->value = rvm_clone(&args[0]);
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "inner_type") == 0 && arg_count == 0) {
            *result = value_string(value_type_name(&ref->value));
            return true;
        }
        /* Proxy: delegate to inner value's methods if applicable */
        if (ref->value.type == VAL_MAP) {
            if (strcmp(method, "get") == 0 && arg_count == 1 && args[0].type == VAL_STR) {
                LatValue *val = lat_map_get(ref->value.as.map.map, args[0].as.str_val);
                *result = val ? value_deep_clone(val) : value_nil();
                return true;
            }
            if (strcmp(method, "set") == 0 && arg_count == 2 && args[0].type == VAL_STR) {
                if (obj->phase != VTAG_CRYSTAL) {
                    LatValue cloned = rvm_clone(&args[1]);
                    lat_map_set(ref->value.as.map.map, args[0].as.str_val, &cloned);
                }
                *result = value_unit();
                return true;
            }
            if ((strcmp(method, "has") == 0 || strcmp(method, "contains") == 0) && arg_count == 1 && args[0].type == VAL_STR) {
                *result = value_bool(lat_map_get(ref->value.as.map.map, args[0].as.str_val) != NULL);
                return true;
            }
            if (strcmp(method, "keys") == 0 && arg_count == 0) {
                size_t cap = ref->value.as.map.map->cap;
                LatValue *keys = malloc(cap * sizeof(LatValue));
                size_t cnt = 0;
                for (size_t i = 0; i < cap; i++) {
                    if (ref->value.as.map.map->entries[i].state == MAP_OCCUPIED)
                        keys[cnt++] = value_string(ref->value.as.map.map->entries[i].key);
                }
                *result = value_array(keys, cnt);
                free(keys);
                return true;
            }
            if (strcmp(method, "values") == 0 && arg_count == 0) {
                size_t cap = ref->value.as.map.map->cap;
                LatValue *vals = malloc(cap * sizeof(LatValue));
                size_t cnt = 0;
                for (size_t i = 0; i < cap; i++) {
                    if (ref->value.as.map.map->entries[i].state == MAP_OCCUPIED)
                        vals[cnt++] = value_deep_clone((LatValue *)ref->value.as.map.map->entries[i].value);
                }
                *result = value_array(vals, cnt);
                free(vals);
                return true;
            }
            if (strcmp(method, "entries") == 0 && arg_count == 0) {
                size_t cap = ref->value.as.map.map->cap;
                LatValue *entries = malloc(cap * sizeof(LatValue));
                size_t cnt = 0;
                for (size_t i = 0; i < cap; i++) {
                    if (ref->value.as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                    LatValue pair[2];
                    pair[0] = value_string(ref->value.as.map.map->entries[i].key);
                    pair[1] = value_deep_clone((LatValue *)ref->value.as.map.map->entries[i].value);
                    entries[cnt++] = value_array(pair, 2);
                }
                *result = value_array(entries, cnt);
                free(entries);
                return true;
            }
            if (strcmp(method, "len") == 0 && arg_count == 0) {
                *result = value_int((int64_t)lat_map_len(ref->value.as.map.map));
                return true;
            }
            if (strcmp(method, "merge") == 0 && arg_count == 1 && args[0].type == VAL_MAP) {
                if (obj->phase != VTAG_CRYSTAL) {
                    for (size_t i = 0; i < args[0].as.map.map->cap; i++) {
                        if (args[0].as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                        LatValue v = rvm_clone((LatValue *)args[0].as.map.map->entries[i].value);
                        lat_map_set(ref->value.as.map.map, args[0].as.map.map->entries[i].key, &v);
                    }
                }
                *result = value_unit();
                return true;
            }
        }
        if (ref->value.type == VAL_ARRAY) {
            if (strcmp(method, "push") == 0 && arg_count == 1) {
                LatValue val = rvm_clone(&args[0]);
                if (ref->value.as.array.len >= ref->value.as.array.cap) {
                    ref->value.as.array.cap = ref->value.as.array.cap ? ref->value.as.array.cap * 2 : 4;
                    ref->value.as.array.elems = realloc(ref->value.as.array.elems,
                        ref->value.as.array.cap * sizeof(LatValue));
                }
                ref->value.as.array.elems[ref->value.as.array.len++] = val;
                *result = value_unit();
                return true;
            }
            if (strcmp(method, "pop") == 0 && arg_count == 0) {
                if (ref->value.as.array.len == 0) { *result = value_nil(); }
                else { *result = ref->value.as.array.elems[--ref->value.as.array.len]; }
                return true;
            }
            if (strcmp(method, "len") == 0 && arg_count == 0) {
                *result = value_int((int64_t)ref->value.as.array.len);
                return true;
            }
            if (strcmp(method, "contains") == 0 && arg_count == 1) {
                bool found = false;
                for (size_t i = 0; i < ref->value.as.array.len; i++) {
                    if (value_eq(&ref->value.as.array.elems[i], &args[0])) { found = true; break; }
                }
                *result = value_bool(found);
                return true;
            }
        }
    }

    /* ── Channel methods ── */
    if (obj->type == VAL_CHANNEL) {
        if (strcmp(method, "send") == 0 && arg_count == 1) {
            if (!value_is_crystal(&args[0]) && args[0].phase != VTAG_UNPHASED) {
                vm->error = strdup("channel send requires crystal or unphased values");
                *result = value_unit();
                return true;
            }
            LatValue val = rvm_clone(&args[0]);
            channel_send(obj->as.channel.ch, val);
            *result = value_unit();
            return true;
        }
        if (strcmp(method, "recv") == 0 && arg_count == 0) {
            bool ok = false;
            *result = channel_recv(obj->as.channel.ch, &ok);
            if (!ok) *result = value_unit();
            return true;
        }
        if (strcmp(method, "close") == 0 && arg_count == 0) {
            channel_close(obj->as.channel.ch);
            *result = value_unit();
            return true;
        }
    }

    return false;
}

/* Phase system functions (regvm_fire_reactions, regvm_freeze_cascade,
 * regvm_validate_seeds, regvm_sync_env_to_register) have been moved to
 * runtime.c as rt_* functions. */

/* ── VM Dispatch Loop ── */

/* Native function type (same as stack VM) */
typedef LatValue (*VMNativeFn)(LatValue *args, int arg_count);

/* Call a closure from within a builtin handler (map, filter, etc.). */
static LatValue regvm_call_closure(RegVM *vm, LatValue *closure, LatValue *args, int argc) {
    if (closure->type != VAL_CLOSURE) return value_nil();

    /* Check for native C function */
    if (closure->as.closure.default_values == VM_NATIVE_MARKER) {
        VMNativeFn native = (VMNativeFn)closure->as.closure.native_fn;
        LatValue ret = native(args, argc);
        /* Check runtime for native errors — propagate to regvm */
        if (vm->rt->error) {
            vm->error = vm->rt->error;
            vm->rt->error = NULL;
            value_free(&ret);
            return value_nil();
        }
        return ret;
    }

    /* Extension native function */
    if (closure->as.closure.default_values == VM_EXT_MARKER) {
        LatValue ret = ext_call_native(closure->as.closure.native_fn, args, (size_t)argc);
        if (ret.type == VAL_STR && ret.as.str_val &&
            strncmp(ret.as.str_val, "EVAL_ERROR:", 11) == 0) {
            vm->error = strdup(ret.as.str_val + 11);
            value_free(&ret);
            return value_nil();
        }
        return ret;
    }

    RegChunk *fn_chunk = (RegChunk *)closure->as.closure.native_fn;
    if (!fn_chunk) return value_nil();

    /* Guard: detect stack-VM closures that can't run in regvm */
    if (fn_chunk->magic != REGCHUNK_MAGIC) return value_nil();

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
    if (res != REGVM_OK) {
        /* Unwind any frames left by the failed dispatch back to saved_base */
        while (vm->frame_count > saved_base) {
            RegCallFrame *uf = &vm->frames[vm->frame_count - 1];
            for (int i = 0; i < REGVM_REG_MAX; i++)
                value_free_inline(&uf->regs[i]);
            vm->frame_count--;
            vm->reg_stack_top -= REGVM_REG_MAX;
        }
        /* Propagate vm->error to rt->error so runtime-level callers
         * (e.g. rt_fire_reactions) can see and wrap the error */
        if (vm->error && !vm->rt->error) {
            vm->rt->error = vm->error;
            vm->error = NULL;
        }
        return value_nil();
    }
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
        /* Phase query */
        [ROP_IS_CRYSTAL]   = &&L_IS_CRYSTAL,
        /* Type checking */
        [ROP_CHECK_TYPE]   = &&L_CHECK_TYPE,
        [ROP_FREEZE_FIELD] = &&L_FREEZE_FIELD,
        [ROP_THAW_FIELD]   = &&L_THAW_FIELD,
        /* Require */
        [ROP_REQUIRE]      = &&L_REQUIRE,
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
        /* Record history for tracked variables */
        {
            if (vm->rt->tracking_active &&
                frame->chunk->local_names && a < frame->chunk->local_name_cap &&
                frame->chunk->local_names[a] && frame->chunk->local_names[a][0]) {
                rt_record_history(vm->rt, frame->chunk->local_names[a], &R[a]);
            }
        }
        DISPATCH();
    }

    CASE(LOADK) {
        uint8_t a = REG_GET_A(instr);
        uint16_t bx = REG_GET_Bx(instr);
        reg_set(&R[a], rvm_clone(&frame->chunk->constants[bx]));
        /* Record history for tracked variables */
        {
            if (vm->rt->tracking_active &&
                frame->chunk->local_names && a < frame->chunk->local_name_cap &&
                frame->chunk->local_names[a] && frame->chunk->local_names[a][0]) {
                rt_record_history(vm->rt, frame->chunk->local_names[a], &R[a]);
            }
        }
        DISPATCH();
    }

    CASE(LOADI) {
        uint8_t a = REG_GET_A(instr);
        int16_t sbx = REG_GET_sBx(instr);
        reg_set(&R[a], value_int((int64_t)sbx));
        /* Record history for tracked variables */
        {
            if (vm->rt->tracking_active &&
                frame->chunk->local_names && a < frame->chunk->local_name_cap &&
                frame->chunk->local_names[a] && frame->chunk->local_names[a][0]) {
                rt_record_history(vm->rt, frame->chunk->local_names[a], &R[a]);
            }
        }
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
        {
            if (vm->rt->tracking_active &&
                frame->chunk->local_names && a < frame->chunk->local_name_cap &&
                frame->chunk->local_names[a] && frame->chunk->local_names[a][0]) {
                rt_record_history(vm->rt, frame->chunk->local_names[a], &R[a]);
            }
        }
        DISPATCH();
    }

    CASE(LOADFALSE) {
        uint8_t a = REG_GET_A(instr);
        reg_set(&R[a], value_bool(false));
        {
            if (vm->rt->tracking_active &&
                frame->chunk->local_names && a < frame->chunk->local_name_cap &&
                frame->chunk->local_names[a] && frame->chunk->local_names[a][0]) {
                rt_record_history(vm->rt, frame->chunk->local_names[a], &R[a]);
            }
        }
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
            char *buf = bump_alloc(vm->ephemeral, lb + lc + 1);
            memcpy(buf, R[b].as.str_val, lb);
            memcpy(buf + lb, R[c].as.str_val, lc);
            buf[lb + lc] = '\0';
            LatValue v = { .type = VAL_STR, .phase = VTAG_UNPHASED, .region_id = REGION_EPHEMERAL };
            v.as.str_val = buf;
            reg_set(&R[a], v);
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
            double lv = R[b].type == VAL_FLOAT ? R[b].as.float_val : (double)R[b].as.int_val;
            reg_set(&R[a], value_float(lv / rv));  /* float div by zero → Inf/NaN */
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
        char *buf = bump_alloc(vm->ephemeral, ll + rl + 1);
        memcpy(buf, ls, ll);
        memcpy(buf + ll, rs, rl);
        buf[ll + rl] = '\0';
        free(ls); free(rs);
        LatValue v = { .type = VAL_STR, .phase = VTAG_UNPHASED, .region_id = REGION_EPHEMERAL };
        v.as.str_val = buf;
        reg_set(&R[a], v);
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
        /* Record history for tracked globals */
        {
            if (vm->rt->tracking_active)
                rt_record_history(vm->rt, name, &R[a]);
        }
        DISPATCH();
    }

    CASE(DEFINEGLOBAL) {
        uint8_t a = REG_GET_A(instr);
        uint16_t bx = REG_GET_Bx(instr);
        const char *name = frame->chunk->constants[bx].as.str_val;
        LatValue val = rvm_clone(&R[a]);

        /* Phase-dispatch overloading: if defining a phase-constrained
         * closure and one already exists, create an overload array */
        if (val.type == VAL_CLOSURE && val.as.closure.native_fn != NULL &&
            val.as.closure.default_values != VM_NATIVE_MARKER) {
            uint32_t magic;
            memcpy(&magic, val.as.closure.native_fn, sizeof(uint32_t));
            if (magic == REGCHUNK_MAGIC) {
                RegChunk *ch = (RegChunk *)val.as.closure.native_fn;
                if (ch->param_phases) {
                    LatValue existing;
                    if (env_get(vm->env, name, &existing)) {
                        if (existing.type == VAL_CLOSURE && existing.as.closure.native_fn != NULL &&
                            existing.as.closure.default_values != VM_NATIVE_MARKER) {
                            uint32_t emag;
                            memcpy(&emag, existing.as.closure.native_fn, sizeof(uint32_t));
                            if (emag == REGCHUNK_MAGIC) {
                                RegChunk *ech = (RegChunk *)existing.as.closure.native_fn;
                                if (ech->param_phases) {
                                    LatValue elems[2] = { value_deep_clone(&existing), val };
                                    LatValue arr = value_array(elems, 2);
                                    env_define(vm->env, name, arr);
                                    DISPATCH();
                                }
                            }
                        } else if (existing.type == VAL_ARRAY) {
                            size_t new_len = existing.as.array.len + 1;
                            LatValue *new_elems = malloc(new_len * sizeof(LatValue));
                            for (size_t i = 0; i < existing.as.array.len; i++)
                                new_elems[i] = value_deep_clone(&existing.as.array.elems[i]);
                            new_elems[existing.as.array.len] = val;
                            LatValue arr = value_array(new_elems, new_len);
                            free(new_elems);
                            env_define(vm->env, name, arr);
                            DISPATCH();
                        }
                    }
                }
            }
        }

        env_define(vm->env, name, val);
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
        } else if (R[b].type == VAL_TUPLE) {
            char *endp;
            long idx = strtol(field_name, &endp, 10);
            if (*endp == '\0' && idx >= 0 && (size_t)idx < R[b].as.tuple.len)
                reg_set(&R[a], rvm_clone(&R[b].as.tuple.elems[idx]));
            else
                RVM_ERROR("tuple has no field '%s'", field_name);
        } else if (R[b].type == VAL_ENUM) {
            if (strcmp(field_name, "tag") == 0 || strcmp(field_name, "variant_name") == 0)
                reg_set(&R[a], value_string(R[b].as.enm.variant_name));
            else if (strcmp(field_name, "enum_name") == 0)
                reg_set(&R[a], value_string(R[b].as.enm.enum_name));
            else if (strcmp(field_name, "payload") == 0) {
                if (R[b].as.enm.payload_count > 0) {
                    LatValue *elems = malloc(R[b].as.enm.payload_count * sizeof(LatValue));
                    for (size_t pi = 0; pi < R[b].as.enm.payload_count; pi++)
                        elems[pi] = rvm_clone(&R[b].as.enm.payload[pi]);
                    reg_set(&R[a], value_array(elems, R[b].as.enm.payload_count));
                    free(elems);
                } else {
                    reg_set(&R[a], value_array(NULL, 0));
                }
            } else
                RVM_ERROR("enum has no field '%s'", field_name);
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

        /* Phase checks */
        if (R[a].phase == VTAG_CRYSTAL) {
            /* Check per-field phases for structs with partial freeze (freeze except) */
            bool blocked = true;
            if (R[a].type == VAL_STRUCT && R[a].as.strct.field_phases) {
                for (size_t i = 0; i < R[a].as.strct.field_count; i++) {
                    if (strcmp(R[a].as.strct.field_names[i], field_name) == 0) {
                        if (R[a].as.strct.field_phases[i] != VTAG_CRYSTAL) blocked = false;
                        break;
                    }
                }
            }
            if (blocked)
                RVM_ERROR("cannot set field '%s' on a frozen value", field_name);
        }
        /* Also check per-field phases (alloy types) even on non-frozen structs */
        if (R[a].type == VAL_STRUCT && R[a].as.strct.field_phases &&
            R[a].phase != VTAG_CRYSTAL) {
            for (size_t i = 0; i < R[a].as.strct.field_count; i++) {
                if (strcmp(R[a].as.strct.field_names[i], field_name) == 0) {
                    if (R[a].as.strct.field_phases[i] == VTAG_CRYSTAL)
                        RVM_ERROR("cannot assign to frozen field '%s'", field_name);
                    break;
                }
            }
        }

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

        if (R[b].type == VAL_ARRAY && R[c].type == VAL_RANGE) {
            /* Array range slicing: arr[start..end] */
            int64_t start = R[c].as.range.start;
            int64_t end = R[c].as.range.end;
            size_t len = R[b].as.array.len;
            if (start < 0) start = 0;
            if ((size_t)start > len) start = (int64_t)len;
            if (end < 0) end = 0;
            if ((size_t)end > len) end = (int64_t)len;
            if (start >= end) {
                reg_set(&R[a], value_array(NULL, 0));
            } else {
                size_t slice_len = (size_t)(end - start);
                LatValue *elems = malloc(slice_len * sizeof(LatValue));
                for (size_t i = 0; i < slice_len; i++)
                    elems[i] = rvm_clone(&R[b].as.array.elems[start + (int64_t)i]);
                reg_set(&R[a], value_array(elems, slice_len));
                free(elems);
            }
        } else if (R[b].type == VAL_STR && R[c].type == VAL_RANGE) {
            /* String range slicing: str[start..end] */
            int64_t start = R[c].as.range.start;
            int64_t end = R[c].as.range.end;
            size_t len = strlen(R[b].as.str_val);
            if (start < 0) start = 0;
            if ((size_t)start > len) start = (int64_t)len;
            if (end < 0) end = 0;
            if ((size_t)end > len) end = (int64_t)len;
            if (start >= end) {
                reg_set(&R[a], value_string(""));
            } else {
                size_t slice_len = (size_t)(end - start);
                char *slice = malloc(slice_len + 1);
                memcpy(slice, R[b].as.str_val + start, slice_len);
                slice[slice_len] = '\0';
                reg_set(&R[a], value_string_owned(slice));
            }
        } else if (R[b].type == VAL_ARRAY) {
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
        } else if (R[b].type == VAL_BUFFER) {
            if (R[c].type != VAL_INT)
                RVM_ERROR("buffer index must be an integer");
            int64_t idx = R[c].as.int_val;
            if (idx < 0 || (size_t)idx >= R[b].as.buffer.len)
                RVM_ERROR("buffer index out of bounds");
            reg_set(&R[a], value_int((int64_t)R[b].as.buffer.data[idx]));
        } else if (R[b].type == VAL_REF) {
            /* Proxy indexing on Ref inner value */
            LatRef *ref = R[b].as.ref.ref;
            if (ref->value.type == VAL_MAP) {
                if (R[c].type != VAL_STR) RVM_ERROR("map key must be a string");
                LatValue *val = lat_map_get(ref->value.as.map.map, R[c].as.str_val);
                reg_set(&R[a], val ? rvm_clone(val) : value_nil());
            } else if (ref->value.type == VAL_ARRAY) {
                if (R[c].type != VAL_INT) RVM_ERROR("array index must be an integer");
                int64_t idx = R[c].as.int_val;
                if (idx < 0) idx += (int64_t)ref->value.as.array.len;
                if (idx < 0 || (size_t)idx >= ref->value.as.array.len)
                    RVM_ERROR("array index out of bounds");
                reg_set(&R[a], rvm_clone(&ref->value.as.array.elems[idx]));
            } else {
                RVM_ERROR("cannot index Ref(%s)", value_type_name(&ref->value));
            }
        } else {
            RVM_ERROR("cannot index %s", value_type_name(&R[b]));
        }
        DISPATCH();
    }

    CASE(SETINDEX) {
        uint8_t a = REG_GET_A(instr);  /* object */
        uint8_t b = REG_GET_B(instr);  /* index */
        uint8_t c = REG_GET_C(instr);  /* value */

        /* Phase checks for mutation */
        if (R[a].phase == VTAG_CRYSTAL) {
            /* Allow mutation on maps with per-key phases (freeze except) if key is not frozen */
            bool blocked = true;
            if (R[a].type == VAL_MAP && R[b].type == VAL_STR && R[a].as.map.key_phases) {
                PhaseTag *kp = lat_map_get(R[a].as.map.key_phases, R[b].as.str_val);
                if (!kp || *kp != VTAG_CRYSTAL) blocked = false;
            }
            if (blocked)
                RVM_ERROR("cannot modify a frozen value");
        }
        if (R[a].phase == VTAG_SUBLIMATED && R[a].type == VAL_MAP)
            RVM_ERROR("cannot add keys to a sublimated map");
        /* Per-key phase check for non-frozen maps */
        if (R[a].type == VAL_MAP && R[b].type == VAL_STR && R[a].as.map.key_phases) {
            PhaseTag *kp = lat_map_get(R[a].as.map.key_phases, R[b].as.str_val);
            if (kp && *kp == VTAG_CRYSTAL)
                RVM_ERROR("cannot modify frozen key '%s'", R[b].as.str_val);
        }

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
        } else if (R[a].type == VAL_BUFFER) {
            if (R[b].type != VAL_INT) RVM_ERROR("buffer index must be an integer");
            int64_t idx = R[b].as.int_val;
            if (idx < 0 || (size_t)idx >= R[a].as.buffer.len)
                RVM_ERROR("buffer index out of bounds");
            if (R[c].type != VAL_INT) RVM_ERROR("buffer value must be an integer");
            R[a].as.buffer.data[(size_t)idx] = (uint8_t)(R[c].as.int_val & 0xFF);
        } else if (R[a].type == VAL_REF) {
            /* Proxy: set index on inner value */
            LatRef *ref = R[a].as.ref.ref;
            if (ref->value.type == VAL_MAP) {
                if (R[b].type != VAL_STR) RVM_ERROR("map key must be a string");
                LatValue cloned = rvm_clone(&R[c]);
                lat_map_set(ref->value.as.map.map, R[b].as.str_val, &cloned);
            } else if (ref->value.type == VAL_ARRAY) {
                if (R[b].type != VAL_INT) RVM_ERROR("array index must be an integer");
                int64_t idx = R[b].as.int_val;
                if (idx < 0) idx += (int64_t)ref->value.as.array.len;
                if (idx < 0 || (size_t)idx >= ref->value.as.array.len)
                    RVM_ERROR("array index out of bounds");
                value_free(&ref->value.as.array.elems[idx]);
                ref->value.as.array.elems[idx] = rvm_clone(&R[c]);
            } else {
                RVM_ERROR("cannot set index on Ref(%s)", value_type_name(&ref->value));
            }
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

        /* Phase-dispatch overload resolution: VAL_ARRAY of closures */
        if (func->type == VAL_ARRAY) {
            int best_score = -1;
            int best_idx = -1;
            for (size_t ci = 0; ci < func->as.array.len; ci++) {
                LatValue *cand = &func->as.array.elems[ci];
                if (cand->type != VAL_CLOSURE || cand->as.closure.native_fn == NULL) continue;
                if (cand->as.closure.default_values == VM_NATIVE_MARKER) continue;
                uint32_t cmag;
                memcpy(&cmag, cand->as.closure.native_fn, sizeof(uint32_t));
                if (cmag != REGCHUNK_MAGIC) continue;
                RegChunk *ch = (RegChunk *)cand->as.closure.native_fn;
                if (!ch->param_phases) continue;
                bool compatible = true;
                int score = 0;
                for (int j = 0; j < ch->param_phase_count && j < (int)b; j++) {
                    uint8_t pp = ch->param_phases[j];
                    LatValue *arg = &R[a + 1 + j];
                    if (pp == PHASE_FLUID) {
                        if (arg->phase == VTAG_CRYSTAL) { compatible = false; break; }
                        if (arg->phase == VTAG_FLUID) score += 3;
                        else score += 1;
                    } else if (pp == PHASE_CRYSTAL) {
                        if (arg->phase == VTAG_FLUID) { compatible = false; break; }
                        if (arg->phase == VTAG_CRYSTAL) score += 3;
                        else score += 1;
                    } else {
                        if (arg->phase == VTAG_UNPHASED) score += 2;
                        else score += 1;
                    }
                }
                if (compatible && score > best_score) {
                    best_score = score;
                    best_idx = (int)ci;
                }
            }
            if (best_idx >= 0) {
                LatValue matched = value_deep_clone(&func->as.array.elems[best_idx]);
                reg_set(func, matched);
            } else {
                RVM_ERROR("no matching overload for given argument phases");
            }
        }

        if (func->type != VAL_CLOSURE)
            RVM_ERROR("attempt to call a non-function (%s)",
                value_type_name(func));

        /* Check for native function */
        if (func->as.closure.default_values == VM_NATIVE_MARKER) {
            VMNativeFn native = (VMNativeFn)func->as.closure.native_fn;

            /* Sync named locals from current call chain to env.
             * Needed for natives that access variables by name (track, react, bond, seed, etc.)
             * and for phase system operations that read env. */
            {
                for (int fi = 0; fi < vm->frame_count; fi++) {
                    RegCallFrame *sf = &vm->frames[fi];
                    if (!sf->chunk || !sf->chunk->local_names) continue;
                    for (size_t li = 0; li < sf->chunk->local_name_cap; li++) {
                        if (sf->chunk->local_names[li] && sf->chunk->local_names[li][0]) {
                            LatValue clone = value_deep_clone(&sf->regs[li]);
                            if (!env_set(vm->env, sf->chunk->local_names[li], clone))
                                env_define(vm->env, sf->chunk->local_names[li], clone);
                        }
                    }
                }
            }
            /* Collect args */
            LatValue args[16];
            for (int i = 0; i < b; i++)
                args[i] = rvm_clone(&R[a + 1 + i]);
            LatValue ret = native(args, b);
            for (int i = 0; i < b; i++)
                value_free(&args[i]);
            /* Check runtime for native errors */
            if (vm->rt->error) {
                char *err = vm->rt->error;
                vm->rt->error = NULL;
                value_free(&ret);
                RVM_ERROR("%s", err);
            }
            /* Also check if regvm itself got an error (from re-entrant dispatch callbacks) */
            if (vm->error) {
                value_free(&ret);
                return REGVM_RUNTIME_ERROR;
            }
            /* Note: no reverse env→locals sync here — it's too broad and can break
             * closure-captured values. grow() and similar natives that modify variables
             * by name will need specialized handling if needed. */
            reg_set(&R[a], ret);
            DISPATCH();
        }

        /* Extension native function (loaded via require_ext) */
        if (func->as.closure.default_values == VM_EXT_MARKER) {
            LatValue args[16];
            for (int i = 0; i < b; i++)
                args[i] = rvm_clone(&R[a + 1 + i]);
            LatValue ret = ext_call_native(func->as.closure.native_fn, args, (size_t)b);
            for (int i = 0; i < b; i++)
                value_free(&args[i]);
            /* Extension errors return strings prefixed with "EVAL_ERROR:" */
            if (ret.type == VAL_STR && ret.as.str_val &&
                strncmp(ret.as.str_val, "EVAL_ERROR:", 11) == 0) {
                char *msg = strdup(ret.as.str_val + 11);
                value_free(&ret);
                RVM_ERROR("%s", msg);
            }
            reg_set(&R[a], ret);
            DISPATCH();
        }

        /* Compiled function call */
        RegChunk *fn_chunk = (RegChunk *)func->as.closure.native_fn;
        if (!fn_chunk)
            RVM_ERROR("attempt to call a closure with NULL chunk");

        /* Guard: detect stack-VM closures (from require()) that can't run in regvm.
         * RegChunks have a magic header; stack-VM Chunks don't.
         * Use memcpy to avoid misaligned read when fn_chunk is actually a stack Chunk. */
        {
            uint32_t magic;
            memcpy(&magic, fn_chunk, sizeof(uint32_t));
            if (magic != REGCHUNK_MAGIC)
                RVM_ERROR("cannot call stack-VM closure from register VM "
                          "(use 'import' instead of 'require')");
        }

        /* Phase constraint check on parameters */
        if (fn_chunk->param_phases) {
            for (int i = 0; i < fn_chunk->param_phase_count && i < (int)b; i++) {
                uint8_t pp = fn_chunk->param_phases[i];
                if (pp == PHASE_UNSPECIFIED) continue;
                LatValue *arg = &R[a + 1 + i];
                if (pp == PHASE_FLUID && arg->phase == VTAG_CRYSTAL) {
                    RVM_ERROR("phase constraint violation in function '%s'",
                        fn_chunk->name ? fn_chunk->name : "<anonymous>");
                }
                if (pp == PHASE_CRYSTAL && arg->phase == VTAG_FLUID) {
                    RVM_ERROR("phase constraint violation in function '%s'",
                        fn_chunk->name ? fn_chunk->name : "<anonymous>");
                }
            }
        }

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
        /* Copy param_names from prototype */
        if (fn_proto.as.closure.param_names && fn_proto.as.closure.param_count > 0) {
            closure.as.closure.param_names = malloc(fn_proto.as.closure.param_count * sizeof(char *));
            for (size_t pi = 0; pi < fn_proto.as.closure.param_count; pi++)
                closure.as.closure.param_names[pi] = fn_proto.as.closure.param_names[pi]
                    ? strdup(fn_proto.as.closure.param_names[pi]) : NULL;
        } else {
            closure.as.closure.param_names = NULL;
        }
        closure.as.closure.default_values = NULL;
        closure.as.closure.has_variadic = fn_proto.as.closure.has_variadic;
        closure.as.closure.captured_env = NULL;

        /* Process upvalue descriptors that follow the CLOSURE instruction */
        /* Each upvalue descriptor is encoded as a MOVE instruction:
         * A=1 means local, A=0 means upvalue; B=index */
        size_t uv_count = fn_proto.region_id;  /* upvalue count stored by compiler */
        ObjUpvalue **upvals = NULL;

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

        /* Alloy enforcement: apply per-field phase from struct declaration */
        {
            char phase_key[256];
            snprintf(phase_key, sizeof(phase_key), "__struct_phases_%s", struct_name);
            LatValue *phase_ref = env_get_ref(vm->env, phase_key);
            if (phase_ref &&
                phase_ref->type == VAL_ARRAY && (int)phase_ref->as.array.len == c) {
                strct.as.strct.field_phases = calloc(c, sizeof(PhaseTag));
                for (int i = 0; i < c; i++) {
                    int64_t p = phase_ref->as.array.elems[i].as.int_val;
                    if (p == 1) { /* PHASE_CRYSTAL */
                        strct.as.strct.field_values[i] = value_freeze(strct.as.strct.field_values[i]);
                        strct.as.strct.field_phases[i] = VTAG_CRYSTAL;
                    } else if (p == 0) { /* PHASE_FLUID */
                        strct.as.strct.field_phases[i] = VTAG_FLUID;
                    } else {
                        strct.as.strct.field_phases[i] = strct.phase;
                    }
                }
            }
        }

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
        } else if (R[b].type == VAL_MAP) {
            reg_set(&R[a], value_int((int64_t)lat_map_len(R[b].as.map.map)));
        } else if (R[b].type == VAL_SET) {
            reg_set(&R[a], value_int((int64_t)lat_map_len(R[b].as.set.map)));
        } else if (R[b].type == VAL_TUPLE) {
            reg_set(&R[a], value_int((int64_t)R[b].as.tuple.len));
        } else if (R[b].type == VAL_BUFFER) {
            reg_set(&R[a], value_int((int64_t)R[b].as.buffer.len));
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

        /* Check for callable closure field in map */
        if (R[obj_reg].type == VAL_MAP) {
            LatValue *field = lat_map_get(R[obj_reg].as.map.map, method_name);
            if (field && field->type == VAL_CLOSURE) {
                /* Native C function in map */
                if (field->as.closure.default_values == VM_NATIVE_MARKER) {
                    VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                    LatValue *call_args = (argc > 0) ? &R[args_base] : NULL;
                    LatValue ret = native(call_args, argc);
                    if (vm->rt->error) {
                        vm->error = vm->rt->error;
                        vm->rt->error = NULL;
                        value_free(&ret);
                        return REGVM_RUNTIME_ERROR;
                    }
                    reg_set(&R[dst], ret);
                    DISPATCH();
                }
                /* Extension native function in map */
                if (field->as.closure.default_values == VM_EXT_MARKER) {
                    LatValue *call_args = (argc > 0) ? &R[args_base] : NULL;
                    LatValue ret = ext_call_native(field->as.closure.native_fn,
                                                   call_args, (size_t)argc);
                    if (ret.type == VAL_STR && ret.as.str_val &&
                        strncmp(ret.as.str_val, "EVAL_ERROR:", 11) == 0) {
                        vm->error = strdup(ret.as.str_val + 11);
                        value_free(&ret);
                        return REGVM_RUNTIME_ERROR;
                    }
                    reg_set(&R[dst], ret);
                    DISPATCH();
                }
                /* RegChunk closure in map */
                RegChunk *fn_chunk = (RegChunk *)field->as.closure.native_fn;
                if (fn_chunk && fn_chunk->magic == REGCHUNK_MAGIC) {
                    if (vm->frame_count >= REGVM_FRAMES_MAX)
                        RVM_ERROR("call stack overflow");

                    size_t new_base = vm->reg_stack_top;
                    LatValue *new_regs = &vm->reg_stack[new_base];
                    vm->reg_stack_top += REGVM_REG_MAX;
                    for (int i = 0; i < REGVM_REG_MAX; i++)
                        new_regs[i] = value_nil();

                    /* Slot 0 = reserved, slots 1+ = args (no self for map closures) */
                    new_regs[0] = value_unit();
                    for (int i = 0; i < argc; i++) {
                        value_free(&new_regs[1 + i]);
                        new_regs[1 + i] = rvm_clone(&R[args_base + i]);
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
                /* Stack-VM closure in map — use regvm bridge */
                if (field->as.closure.native_fn) {
                    LatValue *call_args = (argc > 0) ? &R[args_base] : NULL;
                    LatValue ret = regvm_call_closure(vm, field, call_args, argc);
                    if (vm->error) return REGVM_RUNTIME_ERROR;
                    reg_set(&R[dst], ret);
                    DISPATCH();
                }
            }
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

                /* ITEM_IMPL compiles self at slot 0, other params at slot 1+ */
                new_regs[0] = rvm_clone(&R[obj_reg]);  /* self */
                for (int i = 0; i < argc; i++) {
                    value_free(&new_regs[1 + i]);
                    new_regs[1 + i] = rvm_clone(&R[args_base + i]);
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
        if (R[b].type == VAL_CHANNEL)
            RVM_ERROR("cannot freeze a channel");
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
        if (R[b].type == VAL_MAP) {
            /* Convert map to array of [key, value] pairs for uniform iteration */
            LatMap *m = R[b].as.map.map;
            size_t cap = m->cap;
            size_t count = lat_map_len(m);
            LatValue *entries = malloc((count > 0 ? count : 1) * sizeof(LatValue));
            size_t idx = 0;
            for (size_t i = 0; i < cap; i++) {
                if (m->entries[i].state != MAP_OCCUPIED) continue;
                LatValue pair[2];
                pair[0] = value_string(m->entries[i].key);
                pair[1] = rvm_clone((LatValue *)m->entries[i].value);
                entries[idx++] = value_array(pair, 2);
            }
            reg_set(&R[a], value_array(entries, idx));
            free(entries);
        } else if (R[b].type == VAL_SET) {
            /* Convert set to array of values for uniform iteration */
            LatMap *m = R[b].as.set.map;
            size_t cap = m->cap;
            size_t count = lat_map_len(m);
            LatValue *elems = malloc((count > 0 ? count : 1) * sizeof(LatValue));
            size_t idx = 0;
            for (size_t i = 0; i < cap; i++) {
                if (m->entries[i].state != MAP_OCCUPIED) continue;
                elems[idx++] = rvm_clone((LatValue *)m->entries[i].value);
            }
            reg_set(&R[a], value_array(elems, idx));
            free(elems);
        } else if (R[b].type == VAL_STR) {
            /* Convert string to array of characters for uniform iteration */
            size_t len = strlen(R[b].as.str_val);
            LatValue *chars = malloc((len > 0 ? len : 1) * sizeof(LatValue));
            for (size_t i = 0; i < len; i++) {
                char ch[2] = { R[b].as.str_val[i], '\0' };
                chars[i] = value_string(ch);
            }
            reg_set(&R[a], value_array(chars, len));
            free(chars);
        } else {
            if (a != b)
                reg_set(&R[a], rvm_clone(&R[b]));
        }
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
        if (R[c].as.int_val < 0 || R[c].as.int_val > 63)
            RVM_ERROR("shift amount out of range (0..63)");
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
        tup.phase = VTAG_CRYSTAL;
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
            /* Match stack VM behavior: string exceptions pass directly,
             * non-string exceptions get "unhandled exception:" wrapper.
             * No [line N] prefix for thrown exceptions. */
            if (thrown.type == VAL_STR) {
                vm->error = strdup(thrown.as.str_val);
            } else {
                char *repr = value_display(&thrown);
                (void)asprintf(&vm->error, "unhandled exception: %s", repr);
                free(repr);
            }
            value_free(&thrown);
            return REGVM_RUNTIME_ERROR;
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
        /* Not a Result — error */
        RVM_ERROR("'?' operator requires a Result value, got %s", value_type_name(&R[a]));
    }

    /* ── Defer ── */

    CASE(DEFER_PUSH) {
        /* A = scope_depth, sBx = offset to jump past the defer body */
        uint8_t scope_d = REG_GET_A(instr);
        int16_t offset = REG_GET_sBx(instr);
        if (vm->defer_count >= REGVM_DEFER_MAX)
            RVM_ERROR("defer stack overflow");
        RegDefer *d = &vm->defers[vm->defer_count++];
        d->ip = frame->ip;       /* Points to start of defer body */
        d->chunk = frame->chunk;
        d->frame_index = (size_t)(vm->frame_count - 1);
        d->regs = frame->regs;
        d->scope_depth = (int)scope_d;
        /* Skip past the defer body */
        frame->ip += offset;
        DISPATCH();
    }

    CASE(DEFER_RUN) {
        /* Execute defers for the current frame in LIFO order.
         * A=min_scope_depth: only run defers with scope_depth >= A.
         * A=0 runs all defers for this frame (used at function return).
         * After each defer body runs, copy modified registers back to
         * the original frame so deferred mutations are visible. */
        uint8_t min_scope = REG_GET_A(instr);
        size_t frame_idx = (size_t)(vm->frame_count - 1);
        while (vm->defer_count > 0) {
            RegDefer *d = &vm->defers[vm->defer_count - 1];
            if (d->frame_index != frame_idx) break;
            if (min_scope > 0 && d->scope_depth < (int)min_scope) break;
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
            int saved_frame = (int)(vm->frame_count - 1);
            RegVMResult dr = regvm_dispatch(vm, saved_frame, &defer_result);
            value_free(&defer_result);
            (void)dr;

            /* HALT leaves the defer frame on the stack with registers intact.
             * Copy modified registers back to the original frame, then pop. */
            frame = &vm->frames[frame_idx];
            R = frame->regs;
            /* The defer frame may still be on the stack (HALT) or popped (error) */
            if (vm->frame_count > (int)frame_idx + 1) {
                RegCallFrame *defer_frame = &vm->frames[vm->frame_count - 1];
                LatValue *defer_regs = defer_frame->regs;
                for (int i = 0; i < REGVM_REG_MAX; i++) {
                    value_free(&R[i]);
                    R[i] = defer_regs[i];
                    defer_regs[i] = value_nil(); /* prevent double-free on cleanup */
                }
                /* Pop the defer frame */
                for (int i = 0; i < REGVM_REG_MAX; i++)
                    value_free_inline(&defer_frame->regs[i]);
                vm->frame_count--;
                vm->reg_stack_top -= REGVM_REG_MAX;
            }
            /* Restore frame/R to the original frame */
            frame = &vm->frames[frame_idx];
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
        /* A=name constant index, B=loc_type (0=local, 1=upvalue, 2=global; high bit=consume seeds), C=slot */
        uint8_t name_ki = REG_GET_A(instr);
        uint8_t raw_loc = REG_GET_B(instr);
        uint8_t slot = REG_GET_C(instr);
        bool consume_seeds = (raw_loc & 0x80) != 0;
        uint8_t loc_type = raw_loc & 0x7F;
        const char *var_name = frame->chunk->constants[name_ki].as.str_val;
        LatValue *target = NULL;
        if (loc_type == 0) {
            if (R[slot].type == VAL_CHANNEL)
                RVM_ERROR("cannot freeze a channel");
            target = &R[slot];
        } else if (loc_type == 1 && frame->upvalues && slot < frame->upvalue_count) {
            target = frame->upvalues[slot]->location;
        } else if (loc_type == 2) {
            LatValue gval;
            if (env_get(vm->env, var_name, &gval)) {
                /* Validate seed contracts */
                char *seed_err = rt_validate_seeds(vm->rt, var_name, &gval, consume_seeds);
                if (seed_err) {
                    value_free(&gval);
                    RVM_ERROR("%s", seed_err);
                }
                LatValue frozen = value_freeze(rvm_clone(&gval));
                value_free(&gval);
                env_set(vm->env, var_name, frozen);
                rt_freeze_cascade(vm->rt, var_name);
                if (vm->rt->error) { vm->error = vm->rt->error; vm->rt->error = NULL; return REGVM_RUNTIME_ERROR; } if (vm->error) return REGVM_RUNTIME_ERROR;
                rt_fire_reactions(vm->rt, var_name, "crystal");
                if (vm->rt->error) { vm->error = vm->rt->error; vm->rt->error = NULL; return REGVM_RUNTIME_ERROR; } if (vm->error) return REGVM_RUNTIME_ERROR;
                /* Record history for tracked globals */
                {
                    if (vm->rt->tracking_active)
                        rt_record_history(vm->rt, var_name, &frozen);
                }
            }
            DISPATCH();
        }
        if (target) {
            /* Validate seed contracts */
            char *seed_err = rt_validate_seeds(vm->rt, var_name, target, consume_seeds);
            if (seed_err) {
                RVM_ERROR("%s", seed_err);
            }
            LatValue frozen = value_freeze(rvm_clone(target));
            value_free(target);
            *target = frozen;
            /* Sync to env for cascade/reactions (locals aren't in env) */
            if (loc_type != 2) { /* not already global */
                if (!env_set(vm->env, var_name, value_deep_clone(&frozen)))
                    env_define(vm->env, var_name, value_deep_clone(&frozen));
            }
            rt_freeze_cascade(vm->rt, var_name);
            if (vm->rt->error) { vm->error = vm->rt->error; vm->rt->error = NULL; return REGVM_RUNTIME_ERROR; } if (vm->error) return REGVM_RUNTIME_ERROR;
            rt_fire_reactions(vm->rt, var_name, "crystal");
            if (vm->rt->error) { vm->error = vm->rt->error; vm->rt->error = NULL; return REGVM_RUNTIME_ERROR; } if (vm->error) return REGVM_RUNTIME_ERROR;
            /* Record history for tracked variables after phase change */
            {
                if (vm->rt->tracking_active)
                    rt_record_history(vm->rt, var_name, target);
            }
        }
        DISPATCH();
    }

    CASE(THAW_VAR) {
        uint8_t name_ki = REG_GET_A(instr);
        uint8_t loc_type = REG_GET_B(instr);
        uint8_t slot = REG_GET_C(instr);
        const char *var_name = frame->chunk->constants[name_ki].as.str_val;

        LatValue *target = NULL;
        if (loc_type == 0) {
            target = &R[slot];
        } else if (loc_type == 1 && frame->upvalues && slot < frame->upvalue_count) {
            target = frame->upvalues[slot]->location;
        } else if (loc_type == 2) {
            LatValue gval;
            if (env_get(vm->env, var_name, &gval)) {
                LatValue thawed = value_thaw(&gval);
                value_free(&gval);
                env_set(vm->env, var_name, thawed);
                rt_fire_reactions(vm->rt, var_name, "fluid");
                /* Record history for tracked globals */
                {
                    if (vm->rt->tracking_active)
                        rt_record_history(vm->rt, var_name, &thawed);
                }
            }
            DISPATCH();
        }
        if (target) {
            LatValue thawed = value_thaw(target);
            value_free(target);
            *target = thawed;
            /* Sync to env for cascade/reactions */
            if (loc_type != 2) {
                if (!env_set(vm->env, var_name, value_deep_clone(&thawed)))
                    env_define(vm->env, var_name, value_deep_clone(&thawed));
            }
            rt_fire_reactions(vm->rt, var_name, "fluid");
            /* Record history for tracked variables after phase change */
            {
                if (vm->rt->tracking_active)
                    rt_record_history(vm->rt, var_name, target);
            }
        }
        DISPATCH();
    }

    CASE(SUBLIMATE_VAR) {
        uint8_t name_ki = REG_GET_A(instr);
        uint8_t loc_type = REG_GET_B(instr);
        uint8_t slot = REG_GET_C(instr);
        const char *var_name = frame->chunk->constants[name_ki].as.str_val;

        LatValue *target = NULL;
        if (loc_type == 0) {
            target = &R[slot];
        } else if (loc_type == 1 && frame->upvalues && slot < frame->upvalue_count) {
            target = frame->upvalues[slot]->location;
        } else if (loc_type == 2) {
            LatValue gval;
            if (env_get(vm->env, var_name, &gval)) {
                gval.phase = VTAG_SUBLIMATED;
                env_set(vm->env, var_name, gval);
                rt_fire_reactions(vm->rt, var_name, "sublimated");
            }
            DISPATCH();
        }
        if (target) {
            target->phase = VTAG_SUBLIMATED;
            /* Sync to env */
            if (loc_type != 2) {
                if (!env_set(vm->env, var_name, value_deep_clone(target)))
                    env_define(vm->env, var_name, value_deep_clone(target));
            }
            rt_fire_reactions(vm->rt, var_name, "sublimated");
        }
        DISPATCH();
    }

    CASE(REACT) {
        /* Compiler emits: emit_ABx(ROP_REACT, dst, name_ki) → A=cb_reg, Bx=name_ki */
        uint8_t cb_reg = REG_GET_A(instr);
        uint16_t name_ki = REG_GET_Bx(instr);
        const char *var_name = frame->chunk->constants[name_ki].as.str_val;
        if (R[cb_reg].type != VAL_CLOSURE) DISPATCH();
        /* Find or create reaction entry */
        size_t ri = vm->rt->reaction_count;
        for (size_t i = 0; i < vm->rt->reaction_count; i++) {
            if (strcmp(vm->rt->reactions[i].var_name, var_name) == 0) { ri = i; break; }
        }
        if (ri == vm->rt->reaction_count) {
            if (vm->rt->reaction_count >= vm->rt->reaction_cap) {
                vm->rt->reaction_cap = vm->rt->reaction_cap ? vm->rt->reaction_cap * 2 : 4;
                vm->rt->reactions = realloc(vm->rt->reactions, vm->rt->reaction_cap * sizeof(*vm->rt->reactions));
            }
            vm->rt->reactions[ri].var_name = strdup(var_name);
            vm->rt->reactions[ri].callbacks = NULL;
            vm->rt->reactions[ri].cb_count = 0;
            vm->rt->reactions[ri].cb_cap = 0;
            vm->rt->reaction_count++;
        }
        if (vm->rt->reactions[ri].cb_count >= vm->rt->reactions[ri].cb_cap) {
            vm->rt->reactions[ri].cb_cap = vm->rt->reactions[ri].cb_cap ? vm->rt->reactions[ri].cb_cap * 2 : 4;
            vm->rt->reactions[ri].callbacks = realloc(vm->rt->reactions[ri].callbacks,
                vm->rt->reactions[ri].cb_cap * sizeof(LatValue));
        }
        vm->rt->reactions[ri].callbacks[vm->rt->reactions[ri].cb_count++] = value_deep_clone(&R[cb_reg]);
        DISPATCH();
    }

    CASE(UNREACT) {
        /* Compiler emits: emit_ABx(ROP_UNREACT, dst, name_ki) → A=dst, Bx=name_ki */
        uint16_t name_ki = REG_GET_Bx(instr);
        const char *var_name = frame->chunk->constants[name_ki].as.str_val;
        for (size_t i = 0; i < vm->rt->reaction_count; i++) {
            if (strcmp(vm->rt->reactions[i].var_name, var_name) != 0) continue;
            free(vm->rt->reactions[i].var_name);
            for (size_t j = 0; j < vm->rt->reactions[i].cb_count; j++)
                value_free(&vm->rt->reactions[i].callbacks[j]);
            free(vm->rt->reactions[i].callbacks);
            vm->rt->reactions[i] = vm->rt->reactions[--vm->rt->reaction_count];
            break;
        }
        DISPATCH();
    }

    CASE(BOND) {
        /* Compiler emits: emit_ABC(ROP_BOND, target_ki, dep_reg, strat_reg) → A=target_ki, B=dep_reg, C=strat_reg */
        uint8_t target_ki = REG_GET_A(instr);
        uint8_t dep_reg = REG_GET_B(instr);
        uint8_t strat_reg = REG_GET_C(instr);
        const char *target_name = frame->chunk->constants[target_ki].as.str_val;
        const char *dep_name = (R[dep_reg].type == VAL_STR) ? R[dep_reg].as.str_val : "";
        const char *strategy = (R[strat_reg].type == VAL_STR) ? R[strat_reg].as.str_val : "mirror";
        if (dep_name[0] == '\0') {
            RVM_ERROR("bond() requires variable names for dependencies");
        }
        /* Validate: check variables exist and target is not already frozen */
        {
            /* Find target variable's phase */
            PhaseTag target_phase = VTAG_UNPHASED;
            LatValue tval;
            bool t_env = env_get(vm->env, target_name, &tval);
            if (t_env) { target_phase = tval.phase; value_free(&tval); }
            else {
                for (int fi = 0; fi < (int)vm->frame_count; fi++) {
                    RegCallFrame *f = &vm->frames[fi];
                    if (!f->chunk || !f->chunk->local_names) continue;
                    for (size_t r = 0; r < f->chunk->local_name_cap; r++) {
                        if (f->chunk->local_names[r] && strcmp(f->chunk->local_names[r], target_name) == 0) {
                            target_phase = f->regs[r].phase;
                            goto found_target;
                        }
                    }
                }
                found_target:;
            }
            if (target_phase == VTAG_CRYSTAL)
                RVM_ERROR("bond: variable '%s' is already frozen", target_name);

            /* Check dep variable exists */
            LatValue dep_val;
            bool dep_found = env_get(vm->env, dep_name, &dep_val);
            if (dep_found) { value_free(&dep_val); }
            else {
                bool found_local = false;
                for (int fi = 0; fi < (int)vm->frame_count; fi++) {
                    RegCallFrame *f = &vm->frames[fi];
                    if (!f->chunk || !f->chunk->local_names) continue;
                    for (size_t r = 0; r < f->chunk->local_name_cap; r++) {
                        if (f->chunk->local_names[r] && strcmp(f->chunk->local_names[r], dep_name) == 0) {
                            found_local = true; break;
                        }
                    }
                    if (found_local) break;
                }
                if (!found_local)
                    RVM_ERROR("bond: undefined variable '%s'", dep_name);
            }
        }
        /* Find or create bond entry */
        size_t bi = vm->rt->bond_count;
        for (size_t i = 0; i < vm->rt->bond_count; i++) {
            if (strcmp(vm->rt->bonds[i].target, target_name) == 0) { bi = i; break; }
        }
        if (bi == vm->rt->bond_count) {
            if (vm->rt->bond_count >= vm->rt->bond_cap) {
                vm->rt->bond_cap = vm->rt->bond_cap ? vm->rt->bond_cap * 2 : 4;
                vm->rt->bonds = realloc(vm->rt->bonds, vm->rt->bond_cap * sizeof(*vm->rt->bonds));
            }
            vm->rt->bonds[bi].target = strdup(target_name);
            vm->rt->bonds[bi].deps = NULL;
            vm->rt->bonds[bi].dep_strategies = NULL;
            vm->rt->bonds[bi].dep_count = 0;
            vm->rt->bonds[bi].dep_cap = 0;
            vm->rt->bond_count++;
        }
        if (vm->rt->bonds[bi].dep_count >= vm->rt->bonds[bi].dep_cap) {
            vm->rt->bonds[bi].dep_cap = vm->rt->bonds[bi].dep_cap ? vm->rt->bonds[bi].dep_cap * 2 : 4;
            vm->rt->bonds[bi].deps = realloc(vm->rt->bonds[bi].deps,
                vm->rt->bonds[bi].dep_cap * sizeof(char *));
            vm->rt->bonds[bi].dep_strategies = realloc(vm->rt->bonds[bi].dep_strategies,
                vm->rt->bonds[bi].dep_cap * sizeof(char *));
        }
        vm->rt->bonds[bi].deps[vm->rt->bonds[bi].dep_count] = strdup(dep_name);
        vm->rt->bonds[bi].dep_strategies[vm->rt->bonds[bi].dep_count] = strdup(strategy);
        vm->rt->bonds[bi].dep_count++;
        DISPATCH();
    }

    CASE(UNBOND) {
        /* Compiler emits: emit_ABx(ROP_UNBOND, target_ki, dep_reg) → A=target_ki, Bx=dep_reg */
        uint8_t target_ki = REG_GET_A(instr);
        uint8_t dep_reg = (uint8_t)REG_GET_Bx(instr);
        const char *target_name = frame->chunk->constants[target_ki].as.str_val;
        const char *dep_name = (R[dep_reg].type == VAL_STR) ? R[dep_reg].as.str_val : "";
        for (size_t i = 0; i < vm->rt->bond_count; i++) {
            if (strcmp(vm->rt->bonds[i].target, target_name) != 0) continue;
            for (size_t j = 0; j < vm->rt->bonds[i].dep_count; j++) {
                if (strcmp(vm->rt->bonds[i].deps[j], dep_name) != 0) continue;
                free(vm->rt->bonds[i].deps[j]);
                if (vm->rt->bonds[i].dep_strategies)
                    free(vm->rt->bonds[i].dep_strategies[j]);
                vm->rt->bonds[i].deps[j] = vm->rt->bonds[i].deps[vm->rt->bonds[i].dep_count - 1];
                if (vm->rt->bonds[i].dep_strategies)
                    vm->rt->bonds[i].dep_strategies[j] = vm->rt->bonds[i].dep_strategies[vm->rt->bonds[i].dep_count - 1];
                vm->rt->bonds[i].dep_count--;
                break;
            }
            if (vm->rt->bonds[i].dep_count == 0) {
                free(vm->rt->bonds[i].target);
                free(vm->rt->bonds[i].deps);
                free(vm->rt->bonds[i].dep_strategies);
                vm->rt->bonds[i] = vm->rt->bonds[--vm->rt->bond_count];
            }
            break;
        }
        DISPATCH();
    }

    CASE(SEED) {
        /* Compiler emits: emit_ABx(ROP_SEED, dst, name_ki) → A=contract_reg, Bx=name_ki */
        uint8_t contract_reg = REG_GET_A(instr);
        uint16_t name_ki = REG_GET_Bx(instr);
        const char *var_name = frame->chunk->constants[name_ki].as.str_val;
        if (R[contract_reg].type != VAL_CLOSURE) DISPATCH();
        if (vm->rt->seed_count >= vm->rt->seed_cap) {
            vm->rt->seed_cap = vm->rt->seed_cap ? vm->rt->seed_cap * 2 : 4;
            vm->rt->seeds = realloc(vm->rt->seeds, vm->rt->seed_cap * sizeof(*vm->rt->seeds));
        }
        vm->rt->seeds[vm->rt->seed_count].var_name = strdup(var_name);
        vm->rt->seeds[vm->rt->seed_count].contract = value_deep_clone(&R[contract_reg]);
        vm->rt->seed_count++;
        DISPATCH();
    }

    CASE(UNSEED) {
        /* Compiler emits: emit_ABx(ROP_UNSEED, dst, name_ki) → A=dst, Bx=name_ki */
        uint16_t name_ki = REG_GET_Bx(instr);
        const char *var_name = frame->chunk->constants[name_ki].as.str_val;
        for (size_t i = 0; i < vm->rt->seed_count; i++) {
            if (strcmp(vm->rt->seeds[i].var_name, var_name) != 0) continue;
            free(vm->rt->seeds[i].var_name);
            value_free(&vm->rt->seeds[i].contract);
            vm->rt->seeds[i] = vm->rt->seeds[--vm->rt->seed_count];
            break;
        }
        DISPATCH();
    }

    /* ── Module/Import ── */

    CASE(IMPORT) {
        uint8_t a = REG_GET_A(instr);
        uint16_t bx = REG_GET_Bx(instr);
        const char *raw_path = frame->chunk->constants[bx].as.str_val;

        /* Check for built-in stdlib module */
        LatValue builtin_mod;
        if (rt_try_builtin_import(raw_path, &builtin_mod)) {
            reg_set(&R[a], builtin_mod);
            DISPATCH();
        }

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
            char *emsg = NULL;
            (void)asprintf(&emsg, "import: cannot find '%s'", file_path);
            free(file_path);
            /* Set error directly without [line N] prefix for import errors */
            vm->error = emsg;
            return REGVM_RUNTIME_ERROR;
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

            /* Filter based on export declarations */
            if (!module_should_export(name,
                    (const char **)mod_chunk->export_names,
                    mod_chunk->export_count, mod_chunk->has_exports))
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

    CASE(REQUIRE) {
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

        /* Resolve to absolute path: try CWD first, then script_dir */
        char resolved[PATH_MAX];
        bool found = (realpath(file_path, resolved) != NULL);
        if (!found && vm->rt->script_dir && file_path[0] != '/') {
            char script_rel[PATH_MAX];
            snprintf(script_rel, sizeof(script_rel), "%s/%s",
                     vm->rt->script_dir, file_path);
            found = (realpath(script_rel, resolved) != NULL);
        }
        if (!found) {
            char *emsg = NULL;
            (void)asprintf(&emsg, "require: cannot find '%s'", raw_path);
            free(file_path);
            /* Set error directly without [line N] prefix, matching native_require */
            vm->error = emsg;
            return REGVM_RUNTIME_ERROR;
        }
        free(file_path);

        /* Dedup: skip if already required */
        if (vm->module_cache) {
            LatValue *cached = lat_map_get(vm->module_cache, resolved);
            if (cached) {
                reg_set(&R[a], value_bool(true));
                DISPATCH();
            }
        }

        /* Read the file */
        char *source = builtin_read_file(resolved);
        if (!source)
            RVM_ERROR("require: cannot read '%s'", resolved);

        /* Lex */
        Lexer req_lex = lexer_new(source);
        char *lex_err = NULL;
        LatVec req_toks = lexer_tokenize(&req_lex, &lex_err);
        free(source);
        if (lex_err) {
            char errmsg[1024];
            snprintf(errmsg, sizeof(errmsg), "require '%s': %s", resolved, lex_err);
            free(lex_err);
            lat_vec_free(&req_toks);
            RVM_ERROR("%s", errmsg);
        }

        /* Parse */
        Parser req_parser = parser_new(&req_toks);
        char *parse_err = NULL;
        Program req_prog = parser_parse(&req_parser, &parse_err);
        if (parse_err) {
            char errmsg[1024];
            snprintf(errmsg, sizeof(errmsg), "require '%s': %s", resolved, parse_err);
            free(parse_err);
            program_free(&req_prog);
            for (size_t ti = 0; ti < req_toks.len; ti++)
                token_free(lat_vec_get(&req_toks, ti));
            lat_vec_free(&req_toks);
            RVM_ERROR("%s", errmsg);
        }

        /* Compile as module (via regcompiler, not stack VM compiler) */
        char *comp_err = NULL;
        RegChunk *req_chunk = reg_compile_module(&req_prog, &comp_err);

        /* Free parse artifacts */
        program_free(&req_prog);
        for (size_t ti = 0; ti < req_toks.len; ti++)
            token_free(lat_vec_get(&req_toks, ti));
        lat_vec_free(&req_toks);

        if (!req_chunk) {
            char errmsg[1024];
            snprintf(errmsg, sizeof(errmsg), "require '%s': %s", resolved,
                     comp_err ? comp_err : "compile error");
            free(comp_err);
            RVM_ERROR("%s", errmsg);
        }

        /* Track chunk */
        regvm_track_chunk(vm, req_chunk);

        /* Mark as loaded (for dedup) before execution */
        if (!vm->module_cache) {
            vm->module_cache = malloc(sizeof(LatMap));
            *vm->module_cache = lat_map_new(sizeof(LatValue));
        }
        LatValue loaded_marker = value_bool(true);
        lat_map_set(vm->module_cache, resolved, &loaded_marker);

        /* Run module — NO scope isolation, defs go directly to global env */
        LatValue req_result;
        RegVMResult req_r = regvm_run_sub(vm, req_chunk, &req_result);
        /* Restore frame/R pointers after dispatch */
        frame = &vm->frames[vm->frame_count - 1];
        R = frame->regs;

        if (req_r != REGVM_OK) {
            /* Propagate the error */
            return REGVM_RUNTIME_ERROR;
        }
        value_free(&req_result);

        reg_set(&R[a], value_bool(true));
        DISPATCH();
    }

    /* ── Concurrency ── */

    CASE(SCOPE) {
        uint8_t dst_reg = REG_GET_A(instr);
        /* Variable-length: read spawn_count, sync_idx, spawn_indices */
        RegInstr data1 = READ_INSTR();
        uint8_t spawn_count = REG_GET_A(data1);
        uint8_t sync_idx = REG_GET_B(data1);
        uint8_t spawn_indices[256];
        /* Read spawn indices from follow-up data words (3 per word: A, B, C) */
        for (uint8_t i = 0; i < spawn_count; i += 3) {
            RegInstr sp = READ_INSTR();
            spawn_indices[i] = REG_GET_A(sp);
            if (i + 1 < spawn_count) spawn_indices[i + 1] = REG_GET_B(sp);
            if (i + 2 < spawn_count) spawn_indices[i + 2] = REG_GET_C(sp);
        }

        /* Export locals to env for sub-chunk access */
        env_push_scope(vm->env);
        for (int fi2 = 0; fi2 < vm->frame_count; fi2++) {
            RegCallFrame *f2 = &vm->frames[fi2];
            if (!f2->chunk) continue;
            for (size_t sl = 0; sl < f2->chunk->local_name_cap; sl++) {
                if (f2->chunk->local_names[sl])
                    env_define(vm->env, f2->chunk->local_names[sl],
                               rvm_clone(&f2->regs[sl]));
            }
        }

        /* Run sync body — its return value becomes the scope result */
        LatValue scope_result = value_unit();
        if (sync_idx != 0xFF) {
            RegChunk *sync_body = (RegChunk *)frame->chunk->constants[sync_idx].as.closure.native_fn;
            if (sync_body) {
                RegVMResult sr = regvm_run_sub(vm, sync_body, &scope_result);
                /* Restore frame/R pointers */
                frame = &vm->frames[vm->frame_count - 1];
                R = frame->regs;
                if (sr != REGVM_OK) {
                    env_pop_scope(vm->env);
                    RVM_ERROR("%s", vm->error ? vm->error : "scope error");
                }
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
                if (sr != REGVM_OK) {
                    value_free(&scope_result);
                    env_pop_scope(vm->env);
                    RVM_ERROR("%s", vm->error ? vm->error : "spawn error");
                }
            }
        }

        env_pop_scope(vm->env);
        reg_set(&R[dst_reg], scope_result);
        DISPATCH();
    }

    CASE(SELECT) {
        uint8_t dst_reg = REG_GET_A(instr);
        /* Variable-length: arm_count, per-arm data */
        RegInstr data1 = READ_INSTR();
        uint8_t arm_count = REG_GET_A(data1);

        /* Read all arm descriptors (2 data words per arm) */
        typedef struct { uint8_t flags, chan_idx, body_idx, binding_idx; } RSelArm;
        RSelArm sel_arms[64];
        for (uint8_t i = 0; i < arm_count && i < 64; i++) {
            RegInstr d1 = READ_INSTR();
            RegInstr d2 = READ_INSTR();
            sel_arms[i].flags = REG_GET_A(d1);
            sel_arms[i].chan_idx = REG_GET_B(d1);
            sel_arms[i].body_idx = REG_GET_C(d1);
            sel_arms[i].binding_idx = REG_GET_A(d2);
        }

        /* Export locals to env for sub-chunk visibility */
        env_push_scope(vm->env);
        for (int fi2 = 0; fi2 < vm->frame_count; fi2++) {
            RegCallFrame *f2 = &vm->frames[fi2];
            if (!f2->chunk) continue;
            for (size_t sl = 0; sl < f2->chunk->local_name_cap; sl++) {
                if (f2->chunk->local_names[sl])
                    env_define(vm->env, f2->chunk->local_names[sl],
                               rvm_clone(&f2->regs[sl]));
            }
        }

        /* Find default arm */
        int default_arm = -1;
        for (uint8_t i = 0; i < arm_count; i++) {
            if (sel_arms[i].flags & 0x01) { default_arm = (int)i; break; }
        }

        /* Evaluate channel expressions */
        LatChannel **channels = calloc(arm_count, sizeof(LatChannel *));
        for (uint8_t i = 0; i < arm_count; i++) {
            if (sel_arms[i].flags & 0x03) continue;  /* skip default/timeout */
            RegChunk *ch_chunk = (RegChunk *)frame->chunk->constants[sel_arms[i].chan_idx].as.closure.native_fn;
            LatValue ch_val;
            RegVMResult cr = regvm_run_sub(vm, ch_chunk, &ch_val);
            frame = &vm->frames[vm->frame_count - 1];
            R = frame->regs;
            if (cr != REGVM_OK || ch_val.type != VAL_CHANNEL) {
                value_free(&ch_val);
                for (uint8_t j = 0; j < i; j++)
                    if (channels[j]) channel_release(channels[j]);
                free(channels);
                env_pop_scope(vm->env);
                RVM_ERROR("select arm: expression is not a Channel");
            }
            channels[i] = ch_val.as.channel.ch;
            channel_retain(channels[i]);
            value_free(&ch_val);
        }

        /* Try non-blocking recv on each channel arm */
        LatValue select_result = value_unit();
        bool select_found = false;
        for (uint8_t i = 0; i < arm_count; i++) {
            if (sel_arms[i].flags & 0x03) continue;
            LatValue recv_val;
            bool closed = false;
            if (channel_try_recv(channels[i], &recv_val, &closed)) {
                /* Got a value — bind in env, run body */
                env_push_scope(vm->env);
                if (sel_arms[i].flags & 0x04) {
                    const char *binding = frame->chunk->constants[sel_arms[i].binding_idx].as.str_val;
                    if (binding)
                        env_define(vm->env, binding, recv_val);
                    else
                        value_free(&recv_val);
                } else {
                    value_free(&recv_val);
                }
                RegChunk *body_chunk = (RegChunk *)frame->chunk->constants[sel_arms[i].body_idx].as.closure.native_fn;
                LatValue arm_result;
                RegVMResult ar = regvm_run_sub(vm, body_chunk, &arm_result);
                frame = &vm->frames[vm->frame_count - 1];
                R = frame->regs;
                env_pop_scope(vm->env);
                if (ar == REGVM_OK) {
                    value_free(&select_result);
                    select_result = arm_result;
                }
                select_found = true;
                break;
            }
            (void)closed;
        }

        /* If no channel was ready, execute default arm if present */
        if (!select_found && default_arm >= 0) {
            env_push_scope(vm->env);
            RegChunk *def_chunk = (RegChunk *)frame->chunk->constants[sel_arms[default_arm].body_idx].as.closure.native_fn;
            LatValue def_result;
            RegVMResult dr = regvm_run_sub(vm, def_chunk, &def_result);
            frame = &vm->frames[vm->frame_count - 1];
            R = frame->regs;
            env_pop_scope(vm->env);
            if (dr == REGVM_OK) {
                value_free(&select_result);
                select_result = def_result;
            }
        }

        for (uint8_t i = 0; i < arm_count; i++)
            if (channels[i]) channel_release(channels[i]);
        free(channels);
        env_pop_scope(vm->env);

        reg_set(&R[dst_reg], select_result);
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
        /* Phase checks */
        if (R[a].phase == VTAG_CRYSTAL) {
            bool blocked = true;
            if (R[a].type == VAL_MAP && R[b].type == VAL_STR && R[a].as.map.key_phases) {
                PhaseTag *kp = lat_map_get(R[a].as.map.key_phases, R[b].as.str_val);
                if (!kp || *kp != VTAG_CRYSTAL) blocked = false;
            }
            if (blocked)
                RVM_ERROR("cannot modify a frozen value");
        }
        if (R[a].phase == VTAG_SUBLIMATED && R[a].type == VAL_MAP)
            RVM_ERROR("cannot add keys to a sublimated map");
        if (R[a].type == VAL_MAP && R[b].type == VAL_STR && R[a].as.map.key_phases) {
            PhaseTag *kp = lat_map_get(R[a].as.map.key_phases, R[b].as.str_val);
            if (kp && *kp == VTAG_CRYSTAL)
                RVM_ERROR("cannot modify frozen key '%s'", R[b].as.str_val);
        }
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
        } else if (R[a].type == VAL_REF) {
            /* Proxy: set index on inner value */
            LatRef *ref = R[a].as.ref.ref;
            if (value_is_crystal(&R[a]))
                RVM_ERROR("cannot mutate a frozen Ref");
            if (ref->value.type == VAL_MAP) {
                if (R[b].type == VAL_STR) {
                    LatValue cloned = rvm_clone(&R[c]);
                    lat_map_set(ref->value.as.map.map, R[b].as.str_val, &cloned);
                }
            } else if (ref->value.type == VAL_ARRAY) {
                if (R[b].type == VAL_INT) {
                    int64_t idx = R[b].as.int_val;
                    if (idx < 0) idx += (int64_t)ref->value.as.array.len;
                    if (idx >= 0 && (size_t)idx < ref->value.as.array.len) {
                        value_free(&ref->value.as.array.elems[idx]);
                        ref->value.as.array.elems[idx] = rvm_clone(&R[c]);
                    }
                }
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
                        closure.as.closure.default_values != VM_NATIVE_MARKER &&
                        closure.as.closure.default_values != VM_EXT_MARKER) {
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

        /* Check for callable closure field in map (global) */
        if (obj_ref->type == VAL_MAP) {
            LatValue *field = lat_map_get(obj_ref->as.map.map, method_name);
            if (field && field->type == VAL_CLOSURE) {
                if (field->as.closure.default_values == VM_NATIVE_MARKER) {
                    VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                    LatValue *call_args = (argc > 0) ? &R[args_base] : NULL;
                    LatValue ret = native(call_args, argc);
                    if (vm->rt->error) {
                        vm->error = vm->rt->error;
                        vm->rt->error = NULL;
                        value_free(&ret);
                        return REGVM_RUNTIME_ERROR;
                    }
                    reg_set(&R[dst], ret);
                    DISPATCH();
                }
                if (field->as.closure.default_values == VM_EXT_MARKER) {
                    LatValue *call_args = (argc > 0) ? &R[args_base] : NULL;
                    LatValue ret = ext_call_native(field->as.closure.native_fn,
                                                   call_args, (size_t)argc);
                    if (ret.type == VAL_STR && ret.as.str_val &&
                        strncmp(ret.as.str_val, "EVAL_ERROR:", 11) == 0) {
                        vm->error = strdup(ret.as.str_val + 11);
                        value_free(&ret);
                        return REGVM_RUNTIME_ERROR;
                    }
                    reg_set(&R[dst], ret);
                    DISPATCH();
                }
                RegChunk *fn_chunk = (RegChunk *)field->as.closure.native_fn;
                if (fn_chunk) {
                    uint32_t magic;
                    memcpy(&magic, fn_chunk, sizeof(uint32_t));
                    if (magic == REGCHUNK_MAGIC) {
                        if (vm->frame_count >= REGVM_FRAMES_MAX)
                            RVM_ERROR("call stack overflow");
                        LatValue *new_regs = &vm->reg_stack[vm->reg_stack_top];
                        vm->reg_stack_top += REGVM_REG_MAX;
                        for (int ri = 0; ri < REGVM_REG_MAX; ri++)
                            new_regs[ri] = value_nil();
                        new_regs[0] = value_unit();
                        for (int ai = 0; ai < argc; ai++)
                            new_regs[1 + ai] = rvm_clone(&R[args_base + ai]);

                        ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                        size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;

                        RegCallFrame *nf = &vm->frames[vm->frame_count++];
                        nf->chunk = fn_chunk;
                        nf->ip = fn_chunk->code;
                        nf->regs = new_regs;
                        nf->reg_count = REGVM_REG_MAX;
                        nf->upvalues = upvals;
                        nf->upvalue_count = uv_count;
                        nf->caller_result_reg = dst;
                        frame = nf;
                        R = frame->regs;
                        DISPATCH();
                    }
                }
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

    CASE(IS_CRYSTAL) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        reg_set(&R[a], value_bool(R[b].phase == VTAG_CRYSTAL));
        DISPATCH();
    }

    CASE(CHECK_TYPE) {
        /* 2-word instruction:
         * word 1: A=value reg, Bx=expected type name constant index
         * word 2: error message constant index (raw 32-bit, 0xFFFFFFFF = default) */
        uint8_t a = REG_GET_A(instr);
        uint16_t bx = REG_GET_Bx(instr);
        uint32_t err_word = *frame->ip++;
        const char *expected = frame->chunk->constants[bx].as.str_val;
        /* Type matching logic (mirrors vm_type_matches from stack VM) */
        bool type_ok = false;
        if (!expected || strcmp(expected, "Any") == 0) {
            type_ok = true;
        } else if (strcmp(expected, "Int") == 0) {
            type_ok = R[a].type == VAL_INT;
        } else if (strcmp(expected, "Float") == 0) {
            type_ok = R[a].type == VAL_FLOAT;
        } else if (strcmp(expected, "String") == 0) {
            type_ok = R[a].type == VAL_STR;
        } else if (strcmp(expected, "Bool") == 0) {
            type_ok = R[a].type == VAL_BOOL;
        } else if (strcmp(expected, "Nil") == 0) {
            type_ok = R[a].type == VAL_NIL;
        } else if (strcmp(expected, "Map") == 0) {
            type_ok = R[a].type == VAL_MAP;
        } else if (strcmp(expected, "Array") == 0) {
            type_ok = R[a].type == VAL_ARRAY;
        } else if (strcmp(expected, "Fn") == 0 || strcmp(expected, "Closure") == 0) {
            type_ok = R[a].type == VAL_CLOSURE;
        } else if (strcmp(expected, "Channel") == 0) {
            type_ok = R[a].type == VAL_CHANNEL;
        } else if (strcmp(expected, "Range") == 0) {
            type_ok = R[a].type == VAL_RANGE;
        } else if (strcmp(expected, "Set") == 0) {
            type_ok = R[a].type == VAL_SET;
        } else if (strcmp(expected, "Tuple") == 0) {
            type_ok = R[a].type == VAL_TUPLE;
        } else if (strcmp(expected, "Buffer") == 0) {
            type_ok = R[a].type == VAL_BUFFER;
        } else if (strcmp(expected, "Ref") == 0) {
            type_ok = R[a].type == VAL_REF;
        } else if (strcmp(expected, "Number") == 0) {
            type_ok = R[a].type == VAL_INT || R[a].type == VAL_FLOAT;
        } else if (R[a].type == VAL_STRUCT && R[a].as.strct.name) {
            type_ok = strcmp(R[a].as.strct.name, expected) == 0;
        } else if (R[a].type == VAL_ENUM && R[a].as.enm.enum_name) {
            type_ok = strcmp(R[a].as.enm.enum_name, expected) == 0;
        }
        if (!type_ok) {
            const char *display;
            if (R[a].type == VAL_STRUCT && R[a].as.strct.name)
                display = R[a].as.strct.name;
            else if (R[a].type == VAL_ENUM && R[a].as.enm.enum_name)
                display = R[a].as.enm.enum_name;
            else if (R[a].type == VAL_CLOSURE)
                display = "Fn";
            else
                display = value_type_name(&R[a]);
            if (err_word != 0xFFFFFFFF) {
                /* Custom error format with %s placeholder for actual type */
                const char *fmt = frame->chunk->constants[err_word].as.str_val;
                RVM_ERROR(fmt, display);
            } else {
                RVM_ERROR("return type expects %s, got %s", expected, display);
            }
        }
        DISPATCH();
    }

    CASE(FREEZE_FIELD) {
        /* A=var reg, B=field name constant */
        uint8_t a = REG_GET_A(instr);
        uint8_t b_ki = REG_GET_B(instr);
        const char *field_name = frame->chunk->constants[b_ki].as.str_val;

        if (R[a].type == VAL_STRUCT) {
            size_t fi = (size_t)-1;
            for (size_t i = 0; i < R[a].as.strct.field_count; i++) {
                if (strcmp(R[a].as.strct.field_names[i], field_name) == 0) { fi = i; break; }
            }
            if (fi == (size_t)-1) RVM_ERROR("struct has no field '%s'", field_name);
            R[a].as.strct.field_values[fi] = value_freeze(R[a].as.strct.field_values[fi]);
            if (!R[a].as.strct.field_phases)
                R[a].as.strct.field_phases = calloc(R[a].as.strct.field_count, sizeof(PhaseTag));
            R[a].as.strct.field_phases[fi] = VTAG_CRYSTAL;
        } else if (R[a].type == VAL_MAP) {
            LatValue *val_ptr = (LatValue *)lat_map_get(R[a].as.map.map, field_name);
            if (val_ptr) *val_ptr = value_freeze(*val_ptr);
            if (!R[a].as.map.key_phases) {
                R[a].as.map.key_phases = calloc(1, sizeof(LatMap));
                *R[a].as.map.key_phases = lat_map_new(sizeof(PhaseTag));
            }
            PhaseTag phase = VTAG_CRYSTAL;
            lat_map_set(R[a].as.map.key_phases, field_name, &phase);
        }
        DISPATCH();
    }

    CASE(THAW_FIELD) {
        /* A=var reg, B=field name constant */
        uint8_t a = REG_GET_A(instr);
        uint8_t b_ki = REG_GET_B(instr);
        const char *field_name = frame->chunk->constants[b_ki].as.str_val;

        if (R[a].type == VAL_STRUCT) {
            if (!R[a].as.strct.field_phases) {
                R[a].as.strct.field_phases = calloc(R[a].as.strct.field_count, sizeof(PhaseTag));
                for (size_t i = 0; i < R[a].as.strct.field_count; i++)
                    R[a].as.strct.field_phases[i] = R[a].phase;
            }
            for (size_t i = 0; i < R[a].as.strct.field_count; i++) {
                if (strcmp(R[a].as.strct.field_names[i], field_name) == 0) {
                    R[a].as.strct.field_phases[i] = VTAG_FLUID;
                    break;
                }
            }
        } else if (R[a].type == VAL_MAP) {
            if (!R[a].as.map.key_phases) {
                R[a].as.map.key_phases = calloc(1, sizeof(LatMap));
                *R[a].as.map.key_phases = lat_map_new(sizeof(PhaseTag));
            }
            PhaseTag phase = VTAG_FLUID;
            lat_map_set(R[a].as.map.key_phases, field_name, &phase);
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

/* ── Dispatch adapters for LatRuntime ── */

/* call_closure: delegates to the register VM's closure call */
static LatValue regvm_dispatch_call_closure(void *opaque_vm, LatValue *closure, LatValue *args, int argc) {
    return regvm_call_closure((RegVM *)opaque_vm, closure, args, argc);
}

/* find_local_value: searches register frame locals for a named variable */
static bool regvm_dispatch_find_local_value(void *opaque_vm, const char *name, LatValue *out) {
    RegVM *rvm = (RegVM *)opaque_vm;
    for (int fi = 0; fi < rvm->frame_count; fi++) {
        RegCallFrame *f = &rvm->frames[fi];
        if (!f->chunk || !f->chunk->local_names) continue;
        for (size_t r = 0; r < f->chunk->local_name_cap; r++) {
            if (f->chunk->local_names[r] && strcmp(f->chunk->local_names[r], name) == 0) {
                *out = value_deep_clone(&f->regs[r]);
                return true;
            }
        }
    }
    return false;
}

/* current_line: returns the source line of the current instruction */
static int regvm_dispatch_current_line(void *opaque_vm) {
    RegVM *rvm = (RegVM *)opaque_vm;
    if (rvm->frame_count <= 0) return 0;
    RegCallFrame *f = &rvm->frames[rvm->frame_count - 1];
    if (f->ip > f->chunk->code) {
        size_t offset = (size_t)(f->ip - f->chunk->code - 1);
        if (offset < f->chunk->lines_len)
            return f->chunk->lines[offset];
    }
    return 0;
}

/* get_var_by_name: searches locals then globals */
static bool regvm_dispatch_get_var_by_name(void *opaque_vm, const char *name, LatValue *out) {
    RegVM *rvm = (RegVM *)opaque_vm;
    /* Search frame locals first */
    for (int fi = 0; fi < rvm->frame_count; fi++) {
        RegCallFrame *f = &rvm->frames[fi];
        if (!f->chunk || !f->chunk->local_names) continue;
        for (size_t r = 0; r < f->chunk->local_name_cap; r++) {
            if (f->chunk->local_names[r] && strcmp(f->chunk->local_names[r], name) == 0) {
                *out = value_deep_clone(&f->regs[r]);
                return true;
            }
        }
    }
    /* Fall back to globals */
    return env_get(rvm->env, name, out);
}

/* set_var_by_name: sets in locals (if found) or globals */
static bool regvm_dispatch_set_var_by_name(void *opaque_vm, const char *name, LatValue val) {
    RegVM *rvm = (RegVM *)opaque_vm;
    /* Try frame locals first */
    for (int fi = 0; fi < rvm->frame_count; fi++) {
        RegCallFrame *f = &rvm->frames[fi];
        if (!f->chunk || !f->chunk->local_names) continue;
        for (size_t r = 0; r < f->chunk->local_name_cap; r++) {
            if (f->chunk->local_names[r] && strcmp(f->chunk->local_names[r], name) == 0) {
                value_free(&f->regs[r]);
                f->regs[r] = val;
                /* Also sync to env */
                LatValue clone = value_deep_clone(&val);
                if (!env_set(rvm->env, name, clone))
                    env_define(rvm->env, name, clone);
                return true;
            }
        }
    }
    /* Fall back to globals */
    if (env_set(rvm->env, name, val))
        return true;
    env_define(rvm->env, name, val);
    return true;
}

/* Set up runtime dispatch pointers for the register VM */
static void regvm_setup_dispatch(RegVM *vm) {
    vm->rt->backend = RT_BACKEND_REG_VM;
    vm->rt->active_vm = vm;
    vm->rt->call_closure = regvm_dispatch_call_closure;
    vm->rt->find_local_value = regvm_dispatch_find_local_value;
    vm->rt->current_line = regvm_dispatch_current_line;
    vm->rt->get_var_by_name = regvm_dispatch_get_var_by_name;
    vm->rt->set_var_by_name = regvm_dispatch_set_var_by_name;
    lat_runtime_set_current(vm->rt);
}

RegVMResult regvm_run(RegVM *vm, RegChunk *chunk, LatValue *result) {
    /* Set up runtime dispatch so native functions can call back into regvm */
    regvm_setup_dispatch(vm);

    /* Reentrant: push a new frame on top of the existing stack.
     * This is safe for recursive calls (e.g. native_lat_eval, native_require)
     * because we don't clobber existing frames. */
    int base_frame = vm->frame_count;
    if (vm->frame_count >= REGVM_FRAMES_MAX) {
        vm->error = strdup("regvm_run: frame overflow");
        return REGVM_RUNTIME_ERROR;
    }
    RegCallFrame *frame = &vm->frames[vm->frame_count++];
    frame->chunk = chunk;
    frame->ip = chunk->code;
    frame->regs = &vm->reg_stack[vm->reg_stack_top];
    frame->reg_count = REGVM_REG_MAX;
    frame->upvalues = NULL;
    frame->upvalue_count = 0;
    frame->caller_result_reg = 0;
    vm->reg_stack_top += REGVM_REG_MAX;

    /* Zero the new register window */
    memset(frame->regs, 0, REGVM_REG_MAX * sizeof(LatValue));

    return regvm_dispatch(vm, base_frame, result);
}

/* REPL variant: reuses existing frame 0 registers (preserves globals/locals) */
RegVMResult regvm_run_repl(RegVM *vm, RegChunk *chunk, LatValue *result) {
    /* Set up runtime dispatch so native functions can call back into regvm */
    regvm_setup_dispatch(vm);
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
