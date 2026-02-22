#include "vm.h"
#include "opcode.h"
#include "compiler.h"  /* for AstPhase (PHASE_FLUID, PHASE_CRYSTAL, PHASE_UNSPECIFIED) */
#include "intern.h"
#include "builtins.h"
#include "lattice.h"
#include "math_ops.h"
#include "fs_ops.h"
#include "path_ops.h"
#include "net.h"
#include "tls.h"
#include "http.h"
#include "json.h"
#include "toml_ops.h"
#include "yaml_ops.h"
#include "crypto_ops.h"
#include "regex_ops.h"
#include "time_ops.h"
#include "datetime_ops.h"
#include "env_ops.h"
#include "process_ops.h"
#include "format_ops.h"
#include "type_ops.h"
#include "string_ops.h"
#include "array_ops.h"
#include "channel.h"
#include "ext.h"
#include "lexer.h"
#include "parser.h"
#include "compiler.h"
#include "latc.h"
#include <stdlib.h>
#include <limits.h>
#include <libgen.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#ifndef __EMSCRIPTEN__
#include <pthread.h>
#endif
#include "memory.h"

/* In the VM dispatch loop, use the inline fast-path for value_free
 * to avoid function call overhead on primitives (int, float, bool, etc.) */
#define value_free(v) value_free_inline(v)

/* Native function pointer for VM builtins.
 * Args array is arg_count elements. Returns a LatValue result. */
typedef LatValue (*VMNativeFn)(LatValue *args, int arg_count);

/* Global VM pointer for native functions that need VM state (Phase 6).
 * Thread-local so each spawn thread has its own VM context. */
static _Thread_local VM *current_vm = NULL;

/* ── Stack operations ── */

static void push(VM *vm, LatValue val) {
    if (vm->stack_top - vm->stack >= VM_STACK_MAX) {
        fprintf(stderr, "fatal: VM stack overflow\n");
        exit(1);
    }
    *vm->stack_top = val;
    vm->stack_top++;
}

static LatValue pop(VM *vm) {
    vm->stack_top--;
    return *vm->stack_top;
}

static LatValue *vm_peek(VM *vm, int distance) {
    return &vm->stack_top[-1 - distance];
}

/* Get the source line for the current instruction in the topmost frame. */
static int vm_current_line(VM *vm) {
    if (vm->frame_count == 0) return 0;
    CallFrame *f = &vm->frames[vm->frame_count - 1];
    if (!f->chunk || !f->chunk->lines || f->chunk->lines_len == 0) return 0;
    size_t offset = (size_t)(f->ip - f->chunk->code);
    if (offset > 0) offset--;  /* ip already advanced past the opcode */
    if (offset >= f->chunk->lines_len) offset = f->chunk->lines_len - 1;
    return f->chunk->lines[offset];
}

static VMResult runtime_error(VM *vm, const char *fmt, ...) {
    char *inner = NULL;
    va_list args;
    va_start(args, fmt);
    (void)vasprintf(&inner, fmt, args);
    va_end(args);
    vm->error = inner;
    return VM_RUNTIME_ERROR;
}

/* Try to route a runtime error through exception handlers.
 * If a handler exists, unwinds to it, pushes the error string, and returns VM_OK
 * (caller should `break` to continue the VM loop).
 * If no handler, returns VM_RUNTIME_ERROR (caller should `return` the result). */
static VMResult vm_handle_error(VM *vm, CallFrame **frame_ptr, const char *fmt, ...) {
    char *inner = NULL;
    va_list args;
    va_start(args, fmt);
    (void)vasprintf(&inner, fmt, args);
    va_end(args);

    if (vm->handler_count > 0) {
        /* Caught by try/catch — pass raw error message without line prefix */
        ExceptionHandler h = vm->handlers[--vm->handler_count];
        while (vm->frame_count - 1 > h.frame_index)
            vm->frame_count--;
        *frame_ptr = &vm->frames[vm->frame_count - 1];
        vm->stack_top = h.stack_top;
        (*frame_ptr)->ip = h.ip;
        push(vm, value_string(inner));
        free(inner);
        return VM_OK;
    }

    /* Uncaught — store raw error (stack trace provides line info) */
    vm->error = inner;
    return VM_RUNTIME_ERROR;
}

/* Like vm_handle_error but for native function errors that already have
 * a message in vm->error.  Does NOT prepend [line N] to match tree-walker
 * behaviour (native errors carry their own descriptive messages). */
static VMResult vm_handle_native_error(VM *vm, CallFrame **frame_ptr) {
    if (vm->handler_count > 0) {
        ExceptionHandler h = vm->handlers[--vm->handler_count];
        while (vm->frame_count - 1 > h.frame_index)
            vm->frame_count--;
        *frame_ptr = &vm->frames[vm->frame_count - 1];
        vm->stack_top = h.stack_top;
        (*frame_ptr)->ip = h.ip;
        push(vm, value_string(vm->error));
        free(vm->error);
        vm->error = NULL;
        return VM_OK;
    }
    /* Uncaught — vm->error already set by native function, no line prefix */
    return VM_RUNTIME_ERROR;
}

static bool is_falsy(LatValue *v) {
    return v->type == VAL_NIL ||
           (v->type == VAL_BOOL && !v->as.bool_val) ||
           v->type == VAL_UNIT;
}

/* ── Upvalue management ── */

static ObjUpvalue *new_upvalue(LatValue *slot) {
    ObjUpvalue *uv = calloc(1, sizeof(ObjUpvalue));
    uv->location = slot;
    uv->closed = value_nil();
    uv->next = NULL;
    return uv;
}

static ObjUpvalue *capture_upvalue(VM *vm, LatValue *local) {
    ObjUpvalue *prev = NULL;
    ObjUpvalue *uv = vm->open_upvalues;

    while (uv != NULL && uv->location > local) {
        prev = uv;
        uv = uv->next;
    }

    if (uv != NULL && uv->location == local)
        return uv;

    ObjUpvalue *created = new_upvalue(local);
    created->next = uv;

    if (prev == NULL)
        vm->open_upvalues = created;
    else
        prev->next = created;

    return created;
}

/* Sentinel to distinguish native C functions from compiled closures.
 * Defined here (before value_clone_fast/close_upvalues) and also used by vm_register_native below. */
#define VM_NATIVE_MARKER ((struct Expr **)(uintptr_t)0x1)
#define VM_EXT_MARKER    ((struct Expr **)(uintptr_t)0x2)

/* Fast-path clone: flat copy for primitives, strdup for strings,
 * full deep clone only for compound types. */
static inline LatValue value_clone_fast(const LatValue *src) {
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
        case VAL_BUFFER: {
            LatValue v = *src;
            v.as.buffer.data = malloc(src->as.buffer.cap);
            memcpy(v.as.buffer.data, src->as.buffer.data, src->as.buffer.len);
            v.region_id = REGION_NONE;
            return v;
        }
        case VAL_REF: {
            ref_retain(src->as.ref.ref);
            LatValue v = *src;
            v.region_id = REGION_NONE;
            return v;
        }
        case VAL_CLOSURE: {
            if (src->as.closure.body == NULL && src->as.closure.native_fn != NULL &&
                src->as.closure.default_values != VM_NATIVE_MARKER &&
                src->as.closure.default_values != VM_EXT_MARKER) {
                /* Bytecode closure: shallow copy + strdup param_names */
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
        case VAL_STRUCT: {
            LatValue v = *src;
            size_t fc = src->as.strct.field_count;
            v.as.strct.name = strdup(src->as.strct.name);
            v.as.strct.field_names = malloc(fc * sizeof(char *));
            v.as.strct.field_values = malloc(fc * sizeof(LatValue));
            for (size_t i = 0; i < fc; i++) {
                v.as.strct.field_names[i] = strdup(src->as.strct.field_names[i]);
                v.as.strct.field_values[i] = value_clone_fast(&src->as.strct.field_values[i]);
            }
            if (src->as.strct.field_phases) {
                v.as.strct.field_phases = malloc(fc * sizeof(PhaseTag));
                memcpy(v.as.strct.field_phases, src->as.strct.field_phases, fc * sizeof(PhaseTag));
            }
            v.region_id = REGION_NONE;
            return v;
        }
        case VAL_ARRAY: {
            LatValue v = *src;
            size_t len = src->as.array.len;
            size_t cap = src->as.array.cap;
            v.as.array.elems = malloc(cap * sizeof(LatValue));
            for (size_t i = 0; i < len; i++)
                v.as.array.elems[i] = value_clone_fast(&src->as.array.elems[i]);
            v.region_id = REGION_NONE;
            return v;
        }
        case VAL_TUPLE: {
            LatValue v = *src;
            size_t len = src->as.tuple.len;
            v.as.tuple.elems = malloc(len * sizeof(LatValue));
            for (size_t i = 0; i < len; i++)
                v.as.tuple.elems[i] = value_clone_fast(&src->as.tuple.elems[i]);
            v.region_id = REGION_NONE;
            return v;
        }
        case VAL_MAP: {
            LatValue v = value_map_new();
            v.phase = src->phase;  /* Preserve phase tag */
            LatMap *sm = src->as.map.map;
            for (size_t i = 0; i < sm->cap; i++) {
                if (sm->entries[i].state == MAP_OCCUPIED) {
                    LatValue cloned = value_clone_fast((LatValue *)sm->entries[i].value);
                    lat_map_set(v.as.map.map, sm->entries[i].key, &cloned);
                }
            }
            if (src->as.map.key_phases) {
                LatMap *ksrc = src->as.map.key_phases;
                v.as.map.key_phases = malloc(sizeof(LatMap));
                *v.as.map.key_phases = lat_map_new(sizeof(PhaseTag));
                for (size_t i = 0; i < ksrc->cap; i++) {
                    if (ksrc->entries[i].state == MAP_OCCUPIED)
                        lat_map_set(v.as.map.key_phases, ksrc->entries[i].key, ksrc->entries[i].value);
                }
            }
            v.region_id = REGION_NONE;
            return v;
        }
        default:
            return value_deep_clone(src);
    }
}

static void close_upvalues(VM *vm, LatValue *last) {
    while (vm->open_upvalues != NULL && vm->open_upvalues->location >= last) {
        ObjUpvalue *uv = vm->open_upvalues;
        uv->closed = value_clone_fast(uv->location);
        uv->location = &uv->closed;
        vm->open_upvalues = uv->next;
    }
}

/* Create a string value allocated in the ephemeral arena.
 * The caller passes a malloc'd string; it's copied into the arena and the original is freed. */
__attribute__((unused))
static inline LatValue vm_ephemeral_string(VM *vm, char *s) {
    if (vm->ephemeral) {
        char *arena_str = bump_strdup(vm->ephemeral, s);
        free(s);
        LatValue v;
        v.type = VAL_STR;
        v.phase = VTAG_UNPHASED;
        v.region_id = REGION_EPHEMERAL;
        v.as.str_val = arena_str;
        vm->ephemeral_on_stack = true;
        return v;
    }
    return value_string_owned(s);
}

/* Concatenate two strings directly into the ephemeral arena (avoids malloc+free). */
static inline LatValue vm_ephemeral_concat(VM *vm, const char *a, size_t la,
                                            const char *b, size_t lb) {
    size_t total = la + lb + 1;
    if (vm->ephemeral) {
        char *buf = bump_alloc(vm->ephemeral, total);
        memcpy(buf, a, la);
        memcpy(buf + la, b, lb);
        buf[la + lb] = '\0';
        vm->ephemeral_on_stack = true;
        LatValue v;
        v.type = VAL_STR;
        v.phase = VTAG_UNPHASED;
        v.region_id = REGION_EPHEMERAL;
        v.as.str_val = buf;
        return v;
    }
    char *buf = malloc(total);
    memcpy(buf, a, la);
    memcpy(buf + la, b, lb);
    buf[la + lb] = '\0';
    return value_string_owned(buf);
}

/* If value is ephemeral, deep-clone to malloc. */
static inline void vm_promote_value(LatValue *v) {
    if (v->region_id == REGION_EPHEMERAL) {
        *v = value_deep_clone(v);
    }
}

/* Promote all ephemeral values in the current frame before entering a new
 * bytecode frame, so the callee's OP_RESET_EPHEMERAL won't invalidate
 * anything in the caller's frame. */
static inline void vm_promote_frame_ephemerals(VM *vm, CallFrame *frame) {
    if (vm->ephemeral_on_stack) {
        for (LatValue *slot = frame->slots; slot < vm->stack_top; slot++)
            vm_promote_value(slot);
        vm->ephemeral_on_stack = false;
    }
}

/* ── Closure invocation helper for builtins ──
 * Calls a compiled closure from within the VM using a temporary wrapper chunk.
 * Returns the closure's return value. */
static LatValue vm_call_closure(VM *vm, LatValue *closure, LatValue *args, int arg_count) {
    if (closure->type != VAL_CLOSURE || closure->as.closure.native_fn == NULL ||
        closure->as.closure.default_values == VM_NATIVE_MARKER) {
        return value_nil();
    }

    /* Reuse the pre-built wrapper chunk, patching the arg count */
    vm->call_wrapper.code[1] = (uint8_t)arg_count;

    /* Push closure + args onto the stack for the wrapper to invoke */
    push(vm, value_clone_fast(closure));
    for (int i = 0; i < arg_count; i++)
        push(vm, value_clone_fast(&args[i]));

    LatValue result;
    vm_run(vm, &vm->call_wrapper, &result);
    return result;
}

/* ── Native builtins ── */

static LatValue native_to_string(LatValue *args, int arg_count) {
    if (arg_count != 1) return value_string("to_string() expects 1 argument");
    char *s = builtin_to_string(&args[0]);
    LatValue r = value_string(s);
    free(s);
    return r;
}

static LatValue native_typeof(LatValue *args, int arg_count) {
    if (arg_count != 1) return value_string("typeof() expects 1 argument");
    return value_string(builtin_typeof_str(&args[0]));
}

static LatValue native_len(LatValue *args, int arg_count) {
    if (arg_count != 1) return value_int(0);
    LatValue *v = &args[0];
    if (v->type == VAL_REF) v = &v->as.ref.ref->value;
    if (v->type == VAL_ARRAY) return value_int((int64_t)v->as.array.len);
    if (v->type == VAL_STR) return value_int((int64_t)strlen(v->as.str_val));
    if (v->type == VAL_MAP) return value_int((int64_t)lat_map_len(v->as.map.map));
    if (v->type == VAL_BUFFER) return value_int((int64_t)v->as.buffer.len);
    return value_int(0);
}

static LatValue native_parse_int(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_STR) return value_nil();
    bool ok;
    int64_t v = builtin_parse_int(args[0].as.str_val, &ok);
    return ok ? value_int(v) : value_nil();
}

static LatValue native_parse_float(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_STR) return value_nil();
    bool ok;
    double v = builtin_parse_float(args[0].as.str_val, &ok);
    return ok ? value_float(v) : value_nil();
}

static LatValue native_ord(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_STR) return value_int(-1);
    return value_int(builtin_ord(args[0].as.str_val));
}

static LatValue native_chr(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_INT) return value_string("");
    char *s = builtin_chr(args[0].as.int_val);
    LatValue r = value_string(s);
    free(s);
    return r;
}

static LatValue native_abs(LatValue *args, int arg_count) {
    if (arg_count != 1) return value_int(0);
    if (args[0].type == VAL_INT) return value_int(args[0].as.int_val < 0 ? -args[0].as.int_val : args[0].as.int_val);
    if (args[0].type == VAL_FLOAT) return value_float(args[0].as.float_val < 0 ? -args[0].as.float_val : args[0].as.float_val);
    return value_int(0);
}

static LatValue native_floor(LatValue *args, int arg_count) {
    if (arg_count != 1) return value_int(0);
    if (args[0].type == VAL_FLOAT) return value_int((int64_t)args[0].as.float_val);
    if (args[0].type == VAL_INT) return args[0];
    return value_int(0);
}

static LatValue native_ceil(LatValue *args, int arg_count) {
    if (arg_count != 1) return value_int(0);
    if (args[0].type == VAL_FLOAT) {
        double v = args[0].as.float_val;
        int64_t i = (int64_t)v;
        return value_int(v > (double)i ? i + 1 : i);
    }
    if (args[0].type == VAL_INT) return args[0];
    return value_int(0);
}

static LatValue native_exit(LatValue *args, int arg_count) {
    int code = 0;
    if (arg_count >= 1 && args[0].type == VAL_INT) code = (int)args[0].as.int_val;
    exit(code);
    return value_unit();
}

static LatValue native_error(LatValue *args, int arg_count) {
    if (arg_count < 1) return value_nil();
    /* Create an error map: {"tag" => "err", "value" => arg} */
    LatValue map_val = value_map_new();
    LatValue tag = value_string("err");
    lat_map_set(map_val.as.map.map, "tag", &tag);
    LatValue val = value_deep_clone(&args[0]);
    lat_map_set(map_val.as.map.map, "value", &val);
    return map_val;
}

static LatValue native_is_error(LatValue *args, int arg_count) {
    if (arg_count < 1) return value_bool(false);
    if (args[0].type != VAL_MAP) return value_bool(false);
    LatValue *tag = lat_map_get(args[0].as.map.map, "tag");
    if (!tag || tag->type != VAL_STR) return value_bool(false);
    return value_bool(strcmp(tag->as.str_val, "err") == 0);
}

static LatValue native_map_new(LatValue *args, int arg_count) {
    (void)args; (void)arg_count;
    return value_map_new();
}

static LatValue native_set_new(LatValue *args, int arg_count) {
    (void)args; (void)arg_count;
    return value_set_new();
}

static LatValue native_set_from(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_ARRAY) return value_set_new();
    LatValue set = value_set_new();
    for (size_t i = 0; i < args[0].as.array.len; i++) {
        char *key = value_display(&args[0].as.array.elems[i]);
        LatValue clone = value_deep_clone(&args[0].as.array.elems[i]);
        lat_map_set(set.as.set.map, key, &clone);
        free(key);
    }
    return set;
}

static LatValue native_channel_new(LatValue *args, int arg_count) {
    (void)args; (void)arg_count;
    LatChannel *ch = channel_new();
    LatValue val = value_channel(ch);
    channel_release(ch);
    return val;
}

/* ── Buffer constructors ── */

static LatValue native_buffer_new(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_INT) return value_buffer_alloc(0);
    int64_t size = args[0].as.int_val;
    if (size < 0) size = 0;
    return value_buffer_alloc((size_t)size);
}

static LatValue native_buffer_from(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_ARRAY) return value_buffer(NULL, 0);
    size_t len = args[0].as.array.len;
    uint8_t *data = malloc(len > 0 ? len : 1);
    for (size_t i = 0; i < len; i++) {
        if (args[0].as.array.elems[i].type == VAL_INT)
            data[i] = (uint8_t)(args[0].as.array.elems[i].as.int_val & 0xFF);
        else
            data[i] = 0;
    }
    LatValue buf = value_buffer(data, len);
    free(data);
    return buf;
}

static LatValue native_buffer_from_string(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_STR) return value_buffer(NULL, 0);
    const char *s = args[0].as.str_val;
    size_t len = strlen(s);
    return value_buffer((const uint8_t *)s, len);
}

static LatValue native_ref_new(LatValue *args, int arg_count) {
    if (arg_count != 1) return value_nil();
    return value_ref(args[0]);
}

static LatValue native_read_file_bytes(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_nil();
    FILE *f = fopen(args[0].as.str_val, "rb");
    if (!f) return value_nil();
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen < 0) { fclose(f); return value_nil(); }
    uint8_t *data = malloc((size_t)flen);
    size_t nread = fread(data, 1, (size_t)flen, f);
    fclose(f);
    LatValue buf = value_buffer(data, nread);
    free(data);
    return buf;
}

static LatValue native_write_file_bytes(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_BUFFER) return value_bool(false);
    FILE *f = fopen(args[0].as.str_val, "wb");
    if (!f) return value_bool(false);
    size_t written = fwrite(args[1].as.buffer.data, 1, args[1].as.buffer.len, f);
    fclose(f);
    return value_bool(written == args[1].as.buffer.len);
}

/* ── Phase 6: Phase system helpers ── */

/* Find a local variable's value by name in the current call frame */
static bool vm_find_local_value(VM *vm, const char *name, LatValue *out) {
    if (vm->frame_count == 0) return false;
    CallFrame *frame = &vm->frames[vm->frame_count - 1];
    Chunk *chunk = frame->chunk;
    if (!chunk->local_names) return false;
    for (size_t i = 0; i < chunk->local_name_cap; i++) {
        if (chunk->local_names[i] && strcmp(chunk->local_names[i], name) == 0) {
            *out = value_deep_clone(&frame->slots[i]);
            return true;
        }
    }
    return false;
}

/* Record a history snapshot for a tracked variable */
static void vm_record_history(VM *vm, const char *name, LatValue *val) {
    for (size_t i = 0; i < vm->tracked_count; i++) {
        if (strcmp(vm->tracked_vars[i].name, name) != 0) continue;
        if (vm->tracked_vars[i].snap_count >= vm->tracked_vars[i].snap_cap) {
            vm->tracked_vars[i].snap_cap = vm->tracked_vars[i].snap_cap ? vm->tracked_vars[i].snap_cap * 2 : 4;
            vm->tracked_vars[i].snapshots = realloc(vm->tracked_vars[i].snapshots,
                vm->tracked_vars[i].snap_cap * sizeof(*vm->tracked_vars[i].snapshots));
        }
        size_t si = vm->tracked_vars[i].snap_count++;
        const char *phase_name = builtin_phase_of_str(val);
        vm->tracked_vars[i].snapshots[si].phase = strdup(phase_name);
        vm->tracked_vars[i].snapshots[si].value = value_deep_clone(val);
        vm->tracked_vars[i].snapshots[si].line = vm_current_line(vm);
        const char *fn = NULL;
        if (vm->frame_count > 0) {
            CallFrame *f = &vm->frames[vm->frame_count - 1];
            fn = f->chunk ? f->chunk->name : NULL;
        }
        vm->tracked_vars[i].snapshots[si].fn_name = fn ? strdup(fn) : NULL;
        return;
    }
}

/* ── Phase 6: Phase system native functions ── */

static LatValue native_track(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR || !current_vm) return value_unit();
    const char *name = args[0].as.str_val;
    /* Check if already tracked */
    for (size_t i = 0; i < current_vm->tracked_count; i++) {
        if (strcmp(current_vm->tracked_vars[i].name, name) == 0) return value_unit();
    }
    /* Find the variable's current value (try env first, then locals) */
    LatValue val;
    bool found = env_get(current_vm->env, name, &val);
    if (!found) found = vm_find_local_value(current_vm, name, &val);
    if (!found) {
        char *msg = NULL;
        (void)asprintf(&msg, "track: undefined variable '%s'", name);
        current_vm->error = msg;
        return value_unit();
    }
    /* Register tracking */
    if (current_vm->tracked_count >= current_vm->tracked_cap) {
        current_vm->tracked_cap = current_vm->tracked_cap ? current_vm->tracked_cap * 2 : 4;
        current_vm->tracked_vars = realloc(current_vm->tracked_vars,
            current_vm->tracked_cap * sizeof(*current_vm->tracked_vars));
    }
    size_t idx = current_vm->tracked_count++;
    current_vm->tracking_active = true;
    current_vm->tracked_vars[idx].name = strdup(name);
    current_vm->tracked_vars[idx].snapshots = NULL;
    current_vm->tracked_vars[idx].snap_count = 0;
    current_vm->tracked_vars[idx].snap_cap = 0;
    /* Record initial snapshot */
    vm_record_history(current_vm, name, &val);
    value_free(&val);
    return value_unit();
}

static LatValue native_phases(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR || !current_vm) {
        return value_array(NULL, 0);
    }
    const char *name = args[0].as.str_val;
    for (size_t i = 0; i < current_vm->tracked_count; i++) {
        if (strcmp(current_vm->tracked_vars[i].name, name) != 0) continue;
        size_t n = current_vm->tracked_vars[i].snap_count;
        LatValue *elems = malloc(n * sizeof(LatValue));
        for (size_t j = 0; j < n; j++) {
            LatValue m = value_map_new();
            LatValue phase_val = value_string(current_vm->tracked_vars[i].snapshots[j].phase);
            LatValue val_clone = value_deep_clone(&current_vm->tracked_vars[i].snapshots[j].value);
            LatValue line_val = value_int(current_vm->tracked_vars[i].snapshots[j].line);
            LatValue fn_val = current_vm->tracked_vars[i].snapshots[j].fn_name
                ? value_string(current_vm->tracked_vars[i].snapshots[j].fn_name)
                : value_nil();
            lat_map_set(m.as.map.map, "phase", &phase_val);
            lat_map_set(m.as.map.map, "value", &val_clone);
            lat_map_set(m.as.map.map, "line", &line_val);
            lat_map_set(m.as.map.map, "fn", &fn_val);
            elems[j] = m;
        }
        LatValue arr = value_array(elems, n);
        free(elems);
        return arr;
    }
    return value_array(NULL, 0);
}

/// @builtin history(name: String) -> Array
/// @category Temporal
/// Returns the full enriched timeline of a tracked variable as an array of Maps
/// with keys: phase, value, line, fn.
/// @example history(x)  // [{phase: "fluid", value: 10, line: 3, fn: "main"}, ...]
static LatValue native_history(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR || !current_vm)
        return value_array(NULL, 0);
    const char *name = args[0].as.str_val;
    for (size_t i = 0; i < current_vm->tracked_count; i++) {
        if (strcmp(current_vm->tracked_vars[i].name, name) != 0) continue;
        size_t n = current_vm->tracked_vars[i].snap_count;
        LatValue *elems = malloc(n * sizeof(LatValue));
        for (size_t j = 0; j < n; j++) {
            LatValue m = value_map_new();
            LatValue phase_val = value_string(current_vm->tracked_vars[i].snapshots[j].phase);
            LatValue val_clone = value_deep_clone(&current_vm->tracked_vars[i].snapshots[j].value);
            LatValue line_val = value_int(current_vm->tracked_vars[i].snapshots[j].line);
            LatValue fn_val = current_vm->tracked_vars[i].snapshots[j].fn_name
                ? value_string(current_vm->tracked_vars[i].snapshots[j].fn_name)
                : value_nil();
            lat_map_set(m.as.map.map, "phase", &phase_val);
            lat_map_set(m.as.map.map, "value", &val_clone);
            lat_map_set(m.as.map.map, "line", &line_val);
            lat_map_set(m.as.map.map, "fn", &fn_val);
            elems[j] = m;
        }
        LatValue arr = value_array(elems, n);
        free(elems);
        return arr;
    }
    return value_array(NULL, 0);
}

static LatValue native_rewind(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_INT || !current_vm)
        return value_nil();
    const char *name = args[0].as.str_val;
    int64_t steps = args[1].as.int_val;
    for (size_t i = 0; i < current_vm->tracked_count; i++) {
        if (strcmp(current_vm->tracked_vars[i].name, name) != 0) continue;
        int64_t idx = (int64_t)current_vm->tracked_vars[i].snap_count - 1 - steps;
        if (idx < 0 || idx >= (int64_t)current_vm->tracked_vars[i].snap_count)
            return value_nil();
        return value_deep_clone(&current_vm->tracked_vars[i].snapshots[idx].value);
    }
    return value_nil();
}

static LatValue native_pressurize(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR || !current_vm)
        return value_unit();
    const char *name = args[0].as.str_val;
    const char *mode = args[1].as.str_val;
    /* Validate mode */
    if (strcmp(mode, "no_grow") != 0 && strcmp(mode, "no_shrink") != 0 &&
        strcmp(mode, "no_resize") != 0 && strcmp(mode, "read_heavy") != 0)
        return value_unit();
    /* Update existing or add new */
    for (size_t i = 0; i < current_vm->pressure_count; i++) {
        if (strcmp(current_vm->pressures[i].name, name) == 0) {
            free(current_vm->pressures[i].mode);
            current_vm->pressures[i].mode = strdup(mode);
            return value_unit();
        }
    }
    if (current_vm->pressure_count >= current_vm->pressure_cap) {
        current_vm->pressure_cap = current_vm->pressure_cap ? current_vm->pressure_cap * 2 : 4;
        current_vm->pressures = realloc(current_vm->pressures,
            current_vm->pressure_cap * sizeof(*current_vm->pressures));
    }
    size_t idx = current_vm->pressure_count++;
    current_vm->pressures[idx].name = strdup(name);
    current_vm->pressures[idx].mode = strdup(mode);
    return value_unit();
}

static LatValue native_depressurize(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR || !current_vm) return value_unit();
    const char *name = args[0].as.str_val;
    for (size_t i = 0; i < current_vm->pressure_count; i++) {
        if (strcmp(current_vm->pressures[i].name, name) == 0) {
            free(current_vm->pressures[i].name);
            free(current_vm->pressures[i].mode);
            current_vm->pressures[i] = current_vm->pressures[--current_vm->pressure_count];
            return value_unit();
        }
    }
    return value_unit();
}

static LatValue native_pressure_of(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR || !current_vm) return value_nil();
    const char *name = args[0].as.str_val;
    for (size_t i = 0; i < current_vm->pressure_count; i++) {
        if (strcmp(current_vm->pressures[i].name, name) == 0)
            return value_string(current_vm->pressures[i].mode);
    }
    return value_nil();
}

/* ── Phase system: variable access by name helpers ── */

static bool vm_get_var_by_name(VM *vm, const char *name, LatValue *out) {
    /* Check current frame's locals first */
    if (vm->frame_count > 0) {
        CallFrame *frame = &vm->frames[vm->frame_count - 1];
        Chunk *chunk = frame->chunk;
        if (chunk->local_names) {
            for (size_t i = 0; i < chunk->local_name_cap; i++) {
                if (chunk->local_names[i] && strcmp(chunk->local_names[i], name) == 0) {
                    *out = value_deep_clone(&frame->slots[i]);
                    return true;
                }
            }
        }
    }
    return env_get(vm->env, name, out);
}

static bool vm_set_var_by_name(VM *vm, const char *name, LatValue val) {
    /* Check current frame's locals first */
    if (vm->frame_count > 0) {
        CallFrame *frame = &vm->frames[vm->frame_count - 1];
        Chunk *chunk = frame->chunk;
        if (chunk->local_names) {
            for (size_t i = 0; i < chunk->local_name_cap; i++) {
                if (chunk->local_names[i] && strcmp(chunk->local_names[i], name) == 0) {
                    value_free(&frame->slots[i]);
                    frame->slots[i] = val;
                    return true;
                }
            }
        }
    }
    env_set(vm->env, name, val);
    return true;
}

/* Write back a value to a variable location (local/upvalue/global) and record history */
static void vm_write_back(VM *vm, CallFrame *frame, uint8_t loc_type, uint8_t loc_slot, const char *name, LatValue val) {
    switch (loc_type) {
        case 0: /* local */
            value_free(&frame->slots[loc_slot]);
            frame->slots[loc_slot] = value_deep_clone(&val);
            break;
        case 1: /* upvalue */
            if (frame->upvalues && loc_slot < frame->upvalue_count && frame->upvalues[loc_slot]) {
                value_free(frame->upvalues[loc_slot]->location);
                *frame->upvalues[loc_slot]->location = value_deep_clone(&val);
            }
            break;
        case 2: /* global */
            env_set(vm->env, name, value_deep_clone(&val));
            break;
    }
    vm_record_history(vm, name, &val);
}

/* ── Phase system: fire reactions ── */

static VMResult vm_fire_reactions(VM *vm, CallFrame **frame_ptr, const char *name, const char *phase) {
    (void)frame_ptr;
    for (size_t i = 0; i < vm->reaction_count; i++) {
        if (strcmp(vm->reactions[i].var_name, name) != 0) continue;
        LatValue cur;
        bool found = vm_get_var_by_name(vm, name, &cur);
        if (!found) return VM_OK;
        for (size_t j = 0; j < vm->reactions[i].cb_count; j++) {
            LatValue *cb = &vm->reactions[i].callbacks[j];
            LatValue args[2];
            args[0] = value_string(phase);
            args[1] = value_deep_clone(&cur);
            LatValue result = vm_call_closure(vm, cb, args, 2);
            value_free(&args[0]);
            value_free(&args[1]);
            value_free(&result);
            if (vm->error) {
                /* Wrap the error with "reaction error:" prefix */
                char *wrapped = NULL;
                (void)asprintf(&wrapped, "reaction error: %s", vm->error);
                free(vm->error);
                vm->error = wrapped;
                value_free(&cur);
                return VM_RUNTIME_ERROR;
            }
        }
        value_free(&cur);
        return VM_OK;
    }
    return VM_OK;
}

/* ── Phase system: freeze cascade ── */

static VMResult vm_freeze_cascade(VM *vm, CallFrame **frame_ptr, const char *target_name) {
    for (size_t bi = 0; bi < vm->bond_count; bi++) {
        if (strcmp(vm->bonds[bi].target, target_name) != 0) continue;
        /* Found bond entry for target — process all deps by strategy */
        for (size_t di = 0; di < vm->bonds[bi].dep_count; di++) {
            const char *dep = vm->bonds[bi].deps[di];
            const char *strategy = vm->bonds[bi].dep_strategies ? vm->bonds[bi].dep_strategies[di] : "mirror";
            LatValue dval;
            if (!vm_get_var_by_name(vm, dep, &dval)) continue;
            if (dval.type == VAL_CHANNEL) { value_free(&dval); continue; }

            if (strcmp(strategy, "mirror") == 0) {
                if (dval.phase == VTAG_CRYSTAL) { value_free(&dval); continue; }
                LatValue frozen = value_freeze(dval);
                vm_set_var_by_name(vm, dep, value_deep_clone(&frozen));
                value_free(&frozen);
                vm_fire_reactions(vm, frame_ptr, dep, "crystal");
                if (vm->error) return VM_RUNTIME_ERROR;
                VMResult r = vm_freeze_cascade(vm, frame_ptr, dep);
                if (r != VM_OK) return r;
            } else if (strcmp(strategy, "inverse") == 0) {
                if (dval.phase != VTAG_CRYSTAL && dval.phase != VTAG_SUBLIMATED) { value_free(&dval); continue; }
                LatValue thawed = value_thaw(&dval);
                value_free(&dval);
                vm_set_var_by_name(vm, dep, value_deep_clone(&thawed));
                value_free(&thawed);
                vm_fire_reactions(vm, frame_ptr, dep, "fluid");
                if (vm->error) return VM_RUNTIME_ERROR;
            } else if (strcmp(strategy, "gate") == 0) {
                if (dval.phase != VTAG_CRYSTAL) {
                    value_free(&dval);
                    char *err = NULL;
                    (void)asprintf(&err, "gate bond: '%s' must be crystal before '%s' can freeze", dep, target_name);
                    vm->error = err;
                    return VM_RUNTIME_ERROR;
                }
                value_free(&dval);
            } else {
                value_free(&dval);
            }
        }
        /* Consume the bond entry (one-shot) */
        for (size_t di = 0; di < vm->bonds[bi].dep_count; di++) {
            free(vm->bonds[bi].deps[di]);
            if (vm->bonds[bi].dep_strategies) free(vm->bonds[bi].dep_strategies[di]);
        }
        free(vm->bonds[bi].deps);
        free(vm->bonds[bi].dep_strategies);
        free(vm->bonds[bi].target);
        vm->bonds[bi] = vm->bonds[--vm->bond_count];
        break;
    }
    return VM_OK;
}

/* ── Phase system: validate seed contracts ── */

static char *vm_validate_seeds(VM *vm, const char *name, LatValue *val, bool consume) {
    for (size_t si = 0; si < vm->seed_count; si++) {
        if (strcmp(vm->seeds[si].var_name, name) != 0) continue;
        LatValue check_val = value_deep_clone(val);
        LatValue result = vm_call_closure(vm, &vm->seeds[si].contract, &check_val, 1);
        value_free(&check_val);
        if (vm->error) {
            char *msg = NULL;
            char *inner = vm->error;
            vm->error = NULL;
            (void)asprintf(&msg, "seed contract failed: %s", inner);
            free(inner);
            value_free(&result);
            return msg;
        }
        if (!value_is_truthy(&result)) {
            value_free(&result);
            if (consume) {
                free(vm->seeds[si].var_name);
                value_free(&vm->seeds[si].contract);
                vm->seeds[si] = vm->seeds[--vm->seed_count];
            }
            return strdup("grow() seed contract returned false");
        }
        value_free(&result);
        if (consume) {
            free(vm->seeds[si].var_name);
            value_free(&vm->seeds[si].contract);
            vm->seeds[si] = vm->seeds[--vm->seed_count];
            si--; /* re-check this index */
        }
    }
    return NULL;
}

/* Full grow() implementation as native function */
static LatValue native_grow(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR || !current_vm) return value_nil();
    const char *vname = args[0].as.str_val;
    LatValue val;
    if (!vm_get_var_by_name(current_vm, vname, &val)) return value_nil();

    /* Validate and consume all seeds */
    char *err = vm_validate_seeds(current_vm, vname, &val, true);
    if (err) {
        value_free(&val);
        /* Report error through VM */
        current_vm->error = err;
        return value_nil();
    }

    /* Freeze */
    LatValue frozen = value_freeze(val);
    LatValue ret = value_deep_clone(&frozen);
    vm_set_var_by_name(current_vm, vname, value_deep_clone(&frozen));
    vm_record_history(current_vm, vname, &frozen);
    value_free(&frozen);

    /* Cascade and reactions */
    CallFrame *frame = &current_vm->frames[current_vm->frame_count - 1];
    vm_freeze_cascade(current_vm, &frame, vname);
    vm_fire_reactions(current_vm, &frame, vname, "crystal");

    return ret;
}

static LatValue native_phase_of(LatValue *args, int arg_count) {
    if (arg_count != 1) return value_string("unknown");
    return value_string(builtin_phase_of_str(&args[0]));
}

static LatValue native_assert(LatValue *args, int arg_count) {
    if (arg_count < 1) return value_unit();
    bool ok = false;
    if (args[0].type == VAL_BOOL) ok = args[0].as.bool_val;
    else if (args[0].type == VAL_INT) ok = args[0].as.int_val != 0;
    else ok = args[0].type != VAL_NIL;
    if (!ok) {
        const char *msg = (arg_count >= 2 && args[1].type == VAL_STR) ? args[1].as.str_val : "assertion failed";
        /* Use VM error mechanism instead of exit() so tests can catch failures */
        if (current_vm) {
            char *err = NULL;
            (void)asprintf(&err, "assertion failed: %s", msg);
            current_vm->error = err;
        } else {
            fprintf(stderr, "assertion failed: %s\n", msg);
            exit(1);
        }
    }
    return value_unit();
}

static LatValue native_version(LatValue *args, int arg_count) {
    (void)args; (void)arg_count;
    return value_string(LATTICE_VERSION);
}

static LatValue native_input(LatValue *args, int arg_count) {
    const char *prompt = NULL;
    if (arg_count >= 1 && args[0].type == VAL_STR) prompt = args[0].as.str_val;
    char *line = builtin_input(prompt);
    if (!line) return value_nil();
    LatValue r = value_string(line);
    free(line);
    return r;
}

/* ── Math natives (via math_ops.h) ── */

#define MATH1(cname, mathfn) \
static LatValue cname(LatValue *args, int ac) { \
    if (ac != 1) return value_nil(); \
    char *err = NULL; \
    LatValue r = mathfn(&args[0], &err); \
    if (err) { current_vm->error = err; return value_nil(); } \
    return r; \
}

#define MATH2(cname, mathfn) \
static LatValue cname(LatValue *args, int ac) { \
    if (ac != 2) return value_nil(); \
    char *err = NULL; \
    LatValue r = mathfn(&args[0], &args[1], &err); \
    if (err) { current_vm->error = err; return value_nil(); } \
    return r; \
}

#define MATH3(cname, mathfn) \
static LatValue cname(LatValue *args, int ac) { \
    if (ac != 3) return value_nil(); \
    char *err = NULL; \
    LatValue r = mathfn(&args[0], &args[1], &args[2], &err); \
    if (err) { current_vm->error = err; return value_nil(); } \
    return r; \
}

MATH1(native_round, math_round)
MATH1(native_sqrt, math_sqrt)
MATH2(native_pow, math_pow)
MATH2(native_min, math_min)
MATH2(native_max, math_max)
MATH1(native_log, math_log)
MATH1(native_log2, math_log2)
MATH1(native_log10, math_log10)
MATH1(native_sin, math_sin)
MATH1(native_cos, math_cos)
MATH1(native_tan, math_tan)
MATH1(native_asin, math_asin)
MATH1(native_acos, math_acos)
MATH1(native_atan, math_atan)
MATH2(native_atan2, math_atan2)
MATH1(native_exp, math_exp)
MATH1(native_sign, math_sign)
MATH2(native_gcd, math_gcd)
MATH2(native_lcm, math_lcm)
MATH1(native_is_nan, math_is_nan)
MATH1(native_is_inf, math_is_inf)
MATH1(native_sinh, math_sinh)
MATH1(native_cosh, math_cosh)
MATH1(native_tanh, math_tanh)
MATH3(native_lerp, math_lerp)
MATH3(native_clamp, math_clamp)

static LatValue native_random(LatValue *args, int ac) {
    (void)args; (void)ac;
    return math_random();
}
static LatValue native_random_int(LatValue *args, int ac) {
    if (ac != 2) return value_nil();
    char *err = NULL;
    LatValue r = math_random_int(&args[0], &args[1], &err);
    if (err) { current_vm->error = err; return value_nil(); }
    return r;
}
static LatValue native_math_pi(LatValue *args, int ac) {
    (void)args; (void)ac;
    return math_pi();
}
static LatValue native_math_e(LatValue *args, int ac) {
    (void)args; (void)ac;
    return math_e();
}

#undef MATH1
#undef MATH2
#undef MATH3

/* ── File system natives ── */

static LatValue native_read_file(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) { current_vm->error = strdup("read_file() expects (path: String)"); return value_nil(); }
    char *contents = builtin_read_file(args[0].as.str_val);
    if (!contents) return value_nil();
    return value_string_owned(contents);
}
static LatValue native_write_file(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) { current_vm->error = strdup("write_file() expects (path: String, data: String)"); return value_bool(false); }
    return value_bool(builtin_write_file(args[0].as.str_val, args[1].as.str_val));
}
static LatValue native_file_exists(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) { current_vm->error = strdup("file_exists() expects (path: String)"); return value_bool(false); }
    return value_bool(fs_file_exists(args[0].as.str_val));
}
static LatValue native_delete_file(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("delete_file: expected (path: Str)");
        return value_bool(false);
    }
    char *err = NULL;
    bool ok = fs_delete_file(args[0].as.str_val, &err);
    if (err) { current_vm->error = err; return value_bool(false); }
    return value_bool(ok);
}
static LatValue native_list_dir(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("list_dir: expected (path: Str)");
        return value_array(NULL, 0);
    }
    char *err = NULL;
    size_t count = 0;
    char **entries = fs_list_dir(args[0].as.str_val, &count, &err);
    if (err) { current_vm->error = err; return value_array(NULL, 0); }
    if (!entries) return value_array(NULL, 0);
    LatValue *elems = malloc((count > 0 ? count : 1) * sizeof(LatValue));
    for (size_t i = 0; i < count; i++) {
        elems[i] = value_string(entries[i]);
        free(entries[i]);
    }
    free(entries);
    LatValue r = value_array(elems, count);
    free(elems);
    return r;
}
static LatValue native_append_file(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) { current_vm->error = strdup("append_file() expects (path: String, data: String)"); return value_bool(false); }
    char *err = NULL;
    bool ok = fs_append_file(args[0].as.str_val, args[1].as.str_val, &err);
    if (err) { current_vm->error = err; return value_bool(false); }
    return value_bool(ok);
}
static LatValue native_mkdir(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_bool(false);
    char *err = NULL;
    bool ok = fs_mkdir(args[0].as.str_val, &err);
    if (err) { current_vm->error = err; return value_bool(false); }
    return value_bool(ok);
}
static LatValue native_fs_rename(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_vm->error = strdup("rename: expected (from: Str, to: Str)");
        return value_bool(false);
    }
    char *err = NULL;
    bool ok = fs_rename(args[0].as.str_val, args[1].as.str_val, &err);
    if (err) { current_vm->error = err; return value_bool(false); }
    return value_bool(ok);
}
static LatValue native_is_dir(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_bool(false);
    return value_bool(fs_is_dir(args[0].as.str_val));
}
static LatValue native_is_file(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_bool(false);
    return value_bool(fs_is_file(args[0].as.str_val));
}
static LatValue native_rmdir(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("rmdir: expected (path: Str)");
        return value_bool(false);
    }
    char *err = NULL;
    bool ok = fs_rmdir(args[0].as.str_val, &err);
    if (err) { current_vm->error = err; return value_bool(false); }
    return value_bool(ok);
}
static LatValue native_glob(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_array(NULL, 0);
    char *err = NULL;
    size_t count = 0;
    char **matches = fs_glob(args[0].as.str_val, &count, &err);
    if (err) { current_vm->error = err; return value_array(NULL, 0); }
    if (!matches) return value_array(NULL, 0);
    LatValue *elems = malloc((count > 0 ? count : 1) * sizeof(LatValue));
    for (size_t i = 0; i < count; i++) {
        elems[i] = value_string(matches[i]);
        free(matches[i]);
    }
    free(matches);
    LatValue r = value_array(elems, count);
    free(elems);
    return r;
}
static LatValue native_stat(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("stat: expected (path: Str)");
        return value_nil();
    }
    int64_t sz, mt, md;
    const char *tp;
    char *err = NULL;
    if (!fs_stat(args[0].as.str_val, &sz, &mt, &md, &tp, &err)) {
        if (err) current_vm->error = err;
        return value_nil();
    }
    LatValue map = value_map_new();
    LatValue v_sz = value_int(sz); lat_map_set(map.as.map.map, "size", &v_sz);
    LatValue v_mt = value_int(mt); lat_map_set(map.as.map.map, "mtime", &v_mt);
    LatValue v_md = value_int(md); lat_map_set(map.as.map.map, "mode", &v_md);
    LatValue v_tp = value_string(tp); lat_map_set(map.as.map.map, "type", &v_tp);
    LatValue v_pm = value_int(md); lat_map_set(map.as.map.map, "permissions", &v_pm);
    return map;
}
static LatValue native_copy_file(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_vm->error = strdup("copy_file: expected (src: Str, dst: Str)");
        return value_bool(false);
    }
    char *err = NULL;
    bool ok = fs_copy_file(args[0].as.str_val, args[1].as.str_val, &err);
    if (err) { current_vm->error = err; return value_bool(false); }
    return value_bool(ok);
}
static LatValue native_realpath(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("realpath: expected (path: Str)");
        return value_nil();
    }
    char *err = NULL;
    char *r = fs_realpath(args[0].as.str_val, &err);
    if (err) { current_vm->error = err; return value_nil(); }
    if (!r) return value_nil();
    return value_string_owned(r);
}
static LatValue native_tempdir(LatValue *args, int ac) {
    (void)args; (void)ac;
    char *err = NULL; char *r = fs_tempdir(&err);
    if (err) { current_vm->error = err; return value_nil(); }
    if (!r) return value_nil();
    return value_string_owned(r);
}
static LatValue native_tempfile(LatValue *args, int ac) {
    (void)args; (void)ac;
    char *err = NULL; char *r = fs_tempfile(&err);
    if (err) { current_vm->error = err; return value_nil(); }
    if (!r) return value_nil();
    return value_string_owned(r);
}
static LatValue native_chmod(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_INT) return value_bool(false);
    char *err = NULL;
    bool ok = fs_chmod(args[0].as.str_val, (int)args[1].as.int_val, &err);
    if (err) { current_vm->error = err; return value_bool(false); }
    return value_bool(ok);
}
static LatValue native_file_size(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("file_size: expected (path: Str)");
        return value_int(-1);
    }
    char *err = NULL;
    int64_t sz = fs_file_size(args[0].as.str_val, &err);
    if (err) { current_vm->error = err; return value_int(-1); }
    return value_int(sz);
}

/* ── Path natives ── */

static LatValue native_path_join(LatValue *args, int ac) {
    if (ac < 1) { current_vm->error = strdup("path_join() expects at least 1 argument"); return value_string(""); }
    for (int i = 0; i < ac; i++) {
        if (args[i].type != VAL_STR) { current_vm->error = strdup("path_join() expects (String...)"); return value_string(""); }
    }
    const char **parts = malloc((size_t)ac * sizeof(char *));
    for (int i = 0; i < ac; i++)
        parts[i] = args[i].as.str_val;
    char *r = path_join(parts, (size_t)ac);
    free(parts); return value_string_owned(r);
}
static LatValue native_path_dir(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) { current_vm->error = strdup("path_dir() expects (path: String)"); return value_string("."); }
    return value_string_owned(path_dir(args[0].as.str_val));
}
static LatValue native_path_base(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) { current_vm->error = strdup("path_base() expects (path: String)"); return value_string(""); }
    return value_string_owned(path_base(args[0].as.str_val));
}
static LatValue native_path_ext(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) { current_vm->error = strdup("path_ext() expects (path: String)"); return value_string(""); }
    return value_string_owned(path_ext(args[0].as.str_val));
}

/* ── Network TCP natives ── */

static LatValue native_tcp_listen(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_INT) {
        current_vm->error = strdup("tcp_listen: expected (host: Str, port: Int)");
        return value_int(-1);
    }
    char *err = NULL;
    int fd = net_tcp_listen(args[0].as.str_val, (int)args[1].as.int_val, &err);
    if (err) { current_vm->error = err; return value_int(-1); }
    return value_int(fd);
}
static LatValue native_tcp_accept(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_vm->error = strdup("tcp_accept: expected (fd: Int)");
        return value_int(-1);
    }
    char *err = NULL;
    int fd = net_tcp_accept((int)args[0].as.int_val, &err);
    if (err) { current_vm->error = err; return value_int(-1); }
    return value_int(fd);
}
static LatValue native_tcp_connect(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_INT) {
        current_vm->error = strdup("tcp_connect: expected (host: Str, port: Int)");
        return value_int(-1);
    }
    char *err = NULL;
    int fd = net_tcp_connect(args[0].as.str_val, (int)args[1].as.int_val, &err);
    if (err) { current_vm->error = err; return value_int(-1); }
    return value_int(fd);
}
static LatValue native_tcp_read(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_vm->error = strdup("tcp_read: expected (fd: Int)");
        return value_string("");
    }
    char *err = NULL;
    char *data = net_tcp_read((int)args[0].as.int_val, &err);
    if (err) { current_vm->error = err; return value_string(""); }
    if (!data) return value_string("");
    return value_string_owned(data);
}
static LatValue native_tcp_read_bytes(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT) return value_string("");
    char *err = NULL;
    char *data = net_tcp_read_bytes((int)args[0].as.int_val, (size_t)args[1].as.int_val, &err);
    if (err) { current_vm->error = err; return value_string(""); }
    if (!data) return value_string("");
    return value_string_owned(data);
}
static LatValue native_tcp_write(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_INT || args[1].type != VAL_STR) return value_bool(false);
    char *err = NULL;
    bool ok = net_tcp_write((int)args[0].as.int_val, args[1].as.str_val, strlen(args[1].as.str_val), &err);
    if (err) { current_vm->error = err; return value_bool(false); }
    return value_bool(ok);
}
static LatValue native_tcp_close(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) return value_unit();
    net_tcp_close((int)args[0].as.int_val); return value_unit();
}
static LatValue native_tcp_peer_addr(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) return value_nil();
    char *err = NULL;
    char *addr = net_tcp_peer_addr((int)args[0].as.int_val, &err);
    if (err) { current_vm->error = err; return value_nil(); }
    if (!addr) return value_nil();
    return value_string_owned(addr);
}
static LatValue native_tcp_set_timeout(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT) return value_bool(false);
    char *err = NULL;
    bool ok = net_tcp_set_timeout((int)args[0].as.int_val, (int)args[1].as.int_val, &err);
    if (err) { current_vm->error = err; return value_bool(false); }
    return value_bool(ok);
}

/* ── TLS natives ── */

static LatValue native_tls_connect(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_INT) {
        current_vm->error = strdup("tls_connect: expected (host: Str, port: Int)");
        return value_int(-1);
    }
    char *err = NULL;
    int fd = net_tls_connect(args[0].as.str_val, (int)args[1].as.int_val, &err);
    if (err) { current_vm->error = err; return value_int(-1); }
    return value_int(fd);
}
static LatValue native_tls_read(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) { current_vm->error = strdup("tls_read() expects (fd: Int)"); return value_string(""); }
    char *err = NULL;
    char *data = net_tls_read((int)args[0].as.int_val, &err);
    if (err) { current_vm->error = err; return value_string(""); }
    if (!data) return value_string("");
    return value_string_owned(data);
}
static LatValue native_tls_read_bytes(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT) { current_vm->error = strdup("tls_read_bytes() expects (fd: Int, n: Int)"); return value_string(""); }
    char *err = NULL;
    char *data = net_tls_read_bytes((int)args[0].as.int_val, (size_t)args[1].as.int_val, &err);
    if (err) { current_vm->error = err; return value_string(""); }
    if (!data) return value_string("");
    return value_string_owned(data);
}
static LatValue native_tls_write(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_INT || args[1].type != VAL_STR) { current_vm->error = strdup("tls_write() expects (fd: Int, data: String)"); return value_bool(false); }
    char *err = NULL;
    bool ok = net_tls_write((int)args[0].as.int_val, args[1].as.str_val, strlen(args[1].as.str_val), &err);
    if (err) { current_vm->error = err; return value_bool(false); }
    return value_bool(ok);
}
static LatValue native_tls_close(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) { current_vm->error = strdup("tls_close() expects (fd: Int)"); return value_unit(); }
    net_tls_close((int)args[0].as.int_val); return value_unit();
}
static LatValue native_tls_available(LatValue *args, int ac) {
    (void)args; (void)ac; return value_bool(net_tls_available());
}

/* ── HTTP natives ── */

static LatValue vm_build_http_response(HttpResponse *resp) {
    LatValue map = value_map_new();
    LatValue st = value_int(resp->status_code);
    lat_map_set(map.as.map.map, "status", &st);
    LatValue bd = value_string(resp->body ? resp->body : "");
    lat_map_set(map.as.map.map, "body", &bd);
    LatValue hdr = value_map_new();
    for (size_t i = 0; i < resp->header_count; i++) {
        LatValue hv = value_string(resp->header_values[i]);
        lat_map_set(hdr.as.map.map, resp->header_keys[i], &hv);
    }
    lat_map_set(map.as.map.map, "headers", &hdr);
    http_response_free(resp);
    return map;
}
static LatValue native_http_get(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("http_get() expects (url: String)");
        return value_nil();
    }
    HttpRequest req = {0}; req.method = "GET"; req.url = args[0].as.str_val;
    char *err = NULL;
    HttpResponse *resp = http_execute(&req, &err);
    if (!resp) { current_vm->error = err ? err : strdup("http_get: request failed"); return value_nil(); }
    return vm_build_http_response(resp);
}
static LatValue native_http_post(LatValue *args, int ac) {
    if (ac < 1 || ac > 2 || args[0].type != VAL_STR) {
        current_vm->error = strdup("http_post() expects (url: String, options?: Map)");
        return value_nil();
    }
    HttpRequest req = {0}; req.method = "POST"; req.url = args[0].as.str_val;
    if (ac >= 2 && args[1].type == VAL_STR) {
        req.body = args[1].as.str_val; req.body_len = strlen(args[1].as.str_val);
    } else if (ac >= 2 && args[1].type == VAL_MAP) {
        LatValue *bv = lat_map_get(args[1].as.map.map, "body");
        if (bv && bv->type == VAL_STR) { req.body = bv->as.str_val; req.body_len = strlen(bv->as.str_val); }
    }
    char *err = NULL;
    HttpResponse *resp = http_execute(&req, &err);
    if (!resp) { current_vm->error = err ? err : strdup("http_post: request failed"); return value_nil(); }
    return vm_build_http_response(resp);
}
static LatValue native_http_request(LatValue *args, int ac) {
    if (ac < 2 || ac > 3 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_vm->error = strdup("http_request() expects (method: String, url: String, options?: Map)");
        return value_nil();
    }
    HttpRequest req = {0};
    req.method = args[0].as.str_val;
    req.url = args[1].as.str_val;
    if (ac == 3 && args[2].type == VAL_MAP) {
        LatValue *bv = lat_map_get(args[2].as.map.map, "body");
        if (bv && bv->type == VAL_STR) { req.body = bv->as.str_val; req.body_len = strlen(bv->as.str_val); }
    }
    char *err = NULL;
    HttpResponse *resp = http_execute(&req, &err);
    if (!resp) { current_vm->error = err ? err : strdup("http_request: request failed"); return value_nil(); }
    return vm_build_http_response(resp);
}

/* ── JSON/TOML/YAML natives ── */

static LatValue native_json_parse(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("json_parse: expected (str: Str)");
        return value_nil();
    }
    char *err = NULL;
    LatValue r = json_parse(args[0].as.str_val, &err);
    if (err) { current_vm->error = err; return value_nil(); }
    return r;
}
static LatValue native_json_stringify(LatValue *args, int ac) {
    if (ac != 1) {
        current_vm->error = strdup("json_stringify: expected 1 argument");
        return value_nil();
    }
    char *err = NULL;
    char *r = json_stringify(&args[0], &err);
    if (err) { current_vm->error = err; return value_nil(); }
    return value_string_owned(r);
}
static LatValue native_toml_parse(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("toml_parse() expects (String)");
        return value_nil();
    }
    char *err = NULL;
    LatValue r = toml_ops_parse(args[0].as.str_val, &err);
    if (err) { current_vm->error = err; return value_nil(); }
    return r;
}
static LatValue native_toml_stringify(LatValue *args, int ac) {
    if (ac != 1) {
        current_vm->error = strdup("toml_stringify: expected 1 argument");
        return value_nil();
    }
    char *err = NULL;
    char *r = toml_ops_stringify(&args[0], &err);
    if (err) { current_vm->error = err; return value_nil(); }
    if (!r) return value_nil();
    return value_string_owned(r);
}
static LatValue native_yaml_parse(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("yaml_parse() expects (String)");
        return value_nil();
    }
    char *err = NULL;
    LatValue r = yaml_ops_parse(args[0].as.str_val, &err);
    if (err) { current_vm->error = err; return value_nil(); }
    return r;
}
static LatValue native_yaml_stringify(LatValue *args, int ac) {
    if (ac != 1) {
        current_vm->error = strdup("yaml_stringify: expected 1 argument");
        return value_nil();
    }
    if (args[0].type != VAL_MAP && args[0].type != VAL_ARRAY) {
        current_vm->error = strdup("yaml_stringify: value must be a Map or Array");
        return value_nil();
    }
    char *err = NULL;
    char *r = yaml_ops_stringify(&args[0], &err);
    if (err) { current_vm->error = err; return value_nil(); }
    if (!r) return value_nil();
    return value_string_owned(r);
}

/* ── Crypto natives ── */

static LatValue native_sha256(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("sha256: expected (str: Str)");
        return value_nil();
    }
    char *err = NULL;
    char *r = crypto_sha256(args[0].as.str_val, strlen(args[0].as.str_val), &err);
    if (err) { current_vm->error = err; return value_nil(); }
    return value_string_owned(r);
}
static LatValue native_md5(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("md5: expected (str: Str)");
        return value_nil();
    }
    char *err = NULL;
    char *r = crypto_md5(args[0].as.str_val, strlen(args[0].as.str_val), &err);
    if (err) { current_vm->error = err; return value_nil(); }
    return value_string_owned(r);
}
static LatValue native_base64_encode(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("base64_encode: expected (str: Str)");
        return value_nil();
    }
    return value_string_owned(crypto_base64_encode(args[0].as.str_val, strlen(args[0].as.str_val)));
}
static LatValue native_base64_decode(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("base64_decode: expected (str: Str)");
        return value_nil();
    }
    char *err = NULL; size_t dl = 0;
    char *r = crypto_base64_decode(args[0].as.str_val, strlen(args[0].as.str_val), &dl, &err);
    if (err) { current_vm->error = err; return value_nil(); }
    return value_string_owned(r);
}

/* ── Regex natives ── */

static LatValue native_regex_match(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_vm->error = strdup("regex_match: expected (pattern: Str, input: Str)");
        return value_bool(false);
    }
    char *err = NULL;
    LatValue r = regex_match(args[0].as.str_val, args[1].as.str_val, &err);
    if (err) { current_vm->error = err; return value_bool(false); }
    return r;
}
static LatValue native_regex_find_all(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_vm->error = strdup("regex_find_all: expected (pattern: Str, input: Str)");
        return value_array(NULL, 0);
    }
    char *err = NULL;
    LatValue r = regex_find_all(args[0].as.str_val, args[1].as.str_val, &err);
    if (err) { current_vm->error = err; return value_array(NULL, 0); }
    return r;
}
static LatValue native_regex_replace(LatValue *args, int ac) {
    if (ac != 3 || args[0].type != VAL_STR || args[1].type != VAL_STR || args[2].type != VAL_STR)
        return value_nil();
    char *err = NULL;
    char *r = regex_replace(args[0].as.str_val, args[1].as.str_val, args[2].as.str_val, &err);
    if (err) { current_vm->error = err; return value_nil(); }
    if (!r) return value_nil();
    return value_string_owned(r);
}

/* ── Time/DateTime natives ── */

static LatValue native_time(LatValue *args, int ac) {
    (void)args;
    if (ac != 0) { current_vm->error = strdup("time() expects no arguments"); return value_int(0); }
    return value_int(time_now_ms());
}
static LatValue native_sleep(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) { current_vm->error = strdup("sleep() expects (ms: Int)"); return value_unit(); }
    char *err = NULL; time_sleep_ms(args[0].as.int_val, &err);
    if (err) { current_vm->error = err; return value_unit(); }
    return value_unit();
}
static LatValue native_time_format(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_INT || args[1].type != VAL_STR) {
        current_vm->error = strdup("time_format: expected (timestamp: Int, format: Str)");
        return value_nil();
    }
    char *err = NULL;
    char *r = datetime_format(args[0].as.int_val, args[1].as.str_val, &err);
    if (err) { current_vm->error = err; return value_nil(); }
    return value_string_owned(r);
}
static LatValue native_time_parse(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_vm->error = strdup("time_parse: expected (str: Str, format: Str)");
        return value_nil();
    }
    char *err = NULL;
    int64_t r = datetime_parse(args[0].as.str_val, args[1].as.str_val, &err);
    if (err) { current_vm->error = err; return value_nil(); }
    return value_int(r);
}

/* ── Environment natives ── */

static LatValue native_env(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) { current_vm->error = strdup("env() expects (key: String)"); return value_unit(); }
    char *val = envvar_get(args[0].as.str_val);
    if (!val) return value_unit();
    return value_string_owned(val);
}
static LatValue native_env_set(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_vm->error = strdup("env_set: expected (key: Str, value: Str)");
        return value_bool(false);
    }
    char *err = NULL;
    bool ok = envvar_set(args[0].as.str_val, args[1].as.str_val, &err);
    if (err) { current_vm->error = err; return value_bool(false); }
    return value_bool(ok);
}
static LatValue native_env_keys(LatValue *args, int ac) {
    (void)args; (void)ac;
    char **keys = NULL; size_t count = 0;
    envvar_keys(&keys, &count);
    LatValue *elems = malloc((count > 0 ? count : 1) * sizeof(LatValue));
    for (size_t i = 0; i < count; i++) { elems[i] = value_string(keys[i]); free(keys[i]); }
    free(keys);
    LatValue r = value_array(elems, count); free(elems);
    return r;
}

/* ── Process natives ── */

static LatValue native_cwd(LatValue *args, int ac) {
    (void)args; (void)ac;
    char *err = NULL; char *r = process_cwd(&err);
    if (err) { current_vm->error = err; return value_nil(); }
    if (!r) return value_nil();
    return value_string_owned(r);
}
static LatValue native_exec_cmd(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_nil();
    char *err = NULL;
    LatValue r = process_exec(args[0].as.str_val, &err);
    if (err) { current_vm->error = err; return value_nil(); }
    return r;
}
static LatValue native_shell(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_nil();
    char *err = NULL;
    LatValue r = process_shell(args[0].as.str_val, &err);
    if (err) { current_vm->error = err; return value_nil(); }
    return r;
}
static LatValue native_platform(LatValue *args, int ac) {
    (void)args; (void)ac; return value_string(process_platform());
}
static LatValue native_hostname(LatValue *args, int ac) {
    (void)args; (void)ac;
    char *err = NULL; char *r = process_hostname(&err);
    if (err) { current_vm->error = err; return value_nil(); }
    if (!r) return value_nil();
    return value_string_owned(r);
}
static LatValue native_pid(LatValue *args, int ac) {
    (void)args; (void)ac; return value_int(process_pid());
}

/* ── Type/utility natives ── */

static LatValue native_to_int(LatValue *args, int ac) {
    if (ac != 1) return value_nil();
    char *err = NULL;
    LatValue r = type_to_int(&args[0], &err);
    if (err) { current_vm->error = err; return value_nil(); }
    return r;
}
static LatValue native_to_float(LatValue *args, int ac) {
    if (ac != 1) return value_nil();
    char *err = NULL;
    LatValue r = type_to_float(&args[0], &err);
    if (err) { current_vm->error = err; return value_nil(); }
    return r;
}
static LatValue native_struct_name(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STRUCT) return value_nil();
    return value_string(args[0].as.strct.name);
}
static LatValue native_struct_fields(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STRUCT) return value_array(NULL, 0);
    size_t fc = args[0].as.strct.field_count;
    LatValue *elems = malloc((fc > 0 ? fc : 1) * sizeof(LatValue));
    for (size_t i = 0; i < fc; i++)
        elems[i] = value_string(args[0].as.strct.field_names[i]);
    LatValue r = value_array(elems, fc); free(elems);
    return r;
}
static LatValue native_struct_to_map(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STRUCT) return value_nil();
    LatValue map = value_map_new();
    for (size_t i = 0; i < args[0].as.strct.field_count; i++) {
        LatValue v = value_deep_clone(&args[0].as.strct.field_values[i]);
        lat_map_set(map.as.map.map, args[0].as.strct.field_names[i], &v);
    }
    return map;
}
static LatValue native_repr(LatValue *args, int ac) {
    if (ac != 1) return value_nil();
    /* Check for custom repr closure on structs */
    if (args[0].type == VAL_STRUCT) {
        for (size_t i = 0; i < args[0].as.strct.field_count; i++) {
            if (strcmp(args[0].as.strct.field_names[i], "repr") == 0 &&
                args[0].as.strct.field_values[i].type == VAL_CLOSURE) {
                LatValue self = value_deep_clone(&args[0]);
                LatValue result = vm_call_closure(current_vm, &args[0].as.strct.field_values[i], &self, 1);
                value_free(&self);
                if (result.type == VAL_STR) return result;
                value_free(&result);
                break; /* fall through to default */
            }
        }
    }
    return value_string_owned(value_repr(&args[0]));
}
static LatValue native_format(LatValue *args, int ac) {
    if (ac < 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("format: expected (fmt: Str, ...)");
        return value_nil();
    }
    char *err = NULL;
    char *r = format_string(args[0].as.str_val, args + 1, (size_t)(ac - 1), &err);
    if (err) { current_vm->error = err; return value_nil(); }
    return value_string_owned(r);
}
static LatValue native_range(LatValue *args, int ac) {
    if (ac < 2 || ac > 3 || args[0].type != VAL_INT || args[1].type != VAL_INT) {
        current_vm->error = strdup("range() expects (start: Int, end: Int, step?: Int)");
        return value_array(NULL, 0);
    }
    int64_t rstart = args[0].as.int_val, rend = args[1].as.int_val;
    int64_t rstep = (rstart <= rend) ? 1 : -1;
    if (ac == 3) {
        if (args[2].type != VAL_INT) { current_vm->error = strdup("range() step must be Int"); return value_array(NULL, 0); }
        rstep = args[2].as.int_val;
    }
    if (rstep == 0) { current_vm->error = strdup("range() step cannot be zero"); return value_array(NULL, 0); }
    size_t rcount = 0;
    if (rstep > 0 && rstart < rend)
        rcount = (size_t)((rend - rstart + rstep - 1) / rstep);
    else if (rstep < 0 && rstart > rend)
        rcount = (size_t)((rstart - rend + (-rstep) - 1) / (-rstep));
    LatValue *relems = malloc((rcount > 0 ? rcount : 1) * sizeof(LatValue));
    int64_t rcur = rstart;
    for (size_t i = 0; i < rcount; i++) { relems[i] = value_int(rcur); rcur += rstep; }
    LatValue r = value_array(relems, rcount); free(relems);
    return r;
}
static LatValue native_print_raw(LatValue *args, int ac) {
    for (int i = 0; i < ac; i++) {
        if (i > 0) printf(" ");
        if (args[i].type == VAL_STR) printf("%s", args[i].as.str_val);
        else { char *s = value_display(&args[i]); printf("%s", s); free(s); }
    }
    fflush(stdout); return value_unit();
}
static LatValue native_eprint(LatValue *args, int ac) {
    for (int i = 0; i < ac; i++) {
        if (i > 0) fprintf(stderr, " ");
        if (args[i].type == VAL_STR) fprintf(stderr, "%s", args[i].as.str_val);
        else { char *s = value_display(&args[i]); fprintf(stderr, "%s", s); free(s); }
    }
    fprintf(stderr, "\n"); return value_unit();
}
static LatValue native_identity(LatValue *args, int ac) {
    if (ac != 1) return value_nil();
    return value_deep_clone(&args[0]);
}
static LatValue native_debug_assert(LatValue *args, int ac) {
    if (ac < 1) return value_unit();
    bool ok = (args[0].type == VAL_BOOL) ? args[0].as.bool_val :
              (args[0].type == VAL_INT) ? args[0].as.int_val != 0 :
              args[0].type != VAL_NIL;
    if (!ok) {
        const char *msg = (ac >= 2 && args[1].type == VAL_STR) ? args[1].as.str_val : "debug assertion failed";
        if (current_vm) {
            char *err = NULL;
            (void)asprintf(&err, "debug assertion failed: %s", msg);
            current_vm->error = err;
        } else {
            fprintf(stderr, "debug assertion failed: %s\n", msg);
            exit(1);
        }
    }
    return value_unit();
}

static LatValue native_panic(LatValue *args, int ac) {
    const char *msg = (ac >= 1 && args[0].type == VAL_STR) ? args[0].as.str_val : "panic";
    current_vm->error = strdup(msg);
    return value_unit();
}

/* Native require: load and execute a file in the global scope (no isolation) */
static LatValue native_require(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("require: expected a string argument");
        return value_bool(false);
    }
    VM *vm = current_vm;
    const char *raw_path = args[0].as.str_val;

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
    if (!found && vm->script_dir && file_path[0] != '/') {
        char script_rel[PATH_MAX];
        snprintf(script_rel, sizeof(script_rel), "%s/%s",
                 vm->script_dir, file_path);
        found = (realpath(script_rel, resolved) != NULL);
    }
    if (!found) {
        (void)asprintf(&current_vm->error, "require: cannot find '%s'", raw_path);
        free(file_path);
        return value_bool(false);
    }
    free(file_path);

    /* Dedup: skip if already required */
    if (lat_map_get(&vm->required_files, resolved)) {
        return value_bool(true);
    }

    /* Mark as loaded before execution (prevents circular requires) */
    bool loaded = true;
    lat_map_set(&vm->required_files, resolved, &loaded);

    /* Read the file */
    char *source = builtin_read_file(resolved);
    if (!source) {
        (void)asprintf(&current_vm->error, "require: cannot read '%s'", resolved);
        return value_bool(false);
    }

    /* Lex */
    Lexer req_lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec req_toks = lexer_tokenize(&req_lex, &lex_err);
    free(source);
    if (lex_err) {
        (void)asprintf(&current_vm->error, "require '%s': %s", resolved, lex_err);
        free(lex_err);
        lat_vec_free(&req_toks);
        return value_bool(false);
    }

    /* Parse */
    Parser req_parser = parser_new(&req_toks);
    char *parse_err = NULL;
    Program req_prog = parser_parse(&req_parser, &parse_err);
    if (parse_err) {
        (void)asprintf(&current_vm->error, "require '%s': %s", resolved, parse_err);
        free(parse_err);
        program_free(&req_prog);
        for (size_t ti = 0; ti < req_toks.len; ti++)
            token_free(lat_vec_get(&req_toks, ti));
        lat_vec_free(&req_toks);
        return value_bool(false);
    }

    /* Compile as module (no auto-call of main) */
    char *comp_err = NULL;
    Chunk *req_chunk = compile_module(&req_prog, &comp_err);

    /* Free parse artifacts */
    program_free(&req_prog);
    for (size_t ti = 0; ti < req_toks.len; ti++)
        token_free(lat_vec_get(&req_toks, ti));
    lat_vec_free(&req_toks);

    if (!req_chunk) {
        (void)asprintf(&current_vm->error, "require '%s': %s", resolved,
                comp_err ? comp_err : "compile error");
        free(comp_err);
        return value_bool(false);
    }

    /* Track the chunk for proper lifetime management */
    if (vm->fn_chunk_count >= vm->fn_chunk_cap) {
        vm->fn_chunk_cap = vm->fn_chunk_cap ? vm->fn_chunk_cap * 2 : 8;
        vm->fn_chunks = realloc(vm->fn_chunks,
                                vm->fn_chunk_cap * sizeof(Chunk *));
    }
    vm->fn_chunks[vm->fn_chunk_count++] = req_chunk;

    /* Run the module chunk directly — NO scope isolation, defs go to global env */
    LatValue req_result;
    VMResult req_r = vm_run(vm, req_chunk, &req_result);
    if (req_r != VM_OK) {
        (void)asprintf(&current_vm->error, "require '%s': runtime error: %s", resolved,
                vm->error ? vm->error : "(unknown)");
        return value_bool(false);
    }
    value_free(&req_result);

    return value_bool(true);
}

/* Native require_ext: load a native extension (.dylib/.so) and return a Map */
static LatValue native_require_ext(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_STR) {
        current_vm->error = strdup("require_ext: expected a string argument");
        return value_nil();
    }
    VM *vm = current_vm;
    const char *ext_name = args[0].as.str_val;

    /* Check cache */
    LatValue *cached = (LatValue *)lat_map_get(&vm->loaded_extensions, ext_name);
    if (cached) {
        return value_deep_clone(cached);
    }

    /* Load extension */
    char *ext_err = NULL;
    LatValue ext_map = ext_load(NULL, ext_name, &ext_err);
    if (ext_err) {
        (void)asprintf(&current_vm->error, "require_ext: %s", ext_err);
        free(ext_err);
        return value_nil();
    }

    /* Mark extension closures with VM_EXT_MARKER so the VM dispatches them
     * through ext_call_native() instead of treating native_fn as a Chunk*. */
    if (ext_map.type == VAL_MAP && ext_map.as.map.map) {
        for (size_t i = 0; i < ext_map.as.map.map->cap; i++) {
            if (ext_map.as.map.map->entries[i].state != MAP_OCCUPIED) continue;
            LatValue *v = (LatValue *)ext_map.as.map.map->entries[i].value;
            if (v->type == VAL_CLOSURE && v->as.closure.native_fn && !v->as.closure.body) {
                v->as.closure.default_values = VM_EXT_MARKER;
            }
        }
    }

    /* Cache the extension */
    LatValue cached_copy = value_deep_clone(&ext_map);
    lat_map_set(&vm->loaded_extensions, ext_name, &cached_copy);

    return ext_map;
}

/* ── Missing native builtins ── */

static LatValue native_args(LatValue *args, int arg_count) {
    (void)args; (void)arg_count;
    VM *vm = current_vm;
    int ac = vm->prog_argc;
    char **av = vm->prog_argv;
    LatValue *elems = NULL;
    if (ac > 0) {
        elems = malloc((size_t)ac * sizeof(LatValue));
        for (int i = 0; i < ac; i++)
            elems[i] = value_string(av[i]);
    }
    LatValue arr = value_array(elems, (size_t)ac);
    free(elems);
    return arr;
}

static LatValue native_struct_from_map(LatValue *args, int arg_count) {
    if (arg_count < 2 || args[0].type != VAL_STR || args[1].type != VAL_MAP) {
        current_vm->error = strdup("struct_from_map: expected (name: Str, fields: Map)");
        return value_nil();
    }
    VM *vm = current_vm;
    const char *sname = args[0].as.str_val;
    /* Look up struct field names from env metadata */
    char meta_key[256];
    snprintf(meta_key, sizeof(meta_key), "__struct_%s", sname);
    LatValue meta;
    if (!env_get(vm->env, meta_key, &meta)) {
        (void)asprintf(&current_vm->error, "struct_from_map: unknown struct '%s'", sname);
        return value_nil();
    }
    if (meta.type != VAL_ARRAY) { value_free(&meta); return value_nil(); }
    size_t fc = meta.as.array.len;
    char **names = malloc(fc * sizeof(char *));
    LatValue *vals = malloc(fc * sizeof(LatValue));
    for (size_t j = 0; j < fc; j++) {
        names[j] = meta.as.array.elems[j].as.str_val;
        LatValue *found = (LatValue *)lat_map_get(args[1].as.map.map, names[j]);
        vals[j] = found ? value_deep_clone(found) : value_nil();
    }
    LatValue st = value_struct(sname, names, vals, fc);
    free(names);
    free(vals);
    value_free(&meta);
    return st;
}

static LatValue native_url_encode(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_STR) return value_nil();
    const char *src = args[0].as.str_val;
    size_t slen = strlen(src);
    size_t cap = slen * 3 + 1;
    char *out = malloc(cap);
    size_t j = 0;
    for (size_t i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = (char)c;
        } else {
            snprintf(out + j, 4, "%%%02X", c);
            j += 3;
        }
    }
    out[j] = '\0';
    return value_string_owned(out);
}

static LatValue native_url_decode(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_STR) return value_nil();
    const char *src = args[0].as.str_val;
    size_t slen = strlen(src);
    char *out = malloc(slen + 1);
    size_t j = 0;
    for (size_t i = 0; i < slen; i++) {
        if (src[i] == '%' && i + 2 < slen) {
            char hex[3] = { src[i+1], src[i+2], '\0' };
            char *end = NULL;
            unsigned long val = strtoul(hex, &end, 16);
            if (end == hex + 2) {
                out[j++] = (char)val;
                i += 2;
            } else {
                out[j++] = src[i];
            }
        } else if (src[i] == '+') {
            out[j++] = ' ';
        } else {
            out[j++] = src[i];
        }
    }
    out[j] = '\0';
    return value_string_owned(out);
}

static LatValue native_csv_parse(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_STR) return value_nil();
    const char *input = args[0].as.str_val;
    size_t pos = 0, input_len = strlen(input);
    size_t rows_cap = 8, rows_len = 0;
    LatValue *rows = malloc(rows_cap * sizeof(LatValue));

    while (pos < input_len) {
        size_t fields_cap = 8, fields_len = 0;
        LatValue *fields = malloc(fields_cap * sizeof(LatValue));
        for (;;) {
            size_t field_cap = 64, field_len = 0;
            char *field = malloc(field_cap);
            if (pos < input_len && input[pos] == '"') {
                pos++;
                for (;;) {
                    if (pos >= input_len) break;
                    if (input[pos] == '"') {
                        if (pos + 1 < input_len && input[pos + 1] == '"') {
                            if (field_len + 1 >= field_cap) { field_cap *= 2; field = realloc(field, field_cap); }
                            field[field_len++] = '"'; pos += 2;
                        } else { pos++; break; }
                    } else {
                        if (field_len + 1 >= field_cap) { field_cap *= 2; field = realloc(field, field_cap); }
                        field[field_len++] = input[pos++];
                    }
                }
            } else {
                while (pos < input_len && input[pos] != ',' && input[pos] != '\n' && input[pos] != '\r') {
                    if (field_len + 1 >= field_cap) { field_cap *= 2; field = realloc(field, field_cap); }
                    field[field_len++] = input[pos++];
                }
            }
            field[field_len] = '\0';
            if (fields_len >= fields_cap) { fields_cap *= 2; fields = realloc(fields, fields_cap * sizeof(LatValue)); }
            fields[fields_len++] = value_string_owned(field);
            if (pos < input_len && input[pos] == ',') { pos++; } else break;
        }
        if (pos < input_len && input[pos] == '\r') pos++;
        if (pos < input_len && input[pos] == '\n') pos++;
        LatValue row = value_array(fields, fields_len);
        free(fields);
        if (rows_len >= rows_cap) { rows_cap *= 2; rows = realloc(rows, rows_cap * sizeof(LatValue)); }
        rows[rows_len++] = row;
    }
    LatValue result = value_array(rows, rows_len);
    free(rows);
    return result;
}

static LatValue native_csv_stringify(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_ARRAY) return value_nil();
    LatValue *data = &args[0];
    size_t out_cap = 256, out_len = 0;
    char *out = malloc(out_cap);
    for (size_t r = 0; r < data->as.array.len; r++) {
        LatValue *row = &data->as.array.elems[r];
        if (row->type != VAL_ARRAY) { free(out); return value_nil(); }
        for (size_t c = 0; c < row->as.array.len; c++) {
            if (c > 0) {
                if (out_len + 1 >= out_cap) { out_cap *= 2; out = realloc(out, out_cap); }
                out[out_len++] = ',';
            }
            char *field_str = value_display(&row->as.array.elems[c]);
            size_t flen = strlen(field_str);
            bool needs_quote = false;
            for (size_t k = 0; k < flen; k++) {
                if (field_str[k] == ',' || field_str[k] == '"' || field_str[k] == '\n' || field_str[k] == '\r') {
                    needs_quote = true; break;
                }
            }
            if (needs_quote) {
                size_t extra = 0;
                for (size_t k = 0; k < flen; k++) { if (field_str[k] == '"') extra++; }
                size_t needed = flen + extra + 2;
                while (out_len + needed >= out_cap) { out_cap *= 2; out = realloc(out, out_cap); }
                out[out_len++] = '"';
                for (size_t k = 0; k < flen; k++) {
                    if (field_str[k] == '"') out[out_len++] = '"';
                    out[out_len++] = field_str[k];
                }
                out[out_len++] = '"';
            } else {
                while (out_len + flen >= out_cap) { out_cap *= 2; out = realloc(out, out_cap); }
                memcpy(out + out_len, field_str, flen);
                out_len += flen;
            }
            free(field_str);
        }
        if (out_len + 1 >= out_cap) { out_cap *= 2; out = realloc(out, out_cap); }
        out[out_len++] = '\n';
    }
    out[out_len] = '\0';
    return value_string_owned(out);
}

static LatValue native_is_complete(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_STR) return value_bool(false);
    const char *source = args[0].as.str_val;
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec toks = lexer_tokenize(&lex, &lex_err);
    if (lex_err) { free(lex_err); lat_vec_free(&toks); return value_bool(false); }
    int depth = 0;
    for (size_t j = 0; j < toks.len; j++) {
        Token *t = lat_vec_get(&toks, j);
        switch (t->type) {
            case TOK_LBRACE: case TOK_LPAREN: case TOK_LBRACKET: depth++; break;
            case TOK_RBRACE: case TOK_RPAREN: case TOK_RBRACKET: depth--; break;
            default: break;
        }
    }
    for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
    lat_vec_free(&toks);
    return value_bool(depth <= 0);
}

static LatValue native_float_to_bits(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_FLOAT) return value_nil();
    double d = args[0].as.float_val;
    uint64_t bits; memcpy(&bits, &d, 8);
    return value_int((int64_t)bits);
}

static LatValue native_bits_to_float(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_INT) return value_nil();
    uint64_t bits = (uint64_t)args[0].as.int_val;
    double d; memcpy(&d, &bits, 8);
    return value_float(d);
}

static LatValue native_tokenize(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_STR) return value_nil();
    const char *source = args[0].as.str_val;
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec toks = lexer_tokenize(&lex, &lex_err);
    if (lex_err) { free(lex_err); lat_vec_free(&toks); return value_nil(); }
    size_t tok_count = toks.len > 0 ? toks.len - 1 : 0;
    LatValue *elems = malloc((tok_count > 0 ? tok_count : 1) * sizeof(LatValue));
    for (size_t j = 0; j < tok_count; j++) {
        Token *t = lat_vec_get(&toks, j);
        const char *type_str = token_type_name(t->type);
        char *text;
        if (t->type == TOK_IDENT || t->type == TOK_STRING_LIT || t->type == TOK_MODE_DIRECTIVE ||
            t->type == TOK_INTERP_START || t->type == TOK_INTERP_MID || t->type == TOK_INTERP_END) {
            text = strdup(t->as.str_val);
        } else if (t->type == TOK_INT_LIT) {
            (void)asprintf(&text, "%lld", (long long)t->as.int_val);
        } else if (t->type == TOK_FLOAT_LIT) {
            (void)asprintf(&text, "%g", t->as.float_val);
        } else {
            text = strdup(token_type_name(t->type));
        }
        char *fnames[3] = { "type", "text", "line" };
        LatValue fvals[3];
        fvals[0] = value_string(type_str);
        fvals[1] = value_string_owned(text);
        fvals[2] = value_int((int64_t)t->line);
        elems[j] = value_struct("Token", fnames, fvals, 3);
    }
    for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
    lat_vec_free(&toks);
    LatValue arr = value_array(elems, tok_count);
    free(elems);
    return arr;
}

static LatValue native_lat_eval(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_STR) return value_nil();
    VM *vm = current_vm;
    const char *source = args[0].as.str_val;
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec toks = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        (void)asprintf(&current_vm->error, "lat_eval: %s", lex_err);
        free(lex_err); lat_vec_free(&toks);
        return value_nil();
    }
    Parser parser = parser_new(&toks);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    if (parse_err) {
        (void)asprintf(&current_vm->error, "lat_eval: %s", parse_err);
        free(parse_err); program_free(&prog);
        for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
        lat_vec_free(&toks);
        return value_nil();
    }
    char *comp_err = NULL;
    Chunk *chunk = compile_repl(&prog, &comp_err);
    program_free(&prog);
    for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
    lat_vec_free(&toks);
    if (!chunk) {
        (void)asprintf(&current_vm->error, "lat_eval: %s", comp_err ? comp_err : "compile error");
        free(comp_err);
        return value_nil();
    }
    if (vm->fn_chunk_count >= vm->fn_chunk_cap) {
        vm->fn_chunk_cap = vm->fn_chunk_cap ? vm->fn_chunk_cap * 2 : 8;
        vm->fn_chunks = realloc(vm->fn_chunks, vm->fn_chunk_cap * sizeof(Chunk *));
    }
    vm->fn_chunks[vm->fn_chunk_count++] = chunk;
    LatValue result;
    VMResult r = vm_run(vm, chunk, &result);
    if (r != VM_OK) {
        /* Propagate the nested VM error */
        return value_nil();
    }
    return result;
}

static LatValue native_pipe(LatValue *args, int arg_count) {
    if (arg_count < 2) return value_nil();
    VM *vm = current_vm;
    LatValue current = value_deep_clone(&args[0]);
    for (int i = 1; i < arg_count; i++) {
        if (args[i].type != VAL_CLOSURE) { value_free(&current); return value_nil(); }
        LatValue result = vm_call_closure(vm, &args[i], &current, 1);
        value_free(&current);
        current = result;
    }
    return current;
}

static LatValue native_compose(LatValue *args, int arg_count) {
    if (arg_count < 2 || args[0].type != VAL_CLOSURE || args[1].type != VAL_CLOSURE)
        return value_nil();
    VM *vm = current_vm;

    /* Store f and g in env with unique names so the composed closure can find them */
    static int compose_counter = 0;
    char f_name[64], g_name[64];
    snprintf(f_name, sizeof(f_name), "__compose_f_%d", compose_counter);
    snprintf(g_name, sizeof(g_name), "__compose_g_%d", compose_counter);
    compose_counter++;
    env_define(vm->env, f_name, value_deep_clone(&args[0]));
    env_define(vm->env, g_name, value_deep_clone(&args[1]));

    /* Build chunk: GET_GLOBAL f, GET_GLOBAL g, GET_LOCAL 1(x), CALL 1, CALL 1, RETURN */
    Chunk *chunk = chunk_new();
    size_t f_idx = chunk_add_constant(chunk, value_string(f_name));
    size_t g_idx = chunk_add_constant(chunk, value_string(g_name));
    chunk_write(chunk, OP_GET_GLOBAL, 0);
    chunk_write(chunk, (uint8_t)f_idx, 0);
    chunk_write(chunk, OP_GET_GLOBAL, 0);
    chunk_write(chunk, (uint8_t)g_idx, 0);
    chunk_write(chunk, OP_GET_LOCAL, 0);
    chunk_write(chunk, 1, 0);
    chunk_write(chunk, OP_CALL, 0);
    chunk_write(chunk, 1, 0);
    chunk_write(chunk, OP_CALL, 0);
    chunk_write(chunk, 1, 0);
    chunk_write(chunk, OP_RETURN, 0);

    if (vm->fn_chunk_count >= vm->fn_chunk_cap) {
        vm->fn_chunk_cap = vm->fn_chunk_cap ? vm->fn_chunk_cap * 2 : 8;
        vm->fn_chunks = realloc(vm->fn_chunks, vm->fn_chunk_cap * sizeof(Chunk *));
    }
    vm->fn_chunks[vm->fn_chunk_count++] = chunk;

    /* Build a compiled closure with 1 parameter */
    char **params = malloc(sizeof(char *));
    params[0] = strdup("x");
    LatValue closure = value_closure(params, 1, NULL, NULL, NULL, false);
    closure.as.closure.native_fn = (void *)chunk;
    free(params);
    return closure;
}

/* ── Bytecode compilation/loading builtins ── */

static char *latc_read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)len, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static LatValue native_compile_file(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_nil();
    const char *path = args[0].as.str_val;

    char *source = latc_read_file(path);
    if (!source) return value_nil();

    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        free(lex_err);
        lat_vec_free(&tokens);
        free(source);
        return value_nil();
    }

    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    if (parse_err) {
        free(parse_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        free(source);
        return value_nil();
    }

    char *comp_err = NULL;
    Chunk *chunk = compile(&prog, &comp_err);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++)
        token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    free(source);

    if (!chunk) {
        free(comp_err);
        return value_nil();
    }

    size_t buf_len;
    uint8_t *buf = chunk_serialize(chunk, &buf_len);
    chunk_free(chunk);

    LatValue result = value_buffer(buf, buf_len);
    free(buf);
    return result;
}

static LatValue native_load_bytecode(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_nil();
    const char *path = args[0].as.str_val;

    char *err = NULL;
    Chunk *chunk = chunk_load(path, &err);
    if (!chunk) {
        free(err);
        return value_nil();
    }

    VM *vm = current_vm;
    if (!vm) {
        chunk_free(chunk);
        return value_nil();
    }

    LatValue result;
    VMResult res = vm_run(vm, chunk, &result);
    chunk_free(chunk);

    if (res != VM_OK) {
        /* Clear the error so the parent VM can continue */
        free(vm->error);
        vm->error = NULL;
        return value_nil();
    }
    return result;
}

/* Register a native function in the VM env. */
static void vm_register_native(VM *vm, const char *name, VMNativeFn fn, int arity) {
    (void)arity;
    LatValue v;
    v.type = VAL_CLOSURE;
    v.phase = VTAG_UNPHASED;
    v.as.closure.param_names = NULL;
    v.as.closure.param_count = 0; /* 0 so value_free/deep_clone don't iterate NULL param_names */
    v.as.closure.body = NULL;
    v.as.closure.captured_env = NULL;
    v.as.closure.default_values = VM_NATIVE_MARKER;
    v.as.closure.has_variadic = false;
    v.as.closure.native_fn = (void *)fn;
    v.region_id = REGION_NONE;
    env_define(vm->env, name, v);
}

/* ── VM lifecycle ── */

void vm_init(VM *vm) {
    memset(vm, 0, sizeof(VM));
    vm->stack_top = vm->stack;
    vm->env = env_new();
    vm->error = NULL;
    vm->open_upvalues = NULL;
    vm->handler_count = 0;
    vm->defer_count = 0;
    vm->struct_meta = NULL;
    vm->fn_chunks = NULL;
    vm->fn_chunk_count = 0;
    vm->fn_chunk_cap = 0;
    vm->module_cache = lat_map_new(sizeof(LatValue));
    vm->required_files = lat_map_new(sizeof(bool));
    vm->loaded_extensions = lat_map_new(sizeof(LatValue));
    vm->script_dir = NULL;
    vm->tracked_vars = NULL;
    vm->tracked_count = 0;
    vm->tracked_cap = 0;
    vm->pressures = NULL;
    vm->pressure_count = 0;
    vm->pressure_cap = 0;
    vm->ephemeral = bump_arena_new();

    /* Pre-build the call wrapper chunk: [OP_CALL, 0, OP_RETURN] */
    memset(&vm->call_wrapper, 0, sizeof(Chunk));
    vm->call_wrapper.code = malloc(3);
    vm->call_wrapper.code[0] = OP_CALL;
    vm->call_wrapper.code[1] = 0;
    vm->call_wrapper.code[2] = OP_RETURN;
    vm->call_wrapper.code_len = 3;
    vm->call_wrapper.code_cap = 3;
    vm->call_wrapper.lines = calloc(3, sizeof(int));
    vm->call_wrapper.lines_len = 3;
    vm->call_wrapper.lines_cap = 3;

    /* Register builtin functions */
    vm_register_native(vm, "to_string", native_to_string, 1);
    vm_register_native(vm, "typeof", native_typeof, 1);
    vm_register_native(vm, "len", native_len, 1);
    vm_register_native(vm, "parse_int", native_parse_int, 1);
    vm_register_native(vm, "parse_float", native_parse_float, 1);
    vm_register_native(vm, "ord", native_ord, 1);
    vm_register_native(vm, "chr", native_chr, 1);
    vm_register_native(vm, "abs", native_abs, 1);
    vm_register_native(vm, "floor", native_floor, 1);
    vm_register_native(vm, "ceil", native_ceil, 1);
    vm_register_native(vm, "exit", native_exit, 1);
    vm_register_native(vm, "error", native_error, 1);
    vm_register_native(vm, "is_error", native_is_error, 1);
    vm_register_native(vm, "Map::new", native_map_new, 0);
    vm_register_native(vm, "Set::new", native_set_new, 0);
    vm_register_native(vm, "Set::from", native_set_from, 1);
    vm_register_native(vm, "Channel::new", native_channel_new, 0);
    vm_register_native(vm, "Buffer::new", native_buffer_new, 1);
    vm_register_native(vm, "Buffer::from", native_buffer_from, 1);
    vm_register_native(vm, "Buffer::from_string", native_buffer_from_string, 1);
    vm_register_native(vm, "Ref::new", native_ref_new, 1);
    vm_register_native(vm, "phase_of", native_phase_of, 1);
    vm_register_native(vm, "assert", native_assert, 2);
    vm_register_native(vm, "version", native_version, 0);
    vm_register_native(vm, "input", native_input, 1);

    /* Phase system */
    vm_register_native(vm, "track", native_track, 1);
    vm_register_native(vm, "phases", native_phases, 1);
    vm_register_native(vm, "history", native_history, 1);
    vm_register_native(vm, "rewind", native_rewind, 2);
    vm_register_native(vm, "pressurize", native_pressurize, 2);
    vm_register_native(vm, "depressurize", native_depressurize, 1);
    vm_register_native(vm, "pressure_of", native_pressure_of, 1);
    vm_register_native(vm, "grow", native_grow, 1);

    /* Math */
    vm_register_native(vm, "round", native_round, 1);
    vm_register_native(vm, "sqrt", native_sqrt, 1);
    vm_register_native(vm, "pow", native_pow, 2);
    vm_register_native(vm, "min", native_min, 2);
    vm_register_native(vm, "max", native_max, 2);
    vm_register_native(vm, "random", native_random, 0);
    vm_register_native(vm, "random_int", native_random_int, 2);
    vm_register_native(vm, "log", native_log, 1);
    vm_register_native(vm, "log2", native_log2, 1);
    vm_register_native(vm, "log10", native_log10, 1);
    vm_register_native(vm, "sin", native_sin, 1);
    vm_register_native(vm, "cos", native_cos, 1);
    vm_register_native(vm, "tan", native_tan, 1);
    vm_register_native(vm, "asin", native_asin, 1);
    vm_register_native(vm, "acos", native_acos, 1);
    vm_register_native(vm, "atan", native_atan, 1);
    vm_register_native(vm, "atan2", native_atan2, 2);
    vm_register_native(vm, "exp", native_exp, 1);
    vm_register_native(vm, "sign", native_sign, 1);
    vm_register_native(vm, "gcd", native_gcd, 2);
    vm_register_native(vm, "lcm", native_lcm, 2);
    vm_register_native(vm, "is_nan", native_is_nan, 1);
    vm_register_native(vm, "is_inf", native_is_inf, 1);
    vm_register_native(vm, "sinh", native_sinh, 1);
    vm_register_native(vm, "cosh", native_cosh, 1);
    vm_register_native(vm, "tanh", native_tanh, 1);
    vm_register_native(vm, "lerp", native_lerp, 3);
    vm_register_native(vm, "clamp", native_clamp, 3);
    vm_register_native(vm, "math_pi", native_math_pi, 0);
    vm_register_native(vm, "math_e", native_math_e, 0);

    /* File system */
    vm_register_native(vm, "read_file", native_read_file, 1);
    vm_register_native(vm, "write_file", native_write_file, 2);
    vm_register_native(vm, "read_file_bytes", native_read_file_bytes, 1);
    vm_register_native(vm, "write_file_bytes", native_write_file_bytes, 2);
    vm_register_native(vm, "file_exists", native_file_exists, 1);
    vm_register_native(vm, "delete_file", native_delete_file, 1);
    vm_register_native(vm, "list_dir", native_list_dir, 1);
    vm_register_native(vm, "append_file", native_append_file, 2);
    vm_register_native(vm, "mkdir", native_mkdir, 1);
    vm_register_native(vm, "rename", native_fs_rename, 2);
    vm_register_native(vm, "is_dir", native_is_dir, 1);
    vm_register_native(vm, "is_file", native_is_file, 1);
    vm_register_native(vm, "rmdir", native_rmdir, 1);
    vm_register_native(vm, "glob", native_glob, 1);
    vm_register_native(vm, "stat", native_stat, 1);
    vm_register_native(vm, "copy_file", native_copy_file, 2);
    vm_register_native(vm, "realpath", native_realpath, 1);
    vm_register_native(vm, "tempdir", native_tempdir, 0);
    vm_register_native(vm, "tempfile", native_tempfile, 0);
    vm_register_native(vm, "chmod", native_chmod, 2);
    vm_register_native(vm, "file_size", native_file_size, 1);

    /* Bytecode compilation/loading */
    vm_register_native(vm, "compile_file", native_compile_file, 1);
    vm_register_native(vm, "load_bytecode", native_load_bytecode, 1);

    /* Path */
    vm_register_native(vm, "path_join", native_path_join, -1);
    vm_register_native(vm, "path_dir", native_path_dir, 1);
    vm_register_native(vm, "path_base", native_path_base, 1);
    vm_register_native(vm, "path_ext", native_path_ext, 1);

    /* Network TCP */
    vm_register_native(vm, "tcp_listen", native_tcp_listen, 2);
    vm_register_native(vm, "tcp_accept", native_tcp_accept, 1);
    vm_register_native(vm, "tcp_connect", native_tcp_connect, 2);
    vm_register_native(vm, "tcp_read", native_tcp_read, 1);
    vm_register_native(vm, "tcp_read_bytes", native_tcp_read_bytes, 2);
    vm_register_native(vm, "tcp_write", native_tcp_write, 2);
    vm_register_native(vm, "tcp_close", native_tcp_close, 1);
    vm_register_native(vm, "tcp_peer_addr", native_tcp_peer_addr, 1);
    vm_register_native(vm, "tcp_set_timeout", native_tcp_set_timeout, 2);

    /* TLS */
    vm_register_native(vm, "tls_connect", native_tls_connect, 2);
    vm_register_native(vm, "tls_read", native_tls_read, 1);
    vm_register_native(vm, "tls_read_bytes", native_tls_read_bytes, 2);
    vm_register_native(vm, "tls_write", native_tls_write, 2);
    vm_register_native(vm, "tls_close", native_tls_close, 1);
    vm_register_native(vm, "tls_available", native_tls_available, 0);

    /* HTTP */
    vm_register_native(vm, "http_get", native_http_get, 1);
    vm_register_native(vm, "http_post", native_http_post, 2);
    vm_register_native(vm, "http_request", native_http_request, 3);

    /* JSON/TOML/YAML */
    vm_register_native(vm, "json_parse", native_json_parse, 1);
    vm_register_native(vm, "json_stringify", native_json_stringify, 1);
    vm_register_native(vm, "toml_parse", native_toml_parse, 1);
    vm_register_native(vm, "toml_stringify", native_toml_stringify, 1);
    vm_register_native(vm, "yaml_parse", native_yaml_parse, 1);
    vm_register_native(vm, "yaml_stringify", native_yaml_stringify, 1);

    /* Crypto */
    vm_register_native(vm, "sha256", native_sha256, 1);
    vm_register_native(vm, "md5", native_md5, 1);
    vm_register_native(vm, "base64_encode", native_base64_encode, 1);
    vm_register_native(vm, "base64_decode", native_base64_decode, 1);

    /* Regex */
    vm_register_native(vm, "regex_match", native_regex_match, 2);
    vm_register_native(vm, "regex_find_all", native_regex_find_all, 2);
    vm_register_native(vm, "regex_replace", native_regex_replace, 3);

    /* Time/DateTime */
    vm_register_native(vm, "time", native_time, 0);
    vm_register_native(vm, "sleep", native_sleep, 1);
    vm_register_native(vm, "time_format", native_time_format, 2);
    vm_register_native(vm, "time_parse", native_time_parse, 2);

    /* Environment */
    vm_register_native(vm, "env", native_env, 1);
    vm_register_native(vm, "env_set", native_env_set, 2);
    vm_register_native(vm, "env_keys", native_env_keys, 0);

    /* Process */
    vm_register_native(vm, "cwd", native_cwd, 0);
    vm_register_native(vm, "exec", native_exec_cmd, 1);
    vm_register_native(vm, "shell", native_shell, 1);
    vm_register_native(vm, "platform", native_platform, 0);
    vm_register_native(vm, "hostname", native_hostname, 0);
    vm_register_native(vm, "pid", native_pid, 0);

    /* Type/utility */
    vm_register_native(vm, "to_int", native_to_int, 1);
    vm_register_native(vm, "to_float", native_to_float, 1);
    vm_register_native(vm, "struct_name", native_struct_name, 1);
    vm_register_native(vm, "struct_fields", native_struct_fields, 1);
    vm_register_native(vm, "struct_to_map", native_struct_to_map, 1);
    vm_register_native(vm, "repr", native_repr, 1);
    vm_register_native(vm, "format", native_format, -1);
    vm_register_native(vm, "range", native_range, -1);
    vm_register_native(vm, "print_raw", native_print_raw, -1);
    vm_register_native(vm, "eprint", native_eprint, -1);
    vm_register_native(vm, "identity", native_identity, 1);
    vm_register_native(vm, "debug_assert", native_debug_assert, 2);
    vm_register_native(vm, "panic", native_panic, 1);

    /* Module loading */
    vm_register_native(vm, "require", native_require, 1);
    vm_register_native(vm, "require_ext", native_require_ext, 1);

    /* Metaprogramming/reflection */
    vm_register_native(vm, "args", native_args, 0);
    vm_register_native(vm, "struct_from_map", native_struct_from_map, 2);
    vm_register_native(vm, "is_complete", native_is_complete, 1);
    vm_register_native(vm, "tokenize", native_tokenize, 1);
    vm_register_native(vm, "lat_eval", native_lat_eval, 1);

    /* Bitwise float conversion (for bytecode serialization) */
    vm_register_native(vm, "float_to_bits", native_float_to_bits, 1);
    vm_register_native(vm, "bits_to_float", native_bits_to_float, 1);

    /* URL encoding */
    vm_register_native(vm, "url_encode", native_url_encode, 1);
    vm_register_native(vm, "url_decode", native_url_decode, 1);

    /* CSV */
    vm_register_native(vm, "csv_parse", native_csv_parse, 1);
    vm_register_native(vm, "csv_stringify", native_csv_stringify, 1);

    /* Functional */
    vm_register_native(vm, "pipe", native_pipe, -1);
    vm_register_native(vm, "compose", native_compose, 2);

    intern_init();
}

void vm_free(VM *vm) {
    /* Free any remaining stack values */
    while (vm->stack_top > vm->stack) {
        vm->stack_top--;
        value_free(vm->stack_top);
    }
    if (vm->env) env_free(vm->env);
    free(vm->error);

    /* Free open upvalues */
    ObjUpvalue *uv = vm->open_upvalues;
    while (uv) {
        ObjUpvalue *next = uv->next;
        if (uv->location == &uv->closed)
            value_free(&uv->closed);
        free(uv);
        uv = next;
    }

    /* Free upvalue arrays in frames */
    for (size_t i = 0; i < vm->frame_count; i++) {
        CallFrame *f = &vm->frames[i];
        for (size_t j = 0; j < f->upvalue_count; j++) {
            if (f->upvalues[j] && f->upvalues[j]->location == &f->upvalues[j]->closed)
                value_free(&f->upvalues[j]->closed);
            free(f->upvalues[j]);
        }
        free(f->upvalues);
    }

    /* Free function chunks */
    for (size_t i = 0; i < vm->fn_chunk_count; i++)
        chunk_free(vm->fn_chunks[i]);
    free(vm->fn_chunks);

    /* Free module cache */
    for (size_t i = 0; i < vm->module_cache.cap; i++) {
        if (vm->module_cache.entries[i].state == MAP_OCCUPIED) {
            LatValue *v = (LatValue *)vm->module_cache.entries[i].value;
            value_free(v);
        }
    }
    lat_map_free(&vm->module_cache);
    lat_map_free(&vm->required_files);

    /* Free extension cache */
    for (size_t i = 0; i < vm->loaded_extensions.cap; i++) {
        if (vm->loaded_extensions.entries[i].state == MAP_OCCUPIED) {
            LatValue *v = (LatValue *)vm->loaded_extensions.entries[i].value;
            value_free(v);
        }
    }
    lat_map_free(&vm->loaded_extensions);
    free(vm->script_dir);

    if (vm->struct_meta) env_free(vm->struct_meta);

    /* Free tracked vars */
    for (size_t i = 0; i < vm->tracked_count; i++) {
        free(vm->tracked_vars[i].name);
        for (size_t j = 0; j < vm->tracked_vars[i].snap_count; j++) {
            free(vm->tracked_vars[i].snapshots[j].phase);
            free(vm->tracked_vars[i].snapshots[j].fn_name);
            value_free(&vm->tracked_vars[i].snapshots[j].value);
        }
        free(vm->tracked_vars[i].snapshots);
    }
    free(vm->tracked_vars);

    /* Free pressures */
    for (size_t i = 0; i < vm->pressure_count; i++) {
        free(vm->pressures[i].name);
        free(vm->pressures[i].mode);
    }
    free(vm->pressures);

    /* Free reactions */
    for (size_t i = 0; i < vm->reaction_count; i++) {
        free(vm->reactions[i].var_name);
        for (size_t j = 0; j < vm->reactions[i].cb_count; j++)
            value_free(&vm->reactions[i].callbacks[j]);
        free(vm->reactions[i].callbacks);
    }
    free(vm->reactions);

    /* Free bonds */
    for (size_t i = 0; i < vm->bond_count; i++) {
        free(vm->bonds[i].target);
        for (size_t j = 0; j < vm->bonds[i].dep_count; j++) {
            free(vm->bonds[i].deps[j]);
            if (vm->bonds[i].dep_strategies) free(vm->bonds[i].dep_strategies[j]);
        }
        free(vm->bonds[i].deps);
        free(vm->bonds[i].dep_strategies);
    }
    free(vm->bonds);

    /* Free seeds */
    for (size_t i = 0; i < vm->seed_count; i++) {
        free(vm->seeds[i].var_name);
        value_free(&vm->seeds[i].contract);
    }
    free(vm->seeds);

    bump_arena_free(vm->ephemeral);

    /* Free call wrapper (inline Chunk, not heap-allocated) */
    free(vm->call_wrapper.code);
    free(vm->call_wrapper.lines);

    intern_free();
}

void vm_print_stack_trace(VM *vm) {
    if (vm->frame_count <= 1) return;  /* No trace for top-level errors */
    fprintf(stderr, "stack trace (most recent call last):\n");
    for (size_t i = 0; i < vm->frame_count; i++) {
        CallFrame *f = &vm->frames[i];
        if (!f->chunk) continue;
        size_t offset = (size_t)(f->ip - f->chunk->code);
        if (offset > 0) offset--;
        int line = 0;
        if (f->chunk->lines && offset < f->chunk->lines_len)
            line = f->chunk->lines[offset];
        const char *name = f->chunk->name;
        if (name && name[0])
            fprintf(stderr, "  [line %d] in %s()\n", line, name);
        else if (i == 0)
            fprintf(stderr, "  [line %d] in <script>\n", line);
        else
            fprintf(stderr, "  [line %d] in <closure>\n", line);
    }
}

/* ── Concurrency infrastructure ── */

void vm_track_chunk(VM *vm, Chunk *ch) {
    if (vm->fn_chunk_count >= vm->fn_chunk_cap) {
        vm->fn_chunk_cap = vm->fn_chunk_cap ? vm->fn_chunk_cap * 2 : 8;
        vm->fn_chunks = realloc(vm->fn_chunks, vm->fn_chunk_cap * sizeof(Chunk *));
    }
    vm->fn_chunks[vm->fn_chunk_count++] = ch;
}

#ifndef __EMSCRIPTEN__

typedef struct {
    Chunk     *chunk;       /* compiled spawn body (parent owns via fn_chunks) */
    VM        *child_vm;    /* independent VM for thread */
    char      *error;       /* NULL on success */
    pthread_t  thread;
} VMSpawnTask;

VM *vm_clone_for_thread(VM *parent) {
    VM *child = calloc(1, sizeof(VM));
    child->stack_top = child->stack;
    child->env = env_clone(parent->env);
    child->error = NULL;
    child->open_upvalues = NULL;
    child->handler_count = 0;
    child->defer_count = 0;
    child->struct_meta = parent->struct_meta; /* shared read-only */
    child->fn_chunks = NULL;
    child->fn_chunk_count = 0;
    child->fn_chunk_cap = 0;
    child->module_cache = lat_map_new(sizeof(LatValue));
    child->required_files = lat_map_new(sizeof(bool));
    child->loaded_extensions = lat_map_new(sizeof(LatValue));
    child->script_dir = parent->script_dir ? strdup(parent->script_dir) : NULL;
    child->prog_argc = parent->prog_argc;
    child->prog_argv = parent->prog_argv;
    child->tracked_vars = NULL;
    child->tracked_count = 0;
    child->tracked_cap = 0;
    child->pressures = NULL;
    child->pressure_count = 0;
    child->pressure_cap = 0;
    child->reactions = NULL;
    child->reaction_count = 0;
    child->reaction_cap = 0;
    child->bonds = NULL;
    child->bond_count = 0;
    child->bond_cap = 0;
    child->seeds = NULL;
    child->seed_count = 0;
    child->seed_cap = 0;
    child->ephemeral = bump_arena_new();

    /* Pre-build the call wrapper chunk */
    memset(&child->call_wrapper, 0, sizeof(Chunk));
    child->call_wrapper.code = malloc(3);
    child->call_wrapper.code[0] = OP_CALL;
    child->call_wrapper.code[1] = 0;
    child->call_wrapper.code[2] = OP_RETURN;
    child->call_wrapper.code_len = 3;
    child->call_wrapper.code_cap = 3;
    child->call_wrapper.lines = calloc(3, sizeof(int));
    child->call_wrapper.lines_len = 3;
    child->call_wrapper.lines_cap = 3;

    return child;
}

void vm_free_child(VM *child) {
    /* Free stack values */
    while (child->stack_top > child->stack) {
        child->stack_top--;
        value_free(child->stack_top);
    }
    if (child->env) env_free(child->env);
    free(child->error);

    /* Free open upvalues */
    ObjUpvalue *uv = child->open_upvalues;
    while (uv) {
        ObjUpvalue *next = uv->next;
        if (uv->location == &uv->closed)
            value_free(&uv->closed);
        free(uv);
        uv = next;
    }

    /* Free upvalue arrays in frames */
    for (size_t i = 0; i < child->frame_count; i++) {
        CallFrame *f = &child->frames[i];
        for (size_t j = 0; j < f->upvalue_count; j++) {
            if (f->upvalues[j] && f->upvalues[j]->location == &f->upvalues[j]->closed)
                value_free(&f->upvalues[j]->closed);
            free(f->upvalues[j]);
        }
        free(f->upvalues);
    }

    /* Free child-owned fn_chunks */
    for (size_t i = 0; i < child->fn_chunk_count; i++)
        chunk_free(child->fn_chunks[i]);
    free(child->fn_chunks);

    /* Free caches (shallow — children don't import modules) */
    lat_map_free(&child->module_cache);
    lat_map_free(&child->required_files);
    lat_map_free(&child->loaded_extensions);
    free(child->script_dir);
    /* struct_meta is shared — parent owns it */

    /* Free tracked vars */
    for (size_t i = 0; i < child->tracked_count; i++) {
        free(child->tracked_vars[i].name);
        for (size_t j = 0; j < child->tracked_vars[i].snap_count; j++) {
            free(child->tracked_vars[i].snapshots[j].phase);
            free(child->tracked_vars[i].snapshots[j].fn_name);
            value_free(&child->tracked_vars[i].snapshots[j].value);
        }
        free(child->tracked_vars[i].snapshots);
    }
    free(child->tracked_vars);
    for (size_t i = 0; i < child->pressure_count; i++) {
        free(child->pressures[i].name);
        free(child->pressures[i].mode);
    }
    free(child->pressures);
    bump_arena_free(child->ephemeral);

    /* Free call wrapper */
    free(child->call_wrapper.code);
    free(child->call_wrapper.lines);

    free(child);
}

/* Export current frame's live locals into child's env as globals,
 * so re-compiled code can access them via OP_GET_GLOBAL. */
static void vm_export_locals_to_env(VM *parent, VM *child) {
    for (size_t fi = 0; fi < parent->frame_count; fi++) {
        CallFrame *f = &parent->frames[fi];
        if (!f->chunk) continue;
        size_t local_count = (size_t)(parent->stack_top - f->slots);
        if (fi + 1 < parent->frame_count)
            local_count = (size_t)(parent->frames[fi + 1].slots - f->slots);
        for (size_t slot = 0; slot < local_count; slot++) {
            if (slot < f->chunk->local_name_cap && f->chunk->local_names[slot]) {
                env_define(child->env, f->chunk->local_names[slot],
                           value_deep_clone(&f->slots[slot]));
            }
        }
    }
}

static void *vm_spawn_thread_fn(void *arg) {
    VMSpawnTask *task = arg;
    current_vm = task->child_vm;

    /* Set up thread-local heap */
    DualHeap *heap = dual_heap_new();
    value_set_heap(heap);
    value_set_arena(NULL);

    LatValue result;
    VMResult r = vm_run(task->child_vm, task->chunk, &result);
    if (r != VM_OK) {
        task->error = task->child_vm->error;
        task->child_vm->error = NULL;
    } else {
        value_free(&result);
    }

    dual_heap_free(heap);
    return NULL;
}

#endif /* __EMSCRIPTEN__ */

/* ── Builtin method helpers ── */

/* Check if a pressure mode blocks growth (push/insert) */
static bool pressure_blocks_grow(const char *mode) {
    return mode && (strcmp(mode, "no_grow") == 0 || strcmp(mode, "no_resize") == 0);
}

/* Check if a pressure mode blocks shrinkage (pop/remove) */
static bool pressure_blocks_shrink(const char *mode) {
    return mode && (strcmp(mode, "no_shrink") == 0 || strcmp(mode, "no_resize") == 0);
}

/* Find pressure mode for a variable name, or NULL if none */
static const char *vm_find_pressure(VM *vm, const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < vm->pressure_count; i++) {
        if (strcmp(vm->pressures[i].name, name) == 0)
            return vm->pressures[i].mode;
    }
    return NULL;
}

/* ── Pre-computed djb2 hashes for builtin method names ── */
#define MHASH_all              0x0b885ddeu
#define MHASH_any              0x0b885e2du
#define MHASH_bytes            0x0f30b64cu
#define MHASH_chars            0x0f392d36u
#define MHASH_chunk            0x0f3981beu
#define MHASH_close            0x0f3b9a5bu
#define MHASH_contains         0x42aa8264u
#define MHASH_count            0x0f3d586eu
#define MHASH_difference       0x52a92470u
#define MHASH_drop             0x7c95d91au
#define MHASH_each             0x7c961b96u
#define MHASH_ends_with        0x9079bb6au
#define MHASH_entries          0x6b84747fu
#define MHASH_enum_name        0x9f13be1au
#define MHASH_enumerate        0x9f82838bu
#define MHASH_filter           0xfd7675abu
#define MHASH_find             0x7c96cb66u
#define MHASH_first            0x0f704b8du
#define MHASH_flat             0x7c96d68cu
#define MHASH_flat_map         0x022d3129u
#define MHASH_for_each         0x0f4aaefcu
#define MHASH_get              0x0b887685u
#define MHASH_group_by         0xdd0fdaecu
#define MHASH_has              0x0b887a41u
#define MHASH_index_of         0x66e4af51u
#define MHASH_insert           0x04d4029au
#define MHASH_intersection     0x40c04d3cu
#define MHASH_is_empty         0xdc1854cfu
#define MHASH_is_subset        0x805437d6u
#define MHASH_is_superset      0x05f3913bu
#define MHASH_is_variant       0x443eb735u
#define MHASH_join             0x7c9915d5u
#define MHASH_keys             0x7c9979c1u
#define MHASH_last             0x7c99f459u
#define MHASH_len              0x0b888bc4u
#define MHASH_map              0x0b888f83u
#define MHASH_max              0x0b888f8bu
#define MHASH_merge            0x0fecc3f5u
#define MHASH_min              0x0b889089u
#define MHASH_pad_left         0xf3895c84u
#define MHASH_pad_right        0x6523b4b7u
#define MHASH_payload          0x9c4949cfu
#define MHASH_pop              0x0b889e14u
#define MHASH_push             0x7c9c7ae5u
#define MHASH_recv             0x7c9d4d95u
#define MHASH_reduce           0x19279c1du
#define MHASH_add              0x0b885cceu
#define MHASH_remove           0x192c7473u
#define MHASH_remove_at        0xd988a4a7u
#define MHASH_repeat           0x192dec66u
#define MHASH_replace          0x3eef4e01u
#define MHASH_reverse          0x3f5854c1u
#define MHASH_send             0x7c9ddb4fu
#define MHASH_set              0x0b88a991u
#define MHASH_slice            0x105d06d5u
#define MHASH_sort             0x7c9e066du
#define MHASH_sort_by          0xa365ac87u
#define MHASH_split            0x105f45f1u
#define MHASH_starts_with      0xf5ef8361u
#define MHASH_substring        0xcc998606u
#define MHASH_sum              0x0b88ab9au
#define MHASH_tag              0x0b88ad41u
#define MHASH_take             0x7c9e564au
#define MHASH_to_array         0xcebde966u
#define MHASH_to_lower         0xcf836790u
#define MHASH_to_upper         0xd026b2b3u
#define MHASH_trim             0x7c9e9e61u
#define MHASH_trim_end         0xcdcebb17u
#define MHASH_trim_start       0x7d6a808eu
#define MHASH_union            0x1082522eu
#define MHASH_unique           0x20cca1bcu
#define MHASH_values           0x22383ff5u
#define MHASH_variant_name     0xb2b2b8bau
#define MHASH_zip              0x0b88c7d8u
/* Ref methods */
#define MHASH_deref            0x0f49e72bu
#define MHASH_inner_type       0xdf644222u
/* Buffer methods */
#define MHASH_push_u16         0x1aaf75a0u
#define MHASH_push_u32         0x1aaf75deu
#define MHASH_read_u8          0x3ddb750du
#define MHASH_write_u8         0x931616bcu
#define MHASH_read_u16         0xf94a15fcu
#define MHASH_write_u16        0xf5d8ed8bu
#define MHASH_read_u32         0xf94a163au
#define MHASH_write_u32        0xf5d8edc9u
#define MHASH_clear            0x0f3b6d8cu
#define MHASH_fill             0x7c96cb2cu
#define MHASH_resize           0x192fa5b7u
#define MHASH_to_string        0xd09c437eu
#define MHASH_to_hex           0x1e83ed8cu
#define MHASH_capacity         0x104ec913u

static inline uint32_t method_hash(const char *s) {
    uint32_t h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

/* Returns true if the builtin method is "simple" — no user closures executed,
 * safe for direct-pointer mutation without clone-mutate-writeback. */
static inline bool vm_invoke_builtin_is_simple(uint32_t mhash) {
    return !(mhash == MHASH_map || mhash == MHASH_filter ||
             mhash == MHASH_reduce || mhash == MHASH_each ||
             mhash == MHASH_sort || mhash == MHASH_find ||
             mhash == MHASH_any || mhash == MHASH_all);
}

static bool vm_invoke_builtin(VM *vm, LatValue *obj, const char *method, int arg_count, const char *var_name) {
    uint32_t mhash = method_hash(method);

    switch (obj->type) {
    /* Array methods */
    case VAL_ARRAY: {
        if (mhash == MHASH_len && strcmp(method, "len") == 0 && arg_count == 0) {
            push(vm, value_int((int64_t)obj->as.array.len));
            return true;
        }
        if (mhash == MHASH_push && strcmp(method, "push") == 0 && arg_count == 1) {
            /* Check phase: crystal and sublimated values are immutable */
            if (obj->phase == VTAG_CRYSTAL || obj->phase == VTAG_SUBLIMATED) {
                LatValue val = pop(vm);
                value_free(&val);
                const char *phase_name = obj->phase == VTAG_CRYSTAL ? "crystal" : "sublimated";
                char *err = NULL;
                (void)asprintf(&err, "cannot push to %s array", phase_name);
                current_vm->error = err;
                push(vm, value_unit());
                return true;
            }
            /* Check pressure constraint */
            const char *pmode = vm_find_pressure(vm, var_name);
            if (pressure_blocks_grow(pmode)) {
                LatValue val = pop(vm);
                value_free(&val);
                char *err = NULL;
                (void)asprintf(&err, "pressurized (%s): cannot push to '%s'", pmode, var_name);
                vm->error = err;
                push(vm, value_unit());
                return true;
            }
            LatValue val = pop(vm);
            vm_promote_value(&val);
            /* Mutate the array in-place */
            if (obj->as.array.len >= obj->as.array.cap) {
                obj->as.array.cap = obj->as.array.cap ? obj->as.array.cap * 2 : 4;
                obj->as.array.elems = realloc(obj->as.array.elems,
                    obj->as.array.cap * sizeof(LatValue));
            }
            obj->as.array.elems[obj->as.array.len++] = val;
            push(vm, value_unit());
            return true;
        }
        if (mhash == MHASH_pop && strcmp(method, "pop") == 0 && arg_count == 0) {
            /* Check phase: crystal and sublimated values are immutable */
            if (obj->phase == VTAG_CRYSTAL || obj->phase == VTAG_SUBLIMATED) {
                const char *phase_name = obj->phase == VTAG_CRYSTAL ? "crystal" : "sublimated";
                char *err = NULL;
                (void)asprintf(&err, "cannot pop from %s array", phase_name);
                current_vm->error = err;
                push(vm, value_unit());
                return true;
            }
            /* Check pressure constraint */
            const char *pmode = vm_find_pressure(vm, var_name);
            if (pressure_blocks_shrink(pmode)) {
                char *err = NULL;
                (void)asprintf(&err, "pressurized (%s): cannot pop from '%s'", pmode, var_name);
                vm->error = err;
                push(vm, value_unit());
                return true;
            }
            if (obj->as.array.len == 0) {
                push(vm, value_nil());
            } else {
                push(vm, obj->as.array.elems[--obj->as.array.len]);
            }
            return true;
        }
        if (mhash == MHASH_contains && strcmp(method, "contains") == 0 && arg_count == 1) {
            LatValue needle = pop(vm);
            bool found = false;
            for (size_t i = 0; i < obj->as.array.len; i++) {
                if (value_eq(&obj->as.array.elems[i], &needle)) {
                    found = true;
                    break;
                }
            }
            value_free(&needle);
            push(vm, value_bool(found));
            return true;
        }
        if (mhash == MHASH_enumerate && strcmp(method, "enumerate") == 0 && arg_count == 0) {
            /* Build array of [index, element] pairs */
            LatValue *pairs = malloc(obj->as.array.len * sizeof(LatValue));
            for (size_t i = 0; i < obj->as.array.len; i++) {
                LatValue pair_elems[2];
                pair_elems[0] = value_int((int64_t)i);
                pair_elems[1] = value_clone_fast(&obj->as.array.elems[i]);
                pairs[i] = value_array(pair_elems, 2);
            }
            LatValue result = value_array(pairs, obj->as.array.len);
            free(pairs);
            push(vm, result);
            return true;
        }
        if (mhash == MHASH_reverse && strcmp(method, "reverse") == 0 && arg_count == 0) {
            LatValue *elems = malloc(obj->as.array.len * sizeof(LatValue));
            for (size_t i = 0; i < obj->as.array.len; i++)
                elems[i] = value_deep_clone(&obj->as.array.elems[obj->as.array.len - 1 - i]);
            LatValue result = value_array(elems, obj->as.array.len);
            free(elems);
            push(vm, result);
            return true;
        }
        if (mhash == MHASH_join && strcmp(method, "join") == 0 && arg_count == 1) {
            LatValue sep = pop(vm);
            const char *sep_str = (sep.type == VAL_STR) ? sep.as.str_val : "";
            size_t sep_len = strlen(sep_str);
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
                if (i > 0) { memcpy(buf + pos, sep_str, sep_len); pos += sep_len; }
                memcpy(buf + pos, parts[i], lens[i]); pos += lens[i];
                free(parts[i]);
            }
            buf[pos] = '\0';
            free(parts); free(lens);
            value_free(&sep);
            push(vm, value_string_owned(buf));
            return true;
        }
        if (mhash == MHASH_map && strcmp(method, "map") == 0 && arg_count == 1) {
            LatValue closure = pop(vm);
            size_t len = obj->as.array.len;
            LatValue *elems = malloc(len * sizeof(LatValue));
            for (size_t i = 0; i < len; i++) {
                LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
                elems[i] = vm_call_closure(vm, &closure, &arg, 1);
                value_free(&arg);
            }
            LatValue result = value_array(elems, len);
            free(elems);
            value_free(&closure);
            push(vm, result);
            return true;
        }
        if (mhash == MHASH_filter && strcmp(method, "filter") == 0 && arg_count == 1) {
            LatValue closure = pop(vm);
            size_t len = obj->as.array.len;
            size_t cap = len > 0 ? len : 1;
            LatValue *elems = malloc(cap * sizeof(LatValue));
            size_t out_len = 0;
            for (size_t i = 0; i < len; i++) {
                LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
                LatValue pred = vm_call_closure(vm, &closure, &arg, 1);
                bool keep = (pred.type == VAL_BOOL && pred.as.bool_val);
                value_free(&pred);
                if (keep) {
                    elems[out_len++] = arg;
                } else {
                    value_free(&arg);
                }
            }
            LatValue result = value_array(elems, out_len);
            free(elems);
            value_free(&closure);
            push(vm, result);
            return true;
        }
        if (mhash == MHASH_reduce && strcmp(method, "reduce") == 0 && arg_count == 2) {
            LatValue acc = pop(vm);       /* second arg: initial value (TOS) */
            LatValue closure = pop(vm);   /* first arg: closure */
            size_t len = obj->as.array.len;
            for (size_t i = 0; i < len; i++) {
                LatValue elem = value_deep_clone(&obj->as.array.elems[i]);
                LatValue args[2] = { acc, elem };
                acc = vm_call_closure(vm, &closure, args, 2);
                value_free(&args[0]);
                value_free(&args[1]);
            }
            value_free(&closure);
            push(vm, acc);
            return true;
        }
        if (mhash == MHASH_each && strcmp(method, "each") == 0 && arg_count == 1) {
            LatValue closure = pop(vm);
            size_t len = obj->as.array.len;
            for (size_t i = 0; i < len; i++) {
                LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
                LatValue r = vm_call_closure(vm, &closure, &arg, 1);
                value_free(&arg);
                value_free(&r);
            }
            value_free(&closure);
            push(vm, value_nil());
            return true;
        }
        if (mhash == MHASH_sort && strcmp(method, "sort") == 0 && arg_count <= 1) {
            LatValue closure;
            bool has_cmp = (arg_count == 1);
            if (has_cmp) closure = pop(vm);

            /* Deep clone the array for sorting */
            size_t len = obj->as.array.len;
            LatValue *elems = malloc(len * sizeof(LatValue));
            for (size_t i = 0; i < len; i++)
                elems[i] = value_deep_clone(&obj->as.array.elems[i]);

            /* Simple insertion sort (stable, fine for typical sizes) */
            for (size_t i = 1; i < len; i++) {
                LatValue key = elems[i];
                size_t j = i;
                while (j > 0) {
                    bool should_swap;
                    if (has_cmp) {
                        LatValue args[2];
                        args[0] = value_clone_fast(&elems[j - 1]);
                        args[1] = value_clone_fast(&key);
                        LatValue cmp = vm_call_closure(vm, &closure, args, 2);
                        should_swap = (cmp.type == VAL_INT && cmp.as.int_val > 0) ||
                                      (cmp.type == VAL_BOOL && !cmp.as.bool_val);
                        value_free(&args[0]);
                        value_free(&args[1]);
                        value_free(&cmp);
                    } else {
                        /* Default: ascending for ints, floats, strings */
                        if (elems[j - 1].type == VAL_INT && key.type == VAL_INT) {
                            should_swap = elems[j - 1].as.int_val > key.as.int_val;
                        } else if (elems[j - 1].type == VAL_FLOAT && key.type == VAL_FLOAT) {
                            should_swap = elems[j - 1].as.float_val > key.as.float_val;
                        } else if ((elems[j - 1].type == VAL_INT || elems[j - 1].type == VAL_FLOAT) &&
                                   (key.type == VAL_INT || key.type == VAL_FLOAT)) {
                            double a_d = elems[j - 1].type == VAL_INT ?
                                (double)elems[j - 1].as.int_val : elems[j - 1].as.float_val;
                            double b_d = key.type == VAL_INT ? (double)key.as.int_val : key.as.float_val;
                            should_swap = a_d > b_d;
                        } else if (elems[j - 1].type == VAL_STR && key.type == VAL_STR) {
                            should_swap = strcmp(elems[j - 1].as.str_val, key.as.str_val) > 0;
                        } else {
                            /* Mixed non-numeric types — error */
                            for (size_t fi = 0; fi < len; fi++) value_free(&elems[fi]);
                            free(elems);
                            current_vm->error = strdup("sort: cannot compare mixed types");
                            push(vm, value_unit());
                            return true;
                        }
                    }
                    if (!should_swap) break;
                    elems[j] = elems[j - 1];
                    j--;
                }
                elems[j] = key;
            }

            LatValue result = value_array(elems, len);
            free(elems);
            if (has_cmp) value_free(&closure);
            push(vm, result);
            return true;
        }
        if (mhash == MHASH_for_each && strcmp(method, "for_each") == 0 && arg_count == 1) {
            LatValue closure = pop(vm);
            size_t len = obj->as.array.len;
            for (size_t i = 0; i < len; i++) {
                LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
                LatValue r = vm_call_closure(vm, &closure, &arg, 1);
                value_free(&arg); value_free(&r);
            }
            value_free(&closure);
            push(vm, value_unit());
            return true;
        }
        if (mhash == MHASH_find && strcmp(method, "find") == 0 && arg_count == 1) {
            LatValue closure = pop(vm);
            for (size_t i = 0; i < obj->as.array.len; i++) {
                LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
                LatValue pred = vm_call_closure(vm, &closure, &arg, 1);
                bool match = (pred.type == VAL_BOOL && pred.as.bool_val);
                value_free(&arg); value_free(&pred);
                if (match) {
                    value_free(&closure);
                    push(vm, value_clone_fast(&obj->as.array.elems[i]));
                    return true;
                }
            }
            value_free(&closure);
            push(vm, value_unit());
            return true;
        }
        if (mhash == MHASH_any && strcmp(method, "any") == 0 && arg_count == 1) {
            LatValue closure = pop(vm);
            for (size_t i = 0; i < obj->as.array.len; i++) {
                LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
                LatValue pred = vm_call_closure(vm, &closure, &arg, 1);
                bool match = (pred.type == VAL_BOOL && pred.as.bool_val);
                value_free(&arg); value_free(&pred);
                if (match) { value_free(&closure); push(vm, value_bool(true)); return true; }
            }
            value_free(&closure); push(vm, value_bool(false)); return true;
        }
        if (mhash == MHASH_all && strcmp(method, "all") == 0 && arg_count == 1) {
            LatValue closure = pop(vm);
            for (size_t i = 0; i < obj->as.array.len; i++) {
                LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
                LatValue pred = vm_call_closure(vm, &closure, &arg, 1);
                bool match = (pred.type == VAL_BOOL && pred.as.bool_val);
                value_free(&arg); value_free(&pred);
                if (!match) { value_free(&closure); push(vm, value_bool(false)); return true; }
            }
            value_free(&closure); push(vm, value_bool(true)); return true;
        }
        if (mhash == MHASH_flat && strcmp(method, "flat") == 0 && arg_count == 0) {
            push(vm, array_flat(obj));
            return true;
        }
        if (mhash == MHASH_slice && strcmp(method, "slice") == 0 && arg_count == 2) {
            LatValue end_v = pop(vm); LatValue start_v = pop(vm);
            char *err = NULL;
            LatValue r = array_slice(obj, start_v.as.int_val, end_v.as.int_val, &err);
            value_free(&start_v); value_free(&end_v);
            if (err) { free(err); push(vm, value_array(NULL, 0)); }
            else push(vm, r);
            return true;
        }
        if (mhash == MHASH_take && strcmp(method, "take") == 0 && arg_count == 1) {
            LatValue n_v = pop(vm);
            int64_t n = n_v.as.int_val;
            value_free(&n_v);
            if (n <= 0) { push(vm, value_array(NULL, 0)); return true; }
            size_t take_n = (size_t)n;
            if (take_n > obj->as.array.len) take_n = obj->as.array.len;
            LatValue *elems = malloc((take_n > 0 ? take_n : 1) * sizeof(LatValue));
            for (size_t i = 0; i < take_n; i++)
                elems[i] = value_deep_clone(&obj->as.array.elems[i]);
            LatValue r = value_array(elems, take_n); free(elems);
            push(vm, r); return true;
        }
        if (mhash == MHASH_drop && strcmp(method, "drop") == 0 && arg_count == 1) {
            LatValue n_v = pop(vm);
            int64_t n = n_v.as.int_val;
            value_free(&n_v);
            size_t start = (n > 0) ? (size_t)n : 0;
            if (start >= obj->as.array.len) { push(vm, value_array(NULL, 0)); return true; }
            size_t cnt = obj->as.array.len - start;
            LatValue *elems = malloc(cnt * sizeof(LatValue));
            for (size_t i = 0; i < cnt; i++)
                elems[i] = value_deep_clone(&obj->as.array.elems[start + i]);
            LatValue r = value_array(elems, cnt); free(elems);
            push(vm, r); return true;
        }
        if (mhash == MHASH_index_of && strcmp(method, "index_of") == 0 && arg_count == 1) {
            LatValue needle = pop(vm);
            int64_t idx = -1;
            for (size_t i = 0; i < obj->as.array.len; i++) {
                if (value_eq(&obj->as.array.elems[i], &needle)) { idx = (int64_t)i; break; }
            }
            value_free(&needle);
            push(vm, value_int(idx)); return true;
        }
        if (mhash == MHASH_zip && strcmp(method, "zip") == 0 && arg_count == 1) {
            LatValue other = pop(vm);
            if (other.type != VAL_ARRAY) { value_free(&other); push(vm, value_nil()); return true; }
            size_t n = obj->as.array.len < other.as.array.len ? obj->as.array.len : other.as.array.len;
            LatValue *pairs = malloc((n > 0 ? n : 1) * sizeof(LatValue));
            for (size_t i = 0; i < n; i++) {
                LatValue pe[2];
                pe[0] = value_deep_clone(&obj->as.array.elems[i]);
                pe[1] = value_deep_clone(&other.as.array.elems[i]);
                pairs[i] = value_array(pe, 2);
            }
            value_free(&other);
            LatValue r = value_array(pairs, n); free(pairs);
            push(vm, r); return true;
        }
        if (mhash == MHASH_unique && strcmp(method, "unique") == 0 && arg_count == 0) {
            size_t n = obj->as.array.len;
            LatValue *res = malloc((n > 0 ? n : 1) * sizeof(LatValue));
            size_t rc = 0;
            for (size_t i = 0; i < n; i++) {
                bool dup = false;
                for (size_t j = 0; j < rc; j++)
                    if (value_eq(&obj->as.array.elems[i], &res[j])) { dup = true; break; }
                if (!dup) res[rc++] = value_deep_clone(&obj->as.array.elems[i]);
            }
            LatValue r = value_array(res, rc); free(res);
            push(vm, r); return true;
        }
        if (mhash == MHASH_remove_at && strcmp(method, "remove_at") == 0 && arg_count == 1) {
            const char *pmode = vm_find_pressure(vm, var_name);
            if (pressure_blocks_shrink(pmode)) {
                LatValue idx_v = pop(vm); value_free(&idx_v);
                char *err = NULL;
                (void)asprintf(&err, "pressurized (%s): cannot remove_at from '%s'", pmode, var_name);
                vm->error = err;
                push(vm, value_unit()); return true;
            }
            LatValue idx_v = pop(vm);
            int64_t idx = idx_v.as.int_val;
            value_free(&idx_v);
            if (idx < 0 || (size_t)idx >= obj->as.array.len) { push(vm, value_nil()); return true; }
            LatValue removed = obj->as.array.elems[(size_t)idx];
            memmove(&obj->as.array.elems[(size_t)idx],
                    &obj->as.array.elems[(size_t)idx + 1],
                    (obj->as.array.len - (size_t)idx - 1) * sizeof(LatValue));
            obj->as.array.len--;
            push(vm, removed);
            return true;
        }
        if (mhash == MHASH_chunk && strcmp(method, "chunk") == 0 && arg_count == 1) {
            LatValue cs_v = pop(vm);
            int64_t cs = cs_v.as.int_val;
            value_free(&cs_v);
            if (cs <= 0) { push(vm, value_array(NULL, 0)); return true; }
            size_t n = obj->as.array.len;
            size_t nc = (n > 0) ? (n + (size_t)cs - 1) / (size_t)cs : 0;
            LatValue *chunks = malloc((nc > 0 ? nc : 1) * sizeof(LatValue));
            for (size_t ci = 0; ci < nc; ci++) {
                size_t s = ci * (size_t)cs, e = s + (size_t)cs;
                if (e > n) e = n;
                size_t cl = e - s;
                LatValue *ce = malloc(cl * sizeof(LatValue));
                for (size_t j = 0; j < cl; j++)
                    ce[j] = value_deep_clone(&obj->as.array.elems[s + j]);
                chunks[ci] = value_array(ce, cl); free(ce);
            }
            LatValue r = value_array(chunks, nc); free(chunks);
            push(vm, r); return true;
        }
        if (mhash == MHASH_sum && strcmp(method, "sum") == 0 && arg_count == 0) {
            bool has_float = false;
            int64_t isum = 0; double fsum = 0.0;
            for (size_t i = 0; i < obj->as.array.len; i++) {
                if (obj->as.array.elems[i].type == VAL_INT) {
                    isum += obj->as.array.elems[i].as.int_val;
                    fsum += (double)obj->as.array.elems[i].as.int_val;
                } else if (obj->as.array.elems[i].type == VAL_FLOAT) {
                    has_float = true;
                    fsum += obj->as.array.elems[i].as.float_val;
                }
            }
            push(vm, has_float ? value_float(fsum) : value_int(isum));
            return true;
        }
        if (mhash == MHASH_min && strcmp(method, "min") == 0 && arg_count == 0) {
            if (obj->as.array.len == 0) { current_vm->error = strdup("min() called on empty array"); push(vm, value_nil()); return true; }
            bool hf = false;
            for (size_t i = 0; i < obj->as.array.len; i++)
                if (obj->as.array.elems[i].type == VAL_FLOAT) hf = true;
            if (hf) {
                double fm = (obj->as.array.elems[0].type == VAL_FLOAT)
                    ? obj->as.array.elems[0].as.float_val : (double)obj->as.array.elems[0].as.int_val;
                for (size_t i = 1; i < obj->as.array.len; i++) {
                    double v = (obj->as.array.elems[i].type == VAL_FLOAT)
                        ? obj->as.array.elems[i].as.float_val : (double)obj->as.array.elems[i].as.int_val;
                    if (v < fm) fm = v;
                }
                push(vm, value_float(fm));
            } else {
                int64_t im = obj->as.array.elems[0].as.int_val;
                for (size_t i = 1; i < obj->as.array.len; i++)
                    if (obj->as.array.elems[i].as.int_val < im) im = obj->as.array.elems[i].as.int_val;
                push(vm, value_int(im));
            }
            return true;
        }
        if (mhash == MHASH_max && strcmp(method, "max") == 0 && arg_count == 0) {
            if (obj->as.array.len == 0) { current_vm->error = strdup("max() called on empty array"); push(vm, value_nil()); return true; }
            bool hf = false;
            for (size_t i = 0; i < obj->as.array.len; i++)
                if (obj->as.array.elems[i].type == VAL_FLOAT) hf = true;
            if (hf) {
                double fm = (obj->as.array.elems[0].type == VAL_FLOAT)
                    ? obj->as.array.elems[0].as.float_val : (double)obj->as.array.elems[0].as.int_val;
                for (size_t i = 1; i < obj->as.array.len; i++) {
                    double v = (obj->as.array.elems[i].type == VAL_FLOAT)
                        ? obj->as.array.elems[i].as.float_val : (double)obj->as.array.elems[i].as.int_val;
                    if (v > fm) fm = v;
                }
                push(vm, value_float(fm));
            } else {
                int64_t im = obj->as.array.elems[0].as.int_val;
                for (size_t i = 1; i < obj->as.array.len; i++)
                    if (obj->as.array.elems[i].as.int_val > im) im = obj->as.array.elems[i].as.int_val;
                push(vm, value_int(im));
            }
            return true;
        }
        if (mhash == MHASH_first && strcmp(method, "first") == 0 && arg_count == 0) {
            push(vm, obj->as.array.len > 0 ? value_deep_clone(&obj->as.array.elems[0]) : value_unit());
            return true;
        }
        if (mhash == MHASH_last && strcmp(method, "last") == 0 && arg_count == 0) {
            push(vm, obj->as.array.len > 0 ? value_deep_clone(&obj->as.array.elems[obj->as.array.len - 1]) : value_unit());
            return true;
        }
        if (mhash == MHASH_flat_map && strcmp(method, "flat_map") == 0 && arg_count == 1) {
            LatValue closure = pop(vm);
            size_t n = obj->as.array.len;
            LatValue *mapped = malloc((n > 0 ? n : 1) * sizeof(LatValue));
            for (size_t i = 0; i < n; i++) {
                LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
                mapped[i] = vm_call_closure(vm, &closure, &arg, 1);
                value_free(&arg);
            }
            size_t total = 0;
            for (size_t i = 0; i < n; i++)
                total += (mapped[i].type == VAL_ARRAY) ? mapped[i].as.array.len : 1;
            LatValue *buf = malloc((total > 0 ? total : 1) * sizeof(LatValue));
            size_t pos = 0;
            for (size_t i = 0; i < n; i++) {
                if (mapped[i].type == VAL_ARRAY) {
                    for (size_t j = 0; j < mapped[i].as.array.len; j++)
                        buf[pos++] = mapped[i].as.array.elems[j];
                    mapped[i].as.array.len = 0; /* prevent double-free of moved elements */
                } else {
                    buf[pos++] = mapped[i];
                    mapped[i].type = VAL_NIL; /* prevent double-free */
                }
            }
            for (size_t i = 0; i < n; i++) value_free(&mapped[i]);
            free(mapped);
            value_free(&closure);
            LatValue r = value_array(buf, pos); free(buf);
            push(vm, r); return true;
        }
        if (mhash == MHASH_sort_by && strcmp(method, "sort_by") == 0 && arg_count == 1) {
            LatValue closure = pop(vm);
            size_t n = obj->as.array.len;
            LatValue *buf = malloc((n > 0 ? n : 1) * sizeof(LatValue));
            for (size_t i = 0; i < n; i++)
                buf[i] = value_deep_clone(&obj->as.array.elems[i]);
            for (size_t i = 1; i < n; i++) {
                LatValue key = buf[i];
                size_t j = i;
                while (j > 0) {
                    LatValue ca[2];
                    ca[0] = value_clone_fast(&key);
                    ca[1] = value_clone_fast(&buf[j - 1]);
                    LatValue cmp = vm_call_closure(vm, &closure, ca, 2);
                    value_free(&ca[0]); value_free(&ca[1]);
                    if (cmp.type != VAL_INT || cmp.as.int_val >= 0) { value_free(&cmp); break; }
                    value_free(&cmp);
                    buf[j] = buf[j - 1]; j--;
                }
                buf[j] = key;
            }
            value_free(&closure);
            LatValue r = value_array(buf, n); free(buf);
            push(vm, r); return true;
        }
        if (mhash == MHASH_group_by && strcmp(method, "group_by") == 0 && arg_count == 1) {
            LatValue closure = pop(vm);
            LatValue grp = value_map_new();
            for (size_t i = 0; i < obj->as.array.len; i++) {
                LatValue arg = value_deep_clone(&obj->as.array.elems[i]);
                LatValue key_v = vm_call_closure(vm, &closure, &arg, 1);
                value_free(&arg);
                char *gk = value_display(&key_v);
                value_free(&key_v);
                LatValue *existing = lat_map_get(grp.as.map.map, gk);
                if (existing) {
                    size_t ol = existing->as.array.len;
                    LatValue *ne = malloc((ol + 1) * sizeof(LatValue));
                    for (size_t j = 0; j < ol; j++)
                        ne[j] = value_deep_clone(&existing->as.array.elems[j]);
                    ne[ol] = value_deep_clone(&obj->as.array.elems[i]);
                    LatValue na = value_array(ne, ol + 1); free(ne);
                    lat_map_set(grp.as.map.map, gk, &na);
                } else {
                    LatValue cl = value_deep_clone(&obj->as.array.elems[i]);
                    LatValue na = value_array(&cl, 1);
                    lat_map_set(grp.as.map.map, gk, &na);
                }
                free(gk);
            }
            value_free(&closure);
            push(vm, grp); return true;
        }
        if (mhash == MHASH_insert && strcmp(method, "insert") == 0 && arg_count == 2) {
            const char *pmode = vm_find_pressure(vm, var_name);
            if (pressure_blocks_grow(pmode)) {
                LatValue val = pop(vm); LatValue idx_v = pop(vm);
                value_free(&val); value_free(&idx_v);
                char *err = NULL;
                (void)asprintf(&err, "pressurized (%s): cannot insert into '%s'", pmode, var_name);
                vm->error = err;
                push(vm, value_unit()); return true;
            }
            LatValue val = pop(vm); LatValue idx_v = pop(vm);
            int64_t idx = idx_v.as.int_val;
            value_free(&idx_v);
            if (idx < 0 || (size_t)idx > obj->as.array.len) {
                value_free(&val);
                vm->error = strdup(".insert() index out of bounds");
                push(vm, value_unit()); return true;
            }
            /* Grow if needed */
            if (obj->as.array.len >= obj->as.array.cap) {
                size_t new_cap = obj->as.array.cap < 4 ? 4 : obj->as.array.cap * 2;
                obj->as.array.elems = realloc(obj->as.array.elems, new_cap * sizeof(LatValue));
                obj->as.array.cap = new_cap;
            }
            /* Shift elements right */
            memmove(&obj->as.array.elems[(size_t)idx + 1],
                    &obj->as.array.elems[(size_t)idx],
                    (obj->as.array.len - (size_t)idx) * sizeof(LatValue));
            obj->as.array.elems[(size_t)idx] = val;
            obj->as.array.len++;
            push(vm, value_unit()); return true;
        }
    } break;

    /* String methods */
    case VAL_STR: {
        if (mhash == MHASH_len && strcmp(method, "len") == 0 && arg_count == 0) {
            push(vm, value_int((int64_t)strlen(obj->as.str_val)));
            return true;
        }
        if (mhash == MHASH_contains && strcmp(method, "contains") == 0 && arg_count == 1) {
            LatValue needle = pop(vm);
            if (needle.type == VAL_STR) {
                push(vm, value_bool(strstr(obj->as.str_val, needle.as.str_val) != NULL));
            } else {
                push(vm, value_bool(false));
            }
            value_free(&needle);
            return true;
        }
        if (mhash == MHASH_split && strcmp(method, "split") == 0 && arg_count == 1) {
            LatValue delim = pop(vm);
            if (delim.type == VAL_STR) {
                /* Split string */
                LatValue arr = value_array(NULL, 0);
                char *str = strdup(obj->as.str_val);
                char *tok = strtok(str, delim.as.str_val);
                while (tok) {
                    LatValue elem = value_string(tok);
                    if (arr.as.array.len >= arr.as.array.cap) {
                        arr.as.array.cap = arr.as.array.cap ? arr.as.array.cap * 2 : 4;
                        arr.as.array.elems = realloc(arr.as.array.elems,
                            arr.as.array.cap * sizeof(LatValue));
                    }
                    arr.as.array.elems[arr.as.array.len++] = elem;
                    tok = strtok(NULL, delim.as.str_val);
                }
                free(str);
                push(vm, arr);
            } else {
                push(vm, value_nil());
            }
            value_free(&delim);
            return true;
        }
        if (mhash == MHASH_trim && strcmp(method, "trim") == 0 && arg_count == 0) {
            const char *s = obj->as.str_val;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            const char *e = s + strlen(s);
            while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) e--;
            char *trimmed = malloc((size_t)(e - s) + 1);
            memcpy(trimmed, s, (size_t)(e - s));
            trimmed[e - s] = '\0';
            push(vm, value_string_owned(trimmed));
            return true;
        }
        if (mhash == MHASH_to_upper && strcmp(method, "to_upper") == 0 && arg_count == 0) {
            char *s = strdup(obj->as.str_val);
            for (size_t i = 0; s[i]; i++)
                if (s[i] >= 'a' && s[i] <= 'z') s[i] -= 32;
            push(vm, value_string_owned(s));
            return true;
        }
        if (mhash == MHASH_to_lower && strcmp(method, "to_lower") == 0 && arg_count == 0) {
            char *s = strdup(obj->as.str_val);
            for (size_t i = 0; s[i]; i++)
                if (s[i] >= 'A' && s[i] <= 'Z') s[i] += 32;
            push(vm, value_string_owned(s));
            return true;
        }
        if (mhash == MHASH_starts_with && strcmp(method, "starts_with") == 0 && arg_count == 1) {
            LatValue prefix = pop(vm);
            if (prefix.type == VAL_STR) {
                size_t plen = strlen(prefix.as.str_val);
                push(vm, value_bool(strncmp(obj->as.str_val, prefix.as.str_val, plen) == 0));
            } else {
                push(vm, value_bool(false));
            }
            value_free(&prefix);
            return true;
        }
        if (mhash == MHASH_ends_with && strcmp(method, "ends_with") == 0 && arg_count == 1) {
            LatValue suffix = pop(vm);
            if (suffix.type == VAL_STR) {
                size_t slen = strlen(obj->as.str_val);
                size_t plen = strlen(suffix.as.str_val);
                if (plen <= slen)
                    push(vm, value_bool(strcmp(obj->as.str_val + slen - plen, suffix.as.str_val) == 0));
                else
                    push(vm, value_bool(false));
            } else {
                push(vm, value_bool(false));
            }
            value_free(&suffix);
            return true;
        }
        if (mhash == MHASH_replace && strcmp(method, "replace") == 0 && arg_count == 2) {
            LatValue replacement = pop(vm);
            LatValue pattern = pop(vm);
            if (pattern.type == VAL_STR && replacement.type == VAL_STR) {
                push(vm, value_string_owned(lat_str_replace(obj->as.str_val, pattern.as.str_val, replacement.as.str_val)));
            } else {
                push(vm, value_nil());
            }
            value_free(&pattern);
            value_free(&replacement);
            return true;
        }
        if (mhash == MHASH_index_of && strcmp(method, "index_of") == 0 && arg_count == 1) {
            LatValue needle = pop(vm);
            if (needle.type == VAL_STR)
                push(vm, value_int(lat_str_index_of(obj->as.str_val, needle.as.str_val)));
            else
                push(vm, value_int(-1));
            value_free(&needle); return true;
        }
        if (mhash == MHASH_substring && strcmp(method, "substring") == 0 && arg_count == 2) {
            LatValue end_v = pop(vm); LatValue start_v = pop(vm);
            int64_t s = (start_v.type == VAL_INT) ? start_v.as.int_val : 0;
            int64_t e = (end_v.type == VAL_INT) ? end_v.as.int_val : (int64_t)strlen(obj->as.str_val);
            push(vm, value_string_owned(lat_str_substring(obj->as.str_val, s, e)));
            value_free(&start_v); value_free(&end_v); return true;
        }
        if (mhash == MHASH_chars && strcmp(method, "chars") == 0 && arg_count == 0) {
            size_t slen = strlen(obj->as.str_val);
            LatValue *elems = malloc((slen > 0 ? slen : 1) * sizeof(LatValue));
            for (size_t i = 0; i < slen; i++) {
                char ch[2] = { obj->as.str_val[i], '\0' };
                elems[i] = value_string(ch);
            }
            LatValue r = value_array(elems, slen); free(elems);
            push(vm, r); return true;
        }
        if (mhash == MHASH_bytes && strcmp(method, "bytes") == 0 && arg_count == 0) {
            size_t slen = strlen(obj->as.str_val);
            LatValue *elems = malloc((slen > 0 ? slen : 1) * sizeof(LatValue));
            for (size_t i = 0; i < slen; i++)
                elems[i] = value_int((int64_t)(unsigned char)obj->as.str_val[i]);
            LatValue r = value_array(elems, slen); free(elems);
            push(vm, r); return true;
        }
        if (mhash == MHASH_reverse && strcmp(method, "reverse") == 0 && arg_count == 0) {
            push(vm, value_string_owned(lat_str_reverse(obj->as.str_val)));
            return true;
        }
        if (mhash == MHASH_repeat && strcmp(method, "repeat") == 0 && arg_count == 1) {
            LatValue n_v = pop(vm);
            size_t n = (n_v.type == VAL_INT && n_v.as.int_val > 0) ? (size_t)n_v.as.int_val : 0;
            value_free(&n_v);
            push(vm, value_string_owned(lat_str_repeat(obj->as.str_val, n)));
            return true;
        }
        if (mhash == MHASH_trim_start && strcmp(method, "trim_start") == 0 && arg_count == 0) {
            const char *s = obj->as.str_val;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            push(vm, value_string(s)); return true;
        }
        if (mhash == MHASH_trim_end && strcmp(method, "trim_end") == 0 && arg_count == 0) {
            const char *s = obj->as.str_val;
            size_t slen = strlen(s);
            while (slen > 0 && (s[slen-1] == ' ' || s[slen-1] == '\t' || s[slen-1] == '\n' || s[slen-1] == '\r')) slen--;
            char *r = malloc(slen + 1);
            memcpy(r, s, slen); r[slen] = '\0';
            push(vm, value_string_owned(r)); return true;
        }
        if (mhash == MHASH_pad_left && strcmp(method, "pad_left") == 0 && arg_count == 2) {
            LatValue ch_v = pop(vm); LatValue n_v = pop(vm);
            int64_t n = (n_v.type == VAL_INT) ? n_v.as.int_val : 0;
            char pad = (ch_v.type == VAL_STR && ch_v.as.str_val[0]) ? ch_v.as.str_val[0] : ' ';
            value_free(&n_v); value_free(&ch_v);
            size_t slen = strlen(obj->as.str_val);
            if ((int64_t)slen >= n) { push(vm, value_clone_fast(obj)); return true; }
            size_t pad_n = (size_t)n - slen;
            char *r = malloc((size_t)n + 1);
            memset(r, pad, pad_n);
            memcpy(r + pad_n, obj->as.str_val, slen);
            r[(size_t)n] = '\0';
            push(vm, value_string_owned(r)); return true;
        }
        if (mhash == MHASH_pad_right && strcmp(method, "pad_right") == 0 && arg_count == 2) {
            LatValue ch_v = pop(vm); LatValue n_v = pop(vm);
            int64_t n = (n_v.type == VAL_INT) ? n_v.as.int_val : 0;
            char pad = (ch_v.type == VAL_STR && ch_v.as.str_val[0]) ? ch_v.as.str_val[0] : ' ';
            value_free(&n_v); value_free(&ch_v);
            size_t slen = strlen(obj->as.str_val);
            if ((int64_t)slen >= n) { push(vm, value_clone_fast(obj)); return true; }
            size_t pad_n = (size_t)n - slen;
            char *r = malloc((size_t)n + 1);
            memcpy(r, obj->as.str_val, slen);
            memset(r + slen, pad, pad_n);
            r[(size_t)n] = '\0';
            push(vm, value_string_owned(r)); return true;
        }
        if (mhash == MHASH_count && strcmp(method, "count") == 0 && arg_count == 1) {
            LatValue needle = pop(vm);
            int64_t cnt = 0;
            if (needle.type == VAL_STR && needle.as.str_val[0]) {
                const char *p = obj->as.str_val;
                size_t nlen = strlen(needle.as.str_val);
                while ((p = strstr(p, needle.as.str_val)) != NULL) { cnt++; p += nlen; }
            }
            value_free(&needle);
            push(vm, value_int(cnt)); return true;
        }
        if (mhash == MHASH_is_empty && strcmp(method, "is_empty") == 0 && arg_count == 0) {
            push(vm, value_bool(obj->as.str_val[0] == '\0'));
            return true;
        }
    } break;

    /* Map methods */
    case VAL_MAP: {
        if (mhash == MHASH_len && strcmp(method, "len") == 0 && arg_count == 0) {
            push(vm, value_int((int64_t)lat_map_len(obj->as.map.map)));
            return true;
        }
        if (mhash == MHASH_get && strcmp(method, "get") == 0 && arg_count == 1) {
            LatValue key = pop(vm);
            if (key.type == VAL_STR) {
                LatValue *found = lat_map_get(obj->as.map.map, key.as.str_val);
                if (found)
                    push(vm, value_deep_clone(found));
                else
                    push(vm, value_nil());
            } else {
                push(vm, value_nil());
            }
            value_free(&key);
            return true;
        }
        if (mhash == MHASH_keys && strcmp(method, "keys") == 0 && arg_count == 0) {
            size_t len = lat_map_len(obj->as.map.map);
            LatValue *keys = malloc((len ? len : 1) * sizeof(LatValue));
            size_t key_idx = 0;
            for (size_t i = 0; i < obj->as.map.map->cap; i++) {
                if (obj->as.map.map->entries[i].state == MAP_OCCUPIED) {
                    keys[key_idx++] = value_string(obj->as.map.map->entries[i].key);
                }
            }
            LatValue arr = value_array(keys, key_idx);
            free(keys);
            push(vm, arr);
            return true;
        }
        if (mhash == MHASH_values && strcmp(method, "values") == 0 && arg_count == 0) {
            size_t len = lat_map_len(obj->as.map.map);
            LatValue *vals = malloc((len ? len : 1) * sizeof(LatValue));
            size_t val_idx = 0;
            for (size_t i = 0; i < obj->as.map.map->cap; i++) {
                if (obj->as.map.map->entries[i].state == MAP_OCCUPIED) {
                    LatValue *stored = (LatValue *)obj->as.map.map->entries[i].value;
                    vals[val_idx++] = value_deep_clone(stored);
                }
            }
            LatValue arr = value_array(vals, val_idx);
            free(vals);
            push(vm, arr);
            return true;
        }
        if (mhash == MHASH_set && strcmp(method, "set") == 0 && arg_count == 2) {
            /* Check phase: crystal and sublimated values are immutable */
            if (obj->phase == VTAG_CRYSTAL || obj->phase == VTAG_SUBLIMATED) {
                LatValue val = pop(vm);
                LatValue key = pop(vm);
                value_free(&val);
                value_free(&key);
                const char *phase_name = obj->phase == VTAG_CRYSTAL ? "crystal" : "sublimated";
                char *err = NULL;
                (void)asprintf(&err, "cannot set on %s map", phase_name);
                current_vm->error = err;
                push(vm, value_unit());
                return true;
            }
            LatValue val = pop(vm);
            LatValue key = pop(vm);
            if (key.type == VAL_STR) {
                lat_map_set(obj->as.map.map, key.as.str_val, &val);
            } else {
                value_free(&val);
            }
            value_free(&key);
            push(vm, value_unit());
            return true;
        }
        if (mhash == MHASH_contains && strcmp(method, "contains") == 0 && arg_count == 1) {
            LatValue key = pop(vm);
            if (key.type == VAL_STR) {
                push(vm, value_bool(lat_map_get(obj->as.map.map, key.as.str_val) != NULL));
            } else {
                push(vm, value_bool(false));
            }
            value_free(&key);
            return true;
        }
        if (mhash == MHASH_has && strcmp(method, "has") == 0 && arg_count == 1) {
            LatValue key = pop(vm);
            if (key.type == VAL_STR)
                push(vm, value_bool(lat_map_get(obj->as.map.map, key.as.str_val) != NULL));
            else
                push(vm, value_bool(false));
            value_free(&key); return true;
        }
        if (mhash == MHASH_remove && strcmp(method, "remove") == 0 && arg_count == 1) {
            LatValue key = pop(vm);
            if (key.type == VAL_STR) {
                lat_map_remove(obj->as.map.map, key.as.str_val);
            }
            value_free(&key);
            push(vm, value_unit()); return true;
        }
        if (mhash == MHASH_entries && strcmp(method, "entries") == 0 && arg_count == 0) {
            size_t n = lat_map_len(obj->as.map.map);
            LatValue *entries = malloc((n > 0 ? n : 1) * sizeof(LatValue));
            size_t ei = 0;
            for (size_t i = 0; i < obj->as.map.map->cap; i++) {
                if (obj->as.map.map->entries[i].state == MAP_OCCUPIED) {
                    LatValue pe[2];
                    pe[0] = value_string(obj->as.map.map->entries[i].key);
                    pe[1] = value_deep_clone((LatValue *)obj->as.map.map->entries[i].value);
                    entries[ei++] = value_array(pe, 2);
                }
            }
            LatValue r = value_array(entries, ei); free(entries);
            push(vm, r); return true;
        }
        if (mhash == MHASH_merge && strcmp(method, "merge") == 0 && arg_count == 1) {
            LatValue other = pop(vm);
            if (other.type == VAL_MAP) {
                for (size_t i = 0; i < other.as.map.map->cap; i++) {
                    if (other.as.map.map->entries[i].state == MAP_OCCUPIED) {
                        LatValue cloned = value_deep_clone((LatValue *)other.as.map.map->entries[i].value);
                        lat_map_set(obj->as.map.map, other.as.map.map->entries[i].key, &cloned);
                    }
                }
            }
            value_free(&other);
            push(vm, value_unit()); return true;
        }
        if (mhash == MHASH_for_each && strcmp(method, "for_each") == 0 && arg_count == 1) {
            LatValue closure = pop(vm);
            for (size_t i = 0; i < obj->as.map.map->cap; i++) {
                if (obj->as.map.map->entries[i].state == MAP_OCCUPIED) {
                    LatValue ca[2];
                    ca[0] = value_string(obj->as.map.map->entries[i].key);
                    ca[1] = value_deep_clone((LatValue *)obj->as.map.map->entries[i].value);
                    LatValue r = vm_call_closure(vm, &closure, ca, 2);
                    value_free(&ca[0]); value_free(&ca[1]); value_free(&r);
                }
            }
            value_free(&closure);
            push(vm, value_unit()); return true;
        }
        if (mhash == MHASH_filter && strcmp(method, "filter") == 0 && arg_count == 1) {
            LatValue closure = pop(vm);
            LatValue result = value_map_new();
            for (size_t i = 0; i < obj->as.map.map->cap; i++) {
                if (obj->as.map.map->entries[i].state == MAP_OCCUPIED) {
                    LatValue ca[2];
                    ca[0] = value_string(obj->as.map.map->entries[i].key);
                    ca[1] = value_deep_clone((LatValue *)obj->as.map.map->entries[i].value);
                    LatValue r = vm_call_closure(vm, &closure, ca, 2);
                    bool keep = (r.type == VAL_BOOL && r.as.bool_val);
                    value_free(&ca[0]); value_free(&r);
                    if (keep) {
                        lat_map_set(result.as.map.map, obj->as.map.map->entries[i].key, &ca[1]);
                    } else {
                        value_free(&ca[1]);
                    }
                }
            }
            value_free(&closure);
            push(vm, result); return true;
        }
        if (mhash == MHASH_map && strcmp(method, "map") == 0 && arg_count == 1) {
            LatValue closure = pop(vm);
            LatValue result = value_map_new();
            for (size_t i = 0; i < obj->as.map.map->cap; i++) {
                if (obj->as.map.map->entries[i].state == MAP_OCCUPIED) {
                    LatValue ca[2];
                    ca[0] = value_string(obj->as.map.map->entries[i].key);
                    ca[1] = value_deep_clone((LatValue *)obj->as.map.map->entries[i].value);
                    LatValue r = vm_call_closure(vm, &closure, ca, 2);
                    value_free(&ca[0]); value_free(&ca[1]);
                    lat_map_set(result.as.map.map, obj->as.map.map->entries[i].key, &r);
                }
            }
            value_free(&closure);
            push(vm, result); return true;
        }
    } break;

    /* Struct methods */
    case VAL_STRUCT: {
        if (mhash == MHASH_get && strcmp(method, "get") == 0 && arg_count == 1) {
            LatValue key = pop(vm);
            if (key.type == VAL_STR) {
                bool found = false;
                for (size_t i = 0; i < obj->as.strct.field_count; i++) {
                    if (strcmp(obj->as.strct.field_names[i], key.as.str_val) == 0) {
                        push(vm, value_deep_clone(&obj->as.strct.field_values[i]));
                        found = true; break;
                    }
                }
                if (!found) push(vm, value_nil());
            } else {
                push(vm, value_nil());
            }
            value_free(&key); return true;
        }
        /* Struct field that is callable */
        for (size_t i = 0; i < obj->as.strct.field_count; i++) {
            if (strcmp(obj->as.strct.field_names[i], method) == 0) {
                LatValue *field_val = &obj->as.strct.field_values[i];
                if (field_val->type == VAL_CLOSURE && field_val->as.closure.native_fn) {
                    /* It's a compiled function - invoke it */
                    /* This will be handled by the main call path */
                    return false;
                }
                return false;
            }
        }
    } break;

    /* Range methods */
    case VAL_RANGE: {
        if (mhash == MHASH_len && strcmp(method, "len") == 0 && arg_count == 0) {
            int64_t len = obj->as.range.end - obj->as.range.start;
            push(vm, value_int(len > 0 ? len : 0));
            return true;
        }
        if (mhash == MHASH_contains && strcmp(method, "contains") == 0 && arg_count == 1) {
            LatValue val = pop(vm);
            if (val.type == VAL_INT) {
                push(vm, value_bool(val.as.int_val >= obj->as.range.start &&
                                    val.as.int_val < obj->as.range.end));
            } else {
                push(vm, value_bool(false));
            }
            value_free(&val);
            return true;
        }
    } break;

    /* Tuple methods */
    case VAL_TUPLE: {
        if (mhash == MHASH_len && strcmp(method, "len") == 0 && arg_count == 0) {
            push(vm, value_int((int64_t)obj->as.tuple.len));
            return true;
        }
    } break;

    /* Enum methods */
    case VAL_ENUM: {
        if (mhash == MHASH_tag && strcmp(method, "tag") == 0 && arg_count == 0) {
            push(vm, value_string(obj->as.enm.variant_name));
            return true;
        }
        if (mhash == MHASH_payload && strcmp(method, "payload") == 0 && arg_count == 0) {
            /* Always return an array of all payloads */
            if (obj->as.enm.payload_count > 0) {
                LatValue *elems = malloc(obj->as.enm.payload_count * sizeof(LatValue));
                for (size_t pi = 0; pi < obj->as.enm.payload_count; pi++)
                    elems[pi] = value_deep_clone(&obj->as.enm.payload[pi]);
                push(vm, value_array(elems, obj->as.enm.payload_count));
                free(elems);
            } else {
                push(vm, value_array(NULL, 0));
            }
            return true;
        }
        if (mhash == MHASH_variant_name && strcmp(method, "variant_name") == 0 && arg_count == 0) {
            push(vm, value_string(obj->as.enm.variant_name));
            return true;
        }
        if (mhash == MHASH_enum_name && strcmp(method, "enum_name") == 0 && arg_count == 0) {
            push(vm, value_string(obj->as.enm.enum_name));
            return true;
        }
        if (mhash == MHASH_is_variant && strcmp(method, "is_variant") == 0 && arg_count == 1) {
            LatValue name = pop(vm);
            bool match = (name.type == VAL_STR && strcmp(obj->as.enm.variant_name, name.as.str_val) == 0);
            value_free(&name);
            push(vm, value_bool(match)); return true;
        }
    } break;

    /* ── Set methods ── */
    case VAL_SET: {
        if (mhash == MHASH_has && strcmp(method, "has") == 0 && arg_count == 1) {
            LatValue val = pop(vm);
            char *key = value_display(&val);
            bool found = lat_map_contains(obj->as.set.map, key);
            free(key);
            value_free(&val);
            push(vm, value_bool(found)); return true;
        }
        if (mhash == MHASH_add && strcmp(method, "add") == 0 && arg_count == 1) {
            LatValue val = pop(vm);
            char *key = value_display(&val);
            LatValue clone = value_deep_clone(&val);
            lat_map_set(obj->as.set.map, key, &clone);
            free(key);
            value_free(&val);
            push(vm, value_unit()); return true;
        }
        if (mhash == MHASH_remove && strcmp(method, "remove") == 0 && arg_count == 1) {
            LatValue val = pop(vm);
            char *key = value_display(&val);
            lat_map_remove(obj->as.set.map, key);
            free(key);
            value_free(&val);
            push(vm, value_unit()); return true;
        }
        if (mhash == MHASH_len && strcmp(method, "len") == 0 && arg_count == 0) {
            push(vm, value_int((int64_t)lat_map_len(obj->as.set.map))); return true;
        }
        if (mhash == MHASH_to_array && strcmp(method, "to_array") == 0 && arg_count == 0) {
            size_t len = lat_map_len(obj->as.set.map);
            LatValue *elems = malloc(len * sizeof(LatValue));
            size_t idx = 0;
            for (size_t i = 0; i < obj->as.set.map->cap; i++) {
                if (obj->as.set.map->entries[i].state == MAP_OCCUPIED) {
                    LatValue *v = (LatValue *)obj->as.set.map->entries[i].value;
                    elems[idx++] = value_deep_clone(v);
                }
            }
            LatValue arr = value_array(elems, len);
            free(elems);
            push(vm, arr); return true;
        }
        if (mhash == MHASH_union && strcmp(method, "union") == 0 && arg_count == 1) {
            LatValue other = pop(vm);
            LatValue result = value_set_new();
            for (size_t i = 0; i < obj->as.set.map->cap; i++) {
                if (obj->as.set.map->entries[i].state == MAP_OCCUPIED) {
                    LatValue *v = (LatValue *)obj->as.set.map->entries[i].value;
                    LatValue c = value_deep_clone(v);
                    lat_map_set(result.as.set.map, obj->as.set.map->entries[i].key, &c);
                }
            }
            if (other.type == VAL_SET) {
                for (size_t i = 0; i < other.as.set.map->cap; i++) {
                    if (other.as.set.map->entries[i].state == MAP_OCCUPIED) {
                        LatValue *v = (LatValue *)other.as.set.map->entries[i].value;
                        LatValue c = value_deep_clone(v);
                        lat_map_set(result.as.set.map, other.as.set.map->entries[i].key, &c);
                    }
                }
            }
            value_free(&other);
            push(vm, result); return true;
        }
        if (mhash == MHASH_intersection && strcmp(method, "intersection") == 0 && arg_count == 1) {
            LatValue other = pop(vm);
            LatValue result = value_set_new();
            if (other.type == VAL_SET) {
                for (size_t i = 0; i < obj->as.set.map->cap; i++) {
                    if (obj->as.set.map->entries[i].state == MAP_OCCUPIED &&
                        lat_map_contains(other.as.set.map, obj->as.set.map->entries[i].key)) {
                        LatValue *v = (LatValue *)obj->as.set.map->entries[i].value;
                        LatValue c = value_deep_clone(v);
                        lat_map_set(result.as.set.map, obj->as.set.map->entries[i].key, &c);
                    }
                }
            }
            value_free(&other);
            push(vm, result); return true;
        }
        if (mhash == MHASH_difference && strcmp(method, "difference") == 0 && arg_count == 1) {
            LatValue other = pop(vm);
            LatValue result = value_set_new();
            if (other.type == VAL_SET) {
                for (size_t i = 0; i < obj->as.set.map->cap; i++) {
                    if (obj->as.set.map->entries[i].state == MAP_OCCUPIED &&
                        !lat_map_contains(other.as.set.map, obj->as.set.map->entries[i].key)) {
                        LatValue *v = (LatValue *)obj->as.set.map->entries[i].value;
                        LatValue c = value_deep_clone(v);
                        lat_map_set(result.as.set.map, obj->as.set.map->entries[i].key, &c);
                    }
                }
            }
            value_free(&other);
            push(vm, result); return true;
        }
        if (mhash == MHASH_is_subset && strcmp(method, "is_subset") == 0 && arg_count == 1) {
            LatValue other = pop(vm);
            bool result = true;
            if (other.type == VAL_SET) {
                for (size_t i = 0; i < obj->as.set.map->cap; i++) {
                    if (obj->as.set.map->entries[i].state == MAP_OCCUPIED &&
                        !lat_map_contains(other.as.set.map, obj->as.set.map->entries[i].key)) {
                        result = false; break;
                    }
                }
            } else result = false;
            value_free(&other);
            push(vm, value_bool(result)); return true;
        }
        if (mhash == MHASH_is_superset && strcmp(method, "is_superset") == 0 && arg_count == 1) {
            LatValue other = pop(vm);
            bool result = true;
            if (other.type == VAL_SET) {
                for (size_t i = 0; i < other.as.set.map->cap; i++) {
                    if (other.as.set.map->entries[i].state == MAP_OCCUPIED &&
                        !lat_map_contains(obj->as.set.map, other.as.set.map->entries[i].key)) {
                        result = false; break;
                    }
                }
            } else result = false;
            value_free(&other);
            push(vm, value_bool(result)); return true;
        }
    } break;

    /* ── Channel methods ── */
    case VAL_CHANNEL: {
        if (mhash == MHASH_send && strcmp(method, "send") == 0 && arg_count == 1) {
            LatValue val = pop(vm);
            if (val.phase == VTAG_FLUID) {
                value_free(&val);
                current_vm->error = strdup("channel.send: can only send crystal (immutable) values");
                push(vm, value_unit()); return true;
            }
            channel_send(obj->as.channel.ch, val);
            push(vm, value_unit()); return true;
        }
        if (mhash == MHASH_recv && strcmp(method, "recv") == 0 && arg_count == 0) {
            bool ok;
            LatValue val = channel_recv(obj->as.channel.ch, &ok);
            if (!ok) { push(vm, value_unit()); return true; }
            push(vm, val); return true;
        }
        if (mhash == MHASH_close && strcmp(method, "close") == 0 && arg_count == 0) {
            channel_close(obj->as.channel.ch);
            push(vm, value_unit()); return true;
        }
    } break;

    /* ── Buffer methods ── */
    case VAL_BUFFER: {
        if (mhash == MHASH_len && strcmp(method, "len") == 0 && arg_count == 0) {
            push(vm, value_int((int64_t)obj->as.buffer.len));
            return true;
        }
        if (mhash == MHASH_capacity && strcmp(method, "capacity") == 0 && arg_count == 0) {
            push(vm, value_int((int64_t)obj->as.buffer.cap));
            return true;
        }
        if (mhash == MHASH_push && strcmp(method, "push") == 0 && arg_count == 1) {
            LatValue val = pop(vm);
            if (val.type != VAL_INT) { value_free(&val); push(vm, value_unit()); return true; }
            if (obj->as.buffer.len >= obj->as.buffer.cap) {
                obj->as.buffer.cap = obj->as.buffer.cap ? obj->as.buffer.cap * 2 : 8;
                obj->as.buffer.data = realloc(obj->as.buffer.data, obj->as.buffer.cap);
            }
            obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)(val.as.int_val & 0xFF);
            push(vm, value_unit());
            return true;
        }
        if (mhash == MHASH_push_u16 && strcmp(method, "push_u16") == 0 && arg_count == 1) {
            LatValue val = pop(vm);
            if (val.type != VAL_INT) { value_free(&val); push(vm, value_unit()); return true; }
            uint16_t v = (uint16_t)(val.as.int_val & 0xFFFF);
            while (obj->as.buffer.len + 2 > obj->as.buffer.cap) {
                obj->as.buffer.cap = obj->as.buffer.cap ? obj->as.buffer.cap * 2 : 8;
                obj->as.buffer.data = realloc(obj->as.buffer.data, obj->as.buffer.cap);
            }
            obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)(v & 0xFF);
            obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)((v >> 8) & 0xFF);
            push(vm, value_unit());
            return true;
        }
        if (mhash == MHASH_push_u32 && strcmp(method, "push_u32") == 0 && arg_count == 1) {
            LatValue val = pop(vm);
            if (val.type != VAL_INT) { value_free(&val); push(vm, value_unit()); return true; }
            uint32_t v = (uint32_t)(val.as.int_val & 0xFFFFFFFF);
            while (obj->as.buffer.len + 4 > obj->as.buffer.cap) {
                obj->as.buffer.cap = obj->as.buffer.cap ? obj->as.buffer.cap * 2 : 8;
                obj->as.buffer.data = realloc(obj->as.buffer.data, obj->as.buffer.cap);
            }
            obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)(v & 0xFF);
            obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)((v >> 8) & 0xFF);
            obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)((v >> 16) & 0xFF);
            obj->as.buffer.data[obj->as.buffer.len++] = (uint8_t)((v >> 24) & 0xFF);
            push(vm, value_unit());
            return true;
        }
        if (mhash == MHASH_read_u8 && strcmp(method, "read_u8") == 0 && arg_count == 1) {
            LatValue idx = pop(vm);
            if (idx.type != VAL_INT || idx.as.int_val < 0 || (size_t)idx.as.int_val >= obj->as.buffer.len) {
                value_free(&idx);
                vm->error = strdup("Buffer.read_u8: index out of bounds");
                push(vm, value_int(0));
                return true;
            }
            push(vm, value_int(obj->as.buffer.data[idx.as.int_val]));
            return true;
        }
        if (mhash == MHASH_write_u8 && strcmp(method, "write_u8") == 0 && arg_count == 2) {
            LatValue val = pop(vm);
            LatValue idx = pop(vm);
            if (idx.type != VAL_INT || idx.as.int_val < 0 || (size_t)idx.as.int_val >= obj->as.buffer.len) {
                value_free(&idx); value_free(&val);
                vm->error = strdup("Buffer.write_u8: index out of bounds");
                push(vm, value_unit());
                return true;
            }
            obj->as.buffer.data[idx.as.int_val] = (uint8_t)(val.as.int_val & 0xFF);
            push(vm, value_unit());
            return true;
        }
        if (mhash == MHASH_read_u16 && strcmp(method, "read_u16") == 0 && arg_count == 1) {
            LatValue idx = pop(vm);
            if (idx.type != VAL_INT || idx.as.int_val < 0 || (size_t)idx.as.int_val + 2 > obj->as.buffer.len) {
                value_free(&idx);
                vm->error = strdup("Buffer.read_u16: index out of bounds");
                push(vm, value_int(0));
                return true;
            }
            size_t i = (size_t)idx.as.int_val;
            uint16_t v = (uint16_t)(obj->as.buffer.data[i] | (obj->as.buffer.data[i+1] << 8));
            push(vm, value_int(v));
            return true;
        }
        if (mhash == MHASH_write_u16 && strcmp(method, "write_u16") == 0 && arg_count == 2) {
            LatValue val = pop(vm);
            LatValue idx = pop(vm);
            if (idx.type != VAL_INT || idx.as.int_val < 0 || (size_t)idx.as.int_val + 2 > obj->as.buffer.len) {
                value_free(&idx); value_free(&val);
                vm->error = strdup("Buffer.write_u16: index out of bounds");
                push(vm, value_unit());
                return true;
            }
            size_t i = (size_t)idx.as.int_val;
            uint16_t v = (uint16_t)(val.as.int_val & 0xFFFF);
            obj->as.buffer.data[i] = (uint8_t)(v & 0xFF);
            obj->as.buffer.data[i+1] = (uint8_t)((v >> 8) & 0xFF);
            push(vm, value_unit());
            return true;
        }
        if (mhash == MHASH_read_u32 && strcmp(method, "read_u32") == 0 && arg_count == 1) {
            LatValue idx = pop(vm);
            if (idx.type != VAL_INT || idx.as.int_val < 0 || (size_t)idx.as.int_val + 4 > obj->as.buffer.len) {
                value_free(&idx);
                vm->error = strdup("Buffer.read_u32: index out of bounds");
                push(vm, value_int(0));
                return true;
            }
            size_t i = (size_t)idx.as.int_val;
            uint32_t v = (uint32_t)obj->as.buffer.data[i]
                       | ((uint32_t)obj->as.buffer.data[i+1] << 8)
                       | ((uint32_t)obj->as.buffer.data[i+2] << 16)
                       | ((uint32_t)obj->as.buffer.data[i+3] << 24);
            push(vm, value_int((int64_t)v));
            return true;
        }
        if (mhash == MHASH_write_u32 && strcmp(method, "write_u32") == 0 && arg_count == 2) {
            LatValue val = pop(vm);
            LatValue idx = pop(vm);
            if (idx.type != VAL_INT || idx.as.int_val < 0 || (size_t)idx.as.int_val + 4 > obj->as.buffer.len) {
                value_free(&idx); value_free(&val);
                vm->error = strdup("Buffer.write_u32: index out of bounds");
                push(vm, value_unit());
                return true;
            }
            size_t i = (size_t)idx.as.int_val;
            uint32_t v = (uint32_t)(val.as.int_val & 0xFFFFFFFF);
            obj->as.buffer.data[i]   = (uint8_t)(v & 0xFF);
            obj->as.buffer.data[i+1] = (uint8_t)((v >> 8) & 0xFF);
            obj->as.buffer.data[i+2] = (uint8_t)((v >> 16) & 0xFF);
            obj->as.buffer.data[i+3] = (uint8_t)((v >> 24) & 0xFF);
            push(vm, value_unit());
            return true;
        }
        if (mhash == MHASH_slice && strcmp(method, "slice") == 0 && arg_count == 2) {
            LatValue end_v = pop(vm);
            LatValue start_v = pop(vm);
            if (start_v.type != VAL_INT || end_v.type != VAL_INT) {
                value_free(&start_v); value_free(&end_v);
                vm->error = strdup("Buffer.slice: expected Int arguments");
                push(vm, value_buffer(NULL, 0));
                return true;
            }
            int64_t s = start_v.as.int_val, e = end_v.as.int_val;
            if (s < 0) s = 0;
            if (e > (int64_t)obj->as.buffer.len) e = (int64_t)obj->as.buffer.len;
            if (s >= e) { push(vm, value_buffer(NULL, 0)); return true; }
            push(vm, value_buffer(obj->as.buffer.data + s, (size_t)(e - s)));
            return true;
        }
        if (mhash == MHASH_clear && strcmp(method, "clear") == 0 && arg_count == 0) {
            obj->as.buffer.len = 0;
            push(vm, value_unit());
            return true;
        }
        if (mhash == MHASH_fill && strcmp(method, "fill") == 0 && arg_count == 1) {
            LatValue val = pop(vm);
            uint8_t byte = (val.type == VAL_INT) ? (uint8_t)(val.as.int_val & 0xFF) : 0;
            memset(obj->as.buffer.data, byte, obj->as.buffer.len);
            push(vm, value_unit());
            return true;
        }
        if (mhash == MHASH_resize && strcmp(method, "resize") == 0 && arg_count == 1) {
            LatValue val = pop(vm);
            if (val.type != VAL_INT || val.as.int_val < 0) { value_free(&val); push(vm, value_unit()); return true; }
            size_t new_len = (size_t)val.as.int_val;
            if (new_len > obj->as.buffer.cap) {
                obj->as.buffer.cap = new_len;
                obj->as.buffer.data = realloc(obj->as.buffer.data, obj->as.buffer.cap);
            }
            if (new_len > obj->as.buffer.len)
                memset(obj->as.buffer.data + obj->as.buffer.len, 0, new_len - obj->as.buffer.len);
            obj->as.buffer.len = new_len;
            push(vm, value_unit());
            return true;
        }
        if (mhash == MHASH_to_string && strcmp(method, "to_string") == 0 && arg_count == 0) {
            char *s = malloc(obj->as.buffer.len + 1);
            memcpy(s, obj->as.buffer.data, obj->as.buffer.len);
            s[obj->as.buffer.len] = '\0';
            push(vm, value_string_owned(s));
            return true;
        }
        if (mhash == MHASH_to_array && strcmp(method, "to_array") == 0 && arg_count == 0) {
            size_t len = obj->as.buffer.len;
            LatValue *elems = malloc((len > 0 ? len : 1) * sizeof(LatValue));
            for (size_t i = 0; i < len; i++)
                elems[i] = value_int(obj->as.buffer.data[i]);
            LatValue arr = value_array(elems, len);
            free(elems);
            push(vm, arr);
            return true;
        }
        if (mhash == MHASH_to_hex && strcmp(method, "to_hex") == 0 && arg_count == 0) {
            size_t len = obj->as.buffer.len;
            char *hex = malloc(len * 2 + 1);
            for (size_t i = 0; i < len; i++)
                snprintf(hex + i * 2, 3, "%02x", obj->as.buffer.data[i]);
            hex[len * 2] = '\0';
            push(vm, value_string_owned(hex));
            return true;
        }
    } break;

    /* ── Ref methods ── */
    case VAL_REF: {
        LatRef *ref = obj->as.ref.ref;
        LatValue *inner = &ref->value;

        /* Ref-specific: get() with 0 args */
        if (mhash == MHASH_get && strcmp(method, "get") == 0 && arg_count == 0) {
            push(vm, value_deep_clone(inner));
            return true;
        }
        /* Ref-specific: deref() alias for get() */
        if (mhash == MHASH_deref && strcmp(method, "deref") == 0 && arg_count == 0) {
            push(vm, value_deep_clone(inner));
            return true;
        }
        /* Ref-specific: set(v) with 1 arg */
        if (mhash == MHASH_set && strcmp(method, "set") == 0 && arg_count == 1) {
            if (obj->phase == VTAG_CRYSTAL) {
                runtime_error(vm, "cannot set on a frozen Ref");
                return true;
            }
            value_free(inner);
            *inner = vm_peek(vm, 0)[0];
            vm->stack_top--;
            *inner = value_deep_clone(inner);
            push(vm, value_unit());
            return true;
        }
        /* Ref-specific: inner_type() */
        if (mhash == MHASH_inner_type && strcmp(method, "inner_type") == 0 && arg_count == 0) {
            push(vm, value_string(value_type_name(inner)));
            return true;
        }

        /* Map proxy (when inner is VAL_MAP) */
        if (inner->type == VAL_MAP) {
            /* get(key) with 1 arg -> Map proxy */
            if (mhash == MHASH_get && strcmp(method, "get") == 0 && arg_count == 1) {
                LatValue key = vm_peek(vm, 0)[0];
                if (key.type != VAL_STR) { push(vm, value_nil()); return true; }
                LatValue *found = lat_map_get(inner->as.map.map, key.as.str_val);
                value_free(&key); vm->stack_top--;
                push(vm, found ? value_deep_clone(found) : value_nil());
                return true;
            }
            /* set(k, v) with 2 args -> Map proxy */
            if (mhash == MHASH_set && strcmp(method, "set") == 0 && arg_count == 2) {
                if (obj->phase == VTAG_CRYSTAL) {
                    runtime_error(vm, "cannot set on a frozen Ref");
                    return true;
                }
                LatValue val2 = vm_peek(vm, 0)[0]; vm->stack_top--;
                LatValue key = vm_peek(vm, 0)[0]; vm->stack_top--;
                if (key.type == VAL_STR) {
                    LatValue *old = (LatValue *)lat_map_get(inner->as.map.map, key.as.str_val);
                    if (old) value_free(old);
                    lat_map_set(inner->as.map.map, key.as.str_val, &val2);
                } else {
                    value_free(&val2);
                }
                value_free(&key);
                push(vm, value_unit());
                return true;
            }
            if (mhash == MHASH_has && strcmp(method, "has") == 0 && arg_count == 1) {
                LatValue key = vm_peek(vm, 0)[0]; vm->stack_top--;
                bool found = key.type == VAL_STR && lat_map_contains(inner->as.map.map, key.as.str_val);
                value_free(&key);
                push(vm, value_bool(found));
                return true;
            }
            if (mhash == MHASH_contains && strcmp(method, "contains") == 0 && arg_count == 1) {
                LatValue needle = vm_peek(vm, 0)[0]; vm->stack_top--;
                bool found = false;
                for (size_t i = 0; i < inner->as.map.map->cap; i++) {
                    if (inner->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                    LatValue *mv = (LatValue *)inner->as.map.map->entries[i].value;
                    if (value_eq(mv, &needle)) { found = true; break; }
                }
                value_free(&needle);
                push(vm, value_bool(found));
                return true;
            }
            if (mhash == MHASH_keys && strcmp(method, "keys") == 0 && arg_count == 0) {
                size_t n = lat_map_len(inner->as.map.map);
                LatValue *elems = malloc((n > 0 ? n : 1) * sizeof(LatValue));
                size_t ei = 0;
                for (size_t i = 0; i < inner->as.map.map->cap; i++) {
                    if (inner->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                    elems[ei++] = value_string(inner->as.map.map->entries[i].key);
                }
                LatValue arr = value_array(elems, ei); free(elems);
                push(vm, arr);
                return true;
            }
            if (mhash == MHASH_values && strcmp(method, "values") == 0 && arg_count == 0) {
                size_t n = lat_map_len(inner->as.map.map);
                LatValue *elems = malloc((n > 0 ? n : 1) * sizeof(LatValue));
                size_t ei = 0;
                for (size_t i = 0; i < inner->as.map.map->cap; i++) {
                    if (inner->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                    LatValue *mv = (LatValue *)inner->as.map.map->entries[i].value;
                    elems[ei++] = value_deep_clone(mv);
                }
                LatValue arr = value_array(elems, ei); free(elems);
                push(vm, arr);
                return true;
            }
            if (mhash == MHASH_entries && strcmp(method, "entries") == 0 && arg_count == 0) {
                size_t n = lat_map_len(inner->as.map.map);
                LatValue *elems = malloc((n > 0 ? n : 1) * sizeof(LatValue));
                size_t ei = 0;
                for (size_t i = 0; i < inner->as.map.map->cap; i++) {
                    if (inner->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                    LatValue pair[2];
                    pair[0] = value_string(inner->as.map.map->entries[i].key);
                    pair[1] = value_deep_clone((LatValue *)inner->as.map.map->entries[i].value);
                    elems[ei++] = value_array(pair, 2);
                }
                LatValue arr = value_array(elems, ei); free(elems);
                push(vm, arr);
                return true;
            }
            if (mhash == MHASH_len && strcmp(method, "len") == 0 && arg_count == 0) {
                push(vm, value_int((int64_t)lat_map_len(inner->as.map.map)));
                return true;
            }
            if (mhash == MHASH_merge && strcmp(method, "merge") == 0 && arg_count == 1) {
                if (obj->phase == VTAG_CRYSTAL) {
                    runtime_error(vm, "cannot merge into a frozen Ref");
                    return true;
                }
                LatValue other = vm_peek(vm, 0)[0]; vm->stack_top--;
                if (other.type == VAL_MAP) {
                    for (size_t i = 0; i < other.as.map.map->cap; i++) {
                        if (other.as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                        LatValue cloned = value_deep_clone((LatValue *)other.as.map.map->entries[i].value);
                        LatValue *old = (LatValue *)lat_map_get(inner->as.map.map, other.as.map.map->entries[i].key);
                        if (old) value_free(old);
                        lat_map_set(inner->as.map.map, other.as.map.map->entries[i].key, &cloned);
                    }
                }
                value_free(&other);
                push(vm, value_unit());
                return true;
            }
        }

        /* Array proxy (when inner is VAL_ARRAY) */
        if (inner->type == VAL_ARRAY) {
            if (mhash == MHASH_push && strcmp(method, "push") == 0 && arg_count == 1) {
                if (obj->phase == VTAG_CRYSTAL) {
                    runtime_error(vm, "cannot push to a frozen Ref");
                    return true;
                }
                LatValue val2 = vm_peek(vm, 0)[0]; vm->stack_top--;
                if (inner->as.array.len >= inner->as.array.cap) {
                    size_t old_cap = inner->as.array.cap;
                    inner->as.array.cap = old_cap < 4 ? 4 : old_cap * 2;
                    inner->as.array.elems = realloc(inner->as.array.elems, inner->as.array.cap * sizeof(LatValue));
                }
                inner->as.array.elems[inner->as.array.len++] = val2;
                push(vm, value_unit());
                return true;
            }
            if (mhash == MHASH_pop && strcmp(method, "pop") == 0 && arg_count == 0) {
                if (obj->phase == VTAG_CRYSTAL) {
                    runtime_error(vm, "cannot pop from a frozen Ref");
                    return true;
                }
                if (inner->as.array.len == 0) {
                    runtime_error(vm, "pop on empty array");
                    return true;
                }
                LatValue popped = inner->as.array.elems[--inner->as.array.len];
                push(vm, popped);
                return true;
            }
            if (mhash == MHASH_len && strcmp(method, "len") == 0 && arg_count == 0) {
                push(vm, value_int((int64_t)inner->as.array.len));
                return true;
            }
            if (mhash == MHASH_contains && strcmp(method, "contains") == 0 && arg_count == 1) {
                LatValue needle = vm_peek(vm, 0)[0]; vm->stack_top--;
                bool found = false;
                for (size_t i = 0; i < inner->as.array.len; i++) {
                    if (value_eq(&inner->as.array.elems[i], &needle)) { found = true; break; }
                }
                value_free(&needle);
                push(vm, value_bool(found));
                return true;
            }
        }
    } break;

    default: break;
    } /* end switch */

    return false;
}

/* ── Runtime type checking (mirrors eval.c type_matches_value) ── */

static bool vm_type_matches(const LatValue *val, const char *type_name) {
    if (!type_name || strcmp(type_name, "Any") == 0) return true;
    if (strcmp(type_name, "Int") == 0)     return val->type == VAL_INT;
    if (strcmp(type_name, "Float") == 0)   return val->type == VAL_FLOAT;
    if (strcmp(type_name, "String") == 0)  return val->type == VAL_STR;
    if (strcmp(type_name, "Bool") == 0)    return val->type == VAL_BOOL;
    if (strcmp(type_name, "Nil") == 0)     return val->type == VAL_NIL;
    if (strcmp(type_name, "Map") == 0)     return val->type == VAL_MAP;
    if (strcmp(type_name, "Array") == 0)   return val->type == VAL_ARRAY;
    if (strcmp(type_name, "Fn") == 0 || strcmp(type_name, "Closure") == 0)
        return val->type == VAL_CLOSURE;
    if (strcmp(type_name, "Channel") == 0) return val->type == VAL_CHANNEL;
    if (strcmp(type_name, "Range") == 0)   return val->type == VAL_RANGE;
    if (strcmp(type_name, "Set") == 0)     return val->type == VAL_SET;
    if (strcmp(type_name, "Tuple") == 0)   return val->type == VAL_TUPLE;
    if (strcmp(type_name, "Buffer") == 0)  return val->type == VAL_BUFFER;
    if (strcmp(type_name, "Ref") == 0)     return val->type == VAL_REF;
    if (strcmp(type_name, "Number") == 0)
        return val->type == VAL_INT || val->type == VAL_FLOAT;
    /* Struct name check */
    if (val->type == VAL_STRUCT && val->as.strct.name)
        return strcmp(val->as.strct.name, type_name) == 0;
    /* Enum name check */
    if (val->type == VAL_ENUM && val->as.enm.enum_name)
        return strcmp(val->as.enm.enum_name, type_name) == 0;
    return false;
}

static const char *vm_value_type_display(const LatValue *val) {
    switch (val->type) {
        case VAL_INT:     return "Int";
        case VAL_FLOAT:   return "Float";
        case VAL_BOOL:    return "Bool";
        case VAL_STR:     return "String";
        case VAL_ARRAY:   return "Array";
        case VAL_STRUCT:  return val->as.strct.name ? val->as.strct.name : "Struct";
        case VAL_CLOSURE: return "Fn";
        case VAL_UNIT:    return "Unit";
        case VAL_NIL:     return "Nil";
        case VAL_RANGE:   return "Range";
        case VAL_MAP:     return "Map";
        case VAL_CHANNEL: return "Channel";
        case VAL_ENUM:    return val->as.enm.enum_name ? val->as.enm.enum_name : "Enum";
        case VAL_SET:     return "Set";
        case VAL_TUPLE:   return "Tuple";
        case VAL_BUFFER:  return "Buffer";
        case VAL_REF:     return "Ref";
    }
    return "Unknown";
}

/* ── Execution ── */

#define READ_BYTE() (*frame->ip++)
#define READ_U16() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))

/* Route runtime errors through exception handlers when possible.
 * Use in place of 'return runtime_error(vm, ...)' inside the dispatch loop. */
#define VM_ERROR(...) do { \
    VMResult _err = vm_handle_error(vm, &frame, __VA_ARGS__); \
    if (_err != VM_OK) return _err; \
} while(0)

/* Adjust stack for default parameters and variadic arguments before a compiled
 * closure call. Returns the adjusted arg_count, or -1 on error. On error,
 * vm->error is set with a heap-allocated message. */
/* Convert a Map or Set on the stack (pointed to by iter) to a key/value array in-place. */
static void vm_iter_convert_to_array(LatValue *iter) {
    bool is_map = (iter->type == VAL_MAP);
    LatMap *hm = is_map ? iter->as.map.map : iter->as.set.map;
    size_t len = lat_map_len(hm);
    LatValue *elms = malloc((len ? len : 1) * sizeof(LatValue));
    size_t ei = 0;
    for (size_t i = 0; i < hm->cap; i++) {
        if (hm->entries[i].state == MAP_OCCUPIED) {
            if (is_map)
                elms[ei++] = value_string(hm->entries[i].key);
            else
                elms[ei++] = value_deep_clone((LatValue *)hm->entries[i].value);
        }
    }
    LatValue arr = value_array(elms, ei);
    free(elms);
    value_free(iter);
    *iter = arr;
}

static int vm_adjust_call_args(VM *vm, Chunk *fn_chunk, int arity, int arg_count) {
    int dc = fn_chunk->default_count;
    bool vd = fn_chunk->fn_has_variadic;
    if (dc == 0 && !vd) {
        if (arg_count != arity) {
            (void)asprintf(&vm->error, "expected %d arguments but got %d", arity, arg_count);
            return -1;
        }
        return arg_count;
    }
    int required = arity - dc - (vd ? 1 : 0);
    int non_variadic = vd ? arity - 1 : arity;

    if (arg_count < required || (!vd && arg_count > arity)) {
        if (vd)
            (void)asprintf(&vm->error, "expected at least %d arguments but got %d", required, arg_count);
        else if (dc > 0)
            (void)asprintf(&vm->error, "expected %d to %d arguments but got %d", required, arity, arg_count);
        else
            (void)asprintf(&vm->error, "expected %d arguments but got %d", arity, arg_count);
        return -1;
    }

    /* Push defaults for missing non-variadic params */
    if (arg_count < non_variadic && fn_chunk->default_values) {
        for (int i = arg_count; i < non_variadic; i++) {
            int def_idx = i - required;
            push(vm, value_clone_fast(&fn_chunk->default_values[def_idx]));
        }
        arg_count = non_variadic;
    }

    /* Bundle variadic args into array */
    if (vd) {
        int extra = arg_count - non_variadic;
        if (extra < 0) extra = 0;
        LatValue *elems = NULL;
        if (extra > 0) {
            elems = malloc(extra * sizeof(LatValue));
            for (int i = extra - 1; i >= 0; i--)
                elems[i] = pop(vm);
        }
        push(vm, value_array(elems, extra));
        free(elems);
        arg_count = arity;
    }

    return arg_count;
}

VMResult vm_run(VM *vm, Chunk *chunk, LatValue *result) {
    current_vm = vm;
    size_t base_frame = vm->frame_count;
    CallFrame *frame = &vm->frames[vm->frame_count++];
    frame->chunk = chunk;
    frame->ip = chunk->code;
    if (vm->next_frame_slots) {
        frame->slots = vm->next_frame_slots;
        frame->cleanup_base = vm->stack_top;  /* Only free above this point on return */
        vm->next_frame_slots = NULL;
    } else {
        frame->slots = vm->stack_top;
        frame->cleanup_base = NULL;
    }
    frame->upvalues = NULL;
    frame->upvalue_count = 0;

#ifdef VM_USE_COMPUTED_GOTO
    static void *dispatch_table[] = {
        [OP_CONSTANT] = &&lbl_OP_CONSTANT,
        [OP_NIL] = &&lbl_OP_NIL, [OP_TRUE] = &&lbl_OP_TRUE,
        [OP_FALSE] = &&lbl_OP_FALSE, [OP_UNIT] = &&lbl_OP_UNIT,
        [OP_POP] = &&lbl_OP_POP, [OP_DUP] = &&lbl_OP_DUP, [OP_SWAP] = &&lbl_OP_SWAP,
        [OP_ADD] = &&lbl_OP_ADD, [OP_SUB] = &&lbl_OP_SUB,
        [OP_MUL] = &&lbl_OP_MUL, [OP_DIV] = &&lbl_OP_DIV, [OP_MOD] = &&lbl_OP_MOD,
        [OP_NEG] = &&lbl_OP_NEG, [OP_NOT] = &&lbl_OP_NOT,
        [OP_BIT_AND] = &&lbl_OP_BIT_AND, [OP_BIT_OR] = &&lbl_OP_BIT_OR,
        [OP_BIT_XOR] = &&lbl_OP_BIT_XOR, [OP_BIT_NOT] = &&lbl_OP_BIT_NOT,
        [OP_LSHIFT] = &&lbl_OP_LSHIFT, [OP_RSHIFT] = &&lbl_OP_RSHIFT,
        [OP_EQ] = &&lbl_OP_EQ, [OP_NEQ] = &&lbl_OP_NEQ,
        [OP_LT] = &&lbl_OP_LT, [OP_GT] = &&lbl_OP_GT,
        [OP_LTEQ] = &&lbl_OP_LTEQ, [OP_GTEQ] = &&lbl_OP_GTEQ,
        [OP_CONCAT] = &&lbl_OP_CONCAT,
        [OP_GET_LOCAL] = &&lbl_OP_GET_LOCAL, [OP_SET_LOCAL] = &&lbl_OP_SET_LOCAL,
        [OP_GET_GLOBAL] = &&lbl_OP_GET_GLOBAL, [OP_SET_GLOBAL] = &&lbl_OP_SET_GLOBAL,
        [OP_DEFINE_GLOBAL] = &&lbl_OP_DEFINE_GLOBAL,
        [OP_GET_UPVALUE] = &&lbl_OP_GET_UPVALUE, [OP_SET_UPVALUE] = &&lbl_OP_SET_UPVALUE,
        [OP_CLOSE_UPVALUE] = &&lbl_OP_CLOSE_UPVALUE,
        [OP_JUMP] = &&lbl_OP_JUMP, [OP_JUMP_IF_FALSE] = &&lbl_OP_JUMP_IF_FALSE,
        [OP_JUMP_IF_TRUE] = &&lbl_OP_JUMP_IF_TRUE,
        [OP_JUMP_IF_NOT_NIL] = &&lbl_OP_JUMP_IF_NOT_NIL, [OP_LOOP] = &&lbl_OP_LOOP,
        [OP_CALL] = &&lbl_OP_CALL, [OP_CLOSURE] = &&lbl_OP_CLOSURE,
        [OP_RETURN] = &&lbl_OP_RETURN,
        [OP_ITER_INIT] = &&lbl_OP_ITER_INIT, [OP_ITER_NEXT] = &&lbl_OP_ITER_NEXT,
        [OP_BUILD_ARRAY] = &&lbl_OP_BUILD_ARRAY, [OP_ARRAY_FLATTEN] = &&lbl_OP_ARRAY_FLATTEN,
        [OP_BUILD_MAP] = &&lbl_OP_BUILD_MAP, [OP_BUILD_TUPLE] = &&lbl_OP_BUILD_TUPLE,
        [OP_BUILD_STRUCT] = &&lbl_OP_BUILD_STRUCT, [OP_BUILD_RANGE] = &&lbl_OP_BUILD_RANGE,
        [OP_BUILD_ENUM] = &&lbl_OP_BUILD_ENUM,
        [OP_INDEX] = &&lbl_OP_INDEX, [OP_SET_INDEX] = &&lbl_OP_SET_INDEX,
        [OP_GET_FIELD] = &&lbl_OP_GET_FIELD, [OP_SET_FIELD] = &&lbl_OP_SET_FIELD,
        [OP_INVOKE] = &&lbl_OP_INVOKE, [OP_INVOKE_LOCAL] = &&lbl_OP_INVOKE_LOCAL,
        [OP_INVOKE_GLOBAL] = &&lbl_OP_INVOKE_GLOBAL,
        [OP_SET_INDEX_LOCAL] = &&lbl_OP_SET_INDEX_LOCAL,
        [OP_PUSH_EXCEPTION_HANDLER] = &&lbl_OP_PUSH_EXCEPTION_HANDLER,
        [OP_POP_EXCEPTION_HANDLER] = &&lbl_OP_POP_EXCEPTION_HANDLER,
        [OP_THROW] = &&lbl_OP_THROW, [OP_TRY_UNWRAP] = &&lbl_OP_TRY_UNWRAP,
        [OP_DEFER_PUSH] = &&lbl_OP_DEFER_PUSH, [OP_DEFER_RUN] = &&lbl_OP_DEFER_RUN,
        [OP_FREEZE] = &&lbl_OP_FREEZE, [OP_THAW] = &&lbl_OP_THAW,
        [OP_CLONE] = &&lbl_OP_CLONE, [OP_MARK_FLUID] = &&lbl_OP_MARK_FLUID,
        [OP_REACT] = &&lbl_OP_REACT, [OP_UNREACT] = &&lbl_OP_UNREACT,
        [OP_BOND] = &&lbl_OP_BOND, [OP_UNBOND] = &&lbl_OP_UNBOND,
        [OP_SEED] = &&lbl_OP_SEED, [OP_UNSEED] = &&lbl_OP_UNSEED,
        [OP_FREEZE_VAR] = &&lbl_OP_FREEZE_VAR, [OP_THAW_VAR] = &&lbl_OP_THAW_VAR,
        [OP_SUBLIMATE_VAR] = &&lbl_OP_SUBLIMATE_VAR, [OP_SUBLIMATE] = &&lbl_OP_SUBLIMATE,
        [OP_PRINT] = &&lbl_OP_PRINT, [OP_IMPORT] = &&lbl_OP_IMPORT,
        [OP_SCOPE] = &&lbl_OP_SCOPE, [OP_SELECT] = &&lbl_OP_SELECT,
        [OP_INC_LOCAL] = &&lbl_OP_INC_LOCAL, [OP_DEC_LOCAL] = &&lbl_OP_DEC_LOCAL,
        [OP_ADD_INT] = &&lbl_OP_ADD_INT, [OP_SUB_INT] = &&lbl_OP_SUB_INT,
        [OP_MUL_INT] = &&lbl_OP_MUL_INT, [OP_LT_INT] = &&lbl_OP_LT_INT,
        [OP_LTEQ_INT] = &&lbl_OP_LTEQ_INT, [OP_LOAD_INT8] = &&lbl_OP_LOAD_INT8,
        [OP_CONSTANT_16] = &&lbl_OP_CONSTANT_16,
        [OP_GET_GLOBAL_16] = &&lbl_OP_GET_GLOBAL_16,
        [OP_SET_GLOBAL_16] = &&lbl_OP_SET_GLOBAL_16,
        [OP_DEFINE_GLOBAL_16] = &&lbl_OP_DEFINE_GLOBAL_16,
        [OP_CLOSURE_16] = &&lbl_OP_CLOSURE_16,
        [OP_RESET_EPHEMERAL] = &&lbl_OP_RESET_EPHEMERAL,
        [OP_SET_LOCAL_POP] = &&lbl_OP_SET_LOCAL_POP,
        [OP_CHECK_TYPE] = &&lbl_OP_CHECK_TYPE,
        [OP_CHECK_RETURN_TYPE] = &&lbl_OP_CHECK_RETURN_TYPE,
        [OP_IS_CRYSTAL] = &&lbl_OP_IS_CRYSTAL,
        [OP_FREEZE_EXCEPT] = &&lbl_OP_FREEZE_EXCEPT,
        [OP_FREEZE_FIELD] = &&lbl_OP_FREEZE_FIELD,
        [OP_HALT] = &&lbl_OP_HALT,
    };
#endif

    for (;;) {
        uint8_t op = READ_BYTE();

#ifdef VM_USE_COMPUTED_GOTO
        goto *dispatch_table[op];
#endif
        switch (op) {
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_CONSTANT:
#endif
            case OP_CONSTANT: {
                uint8_t idx = READ_BYTE();
                push(vm, value_clone_fast(&frame->chunk->constants[idx]));
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_CONSTANT_16:
#endif
            case OP_CONSTANT_16: {
                uint16_t idx = READ_U16();
                push(vm, value_clone_fast(&frame->chunk->constants[idx]));
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_NIL:
#endif
            case OP_NIL:   push(vm, value_nil()); break;
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_TRUE:
#endif
            case OP_TRUE:  push(vm, value_bool(true)); break;
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_FALSE:
#endif
            case OP_FALSE: push(vm, value_bool(false)); break;
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_UNIT:
#endif
            case OP_UNIT:  push(vm, value_unit()); break;

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_POP:
#endif
            case OP_POP: {
                vm->stack_top--;
                value_free(vm->stack_top);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_DUP:
#endif
            case OP_DUP: {
                push(vm, value_clone_fast(vm_peek(vm, 0)));
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_SWAP:
#endif
            case OP_SWAP: {
                LatValue a = vm->stack_top[-1];
                vm->stack_top[-1] = vm->stack_top[-2];
                vm->stack_top[-2] = a;
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_ADD:
#endif
            case OP_ADD: {
                LatValue b = pop(vm), a = pop(vm);
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    push(vm, value_int(a.as.int_val + b.as.int_val));
                } else if (a.type == VAL_FLOAT && b.type == VAL_FLOAT) {
                    push(vm, value_float(a.as.float_val + b.as.float_val));
                } else if (a.type == VAL_INT && b.type == VAL_FLOAT) {
                    push(vm, value_float((double)a.as.int_val + b.as.float_val));
                } else if (a.type == VAL_FLOAT && b.type == VAL_INT) {
                    push(vm, value_float(a.as.float_val + (double)b.as.int_val));
                } else if (a.type == VAL_STR || b.type == VAL_STR) {
                    const char *pa = (a.type == VAL_STR) ? a.as.str_val : NULL;
                    const char *pb = (b.type == VAL_STR) ? b.as.str_val : NULL;
                    char *ra = pa ? NULL : value_repr(&a);
                    char *rb = pb ? NULL : value_repr(&b);
                    if (!pa) pa = ra;
                    if (!pb) pb = rb;
                    size_t la = strlen(pa), lb = strlen(pb);
                    LatValue result = vm_ephemeral_concat(vm, pa, la, pb, lb);
                    free(ra); free(rb);
                    value_free(&a); value_free(&b);
                    push(vm, result);
                    break;
                } else {
                    value_free(&a); value_free(&b);
                    VM_ERROR("operands must be numbers for '+'"); break;
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_SUB:
#endif
            case OP_SUB: {
                LatValue b = pop(vm), a = pop(vm);
                if (a.type == VAL_INT && b.type == VAL_INT)
                    push(vm, value_int(a.as.int_val - b.as.int_val));
                else if (a.type == VAL_FLOAT && b.type == VAL_FLOAT)
                    push(vm, value_float(a.as.float_val - b.as.float_val));
                else if (a.type == VAL_INT && b.type == VAL_FLOAT)
                    push(vm, value_float((double)a.as.int_val - b.as.float_val));
                else if (a.type == VAL_FLOAT && b.type == VAL_INT)
                    push(vm, value_float(a.as.float_val - (double)b.as.int_val));
                else {
                    value_free(&a); value_free(&b);
                    VM_ERROR("operands must be numbers for '-'"); break;
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_MUL:
#endif
            case OP_MUL: {
                LatValue b = pop(vm), a = pop(vm);
                if (a.type == VAL_INT && b.type == VAL_INT)
                    push(vm, value_int(a.as.int_val * b.as.int_val));
                else if (a.type == VAL_FLOAT && b.type == VAL_FLOAT)
                    push(vm, value_float(a.as.float_val * b.as.float_val));
                else if (a.type == VAL_INT && b.type == VAL_FLOAT)
                    push(vm, value_float((double)a.as.int_val * b.as.float_val));
                else if (a.type == VAL_FLOAT && b.type == VAL_INT)
                    push(vm, value_float(a.as.float_val * (double)b.as.int_val));
                else {
                    value_free(&a); value_free(&b);
                    VM_ERROR("operands must be numbers for '*'"); break;
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_DIV:
#endif
            case OP_DIV: {
                LatValue b = pop(vm), a = pop(vm);
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    if (b.as.int_val == 0) { VM_ERROR("division by zero"); break; }
                    push(vm, value_int(a.as.int_val / b.as.int_val));
                } else if (a.type == VAL_FLOAT || b.type == VAL_FLOAT) {
                    double fa = a.type == VAL_INT ? (double)a.as.int_val : a.as.float_val;
                    double fb = b.type == VAL_INT ? (double)b.as.int_val : b.as.float_val;
                    /* Let IEEE 754 produce NaN/Inf for float division by zero */
                    push(vm, value_float(fa / fb));
                } else {
                    value_free(&a); value_free(&b);
                    VM_ERROR("operands must be numbers for '/'"); break;
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_MOD:
#endif
            case OP_MOD: {
                LatValue b = pop(vm), a = pop(vm);
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    if (b.as.int_val == 0) { VM_ERROR("modulo by zero"); break; }
                    push(vm, value_int(a.as.int_val % b.as.int_val));
                } else {
                    value_free(&a); value_free(&b);
                    VM_ERROR("operands must be integers for '%%'"); break;
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_NEG:
#endif
            case OP_NEG: {
                LatValue a = pop(vm);
                if (a.type == VAL_INT) push(vm, value_int(-a.as.int_val));
                else if (a.type == VAL_FLOAT) push(vm, value_float(-a.as.float_val));
                else {
                    value_free(&a);
                    VM_ERROR("operand must be a number for unary '-'"); break;
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_NOT:
#endif
            case OP_NOT: {
                LatValue a = pop(vm);
                bool falsy_val = is_falsy(&a);
                value_free(&a);
                push(vm, value_bool(falsy_val));
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_EQ:
#endif
            case OP_EQ: {
                LatValue b = pop(vm), a = pop(vm);
                bool eq = value_eq(&a, &b);
                value_free(&a); value_free(&b);
                push(vm, value_bool(eq));
                break;
            }
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_NEQ:
#endif
            case OP_NEQ: {
                LatValue b = pop(vm), a = pop(vm);
                bool eq = value_eq(&a, &b);
                value_free(&a); value_free(&b);
                push(vm, value_bool(!eq));
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_LT:
#endif
            case OP_LT: {
                LatValue b = pop(vm), a = pop(vm);
                if (a.type == VAL_INT && b.type == VAL_INT)
                    push(vm, value_bool(a.as.int_val < b.as.int_val));
                else if (a.type == VAL_FLOAT || b.type == VAL_FLOAT) {
                    double fa = a.type == VAL_INT ? (double)a.as.int_val : a.as.float_val;
                    double fb = b.type == VAL_INT ? (double)b.as.int_val : b.as.float_val;
                    push(vm, value_bool(fa < fb));
                } else {
                    value_free(&a); value_free(&b);
                    VM_ERROR("operands must be numbers for '<'"); break;
                }
                break;
            }
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_GT:
#endif
            case OP_GT: {
                LatValue b = pop(vm), a = pop(vm);
                if (a.type == VAL_INT && b.type == VAL_INT)
                    push(vm, value_bool(a.as.int_val > b.as.int_val));
                else if (a.type == VAL_FLOAT || b.type == VAL_FLOAT) {
                    double fa = a.type == VAL_INT ? (double)a.as.int_val : a.as.float_val;
                    double fb = b.type == VAL_INT ? (double)b.as.int_val : b.as.float_val;
                    push(vm, value_bool(fa > fb));
                } else {
                    value_free(&a); value_free(&b);
                    VM_ERROR("operands must be numbers for '>'"); break;
                }
                break;
            }
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_LTEQ:
#endif
            case OP_LTEQ: {
                LatValue b = pop(vm), a = pop(vm);
                if (a.type == VAL_INT && b.type == VAL_INT)
                    push(vm, value_bool(a.as.int_val <= b.as.int_val));
                else if (a.type == VAL_FLOAT || b.type == VAL_FLOAT) {
                    double fa = a.type == VAL_INT ? (double)a.as.int_val : a.as.float_val;
                    double fb = b.type == VAL_INT ? (double)b.as.int_val : b.as.float_val;
                    push(vm, value_bool(fa <= fb));
                } else {
                    value_free(&a); value_free(&b);
                    VM_ERROR("operands must be numbers for '<='"); break;
                }
                break;
            }
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_GTEQ:
#endif
            case OP_GTEQ: {
                LatValue b = pop(vm), a = pop(vm);
                if (a.type == VAL_INT && b.type == VAL_INT)
                    push(vm, value_bool(a.as.int_val >= b.as.int_val));
                else if (a.type == VAL_FLOAT || b.type == VAL_FLOAT) {
                    double fa = a.type == VAL_INT ? (double)a.as.int_val : a.as.float_val;
                    double fb = b.type == VAL_INT ? (double)b.as.int_val : b.as.float_val;
                    push(vm, value_bool(fa >= fb));
                } else {
                    value_free(&a); value_free(&b);
                    VM_ERROR("operands must be numbers for '>='"); break;
                }
                break;
            }

            /* ── Bitwise operations ── */
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_BIT_AND:
#endif
            case OP_BIT_AND: {
                LatValue b = pop(vm), a = pop(vm);
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    push(vm, value_int(a.as.int_val & b.as.int_val));
                } else {
                    value_free(&a); value_free(&b);
                    VM_ERROR("operands must be integers for '&'"); break;
                }
                break;
            }
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_BIT_OR:
#endif
            case OP_BIT_OR: {
                LatValue b = pop(vm), a = pop(vm);
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    push(vm, value_int(a.as.int_val | b.as.int_val));
                } else {
                    value_free(&a); value_free(&b);
                    VM_ERROR("operands must be integers for '|'"); break;
                }
                break;
            }
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_BIT_XOR:
#endif
            case OP_BIT_XOR: {
                LatValue b = pop(vm), a = pop(vm);
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    push(vm, value_int(a.as.int_val ^ b.as.int_val));
                } else {
                    value_free(&a); value_free(&b);
                    VM_ERROR("operands must be integers for '^'"); break;
                }
                break;
            }
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_BIT_NOT:
#endif
            case OP_BIT_NOT: {
                LatValue a = pop(vm);
                if (a.type == VAL_INT) {
                    push(vm, value_int(~a.as.int_val));
                } else {
                    value_free(&a);
                    VM_ERROR("operand must be an integer for '~'"); break;
                }
                break;
            }
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_LSHIFT:
#endif
            case OP_LSHIFT: {
                LatValue b = pop(vm), a = pop(vm);
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    if (b.as.int_val < 0 || b.as.int_val > 63) {
                        VM_ERROR("shift amount out of range (0..63)"); break;
                    }
                    push(vm, value_int(a.as.int_val << b.as.int_val));
                } else {
                    value_free(&a); value_free(&b);
                    VM_ERROR("operands must be integers for '<<'"); break;
                }
                break;
            }
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_RSHIFT:
#endif
            case OP_RSHIFT: {
                LatValue b = pop(vm), a = pop(vm);
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    if (b.as.int_val < 0 || b.as.int_val > 63) {
                        VM_ERROR("shift amount out of range (0..63)"); break;
                    }
                    push(vm, value_int(a.as.int_val >> b.as.int_val));
                } else {
                    value_free(&a); value_free(&b);
                    VM_ERROR("operands must be integers for '>>'"); break;
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_CONCAT:
#endif
            case OP_CONCAT: {
                LatValue b = pop(vm), a = pop(vm);
                const char *pa = (a.type == VAL_STR) ? a.as.str_val : NULL;
                const char *pb = (b.type == VAL_STR) ? b.as.str_val : NULL;
                char *ra = pa ? NULL : value_repr(&a);
                char *rb = pb ? NULL : value_repr(&b);
                if (!pa) pa = ra;
                if (!pb) pb = rb;
                size_t la = strlen(pa), lb = strlen(pb);
                LatValue result = vm_ephemeral_concat(vm, pa, la, pb, lb);
                free(ra); free(rb);
                value_free(&a); value_free(&b);
                push(vm, result);
                break;
            }

            /* ── Variables ── */
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_GET_LOCAL:
#endif
            case OP_GET_LOCAL: {
                uint8_t slot = READ_BYTE();
                push(vm, value_clone_fast(&frame->slots[slot]));
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_SET_LOCAL:
#endif
            case OP_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                value_free(&frame->slots[slot]);
                frame->slots[slot] = value_clone_fast(vm_peek(vm, 0));
                /* Record history for tracked variables */
                if (vm->tracking_active && frame->chunk->local_names &&
                    slot < frame->chunk->local_name_cap && frame->chunk->local_names[slot]) {
                    vm_record_history(vm, frame->chunk->local_names[slot], &frame->slots[slot]);
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_SET_LOCAL_POP:
#endif
            case OP_SET_LOCAL_POP: {
                uint8_t slot = READ_BYTE();
                LatValue *dest = &frame->slots[slot];
                value_free(dest);
                *dest = value_clone_fast(vm_peek(vm, 0));
                if (vm->tracking_active && frame->chunk->local_names &&
                    slot < frame->chunk->local_name_cap && frame->chunk->local_names[slot]) {
                    vm_record_history(vm, frame->chunk->local_names[slot], dest);
                }
                vm->stack_top--;
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_GET_GLOBAL:
#endif
            case OP_GET_GLOBAL: {
                uint8_t idx = READ_BYTE();
                const char *name = frame->chunk->constants[idx].as.str_val;
                size_t hash = frame->chunk->const_hashes[idx];
                LatValue *ref = env_get_ref_prehashed(vm->env, name, hash);
                if (!ref) {
                    VM_ERROR("undefined variable '%s'", name); break;
                }
                if (ref->type == VAL_CLOSURE && ref->as.closure.native_fn != NULL &&
                    (ref->as.closure.default_values == VM_NATIVE_MARKER ||
                     ref->as.closure.default_values == VM_EXT_MARKER)) {
                    push(vm, *ref);
                } else {
                    push(vm, value_clone_fast(ref));
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_GET_GLOBAL_16:
#endif
            case OP_GET_GLOBAL_16: {
                uint16_t idx = READ_U16();
                const char *name = frame->chunk->constants[idx].as.str_val;
                size_t hash = frame->chunk->const_hashes[idx];
                LatValue *ref = env_get_ref_prehashed(vm->env, name, hash);
                if (!ref) {
                    VM_ERROR("undefined variable '%s'", name); break;
                }
                if (ref->type == VAL_CLOSURE && ref->as.closure.native_fn != NULL &&
                    (ref->as.closure.default_values == VM_NATIVE_MARKER ||
                     ref->as.closure.default_values == VM_EXT_MARKER)) {
                    push(vm, *ref);
                } else {
                    push(vm, value_clone_fast(ref));
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_SET_GLOBAL:
#endif
            case OP_SET_GLOBAL: {
                uint8_t idx = READ_BYTE();
                const char *name = frame->chunk->constants[idx].as.str_val;
                LatValue *val = vm_peek(vm, 0);
                env_set(vm->env, name, value_clone_fast(val));
                if (vm->tracking_active) {
                    vm_record_history(vm, name, val);
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_SET_GLOBAL_16:
#endif
            case OP_SET_GLOBAL_16: {
                uint16_t idx = READ_U16();
                const char *name = frame->chunk->constants[idx].as.str_val;
                LatValue *val = vm_peek(vm, 0);
                env_set(vm->env, name, value_clone_fast(val));
                if (vm->tracking_active) {
                    vm_record_history(vm, name, val);
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_DEFINE_GLOBAL:
#endif
            case OP_DEFINE_GLOBAL: {
                uint8_t idx = READ_BYTE();
                const char *name = frame->chunk->constants[idx].as.str_val;
                LatValue val = pop(vm);
                vm_promote_value(&val);

                /* Phase-dispatch overloading: if defining a phase-constrained
                 * closure and one already exists, create an overload array */
                if (val.type == VAL_CLOSURE && val.as.closure.native_fn != NULL &&
                    val.as.closure.default_values != VM_NATIVE_MARKER &&
                    val.as.closure.default_values != VM_EXT_MARKER) {
                    Chunk *ch = (Chunk *)val.as.closure.native_fn;
                    if (ch->param_phases) {
                        LatValue existing;
                        if (env_get(vm->env, name, &existing)) {
                            if (existing.type == VAL_CLOSURE && existing.as.closure.native_fn != NULL &&
                                existing.as.closure.default_values != VM_NATIVE_MARKER &&
                                existing.as.closure.default_values != VM_EXT_MARKER) {
                                Chunk *ech = (Chunk *)existing.as.closure.native_fn;
                                if (ech->param_phases) {
                                    LatValue elems[2] = { existing, val };
                                    LatValue arr = value_array(elems, 2);
                                    value_free(&existing);
                                    value_free(&val);
                                    env_define(vm->env, name, arr);
                                    break;
                                }
                            } else if (existing.type == VAL_ARRAY) {
                                /* Append to existing overload set */
                                size_t new_len = existing.as.array.len + 1;
                                LatValue *new_elems = malloc(new_len * sizeof(LatValue));
                                for (size_t i = 0; i < existing.as.array.len; i++)
                                    new_elems[i] = existing.as.array.elems[i];
                                new_elems[existing.as.array.len] = val;
                                LatValue arr = value_array(new_elems, new_len);
                                free(new_elems);
                                value_free(&existing);
                                value_free(&val);
                                env_define(vm->env, name, arr);
                                break;
                            }
                            value_free(&existing);
                        }
                    }
                }

                env_define(vm->env, name, val);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_DEFINE_GLOBAL_16:
#endif
            case OP_DEFINE_GLOBAL_16: {
                uint16_t idx = READ_U16();
                const char *name = frame->chunk->constants[idx].as.str_val;
                LatValue val = pop(vm);
                vm_promote_value(&val);
                env_define(vm->env, name, val);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_GET_UPVALUE:
#endif
            case OP_GET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                if (frame->upvalues && slot < frame->upvalue_count && frame->upvalues[slot]) {
                    push(vm, value_clone_fast(frame->upvalues[slot]->location));
                } else {
                    push(vm, value_nil());
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_SET_UPVALUE:
#endif
            case OP_SET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                if (frame->upvalues && slot < frame->upvalue_count && frame->upvalues[slot]) {
                    value_free(frame->upvalues[slot]->location);
                    *frame->upvalues[slot]->location = value_clone_fast(vm_peek(vm, 0));
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_CLOSE_UPVALUE:
#endif
            case OP_CLOSE_UPVALUE: {
                close_upvalues(vm, vm->stack_top - 1);
                LatValue v = pop(vm);
                value_free(&v);
                break;
            }

            /* ── Jumps ── */
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_JUMP:
#endif
            case OP_JUMP: {
                uint16_t offset = READ_U16();
                frame->ip += offset;
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_JUMP_IF_FALSE:
#endif
            case OP_JUMP_IF_FALSE: {
                uint16_t offset = READ_U16();
                if (is_falsy(vm_peek(vm, 0)))
                    frame->ip += offset;
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_JUMP_IF_TRUE:
#endif
            case OP_JUMP_IF_TRUE: {
                uint16_t offset = READ_U16();
                if (!is_falsy(vm_peek(vm, 0)))
                    frame->ip += offset;
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_JUMP_IF_NOT_NIL:
#endif
            case OP_JUMP_IF_NOT_NIL: {
                uint16_t offset = READ_U16();
                if (vm_peek(vm, 0)->type != VAL_NIL)
                    frame->ip += offset;
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_LOOP:
#endif
            case OP_LOOP: {
                uint16_t offset = READ_U16();
                frame->ip -= offset;
                break;
            }

            /* ── Functions/closures ── */
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_CALL:
#endif
            case OP_CALL: {
                uint8_t arg_count = READ_BYTE();
                LatValue *callee = vm_peek(vm, arg_count);

                if (callee->type == VAL_CLOSURE && callee->as.closure.native_fn != NULL
                    && callee->as.closure.default_values == VM_NATIVE_MARKER) {
                    /* VM native builtin function */
                    VMNativeFn native = (VMNativeFn)callee->as.closure.native_fn;
                    LatValue *args = (arg_count <= 16) ? vm->fast_args
                                   : malloc(arg_count * sizeof(LatValue));
                    for (int i = arg_count - 1; i >= 0; i--)
                        args[i] = pop(vm);
                    LatValue callee_val = pop(vm); /* pop callee */
                    LatValue ret = native(args, arg_count);
                    for (int i = 0; i < arg_count; i++)
                        value_free(&args[i]);
                    if (arg_count > 16) free(args);
                    value_free(&callee_val);
                    /* Check if native set an error (e.g. grow() seed failure) */
                    if (vm->error) {
                        value_free(&ret);
                        VMResult r = vm_handle_native_error(vm, &frame);
                        if (r != VM_OK) return r;
                        break;
                    }
                    push(vm, ret);
                    break;
                }

                if (callee->type == VAL_CLOSURE && callee->as.closure.native_fn != NULL
                    && callee->as.closure.default_values == VM_EXT_MARKER) {
                    /* Extension native function — call via ext_call_native() */
                    LatValue *args = (arg_count <= 16) ? vm->fast_args
                                   : malloc(arg_count * sizeof(LatValue));
                    for (int i = arg_count - 1; i >= 0; i--)
                        args[i] = pop(vm);
                    LatValue callee_val = pop(vm);
                    LatValue ret = ext_call_native(callee_val.as.closure.native_fn,
                                                   args, (size_t)arg_count);
                    for (int i = 0; i < arg_count; i++)
                        value_free(&args[i]);
                    if (arg_count > 16) free(args);
                    value_free(&callee_val);
                    /* Extension errors return strings prefixed with "EVAL_ERROR:" */
                    if (ret.type == VAL_STR && strncmp(ret.as.str_val, "EVAL_ERROR:", 11) == 0) {
                        vm->error = strdup(ret.as.str_val + 11);
                        value_free(&ret);
                        VMResult r = vm_handle_native_error(vm, &frame);
                        if (r != VM_OK) return r;
                        break;
                    }
                    push(vm, ret);
                    break;
                }

                /* Phase-dispatch overload resolution: VAL_ARRAY of closures */
                if (callee->type == VAL_ARRAY) {
                    int best_score = -1;
                    int best_idx = -1;
                    for (size_t ci = 0; ci < callee->as.array.len; ci++) {
                        LatValue *cand = &callee->as.array.elems[ci];
                        if (cand->type != VAL_CLOSURE || cand->as.closure.native_fn == NULL) continue;
                        if (cand->as.closure.default_values == VM_NATIVE_MARKER) continue;
                        if (cand->as.closure.default_values == VM_EXT_MARKER) continue;
                        Chunk *ch = (Chunk *)cand->as.closure.native_fn;
                        if (!ch->param_phases) continue;
                        bool compatible = true;
                        int score = 0;
                        for (int j = 0; j < ch->param_phase_count && j < (int)arg_count; j++) {
                            uint8_t pp = ch->param_phases[j];
                            LatValue *arg = vm_peek(vm, arg_count - 1 - j);
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
                        LatValue matched = value_clone_fast(&callee->as.array.elems[best_idx]);
                        value_free(callee);
                        *callee = matched;
                        /* Fall through to compiled closure call below */
                    } else {
                        VM_ERROR("no matching overload for given argument phases"); break;
                    }
                }

                if (callee->type == VAL_CLOSURE && callee->as.closure.native_fn != NULL) {
                    /* Compiled function call */
                    Chunk *fn_chunk = (Chunk *)callee->as.closure.native_fn;
                    int arity = (int)callee->as.closure.param_count;

                    /* Phase constraint check */
                    if (fn_chunk->param_phases) {
                        bool phase_mismatch = false;
                        for (int i = 0; i < fn_chunk->param_phase_count && i < (int)arg_count; i++) {
                            uint8_t pp = fn_chunk->param_phases[i];
                            if (pp == PHASE_UNSPECIFIED) continue;
                            LatValue *arg = vm_peek(vm, arg_count - 1 - i);
                            if (pp == PHASE_FLUID && arg->phase == VTAG_CRYSTAL) { phase_mismatch = true; break; }
                            if (pp == PHASE_CRYSTAL && arg->phase == VTAG_FLUID) { phase_mismatch = true; break; }
                        }
                        if (phase_mismatch) {
                            VM_ERROR("phase constraint violation in function '%s'",
                                fn_chunk->name ? fn_chunk->name : "<anonymous>");
                            break;
                        }
                    }

                    int adjusted = vm_adjust_call_args(vm, fn_chunk, arity, (int)arg_count);
                    if (adjusted < 0) {
                        char *err = vm->error; vm->error = NULL;
                        VM_ERROR("%s", err); free(err); break;
                    }
                    (void)adjusted;

                    if (vm->frame_count >= VM_FRAMES_MAX) {
                        VM_ERROR("stack overflow (too many nested calls)"); break;
                    }

                    vm_promote_frame_ephemerals(vm, frame);

                    /* Get upvalues from the callee closure */
                    ObjUpvalue **callee_upvalues = (ObjUpvalue **)callee->as.closure.captured_env;
                    size_t callee_upvalue_count = callee->region_id != REGION_NONE ?
                        callee->region_id : 0;

                    CallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->chunk = fn_chunk;
                    new_frame->ip = fn_chunk->code;
                    new_frame->slots = callee;
                    new_frame->upvalues = callee_upvalues;
                    new_frame->upvalue_count = callee_upvalue_count;
                    frame = new_frame;
                    break;
                }

                /* Unknown callee type - pop args and callee, push nil */
                for (int i = 0; i < arg_count; i++) {
                    LatValue v = pop(vm);
                    value_free(&v);
                }
                LatValue callee_val = pop(vm);
                value_free(&callee_val);
                push(vm, value_nil());
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_CLOSURE:
#endif
            case OP_CLOSURE: {
                uint8_t fn_idx = READ_BYTE();
                uint8_t upvalue_count = READ_BYTE();
                LatValue fn_val = value_clone_fast(&frame->chunk->constants[fn_idx]);

                /* Read upvalue descriptors and capture */
                ObjUpvalue **upvalues = NULL;
                if (upvalue_count > 0) {
                    upvalues = calloc(upvalue_count, sizeof(ObjUpvalue *));
                    for (uint8_t i = 0; i < upvalue_count; i++) {
                        uint8_t is_local = READ_BYTE();
                        uint8_t index = READ_BYTE();
                        if (is_local) {
                            upvalues[i] = capture_upvalue(vm, &frame->slots[index]);
                        } else {
                            if (frame->upvalues && index < frame->upvalue_count)
                                upvalues[i] = frame->upvalues[index];
                            else
                                upvalues[i] = new_upvalue(&frame->slots[0]); /* fallback */
                        }
                    }
                }

                /* Store upvalues in the closure value for later OP_CALL.
                 * We pack them into the closure's captured_env as a hack. */
                fn_val.as.closure.captured_env = (Env *)upvalues;
                fn_val.as.closure.has_variadic = (upvalue_count > 0);
                fn_val.region_id = (size_t)upvalue_count;

                /* Track the chunk so it gets freed */
                if (fn_val.as.closure.native_fn) {
                    /* Don't double-track - the chunk is in the constant pool already */
                }

                push(vm, fn_val);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_CLOSURE_16:
#endif
            case OP_CLOSURE_16: {
                uint16_t fn_idx = READ_U16();
                uint8_t upvalue_count = READ_BYTE();
                LatValue fn_val = value_clone_fast(&frame->chunk->constants[fn_idx]);

                ObjUpvalue **upvalues = NULL;
                if (upvalue_count > 0) {
                    upvalues = calloc(upvalue_count, sizeof(ObjUpvalue *));
                    for (uint8_t i = 0; i < upvalue_count; i++) {
                        uint8_t is_local = READ_BYTE();
                        uint8_t index = READ_BYTE();
                        if (is_local) {
                            upvalues[i] = capture_upvalue(vm, &frame->slots[index]);
                        } else {
                            if (frame->upvalues && index < frame->upvalue_count)
                                upvalues[i] = frame->upvalues[index];
                            else
                                upvalues[i] = new_upvalue(&frame->slots[0]);
                        }
                    }
                }

                fn_val.as.closure.captured_env = (Env *)upvalues;
                fn_val.as.closure.has_variadic = (upvalue_count > 0);
                fn_val.region_id = (size_t)upvalue_count;
                push(vm, fn_val);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_RETURN:
#endif
            case OP_RETURN: {
                LatValue ret = pop(vm);
                /* For defer frames (cleanup_base != NULL), only close upvalues
                 * and free stack values above the entry point, preserving
                 * the parent frame's locals that we share via next_frame_slots. */
                LatValue *base = frame->cleanup_base ? frame->cleanup_base : frame->slots;
                close_upvalues(vm, base);
                vm->frame_count--;
                if (vm->frame_count == base_frame) {
                    /* Returned to the entry frame of this vm_run invocation.
                     * Free remaining stack values down to our entry point. */
                    while (vm->stack_top > base) {
                        vm->stack_top--;
                        value_free(vm->stack_top);
                    }
                    *result = ret;
                    return VM_OK;
                }
                /* Free the callee and args/locals from this frame */
                while (vm->stack_top > base) {
                    vm->stack_top--;
                    value_free(vm->stack_top);
                }
                push(vm, ret);
                frame = &vm->frames[vm->frame_count - 1];
                break;
            }

            /* ── Iterators ── */
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_ITER_INIT:
#endif
            case OP_ITER_INIT: {
                /* TOS is a range, array, map, or set. Push index 0 on top. */
                LatValue *iter = vm_peek(vm, 0);
                if (iter->type == VAL_MAP || iter->type == VAL_SET) {
                    /* Convert map/set to array for iteration */
                    vm_iter_convert_to_array(iter);
                }
                if (iter->type != VAL_RANGE && iter->type != VAL_ARRAY) {
                    VM_ERROR("cannot iterate over %s", value_type_name(iter)); break;
                }
                push(vm, value_int(0)); /* index */
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_ITER_NEXT:
#endif
            case OP_ITER_NEXT: {
                uint16_t offset = READ_U16();
                LatValue *idx_val = vm_peek(vm, 0);  /* index */
                LatValue *iter = vm_peek(vm, 1);      /* collection */
                int64_t idx = idx_val->as.int_val;

                if (iter->type == VAL_RANGE) {
                    if (idx >= iter->as.range.end - iter->as.range.start) {
                        /* Exhausted */
                        frame->ip += offset;
                        break;
                    }
                    /* Push next value */
                    push(vm, value_int(iter->as.range.start + idx));
                    /* Increment index */
                    idx_val->as.int_val = idx + 1;
                } else if (iter->type == VAL_ARRAY) {
                    if ((size_t)idx >= iter->as.array.len) {
                        frame->ip += offset;
                        break;
                    }
                    push(vm, value_clone_fast(&iter->as.array.elems[idx]));
                    idx_val->as.int_val = idx + 1;
                } else {
                    frame->ip += offset;
                }
                break;
            }

            /* ── Data structures ── */
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_BUILD_ARRAY:
#endif
            case OP_BUILD_ARRAY: {
                uint8_t count = READ_BYTE();
                LatValue *elems = NULL;
                LatValue elem_buf[16];
                if (count > 0) {
                    elems = (count <= 16) ? elem_buf : malloc(count * sizeof(LatValue));
                    for (int i = count - 1; i >= 0; i--)
                        elems[i] = pop(vm);
                    for (int i = 0; i < count; i++)
                        vm_promote_value(&elems[i]);
                }
                LatValue arr = value_array(elems, count);
                if (count > 16) free(elems);
                push(vm, arr);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_ARRAY_FLATTEN:
#endif
            case OP_ARRAY_FLATTEN: {
                /* One-level flatten for spread operator: [1, [2, 3], 4] → [1, 2, 3, 4] */
                LatValue arr = pop(vm);
                if (arr.type != VAL_ARRAY) {
                    push(vm, arr);
                    break;
                }
                size_t total = 0;
                for (size_t i = 0; i < arr.as.array.len; i++) {
                    if (arr.as.array.elems[i].type == VAL_ARRAY)
                        total += arr.as.array.elems[i].as.array.len;
                    else
                        total++;
                }
                LatValue *flat = malloc(total * sizeof(LatValue));
                size_t pos = 0;
                for (size_t i = 0; i < arr.as.array.len; i++) {
                    if (arr.as.array.elems[i].type == VAL_ARRAY) {
                        LatValue *inner = arr.as.array.elems[i].as.array.elems;
                        for (size_t j = 0; j < arr.as.array.elems[i].as.array.len; j++)
                            flat[pos++] = value_deep_clone(&inner[j]);
                    } else {
                        flat[pos++] = value_deep_clone(&arr.as.array.elems[i]);
                    }
                }
                value_free(&arr);
                LatValue result_arr = value_array(flat, total);
                free(flat);
                push(vm, result_arr);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_BUILD_MAP:
#endif
            case OP_BUILD_MAP: {
                uint8_t pair_count = READ_BYTE();
                LatValue map = value_map_new();
                /* Pop pairs in reverse */
                LatValue *pairs = NULL;
                if (pair_count > 0) {
                    pairs = malloc(pair_count * 2 * sizeof(LatValue));
                    for (int i = pair_count * 2 - 1; i >= 0; i--)
                        pairs[i] = pop(vm);
                    for (uint8_t i = 0; i < pair_count; i++) {
                        LatValue key = pairs[i * 2];
                        LatValue val = pairs[i * 2 + 1];
                        vm_promote_value(&val);
                        if (key.type == VAL_STR) {
                            lat_map_set(map.as.map.map, key.as.str_val, &val);
                        }
                        value_free(&key);
                    }
                    free(pairs);
                }
                push(vm, map);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_BUILD_TUPLE:
#endif
            case OP_BUILD_TUPLE: {
                uint8_t count = READ_BYTE();
                LatValue *elems = NULL;
                LatValue elem_buf[16];
                if (count > 0) {
                    elems = (count <= 16) ? elem_buf : malloc(count * sizeof(LatValue));
                    for (int i = count - 1; i >= 0; i--)
                        elems[i] = pop(vm);
                    for (int i = 0; i < count; i++)
                        vm_promote_value(&elems[i]);
                }
                LatValue tup = value_tuple(elems, count);
                if (count > 16) free(elems);
                push(vm, tup);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_BUILD_STRUCT:
#endif
            case OP_BUILD_STRUCT: {
                uint8_t name_idx = READ_BYTE();
                uint8_t field_count = READ_BYTE();
                const char *struct_name = frame->chunk->constants[name_idx].as.str_val;

                /* Borrow field name pointers from constant pool (no strdup here) */
                const char *fn_buf[16];
                LatValue fv_buf[16];
                const char **field_names = (field_count <= 16) ? fn_buf : malloc(field_count * sizeof(const char *));
                LatValue *field_values = (field_count <= 16) ? fv_buf : malloc(field_count * sizeof(LatValue));

                size_t base_const = name_idx + 1;
                for (uint8_t i = 0; i < field_count; i++)
                    field_names[i] = frame->chunk->constants[base_const + i].as.str_val;

                /* Pop field values in reverse */
                for (int i = field_count - 1; i >= 0; i--)
                    field_values[i] = pop(vm);
                for (int i = 0; i < field_count; i++)
                    vm_promote_value(&field_values[i]);

                LatValue s = value_struct_vm(struct_name, field_names, field_values, field_count);

                /* Alloy enforcement: apply per-field phase from struct declaration */
                char phase_key[256];
                snprintf(phase_key, sizeof(phase_key), "__struct_phases_%s", struct_name);
                LatValue *phase_ref = env_get_ref(vm->env, phase_key);
                if (phase_ref &&
                    phase_ref->type == VAL_ARRAY && phase_ref->as.array.len == field_count) {
                    s.as.strct.field_phases = calloc(field_count, sizeof(PhaseTag));
                    for (uint8_t i = 0; i < field_count; i++) {
                        int64_t p = phase_ref->as.array.elems[i].as.int_val;
                        if (p == 1) { /* PHASE_CRYSTAL */
                            s.as.strct.field_values[i] = value_freeze(s.as.strct.field_values[i]);
                            s.as.strct.field_phases[i] = VTAG_CRYSTAL;
                        } else if (p == 0) { /* PHASE_FLUID */
                            s.as.strct.field_phases[i] = VTAG_FLUID;
                        } else {
                            s.as.strct.field_phases[i] = s.phase;
                        }
                    }
                }

                if (field_count > 16) { free(field_names); free(field_values); }
                push(vm, s);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_BUILD_RANGE:
#endif
            case OP_BUILD_RANGE: {
                LatValue end = pop(vm), start = pop(vm);
                if (start.type == VAL_INT && end.type == VAL_INT) {
                    push(vm, value_range(start.as.int_val, end.as.int_val));
                } else {
                    value_free(&start); value_free(&end);
                    VM_ERROR("range bounds must be integers"); break;
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_BUILD_ENUM:
#endif
            case OP_BUILD_ENUM: {
                uint8_t enum_idx = READ_BYTE();
                uint8_t var_idx = READ_BYTE();
                uint8_t payload_count = READ_BYTE();
                const char *enum_name = frame->chunk->constants[enum_idx].as.str_val;
                const char *variant_name = frame->chunk->constants[var_idx].as.str_val;

                LatValue *payload = NULL;
                if (payload_count > 0) {
                    payload = malloc(payload_count * sizeof(LatValue));
                    for (int i = payload_count - 1; i >= 0; i--)
                        payload[i] = pop(vm);
                }
                LatValue e = value_enum(enum_name, variant_name, payload, payload_count);
                free(payload);
                push(vm, e);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_INDEX:
#endif
            case OP_INDEX: {
                LatValue idx = pop(vm);
                LatValue obj = pop(vm);
                /* Ref proxy: delegate indexing to inner value */
                if (obj.type == VAL_REF) {
                    LatValue *inner = &obj.as.ref.ref->value;
                    if (inner->type == VAL_ARRAY && idx.type == VAL_INT) {
                        int64_t i = idx.as.int_val;
                        if (i < 0 || (size_t)i >= inner->as.array.len) {
                            value_free(&obj);
                            VM_ERROR("array index out of bounds: %lld (len %zu)",
                                     (long long)i, inner->as.array.len); break;
                        }
                        LatValue elem = value_deep_clone(&inner->as.array.elems[i]);
                        value_free(&obj);
                        push(vm, elem);
                        break;
                    }
                    if (inner->type == VAL_MAP && idx.type == VAL_STR) {
                        LatValue *found = lat_map_get(inner->as.map.map, idx.as.str_val);
                        if (found)
                            push(vm, value_deep_clone(found));
                        else
                            push(vm, value_nil());
                        value_free(&obj);
                        value_free(&idx);
                        break;
                    }
                    const char *it = value_type_name(&idx);
                    const char *innert = value_type_name(inner);
                    value_free(&obj); value_free(&idx);
                    VM_ERROR("invalid index operation: Ref<%s>[%s]", innert, it); break;
                }
                if (obj.type == VAL_ARRAY && idx.type == VAL_INT) {
                    int64_t i = idx.as.int_val;
                    if (i < 0 || (size_t)i >= obj.as.array.len) {
                        value_free(&obj);
                        VM_ERROR("array index out of bounds: %lld (len %zu)",
                                 (long long)i, obj.as.array.len); break;
                    }
                    LatValue elem = value_deep_clone(&obj.as.array.elems[i]);
                    value_free(&obj);
                    push(vm, elem);
                } else if (obj.type == VAL_MAP && idx.type == VAL_STR) {
                    LatValue *found = lat_map_get(obj.as.map.map, idx.as.str_val);
                    if (found)
                        push(vm, value_deep_clone(found));
                    else
                        push(vm, value_nil());
                    value_free(&obj);
                    value_free(&idx);
                } else if (obj.type == VAL_STR && idx.type == VAL_INT) {
                    int64_t i = idx.as.int_val;
                    size_t len = strlen(obj.as.str_val);
                    if (i < 0 || (size_t)i >= len) {
                        value_free(&obj);
                        VM_ERROR("string index out of bounds"); break;
                    }
                    char ch[2] = { obj.as.str_val[i], '\0' };
                    value_free(&obj);
                    push(vm, value_string(ch));
                } else if (obj.type == VAL_TUPLE && idx.type == VAL_INT) {
                    int64_t i = idx.as.int_val;
                    if (i < 0 || (size_t)i >= obj.as.tuple.len) {
                        value_free(&obj);
                        VM_ERROR("tuple index out of bounds"); break;
                    }
                    LatValue elem = value_deep_clone(&obj.as.tuple.elems[i]);
                    value_free(&obj);
                    push(vm, elem);
                } else if (obj.type == VAL_STR && idx.type == VAL_RANGE) {
                    /* String range slicing: "hello"[1..4] → "ell" */
                    int64_t start = idx.as.range.start;
                    int64_t end = idx.as.range.end;
                    size_t len = strlen(obj.as.str_val);
                    if (start < 0) start = 0;
                    if (end < 0) end = 0;
                    if ((size_t)start > len) start = (int64_t)len;
                    if ((size_t)end > len) end = (int64_t)len;
                    if (start >= end) {
                        value_free(&obj);
                        push(vm, value_string(""));
                    } else {
                        size_t slice_len = (size_t)(end - start);
                        char *slice = malloc(slice_len + 1);
                        memcpy(slice, obj.as.str_val + start, slice_len);
                        slice[slice_len] = '\0';
                        value_free(&obj);
                        push(vm, value_string_owned(slice));
                    }
                } else if (obj.type == VAL_ARRAY && idx.type == VAL_RANGE) {
                    /* Array range slicing: [1,2,3,4][1..3] → [2,3] */
                    int64_t start = idx.as.range.start;
                    int64_t end = idx.as.range.end;
                    size_t len = obj.as.array.len;
                    if (start < 0) start = 0;
                    if ((size_t)start > len) start = (int64_t)len;
                    if (end < 0) end = 0;
                    if ((size_t)end > len) end = (int64_t)len;
                    if (start >= end) {
                        value_free(&obj);
                        push(vm, value_array(NULL, 0));
                    } else {
                        size_t slice_len = (size_t)(end - start);
                        LatValue *elems = malloc(slice_len * sizeof(LatValue));
                        for (size_t i = 0; i < slice_len; i++)
                            elems[i] = value_deep_clone(&obj.as.array.elems[start + i]);
                        value_free(&obj);
                        push(vm, value_array(elems, slice_len));
                        free(elems);
                    }
                } else if (obj.type == VAL_BUFFER && idx.type == VAL_INT) {
                    int64_t i = idx.as.int_val;
                    if (i < 0 || (size_t)i >= obj.as.buffer.len) {
                        value_free(&obj);
                        VM_ERROR("buffer index out of bounds: %lld (len %zu)",
                                 (long long)i, obj.as.buffer.len); break;
                    }
                    LatValue elem = value_int(obj.as.buffer.data[i]);
                    value_free(&obj);
                    push(vm, elem);
                } else {
                    const char *ot = value_type_name(&obj);
                    const char *it = value_type_name(&idx);
                    value_free(&obj); value_free(&idx);
                    VM_ERROR("invalid index operation: %s[%s]", ot, it); break;
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_SET_INDEX:
#endif
            case OP_SET_INDEX: {
                LatValue idx = pop(vm);
                LatValue obj = pop(vm);
                LatValue val = pop(vm);
                /* Ref proxy: delegate set-index to inner value */
                if (obj.type == VAL_REF) {
                    if (obj.phase == VTAG_CRYSTAL) {
                        value_free(&obj); value_free(&idx); value_free(&val);
                        VM_ERROR("cannot assign index on a frozen Ref"); break;
                    }
                    LatValue *inner = &obj.as.ref.ref->value;
                    if (inner->type == VAL_ARRAY && idx.type == VAL_INT) {
                        int64_t i = idx.as.int_val;
                        if (i < 0 || (size_t)i >= inner->as.array.len) {
                            value_free(&obj); value_free(&val);
                            VM_ERROR("array index out of bounds in assignment"); break;
                        }
                        value_free(&inner->as.array.elems[i]);
                        inner->as.array.elems[i] = val;
                        push(vm, obj);
                        break;
                    }
                    if (inner->type == VAL_MAP && idx.type == VAL_STR) {
                        LatValue *old = (LatValue *)lat_map_get(inner->as.map.map, idx.as.str_val);
                        if (old) value_free(old);
                        lat_map_set(inner->as.map.map, idx.as.str_val, &val);
                        value_free(&idx);
                        push(vm, obj);
                        break;
                    }
                    value_free(&obj); value_free(&idx); value_free(&val);
                    VM_ERROR("invalid index assignment on Ref"); break;
                }
                if (obj.type == VAL_ARRAY && idx.type == VAL_INT) {
                    int64_t i = idx.as.int_val;
                    if (i < 0 || (size_t)i >= obj.as.array.len) {
                        value_free(&obj); value_free(&val);
                        VM_ERROR("array index out of bounds in assignment"); break;
                    }
                    value_free(&obj.as.array.elems[i]);
                    obj.as.array.elems[i] = val;
                    push(vm, obj);
                } else if (obj.type == VAL_MAP && idx.type == VAL_STR) {
                    lat_map_set(obj.as.map.map, idx.as.str_val, &val);
                    value_free(&idx);
                    push(vm, obj);
                } else if (obj.type == VAL_BUFFER && idx.type == VAL_INT) {
                    int64_t i = idx.as.int_val;
                    if (i < 0 || (size_t)i >= obj.as.buffer.len) {
                        value_free(&obj); value_free(&val);
                        VM_ERROR("buffer index out of bounds in assignment"); break;
                    }
                    obj.as.buffer.data[i] = (uint8_t)(val.as.int_val & 0xFF);
                    value_free(&val);
                    push(vm, obj);
                } else {
                    value_free(&obj); value_free(&idx); value_free(&val);
                    VM_ERROR("invalid index assignment"); break;
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_GET_FIELD:
#endif
            case OP_GET_FIELD: {
                uint8_t name_idx = READ_BYTE();
                const char *field_name = frame->chunk->constants[name_idx].as.str_val;
                LatValue obj = pop(vm);

                if (obj.type == VAL_STRUCT) {
                    bool found = false;
                    for (size_t i = 0; i < obj.as.strct.field_count; i++) {
                        if (strcmp(obj.as.strct.field_names[i], field_name) == 0) {
                            /* Steal the value from the dying struct */
                            LatValue stolen = obj.as.strct.field_values[i];
                            obj.as.strct.field_values[i] = (LatValue){.type = VAL_NIL};
                            push(vm, stolen);
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        value_free(&obj);
                        VM_ERROR("struct has no field '%s'", field_name); break;
                    }
                    value_free(&obj);
                } else if (obj.type == VAL_MAP) {
                    LatValue *val = lat_map_get(obj.as.map.map, field_name);
                    if (val) {
                        /* Steal the value from the dying map */
                        LatValue stolen = *val;
                        val->type = VAL_NIL;
                        push(vm, stolen);
                    } else {
                        push(vm, value_nil());
                    }
                    value_free(&obj);
                } else if (obj.type == VAL_TUPLE) {
                    /* Tuple field access: t.0, t.1, etc. */
                    char *end;
                    long idx = strtol(field_name, &end, 10);
                    if (*end == '\0' && idx >= 0 && (size_t)idx < obj.as.tuple.len) {
                        LatValue stolen = obj.as.tuple.elems[idx];
                        obj.as.tuple.elems[idx] = (LatValue){.type = VAL_NIL};
                        push(vm, stolen);
                    } else {
                        value_free(&obj);
                        VM_ERROR("tuple has no field '%s'", field_name); break;
                    }
                    value_free(&obj);
                } else if (obj.type == VAL_ENUM) {
                    if (strcmp(field_name, "tag") == 0) {
                        push(vm, value_string(obj.as.enm.variant_name));
                    } else if (strcmp(field_name, "payload") == 0) {
                        /* Always return an array of all payloads */
                        if (obj.as.enm.payload_count > 0) {
                            LatValue *elems = malloc(obj.as.enm.payload_count * sizeof(LatValue));
                            for (size_t i = 0; i < obj.as.enm.payload_count; i++) {
                                elems[i] = obj.as.enm.payload[i];
                                obj.as.enm.payload[i] = (LatValue){.type = VAL_NIL};
                            }
                            push(vm, value_array(elems, obj.as.enm.payload_count));
                            free(elems);
                        } else {
                            push(vm, value_array(NULL, 0));
                        }
                    } else {
                        push(vm, value_nil());
                    }
                    value_free(&obj);
                } else {
                    const char *tn = value_type_name(&obj);
                    value_free(&obj);
                    VM_ERROR("cannot access field '%s' on %s", field_name, tn); break;
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_SET_FIELD:
#endif
            case OP_SET_FIELD: {
                uint8_t name_idx = READ_BYTE();
                const char *field_name = frame->chunk->constants[name_idx].as.str_val;
                LatValue obj = pop(vm);
                LatValue val = pop(vm);
                vm_promote_value(&val);

                if (obj.type == VAL_STRUCT) {
                    /* Check overall struct phase (only if no per-field phases set) */
                    if ((obj.phase == VTAG_CRYSTAL || obj.phase == VTAG_SUBLIMATED) &&
                        !obj.as.strct.field_phases) {
                        value_free(&obj); value_free(&val);
                        VM_ERROR("cannot assign to field '%s' on a %s struct", field_name,
                            obj.phase == VTAG_CRYSTAL ? "frozen" : "sublimated"); break;
                    }
                    /* Check per-field phase constraints (alloy types) */
                    bool field_frozen = false;
                    if (obj.as.strct.field_phases) {
                        for (size_t i = 0; i < obj.as.strct.field_count; i++) {
                            if (strcmp(obj.as.strct.field_names[i], field_name) == 0) {
                                if (obj.as.strct.field_phases[i] == VTAG_CRYSTAL)
                                    field_frozen = true;
                                break;
                            }
                        }
                    }
                    if (field_frozen) {
                        value_free(&obj); value_free(&val);
                        VM_ERROR("cannot assign to frozen field '%s'", field_name); break;
                    }
                    bool found = false;
                    for (size_t i = 0; i < obj.as.strct.field_count; i++) {
                        if (strcmp(obj.as.strct.field_names[i], field_name) == 0) {
                            value_free(&obj.as.strct.field_values[i]);
                            obj.as.strct.field_values[i] = val;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        value_free(&obj); value_free(&val);
                        VM_ERROR("struct has no field '%s'", field_name); break;
                    }
                    push(vm, obj);
                } else if (obj.type == VAL_MAP) {
                    lat_map_set(obj.as.map.map, field_name, &val);
                    push(vm, obj);
                } else {
                    value_free(&obj); value_free(&val);
                    VM_ERROR("cannot set field on non-struct/map value"); break;
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_INVOKE:
#endif
            case OP_INVOKE: {
                uint8_t method_idx = READ_BYTE();
                uint8_t arg_count = READ_BYTE();
                const char *method_name = frame->chunk->constants[method_idx].as.str_val;

                /* Object is below args on the stack */
                LatValue *obj = vm_peek(vm, arg_count);

                if (vm_invoke_builtin(vm, obj, method_name, arg_count, NULL)) {
                    if (vm->error) {
                        VMResult r = vm_handle_native_error(vm, &frame);
                        if (r != VM_OK) return r;
                        break;
                    }
                    /* Builtin handled it. Pop the object and replace with result. */
                    LatValue result_val = pop(vm); /* pop result pushed by builtin */
                    /* Pop the object */
                    /* The object is at stack_top[-1] now (or at the right place). Actually...
                     * We need to be careful here. The args were popped by builtin if needed.
                     * The object is still on the stack. Let's clean up. */
                    LatValue obj_val = pop(vm);
                    value_free(&obj_val);
                    push(vm, result_val);
                } else {
                    /* Check if map/struct has a callable closure field */
                    if (obj->type == VAL_MAP) {
                        LatValue *field = lat_map_get(obj->as.map.map, method_name);
                        if (field && field->type == VAL_CLOSURE && field->as.closure.native_fn &&
                            field->as.closure.default_values != VM_NATIVE_MARKER) {
                            /* Bytecode closure stored in map - call it */
                            Chunk *fn_chunk = (Chunk *)field->as.closure.native_fn;
                            int arity = (int)field->as.closure.param_count;
                            int adjusted = vm_adjust_call_args(vm, fn_chunk, arity, (int)arg_count);
                            if (adjusted < 0) {
                                char *err = vm->error; vm->error = NULL;
                                VM_ERROR("%s", err); free(err); break;
                            }
                            (void)adjusted;
                            ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                            size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;
                            if (vm->frame_count >= VM_FRAMES_MAX) {
                                VM_ERROR("stack overflow (too many nested calls)"); break;
                            }
                            vm_promote_frame_ephemerals(vm, frame);
                            /* Replace the map on the stack with a closure placeholder
                             * so OP_RETURN can clean up properly. */
                            LatValue closure_copy = value_deep_clone(field);
                            value_free(obj);
                            *obj = closure_copy;
                            CallFrame *new_frame = &vm->frames[vm->frame_count++];
                            new_frame->chunk = fn_chunk;
                            new_frame->ip = fn_chunk->code;
                            new_frame->slots = obj;
                            new_frame->upvalues = upvals;
                            new_frame->upvalue_count = uv_count;
                            frame = new_frame;
                            break;
                        }
                        if (field && field->type == VAL_CLOSURE &&
                            field->as.closure.default_values == VM_NATIVE_MARKER) {
                            /* VM native function stored in map */
                            VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                            LatValue *args = (arg_count <= 16) ? vm->fast_args
                                           : malloc(arg_count * sizeof(LatValue));
                            for (int i = arg_count - 1; i >= 0; i--)
                                args[i] = pop(vm);
                            LatValue obj_val = pop(vm);
                            LatValue ret = native(args, arg_count);
                            for (int i = 0; i < arg_count; i++)
                                value_free(&args[i]);
                            if (arg_count > 16) free(args);
                            value_free(&obj_val);
                            push(vm, ret);
                            break;
                        }
                    }

                    /* Check if struct has a callable closure field */
                    if (obj->type == VAL_STRUCT) {
                        bool handled = false;
                        for (size_t fi = 0; fi < obj->as.strct.field_count; fi++) {
                            if (strcmp(obj->as.strct.field_names[fi], method_name) != 0)
                                continue;
                            LatValue *field = &obj->as.strct.field_values[fi];
                            if (field->type == VAL_CLOSURE && field->as.closure.native_fn &&
                                field->as.closure.default_values != VM_NATIVE_MARKER) {
                                /* Bytecode closure in struct field — inject self */
                                Chunk *fn_chunk = (Chunk *)field->as.closure.native_fn;
                                ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                                size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;
                                if (vm->frame_count >= VM_FRAMES_MAX) {
                                    VM_ERROR("stack overflow (too many nested calls)"); break;
                                }
                                vm_promote_frame_ephemerals(vm, frame);
                                LatValue self_copy = value_deep_clone(obj);
                                LatValue closure_copy = value_deep_clone(field);
                                /* Shift args up by 1 to make room for self */
                                push(vm, value_nil());
                                for (int si = arg_count; si >= 1; si--)
                                    obj[si + 1] = obj[si];
                                obj[1] = self_copy;
                                value_free(obj);
                                *obj = closure_copy;
                                CallFrame *new_frame = &vm->frames[vm->frame_count++];
                                new_frame->chunk = fn_chunk;
                                new_frame->ip = fn_chunk->code;
                                new_frame->slots = obj;
                                new_frame->upvalues = upvals;
                                new_frame->upvalue_count = uv_count;
                                frame = new_frame;
                                handled = true;
                                break;
                            }
                            if (field->type == VAL_CLOSURE &&
                                field->as.closure.default_values == VM_NATIVE_MARKER) {
                                /* VM native in struct field — inject self */
                                VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                                LatValue self_copy = value_deep_clone(obj);
                                int total_args = arg_count + 1;
                                LatValue *args = malloc(total_args * sizeof(LatValue));
                                args[0] = self_copy;
                                for (int ai = arg_count - 1; ai >= 0; ai--)
                                    args[ai + 1] = pop(vm);
                                LatValue obj_val = pop(vm);
                                LatValue ret = native(args, total_args);
                                for (int ai = 0; ai < total_args; ai++)
                                    value_free(&args[ai]);
                                free(args);
                                value_free(&obj_val);
                                push(vm, ret);
                                handled = true;
                                break;
                            }
                            break; /* found field but not callable */
                        }
                        if (handled) break;
                    }

                    /* Try to find it as a compiled method via "TypeName::method" global */
                    const char *type_name = (obj->type == VAL_STRUCT) ? obj->as.strct.name :
                                            (obj->type == VAL_ENUM)   ? obj->as.enm.enum_name :
                                            value_type_name(obj);
                    char key[256];
                    snprintf(key, sizeof(key), "%s::%s", type_name, method_name);
                    LatValue *method_ref = env_get_ref(vm->env, key);
                    if (method_ref &&
                        method_ref->type == VAL_CLOSURE && method_ref->as.closure.native_fn) {
                        /* Found a compiled method - call it with self + args */
                        Chunk *fn_chunk = (Chunk *)method_ref->as.closure.native_fn;
                        /* Rearrange stack: obj is already below args, use as slot 0 */
                        if (vm->frame_count >= VM_FRAMES_MAX) {
                            VM_ERROR("stack overflow (too many nested calls)"); break;
                        }
                        vm_promote_frame_ephemerals(vm, frame);
                        CallFrame *new_frame = &vm->frames[vm->frame_count++];
                        new_frame->chunk = fn_chunk;
                        new_frame->ip = fn_chunk->code;
                        new_frame->slots = obj; /* self is in slot 0 */
                        new_frame->upvalues = NULL;
                        new_frame->upvalue_count = 0;
                        frame = new_frame;
                    } else {
                        /* Method not found - error */
                        const char *tname = value_type_name(obj);
                        for (int i = 0; i < arg_count; i++) {
                            LatValue v = pop(vm);
                            value_free(&v);
                        }
                        LatValue obj_val = pop(vm);
                        value_free(&obj_val);
                        VM_ERROR("type '%s' has no method '%s'", tname, method_name);
                        break;
                    }
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_INVOKE_LOCAL:
#endif
            case OP_INVOKE_LOCAL: {
                uint8_t slot = READ_BYTE();
                uint8_t method_idx = READ_BYTE();
                uint8_t arg_count = READ_BYTE();
                const char *method_name = frame->chunk->constants[method_idx].as.str_val;
                LatValue *obj = &frame->slots[slot]; /* Direct pointer to local */

                const char *local_var_name = (frame->chunk->local_names && slot < frame->chunk->local_name_cap)
                    ? frame->chunk->local_names[slot] : NULL;
                if (vm_invoke_builtin(vm, obj, method_name, arg_count, local_var_name)) {
                    if (vm->error) {
                        VMResult r = vm_handle_native_error(vm, &frame);
                        if (r != VM_OK) return r;
                        break;
                    }
                    /* Builtin popped args and pushed result.
                     * obj was mutated in-place (e.g. push modified the array). */
                    break;
                }

                /* Check if map has a callable closure field */
                if (obj->type == VAL_MAP) {
                    LatValue *field = lat_map_get(obj->as.map.map, method_name);
                    if (field && field->type == VAL_CLOSURE && field->as.closure.native_fn &&
                        field->as.closure.default_values != VM_NATIVE_MARKER) {
                        /* Bytecode closure stored in local map - call it */
                        Chunk *fn_chunk = (Chunk *)field->as.closure.native_fn;
                        int arity = (int)field->as.closure.param_count;
                        int adjusted = vm_adjust_call_args(vm, fn_chunk, arity, (int)arg_count);
                        if (adjusted < 0) {
                            char *err = vm->error; vm->error = NULL;
                            VM_ERROR("%s", err); free(err); break;
                        }
                        arg_count = (uint8_t)adjusted;
                        ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                        size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;
                        if (vm->frame_count >= VM_FRAMES_MAX) {
                            VM_ERROR("stack overflow (too many nested calls)"); break;
                        }
                        vm_promote_frame_ephemerals(vm, frame);
                        /* Set up frame: closure in slot 0, args follow */
                        LatValue closure_copy = value_deep_clone(field);
                        LatValue *arg_base = vm->stack_top - arg_count;
                        push(vm, value_nil()); /* make room */
                        for (int si = arg_count - 1; si >= 0; si--)
                            arg_base[si + 1] = arg_base[si];
                        arg_base[0] = closure_copy;
                        CallFrame *new_frame = &vm->frames[vm->frame_count++];
                        new_frame->chunk = fn_chunk;
                        new_frame->ip = fn_chunk->code;
                        new_frame->slots = arg_base;
                        new_frame->upvalues = upvals;
                        new_frame->upvalue_count = uv_count;
                        frame = new_frame;
                        break;
                    }
                    if (field && field->type == VAL_CLOSURE &&
                        field->as.closure.default_values == VM_NATIVE_MARKER) {
                        /* VM native function stored in local map */
                        VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                        LatValue *args = (arg_count <= 16) ? vm->fast_args
                                       : malloc(arg_count * sizeof(LatValue));
                        for (int i = arg_count - 1; i >= 0; i--)
                            args[i] = pop(vm);
                        LatValue ret = native(args, arg_count);
                        for (int i = 0; i < arg_count; i++)
                            value_free(&args[i]);
                        if (arg_count > 16) free(args);
                        if (vm->error) {
                            value_free(&ret);
                            char *err = vm->error; vm->error = NULL;
                            VM_ERROR("%s", err); free(err); break;
                        }
                        push(vm, ret);
                        break;
                    }
                }

                /* Check if struct has a callable closure field */
                if (obj->type == VAL_STRUCT) {
                    bool handled = false;
                    for (size_t fi = 0; fi < obj->as.strct.field_count; fi++) {
                        if (strcmp(obj->as.strct.field_names[fi], method_name) != 0)
                            continue;
                        LatValue *field = &obj->as.strct.field_values[fi];
                        if (field->type == VAL_CLOSURE && field->as.closure.native_fn &&
                            field->as.closure.default_values != VM_NATIVE_MARKER) {
                            /* Bytecode closure — inject [closure, self] below args */
                            Chunk *fn_chunk = (Chunk *)field->as.closure.native_fn;
                            ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                            size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;
                            if (vm->frame_count >= VM_FRAMES_MAX) {
                                VM_ERROR("stack overflow (too many nested calls)"); break;
                            }
                            vm_promote_frame_ephemerals(vm, frame);
                            LatValue self_copy = value_deep_clone(obj);
                            LatValue closure_copy = value_deep_clone(field);
                            LatValue *arg_base = vm->stack_top - arg_count;
                            push(vm, value_nil()); push(vm, value_nil());
                            for (int si = arg_count - 1; si >= 0; si--)
                                arg_base[si + 2] = arg_base[si];
                            arg_base[0] = closure_copy;
                            arg_base[1] = self_copy;
                            CallFrame *new_frame = &vm->frames[vm->frame_count++];
                            new_frame->chunk = fn_chunk;
                            new_frame->ip = fn_chunk->code;
                            new_frame->slots = arg_base;
                            new_frame->upvalues = upvals;
                            new_frame->upvalue_count = uv_count;
                            frame = new_frame;
                            handled = true;
                            break;
                        }
                        if (field->type == VAL_CLOSURE &&
                            field->as.closure.default_values == VM_NATIVE_MARKER) {
                            /* VM native — inject self */
                            VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                            LatValue self_copy = value_deep_clone(obj);
                            int total_args = arg_count + 1;
                            LatValue *args = malloc(total_args * sizeof(LatValue));
                            args[0] = self_copy;
                            for (int ai = arg_count - 1; ai >= 0; ai--)
                                args[ai + 1] = pop(vm);
                            LatValue ret = native(args, total_args);
                            for (int ai = 0; ai < total_args; ai++)
                                value_free(&args[ai]);
                            free(args);
                            push(vm, ret);
                            handled = true;
                            break;
                        }
                        break;
                    }
                    if (handled) break;
                }

                {
                    /* Try compiled method via "TypeName::method" global */
                    const char *type_name = (obj->type == VAL_STRUCT) ? obj->as.strct.name :
                                            (obj->type == VAL_ENUM)   ? obj->as.enm.enum_name :
                                            value_type_name(obj);
                    char key[256];
                    snprintf(key, sizeof(key), "%s::%s", type_name, method_name);
                    LatValue *method_ref = env_get_ref(vm->env, key);
                    if (method_ref &&
                        method_ref->type == VAL_CLOSURE && method_ref->as.closure.native_fn) {
                        Chunk *fn_chunk = (Chunk *)method_ref->as.closure.native_fn;
                        if (vm->frame_count >= VM_FRAMES_MAX) {
                            VM_ERROR("stack overflow (too many nested calls)"); break;
                        }
                        vm_promote_frame_ephemerals(vm, frame);
                        /* Push self (deep clone of local) below args for the new frame. */
                        LatValue *arg_base = vm->stack_top - arg_count;
                        push(vm, value_nil());
                        for (int i = arg_count; i > 0; i--)
                            vm->stack_top[-1 - (arg_count - i)] = arg_base[i - 1];
                        *arg_base = value_deep_clone(obj);
                        CallFrame *new_frame = &vm->frames[vm->frame_count++];
                        new_frame->chunk = fn_chunk;
                        new_frame->ip = fn_chunk->code;
                        new_frame->slots = arg_base;
                        new_frame->upvalues = NULL;
                        new_frame->upvalue_count = 0;
                        frame = new_frame;
                    } else {
                        /* Method not found - pop args, push nil */
                        for (int i = 0; i < arg_count; i++) {
                            LatValue v = pop(vm);
                            value_free(&v);
                        }
                        push(vm, value_nil());
                    }
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_INVOKE_GLOBAL:
#endif
            case OP_INVOKE_GLOBAL: {
                uint8_t name_idx = READ_BYTE();
                uint8_t method_idx = READ_BYTE();
                uint8_t arg_count = READ_BYTE();
                const char *global_name = frame->chunk->constants[name_idx].as.str_val;
                const char *method_name = frame->chunk->constants[method_idx].as.str_val;

                /* Fast path: simple builtins (no closures) can mutate in place */
                uint32_t mhash_g = method_hash(method_name);
                if (vm_invoke_builtin_is_simple(mhash_g)) {
                    LatValue *ref = env_get_ref(vm->env, global_name);
                    if (!ref) {
                        VM_ERROR("undefined variable '%s'", global_name); break;
                    }
                    if (vm_invoke_builtin(vm, ref, method_name, arg_count, global_name)) {
                        if (vm->error) {
                            VMResult r = vm_handle_native_error(vm, &frame);
                            if (r != VM_OK) return r;
                            break;
                        }
                        /* Record history for tracked variables */
                        if (vm->tracking_active) {
                            vm_record_history(vm, global_name, ref);
                        }
                        break;
                    }
                }

                /* Slow path: closure-invoking builtins or non-builtin dispatch */
                LatValue obj_val;
                if (!env_get(vm->env, global_name, &obj_val)) {
                    VM_ERROR("undefined variable '%s'", global_name); break;
                }

                if (vm_invoke_builtin(vm, &obj_val, method_name, arg_count, global_name)) {
                    if (vm->error) {
                        value_free(&obj_val);
                        VMResult r = vm_handle_native_error(vm, &frame);
                        if (r != VM_OK) return r;
                        break;
                    }
                    /* Write back the mutated object to the global env */
                    env_set(vm->env, global_name, obj_val);
                    /* Record history for tracked variables */
                    if (vm->tracking_active) {
                        LatValue cur;
                        if (env_get(vm->env, global_name, &cur)) {
                            vm_record_history(vm, global_name, &cur);
                            value_free(&cur);
                        }
                    }
                    break;
                }

                /* Not a builtin — insert object below args on stack and
                 * dispatch like OP_INVOKE (struct closures, impl methods, etc.) */
                push(vm, value_nil()); /* make room */
                LatValue *base = vm->stack_top - arg_count - 1;
                memmove(base + 1, base, arg_count * sizeof(LatValue));
                *base = obj_val;
                LatValue *obj = base;

                /* Check if map/struct has a callable closure field */
                if (obj->type == VAL_MAP) {
                    LatValue *field = lat_map_get(obj->as.map.map, method_name);
                    if (field && field->type == VAL_CLOSURE && field->as.closure.native_fn &&
                        field->as.closure.default_values != VM_NATIVE_MARKER) {
                        Chunk *fn_chunk = (Chunk *)field->as.closure.native_fn;
                        int arity = (int)field->as.closure.param_count;
                        int adjusted = vm_adjust_call_args(vm, fn_chunk, arity, (int)arg_count);
                        if (adjusted < 0) {
                            char *err = vm->error; vm->error = NULL;
                            value_free(&obj_val);
                            VM_ERROR("%s", err); free(err); break;
                        }
                        arg_count = (uint8_t)adjusted;
                        /* Recalculate obj pointer — vm_adjust may have pushed defaults */
                        obj = vm->stack_top - arg_count - 1;
                        ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                        size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;
                        if (vm->frame_count >= VM_FRAMES_MAX) {
                            VM_ERROR("stack overflow (too many nested calls)"); break;
                        }
                        vm_promote_frame_ephemerals(vm, frame);
                        LatValue closure_copy = value_deep_clone(field);
                        value_free(obj);
                        *obj = closure_copy;
                        CallFrame *new_frame = &vm->frames[vm->frame_count++];
                        new_frame->chunk = fn_chunk;
                        new_frame->ip = fn_chunk->code;
                        new_frame->slots = obj;
                        new_frame->upvalues = upvals;
                        new_frame->upvalue_count = uv_count;
                        frame = new_frame;
                        break;
                    }
                    if (field && field->type == VAL_CLOSURE &&
                        field->as.closure.default_values == VM_NATIVE_MARKER) {
                        VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                        LatValue *args = (arg_count <= 16) ? vm->fast_args
                                       : malloc(arg_count * sizeof(LatValue));
                        for (int i = arg_count - 1; i >= 0; i--)
                            args[i] = pop(vm);
                        LatValue obj_popped = pop(vm);
                        LatValue ret = native(args, arg_count);
                        for (int i = 0; i < arg_count; i++)
                            value_free(&args[i]);
                        if (arg_count > 16) free(args);
                        value_free(&obj_popped);
                        push(vm, ret);
                        break;
                    }
                }

                if (obj->type == VAL_STRUCT) {
                    bool handled = false;
                    for (size_t fi = 0; fi < obj->as.strct.field_count; fi++) {
                        if (strcmp(obj->as.strct.field_names[fi], method_name) != 0)
                            continue;
                        LatValue *field = &obj->as.strct.field_values[fi];
                        if (field->type == VAL_CLOSURE && field->as.closure.native_fn &&
                            field->as.closure.default_values != VM_NATIVE_MARKER) {
                            Chunk *fn_chunk = (Chunk *)field->as.closure.native_fn;
                            ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                            size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;
                            if (vm->frame_count >= VM_FRAMES_MAX) {
                                VM_ERROR("stack overflow (too many nested calls)"); break;
                            }
                            vm_promote_frame_ephemerals(vm, frame);
                            LatValue self_copy = value_deep_clone(obj);
                            LatValue closure_copy = value_deep_clone(field);
                            push(vm, value_nil());
                            for (int si = arg_count; si >= 1; si--)
                                obj[si + 1] = obj[si];
                            obj[1] = self_copy;
                            value_free(obj);
                            *obj = closure_copy;
                            CallFrame *new_frame = &vm->frames[vm->frame_count++];
                            new_frame->chunk = fn_chunk;
                            new_frame->ip = fn_chunk->code;
                            new_frame->slots = obj;
                            new_frame->upvalues = upvals;
                            new_frame->upvalue_count = uv_count;
                            frame = new_frame;
                            handled = true;
                            break;
                        }
                        if (field->type == VAL_CLOSURE &&
                            field->as.closure.default_values == VM_NATIVE_MARKER) {
                            VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                            LatValue self_copy = value_deep_clone(obj);
                            int total_args = arg_count + 1;
                            LatValue *args = malloc(total_args * sizeof(LatValue));
                            args[0] = self_copy;
                            for (int ai = arg_count - 1; ai >= 0; ai--)
                                args[ai + 1] = pop(vm);
                            LatValue obj_popped = pop(vm);
                            LatValue ret = native(args, total_args);
                            for (int ai = 0; ai < total_args; ai++)
                                value_free(&args[ai]);
                            free(args);
                            value_free(&obj_popped);
                            push(vm, ret);
                            handled = true;
                            break;
                        }
                        break;
                    }
                    if (handled) break;
                }

                /* Try compiled method via "TypeName::method" global */
                {
                    const char *type_name = (obj->type == VAL_STRUCT) ? obj->as.strct.name :
                                            (obj->type == VAL_ENUM)   ? obj->as.enm.enum_name :
                                            value_type_name(obj);
                    char key[256];
                    snprintf(key, sizeof(key), "%s::%s", type_name, method_name);
                    LatValue *method_ref = env_get_ref(vm->env, key);
                    if (method_ref &&
                        method_ref->type == VAL_CLOSURE && method_ref->as.closure.native_fn) {
                        Chunk *fn_chunk = (Chunk *)method_ref->as.closure.native_fn;
                        if (vm->frame_count >= VM_FRAMES_MAX) {
                            VM_ERROR("stack overflow (too many nested calls)"); break;
                        }
                        vm_promote_frame_ephemerals(vm, frame);
                        /* Replace obj with self clone, shift args */
                        LatValue self_copy = value_deep_clone(obj);
                        value_free(obj);
                        *obj = self_copy;
                        CallFrame *new_frame = &vm->frames[vm->frame_count++];
                        new_frame->chunk = fn_chunk;
                        new_frame->ip = fn_chunk->code;
                        new_frame->slots = obj;
                        new_frame->upvalues = NULL;
                        new_frame->upvalue_count = 0;
                        frame = new_frame;
                    } else {
                        for (int i = 0; i < arg_count; i++) {
                            LatValue v = pop(vm);
                            value_free(&v);
                        }
                        LatValue obj_popped = pop(vm);
                        value_free(&obj_popped);
                        push(vm, value_nil());
                    }
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_SET_INDEX_LOCAL:
#endif
            case OP_SET_INDEX_LOCAL: {
                uint8_t slot = READ_BYTE();
                LatValue idx = pop(vm);
                LatValue val = pop(vm);
                vm_promote_value(&val);
                LatValue *obj = &frame->slots[slot]; /* Direct pointer to local */

                /* Ref proxy: delegate set-index to inner value */
                if (obj->type == VAL_REF) {
                    if (obj->phase == VTAG_CRYSTAL) {
                        value_free(&val);
                        VM_ERROR("cannot assign index on a frozen Ref"); break;
                    }
                    LatValue *inner = &obj->as.ref.ref->value;
                    if (inner->type == VAL_ARRAY && idx.type == VAL_INT) {
                        int64_t i = idx.as.int_val;
                        if (i < 0 || (size_t)i >= inner->as.array.len) {
                            value_free(&val);
                            VM_ERROR("array index out of bounds: %lld (len %zu)",
                                (long long)i, inner->as.array.len); break;
                        }
                        value_free(&inner->as.array.elems[i]);
                        inner->as.array.elems[i] = val;
                        break;
                    }
                    if (inner->type == VAL_MAP && idx.type == VAL_STR) {
                        LatValue *old = (LatValue *)lat_map_get(inner->as.map.map, idx.as.str_val);
                        if (old) value_free(old);
                        lat_map_set(inner->as.map.map, idx.as.str_val, &val);
                        value_free(&idx);
                        break;
                    }
                    value_free(&val); value_free(&idx);
                    VM_ERROR("invalid index assignment on Ref"); break;
                }
                /* Phase check: reject mutation on crystal/sublimated values */
                if (obj->phase == VTAG_CRYSTAL || obj->phase == VTAG_SUBLIMATED) {
                    /* Check per-field phases for structs/maps with partial freeze */
                    bool field_frozen = false;
                    if (obj->type == VAL_MAP && idx.type == VAL_STR && obj->as.map.key_phases) {
                        PhaseTag *kp = lat_map_get(obj->as.map.key_phases, idx.as.str_val);
                        if (kp && *kp == VTAG_CRYSTAL) field_frozen = true;
                    }
                    if (obj->phase == VTAG_CRYSTAL || obj->phase == VTAG_SUBLIMATED || field_frozen) {
                        value_free(&val); value_free(&idx);
                        VM_ERROR("cannot modify a %s value",
                            obj->phase == VTAG_CRYSTAL ? "frozen" : "sublimated"); break;
                    }
                }
                /* Check per-key phase on non-frozen maps */
                if (obj->type == VAL_MAP && idx.type == VAL_STR && obj->as.map.key_phases) {
                    PhaseTag *kp = lat_map_get(obj->as.map.key_phases, idx.as.str_val);
                    if (kp && *kp == VTAG_CRYSTAL) {
                        value_free(&val); value_free(&idx);
                        VM_ERROR("cannot modify frozen key '%s'", idx.as.str_val); break;
                    }
                }
                if (obj->type == VAL_ARRAY && idx.type == VAL_INT) {
                    int64_t i = idx.as.int_val;
                    if (i < 0 || (size_t)i >= obj->as.array.len) {
                        value_free(&val);
                        VM_ERROR("array index out of bounds: %lld (len %zu)",
                            (long long)i, obj->as.array.len); break;
                    }
                    value_free(&obj->as.array.elems[i]);
                    obj->as.array.elems[i] = val;
                } else if (obj->type == VAL_MAP && idx.type == VAL_STR) {
                    lat_map_set(obj->as.map.map, idx.as.str_val, &val);
                    value_free(&idx);
                } else if (obj->type == VAL_BUFFER && idx.type == VAL_INT) {
                    int64_t i = idx.as.int_val;
                    if (i < 0 || (size_t)i >= obj->as.buffer.len) {
                        value_free(&val);
                        VM_ERROR("buffer index out of bounds: %lld (len %zu)",
                            (long long)i, obj->as.buffer.len); break;
                    }
                    obj->as.buffer.data[i] = (uint8_t)(val.as.int_val & 0xFF);
                    value_free(&val);
                } else {
                    value_free(&val);
                    value_free(&idx);
                    VM_ERROR("invalid index assignment"); break;
                }
                break;
            }

            /* ── Exception handling ── */
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_PUSH_EXCEPTION_HANDLER:
#endif
            case OP_PUSH_EXCEPTION_HANDLER: {
                uint16_t offset = READ_U16();
                if (vm->handler_count >= VM_HANDLER_MAX) {
                    VM_ERROR("too many nested exception handlers"); break;
                }
                ExceptionHandler *h = &vm->handlers[vm->handler_count++];
                h->ip = frame->ip + offset;
                h->chunk = frame->chunk;
                h->frame_index = vm->frame_count - 1;
                h->stack_top = vm->stack_top;
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_POP_EXCEPTION_HANDLER:
#endif
            case OP_POP_EXCEPTION_HANDLER: {
                if (vm->handler_count > 0)
                    vm->handler_count--;
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_THROW:
#endif
            case OP_THROW: {
                LatValue err = pop(vm);
                if (vm->handler_count > 0) {
                    ExceptionHandler h = vm->handlers[--vm->handler_count];
                    /* Unwind stack */
                    while (vm->frame_count - 1 > h.frame_index) {
                        vm->frame_count--;
                    }
                    frame = &vm->frames[vm->frame_count - 1];
                    vm->stack_top = h.stack_top;
                    frame->ip = h.ip;
                    push(vm, err);
                } else {
                    VMResult res;
                    if (err.type == VAL_STR) {
                        res = runtime_error(vm, "%s", err.as.str_val);
                    } else {
                        char *repr = value_repr(&err);
                        res = runtime_error(vm, "unhandled exception: %s", repr);
                        free(repr);
                    }
                    value_free(&err);
                    return res;
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_TRY_UNWRAP:
#endif
            case OP_TRY_UNWRAP: {
                /* Check if TOS is a map with "tag" = "ok" or "tag" = "err" */
                LatValue *val = vm_peek(vm, 0);
                if (val->type == VAL_MAP) {
                    LatValue *tag = lat_map_get(val->as.map.map, "tag");
                    if (tag && tag->type == VAL_STR) {
                        if (strcmp(tag->as.str_val, "ok") == 0) {
                            LatValue *inner = lat_map_get(val->as.map.map, "value");
                            LatValue result_val = inner ? value_deep_clone(inner) : value_nil();
                            LatValue old = pop(vm);
                            value_free(&old);
                            push(vm, result_val);
                            break;
                        } else if (strcmp(tag->as.str_val, "err") == 0) {
                            /* Return the error from the current function */
                            LatValue err_map = pop(vm);
                            close_upvalues(vm, frame->slots);
                            vm->frame_count--;
                            if (vm->frame_count == 0) {
                                *result = err_map;
                                return VM_OK;
                            }
                            vm->stack_top = frame->slots;
                            push(vm, err_map);
                            frame = &vm->frames[vm->frame_count - 1];
                            break;
                        }
                    }
                }
                /* Not a result map - error */
                value_free(val);
                (void)pop(vm);  /* we already freed the peeked value */
                VM_ERROR("'?' operator requires a result map with {tag: \"ok\"|\"err\", value: ...}");
                break;
            }

            /* ── Defer ── */
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_DEFER_PUSH:
#endif
            case OP_DEFER_PUSH: {
                uint8_t sdepth = READ_BYTE();
                uint16_t offset = READ_U16();
                if (vm->defer_count < VM_DEFER_MAX) {
                    VMDeferEntry *d = &vm->defers[vm->defer_count++];
                    d->ip = frame->ip; /* points to start of defer body (current ip after reading offset) */
                    d->chunk = frame->chunk;
                    d->frame_index = vm->frame_count - 1;
                    d->slots = frame->slots;
                    d->scope_depth = sdepth;
                }
                frame->ip += offset; /* skip defer body */
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_DEFER_RUN:
#endif
            case OP_DEFER_RUN: {
                /* Execute pending defers for the current function in LIFO order.
                 * Only run defers that belong to the current call frame AND
                 * have scope_depth >= the operand (scope-aware execution). */
                uint8_t min_depth = READ_BYTE();
                size_t current_frame_idx = vm->frame_count - 1;
                while (vm->defer_count > 0) {
                    VMDeferEntry *d = &vm->defers[vm->defer_count - 1];
                    if (d->frame_index != current_frame_idx) break;
                    if (d->scope_depth < min_depth) break;
                    vm->defer_count--;

                    /* Save return value (TOS) — we push it back after defer */
                    LatValue ret_val = pop(vm);

                    /* Create a view chunk over the defer body's bytecode.
                     * The body starts at d->ip and ends with OP_RETURN.
                     * vm_run will push a new frame and execute until OP_RETURN.
                     * Use next_frame_slots so the defer body shares the parent
                     * frame's locals (defer body bytecode uses parent slot indices). */
                    Chunk wrapper;
                    memset(&wrapper, 0, sizeof(wrapper));
                    wrapper.code = d->ip;
                    wrapper.code_len = (size_t)(d->chunk->code + d->chunk->code_len - d->ip);
                    wrapper.constants = d->chunk->constants;
                    wrapper.const_len = d->chunk->const_len;
                    wrapper.const_hashes = d->chunk->const_hashes;
                    wrapper.lines = d->chunk->lines ? d->chunk->lines + (d->ip - d->chunk->code) : NULL;

                    vm->next_frame_slots = d->slots;
                    LatValue defer_result;
                    vm_run(vm, &wrapper, &defer_result);
                    value_free(&defer_result);

                    /* Restore the return value */
                    push(vm, ret_val);
                }
                break;
            }

            /* ── Phase system ── */
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_FREEZE:
#endif
            case OP_FREEZE: {
                LatValue val = pop(vm);
                if (val.type == VAL_CHANNEL) {
                    value_free(&val);
                    VM_ERROR("cannot freeze a channel"); break;
                }
                LatValue frozen = value_freeze(val);
                push(vm, frozen);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_THAW:
#endif
            case OP_THAW: {
                LatValue val = pop(vm);
                LatValue thawed = value_thaw(&val);
                value_free(&val);
                push(vm, thawed);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_CLONE:
#endif
            case OP_CLONE: {
                LatValue val = pop(vm);
                LatValue cloned = value_deep_clone(&val);
                value_free(&val);
                push(vm, cloned);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_MARK_FLUID:
#endif
            case OP_MARK_FLUID: {
                vm_peek(vm, 0)->phase = VTAG_FLUID;
                break;
            }

            /* ── Phase system: reactions, bonds, seeds ── */

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_REACT:
#endif
            case OP_REACT: {
                uint8_t name_idx = READ_BYTE();
                const char *var_name = frame->chunk->constants[name_idx].as.str_val;
                LatValue callback = pop(vm);
                if (callback.type != VAL_CLOSURE) {
                    value_free(&callback);
                    push(vm, value_unit());
                    break;
                }
                /* Find or create reaction entry */
                size_t ri = vm->reaction_count;
                for (size_t i = 0; i < vm->reaction_count; i++) {
                    if (strcmp(vm->reactions[i].var_name, var_name) == 0) { ri = i; break; }
                }
                if (ri == vm->reaction_count) {
                    if (vm->reaction_count >= vm->reaction_cap) {
                        vm->reaction_cap = vm->reaction_cap ? vm->reaction_cap * 2 : 4;
                        vm->reactions = realloc(vm->reactions, vm->reaction_cap * sizeof(*vm->reactions));
                    }
                    vm->reactions[ri].var_name = strdup(var_name);
                    vm->reactions[ri].callbacks = NULL;
                    vm->reactions[ri].cb_count = 0;
                    vm->reactions[ri].cb_cap = 0;
                    vm->reaction_count++;
                }
                if (vm->reactions[ri].cb_count >= vm->reactions[ri].cb_cap) {
                    vm->reactions[ri].cb_cap = vm->reactions[ri].cb_cap ? vm->reactions[ri].cb_cap * 2 : 4;
                    vm->reactions[ri].callbacks = realloc(vm->reactions[ri].callbacks,
                        vm->reactions[ri].cb_cap * sizeof(LatValue));
                }
                vm->reactions[ri].callbacks[vm->reactions[ri].cb_count++] = value_deep_clone(&callback);
                value_free(&callback);
                push(vm, value_unit());
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_UNREACT:
#endif
            case OP_UNREACT: {
                uint8_t name_idx = READ_BYTE();
                const char *var_name = frame->chunk->constants[name_idx].as.str_val;
                for (size_t i = 0; i < vm->reaction_count; i++) {
                    if (strcmp(vm->reactions[i].var_name, var_name) != 0) continue;
                    free(vm->reactions[i].var_name);
                    for (size_t j = 0; j < vm->reactions[i].cb_count; j++)
                        value_free(&vm->reactions[i].callbacks[j]);
                    free(vm->reactions[i].callbacks);
                    vm->reactions[i] = vm->reactions[--vm->reaction_count];
                    break;
                }
                push(vm, value_unit());
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_BOND:
#endif
            case OP_BOND: {
                uint8_t target_idx = READ_BYTE();
                const char *target_name = frame->chunk->constants[target_idx].as.str_val;
                LatValue strategy_v = pop(vm);
                LatValue dep_v = pop(vm);
                const char *dep_name = (dep_v.type == VAL_STR) ? dep_v.as.str_val : "";
                const char *strategy = (strategy_v.type == VAL_STR) ? strategy_v.as.str_val : "mirror";

                /* Validate: dep must be a named variable (non-empty) */
                if (dep_name[0] == '\0') {
                    value_free(&dep_v); value_free(&strategy_v);
                    vm->error = strdup("bond() requires variable names for dependencies");
                    VMResult r = vm_handle_native_error(vm, &frame);
                    if (r != VM_OK) return r;
                    break;
                }
                /* Validate: target must not be already frozen */
                {
                    LatValue target_val;
                    bool target_found = env_get(vm->env, target_name, &target_val);
                    if (!target_found) target_found = vm_find_local_value(vm, target_name, &target_val);
                    if (target_found) {
                        if (target_val.phase == VTAG_CRYSTAL) {
                            value_free(&target_val); value_free(&dep_v); value_free(&strategy_v);
                            char *msg = NULL;
                            (void)asprintf(&msg, "cannot bond already-frozen variable '%s'", target_name);
                            vm->error = msg;
                            VMResult r = vm_handle_native_error(vm, &frame);
                            if (r != VM_OK) return r;
                            break;
                        }
                        value_free(&target_val);
                    }
                }
                /* Validate: dep variable must exist */
                {
                    LatValue dep_val;
                    bool dep_found = env_get(vm->env, dep_name, &dep_val);
                    if (!dep_found) dep_found = vm_find_local_value(vm, dep_name, &dep_val);
                    if (!dep_found) {
                        char *msg = NULL;
                        (void)asprintf(&msg, "cannot bond undefined variable '%s'", dep_name);
                        value_free(&dep_v); value_free(&strategy_v);
                        vm->error = msg;
                        VMResult r = vm_handle_native_error(vm, &frame);
                        if (r != VM_OK) return r;
                        break;
                    }
                    value_free(&dep_val);
                }
                /* Find or create bond entry */
                size_t bi = vm->bond_count;
                for (size_t i = 0; i < vm->bond_count; i++) {
                    if (strcmp(vm->bonds[i].target, target_name) == 0) { bi = i; break; }
                }
                if (bi == vm->bond_count) {
                    if (vm->bond_count >= vm->bond_cap) {
                        vm->bond_cap = vm->bond_cap ? vm->bond_cap * 2 : 4;
                        vm->bonds = realloc(vm->bonds, vm->bond_cap * sizeof(*vm->bonds));
                    }
                    vm->bonds[bi].target = strdup(target_name);
                    vm->bonds[bi].deps = NULL;
                    vm->bonds[bi].dep_strategies = NULL;
                    vm->bonds[bi].dep_count = 0;
                    vm->bonds[bi].dep_cap = 0;
                    vm->bond_count++;
                }
                if (vm->bonds[bi].dep_count >= vm->bonds[bi].dep_cap) {
                    vm->bonds[bi].dep_cap = vm->bonds[bi].dep_cap ? vm->bonds[bi].dep_cap * 2 : 4;
                    vm->bonds[bi].deps = realloc(vm->bonds[bi].deps,
                        vm->bonds[bi].dep_cap * sizeof(char *));
                    vm->bonds[bi].dep_strategies = realloc(vm->bonds[bi].dep_strategies,
                        vm->bonds[bi].dep_cap * sizeof(char *));
                }
                vm->bonds[bi].deps[vm->bonds[bi].dep_count] = strdup(dep_name);
                vm->bonds[bi].dep_strategies[vm->bonds[bi].dep_count] = strdup(strategy);
                vm->bonds[bi].dep_count++;
                value_free(&dep_v);
                value_free(&strategy_v);
                push(vm, value_unit());
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_UNBOND:
#endif
            case OP_UNBOND: {
                uint8_t target_idx = READ_BYTE();
                const char *target_name = frame->chunk->constants[target_idx].as.str_val;
                LatValue dep_v = pop(vm);
                const char *dep_name = (dep_v.type == VAL_STR) ? dep_v.as.str_val : "";
                for (size_t i = 0; i < vm->bond_count; i++) {
                    if (strcmp(vm->bonds[i].target, target_name) != 0) continue;
                    for (size_t j = 0; j < vm->bonds[i].dep_count; j++) {
                        if (strcmp(vm->bonds[i].deps[j], dep_name) != 0) continue;
                        free(vm->bonds[i].deps[j]);
                        if (vm->bonds[i].dep_strategies) free(vm->bonds[i].dep_strategies[j]);
                        /* Swap-remove */
                        vm->bonds[i].deps[j] = vm->bonds[i].deps[vm->bonds[i].dep_count - 1];
                        if (vm->bonds[i].dep_strategies)
                            vm->bonds[i].dep_strategies[j] = vm->bonds[i].dep_strategies[vm->bonds[i].dep_count - 1];
                        vm->bonds[i].dep_count--;
                        break;
                    }
                    /* If empty, remove the bond entry */
                    if (vm->bonds[i].dep_count == 0) {
                        free(vm->bonds[i].target);
                        free(vm->bonds[i].deps);
                        free(vm->bonds[i].dep_strategies);
                        vm->bonds[i] = vm->bonds[--vm->bond_count];
                    }
                    break;
                }
                value_free(&dep_v);
                push(vm, value_unit());
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_SEED:
#endif
            case OP_SEED: {
                uint8_t name_idx = READ_BYTE();
                const char *var_name = frame->chunk->constants[name_idx].as.str_val;
                LatValue contract = pop(vm);
                if (contract.type != VAL_CLOSURE) {
                    value_free(&contract);
                    push(vm, value_unit());
                    break;
                }
                if (vm->seed_count >= vm->seed_cap) {
                    vm->seed_cap = vm->seed_cap ? vm->seed_cap * 2 : 4;
                    vm->seeds = realloc(vm->seeds, vm->seed_cap * sizeof(*vm->seeds));
                }
                vm->seeds[vm->seed_count].var_name = strdup(var_name);
                vm->seeds[vm->seed_count].contract = value_deep_clone(&contract);
                vm->seed_count++;
                value_free(&contract);
                push(vm, value_unit());
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_UNSEED:
#endif
            case OP_UNSEED: {
                uint8_t name_idx = READ_BYTE();
                const char *var_name = frame->chunk->constants[name_idx].as.str_val;
                for (size_t i = 0; i < vm->seed_count; i++) {
                    if (strcmp(vm->seeds[i].var_name, var_name) != 0) continue;
                    free(vm->seeds[i].var_name);
                    value_free(&vm->seeds[i].contract);
                    vm->seeds[i] = vm->seeds[--vm->seed_count];
                    break;
                }
                push(vm, value_unit());
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_FREEZE_VAR:
#endif
            case OP_FREEZE_VAR: {
                uint8_t name_idx = READ_BYTE();
                uint8_t loc_type = READ_BYTE();
                uint8_t loc_slot = READ_BYTE();
                const char *var_name = frame->chunk->constants[name_idx].as.str_val;
                LatValue val = pop(vm);
                if (val.type == VAL_CHANNEL) {
                    value_free(&val);
                    VM_ERROR("cannot freeze a channel"); break;
                }
                /* Validate seed contracts (don't consume — matches tree-walker freeze behavior) */
                char *seed_err = vm_validate_seeds(vm, var_name, &val, false);
                if (seed_err) {
                    value_free(&val);
                    vm->error = seed_err;
                    VMResult r = vm_handle_native_error(vm, &frame);
                    if (r != VM_OK) return r;
                    break;
                }
                LatValue frozen = value_freeze(val);
                LatValue ret = value_deep_clone(&frozen);
                vm_write_back(vm, frame, loc_type, loc_slot, var_name, frozen);
                value_free(&frozen);
                VMResult cr = vm_freeze_cascade(vm, &frame, var_name);
                if (cr != VM_OK) { value_free(&ret); return cr; }
                VMResult rr = vm_fire_reactions(vm, &frame, var_name, "crystal");
                if (rr != VM_OK) { value_free(&ret); return rr; }
                push(vm, ret);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_THAW_VAR:
#endif
            case OP_THAW_VAR: {
                uint8_t name_idx = READ_BYTE();
                uint8_t loc_type = READ_BYTE();
                uint8_t loc_slot = READ_BYTE();
                const char *var_name = frame->chunk->constants[name_idx].as.str_val;
                LatValue val = pop(vm);
                LatValue thawed = value_thaw(&val);
                value_free(&val);
                LatValue ret = value_deep_clone(&thawed);
                vm_write_back(vm, frame, loc_type, loc_slot, var_name, thawed);
                value_free(&thawed);
                VMResult rr = vm_fire_reactions(vm, &frame, var_name, "fluid");
                if (rr != VM_OK) { value_free(&ret); return rr; }
                push(vm, ret);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_SUBLIMATE_VAR:
#endif
            case OP_SUBLIMATE_VAR: {
                uint8_t name_idx = READ_BYTE();
                uint8_t loc_type = READ_BYTE();
                uint8_t loc_slot = READ_BYTE();
                const char *var_name = frame->chunk->constants[name_idx].as.str_val;
                LatValue val = pop(vm);
                val.phase = VTAG_SUBLIMATED;
                LatValue ret = value_deep_clone(&val);
                vm_write_back(vm, frame, loc_type, loc_slot, var_name, val);
                value_free(&val);
                VMResult rr = vm_fire_reactions(vm, &frame, var_name, "sublimated");
                if (rr != VM_OK) { value_free(&ret); return rr; }
                push(vm, ret);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_SUBLIMATE:
#endif
            case OP_SUBLIMATE: {
                LatValue val = pop(vm);
                val.phase = VTAG_SUBLIMATED;
                push(vm, val);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_IS_CRYSTAL:
#endif
            case OP_IS_CRYSTAL: {
                LatValue val = pop(vm);
                bool is_crystal = (val.phase == VTAG_CRYSTAL);
                value_free(&val);
                push(vm, value_bool(is_crystal));
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_FREEZE_EXCEPT:
#endif
            case OP_FREEZE_EXCEPT: {
                uint8_t name_idx = READ_BYTE();
                uint8_t loc_type = READ_BYTE();
                uint8_t loc_slot = READ_BYTE();
                uint8_t except_count = READ_BYTE();
                const char *var_name = frame->chunk->constants[name_idx].as.str_val;

                /* Pop except field names from stack (pushed first-to-last) */
                char **except_names = malloc(except_count * sizeof(char *));
                for (int i = except_count - 1; i >= 0; i--) {
                    LatValue v = pop(vm);
                    except_names[i] = (v.type == VAL_STR) ? strdup(v.as.str_val) : strdup("");
                    value_free(&v);
                }

                /* Get a working copy of the variable value */
                LatValue val;
                switch (loc_type) {
                    case 0: val = value_deep_clone(&frame->slots[loc_slot]); break;
                    case 1:
                        if (frame->upvalues && loc_slot < frame->upvalue_count && frame->upvalues[loc_slot])
                            val = value_deep_clone(frame->upvalues[loc_slot]->location);
                        else val = value_nil();
                        break;
                    default: {
                        LatValue tmp;
                        if (!env_get(vm->env, var_name, &tmp)) tmp = value_nil();
                        val = tmp;
                        break;
                    }
                }

                if (val.type == VAL_STRUCT) {
                    if (!val.as.strct.field_phases) {
                        val.as.strct.field_phases = calloc(val.as.strct.field_count, sizeof(PhaseTag));
                        for (size_t i = 0; i < val.as.strct.field_count; i++)
                            val.as.strct.field_phases[i] = val.phase;
                    }
                    for (size_t i = 0; i < val.as.strct.field_count; i++) {
                        bool exempted = false;
                        for (uint8_t j = 0; j < except_count; j++) {
                            if (strcmp(val.as.strct.field_names[i], except_names[j]) == 0) {
                                exempted = true; break;
                            }
                        }
                        if (!exempted) {
                            val.as.strct.field_values[i] = value_freeze(val.as.strct.field_values[i]);
                            val.as.strct.field_phases[i] = VTAG_CRYSTAL;
                        } else {
                            val.as.strct.field_phases[i] = VTAG_FLUID;
                        }
                    }
                } else if (val.type == VAL_MAP) {
                    if (!val.as.map.key_phases) {
                        val.as.map.key_phases = calloc(1, sizeof(LatMap));
                        *val.as.map.key_phases = lat_map_new(sizeof(PhaseTag));
                    }
                    for (size_t i = 0; i < val.as.map.map->cap; i++) {
                        if (val.as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                        const char *key = val.as.map.map->entries[i].key;
                        bool exempted = false;
                        for (uint8_t j = 0; j < except_count; j++) {
                            if (strcmp(key, except_names[j]) == 0) {
                                exempted = true; break;
                            }
                        }
                        PhaseTag phase;
                        if (!exempted) {
                            LatValue *vp = (LatValue *)val.as.map.map->entries[i].value;
                            *vp = value_freeze(*vp);
                            phase = VTAG_CRYSTAL;
                        } else {
                            phase = VTAG_FLUID;
                        }
                        lat_map_set(val.as.map.key_phases, key, &phase);
                    }
                }

                /* Write back and push result */
                LatValue ret = value_deep_clone(&val);
                vm_write_back(vm, frame, loc_type, loc_slot, var_name, val);
                value_free(&val);
                push(vm, ret);
                for (uint8_t i = 0; i < except_count; i++) free(except_names[i]);
                free(except_names);
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_FREEZE_FIELD:
#endif
            case OP_FREEZE_FIELD: {
                uint8_t pname_idx = READ_BYTE();
                uint8_t loc_type = READ_BYTE();
                uint8_t loc_slot = READ_BYTE();
                const char *parent_name = frame->chunk->constants[pname_idx].as.str_val;
                LatValue field_name = pop(vm);

                /* Get a working copy of the parent variable */
                LatValue parent;
                switch (loc_type) {
                    case 0: parent = value_deep_clone(&frame->slots[loc_slot]); break;
                    case 1:
                        if (frame->upvalues && loc_slot < frame->upvalue_count && frame->upvalues[loc_slot])
                            parent = value_deep_clone(frame->upvalues[loc_slot]->location);
                        else parent = value_nil();
                        break;
                    default: {
                        LatValue tmp;
                        if (!env_get(vm->env, parent_name, &tmp)) tmp = value_nil();
                        parent = tmp;
                        break;
                    }
                }

                if (parent.type == VAL_STRUCT && field_name.type == VAL_STR) {
                    const char *fname = field_name.as.str_val;
                    size_t fi = (size_t)-1;
                    for (size_t i = 0; i < parent.as.strct.field_count; i++) {
                        if (strcmp(parent.as.strct.field_names[i], fname) == 0) { fi = i; break; }
                    }
                    if (fi == (size_t)-1) {
                        value_free(&parent); value_free(&field_name);
                        VM_ERROR("struct has no field '%s'", fname); break;
                    }
                    parent.as.strct.field_values[fi] = value_freeze(parent.as.strct.field_values[fi]);
                    if (!parent.as.strct.field_phases)
                        parent.as.strct.field_phases = calloc(parent.as.strct.field_count, sizeof(PhaseTag));
                    parent.as.strct.field_phases[fi] = VTAG_CRYSTAL;
                    LatValue ret = value_deep_clone(&parent.as.strct.field_values[fi]);
                    vm_write_back(vm, frame, loc_type, loc_slot, parent_name, parent);
                    value_free(&parent);
                    value_free(&field_name);
                    push(vm, ret);
                } else if (parent.type == VAL_MAP && field_name.type == VAL_STR) {
                    const char *key = field_name.as.str_val;
                    LatValue *val_ptr = (LatValue *)lat_map_get(parent.as.map.map, key);
                    if (!val_ptr) {
                        value_free(&parent); value_free(&field_name);
                        VM_ERROR("map has no key '%s'", key); break;
                    }
                    *val_ptr = value_freeze(*val_ptr);
                    if (!parent.as.map.key_phases) {
                        parent.as.map.key_phases = calloc(1, sizeof(LatMap));
                        *parent.as.map.key_phases = lat_map_new(sizeof(PhaseTag));
                    }
                    PhaseTag crystal = VTAG_CRYSTAL;
                    lat_map_set(parent.as.map.key_phases, key, &crystal);
                    LatValue ret = value_deep_clone(val_ptr);
                    vm_write_back(vm, frame, loc_type, loc_slot, parent_name, parent);
                    value_free(&parent);
                    value_free(&field_name);
                    push(vm, ret);
                } else {
                    value_free(&parent); value_free(&field_name);
                    VM_ERROR("freeze field requires a struct or map"); break;
                }
                break;
            }

            /* ── Print ── */
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_PRINT:
#endif
            case OP_PRINT: {
                uint8_t argc = READ_BYTE();
                LatValue *vals = malloc(argc * sizeof(LatValue));
                for (int i = argc - 1; i >= 0; i--)
                    vals[i] = pop(vm);
                for (uint8_t i = 0; i < argc; i++) {
                    if (i > 0) printf(" ");
                    if (vals[i].type == VAL_STR) {
                        printf("%s", vals[i].as.str_val);
                    } else {
                        char *repr = value_repr(&vals[i]);
                        printf("%s", repr);
                        free(repr);
                    }
                    value_free(&vals[i]);
                }
                printf("\n");
                free(vals);
                push(vm, value_unit());
                break;
            }

            /* ── Import ── */
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_IMPORT:
#endif
            case OP_IMPORT: {
                uint8_t path_idx = READ_BYTE();
                const char *raw_path = frame->chunk->constants[path_idx].as.str_val;

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
                    char errbuf[512];
                    snprintf(errbuf, sizeof(errbuf), "import: cannot find '%s'", file_path);
                    free(file_path);
                    VM_ERROR("%s", errbuf); break;
                }
                free(file_path);

                /* Check module cache */
                LatValue *cached = lat_map_get(&vm->module_cache, resolved);
                if (cached) {
                    push(vm, value_deep_clone(cached));
                    break;
                }

                /* Read the file */
                char *source = builtin_read_file(resolved);
                if (!source) {
                    VM_ERROR("import: cannot read '%s'", resolved); break;
                }

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
                    VM_ERROR("%s", errmsg); break;
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
                    VM_ERROR("%s", errmsg); break;
                }

                /* Compile as module (no auto-call of main) */
                char *comp_err = NULL;
                Chunk *mod_chunk = compile_module(&mod_prog, &comp_err);

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
                    VM_ERROR("%s", errmsg); break;
                }

                /* Track the chunk for proper lifetime management */
                if (vm->fn_chunk_count >= vm->fn_chunk_cap) {
                    vm->fn_chunk_cap = vm->fn_chunk_cap ? vm->fn_chunk_cap * 2 : 8;
                    vm->fn_chunks = realloc(vm->fn_chunks,
                                            vm->fn_chunk_cap * sizeof(Chunk *));
                }
                vm->fn_chunks[vm->fn_chunk_count++] = mod_chunk;

                /* Push a module scope so module globals are isolated */
                env_push_scope(vm->env);

                /* Run the module chunk */
                LatValue mod_result;
                VMResult mod_r = vm_run(vm, mod_chunk, &mod_result);
                if (mod_r != VM_OK) {
                    env_pop_scope(vm->env);
                    push(vm, value_nil());
                    break;
                }
                value_free(&mod_result);

                /* Build module Map from the module scope */
                LatValue module_map = value_map_new();
                Scope *mod_scope = &vm->env->scopes[vm->env->count - 1];
                for (size_t mi = 0; mi < mod_scope->cap; mi++) {
                    if (mod_scope->entries[mi].state != MAP_OCCUPIED) continue;
                    const char *name = mod_scope->entries[mi].key;
                    LatValue *val_ptr = (LatValue *)mod_scope->entries[mi].value;

                    /* Copy all module bindings to base scope so that closures
                     * exported from the module can still resolve their globals
                     * (OP_GET_GLOBAL) after the module scope is popped. */
                    env_define_at(vm->env, 0, name, value_deep_clone(val_ptr));

                    /* Skip internal metadata from the exported Map */
                    if ((name[0] == '_' && name[1] == '_') || strchr(name, ':'))
                        continue;

                    LatValue exported = value_deep_clone(val_ptr);
                    lat_map_set(module_map.as.map.map, name, &exported);
                }

                env_pop_scope(vm->env);

                /* Cache the module map */
                LatValue cache_copy = value_deep_clone(&module_map);
                lat_map_set(&vm->module_cache, resolved, &cache_copy);

                push(vm, module_map);
                break;
            }

            /* ── Concurrency ── */

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_SCOPE:
#endif
            case OP_SCOPE: {
                /* Read all inline data upfront */
                uint8_t spawn_count = READ_BYTE();
                uint8_t sync_idx = READ_BYTE();
                uint8_t spawn_indices[256];
                for (uint8_t i = 0; i < spawn_count; i++)
                    spawn_indices[i] = READ_BYTE();

#ifdef __EMSCRIPTEN__
                (void)sync_idx;
                push(vm, value_unit());
#else
                /* Export current locals so sub-chunks can see them via env */
                env_push_scope(vm->env);
                for (size_t fi2 = 0; fi2 < vm->frame_count; fi2++) {
                    CallFrame *f2 = &vm->frames[fi2];
                    if (!f2->chunk) continue;
                    size_t lc = (fi2 + 1 < vm->frame_count)
                        ? (size_t)(vm->frames[fi2 + 1].slots - f2->slots)
                        : (size_t)(vm->stack_top - f2->slots);
                    for (size_t sl = 0; sl < lc; sl++) {
                        if (sl < f2->chunk->local_name_cap && f2->chunk->local_names[sl])
                            env_define(vm->env, f2->chunk->local_names[sl],
                                       value_deep_clone(&f2->slots[sl]));
                    }
                }

                if (spawn_count == 0) {
                    /* No spawns — run sync body */
                    if (sync_idx != 0xFF) {
                        Chunk *body = (Chunk *)frame->chunk->constants[sync_idx].as.closure.native_fn;
                        LatValue scope_result;
                        VMResult sr = vm_run(vm, body, &scope_result);
                        env_pop_scope(vm->env);
                        if (sr != VM_OK) {
                            return runtime_error(vm, "%s", vm->error ? vm->error : "scope error");
                        }
                        push(vm, scope_result);
                    } else {
                        env_pop_scope(vm->env);
                        push(vm, value_unit());
                    }
                } else {
                    /* Has spawns — run concurrently */
                    char *first_error = NULL;

                    /* Run sync body first (non-spawn statements) */
                    if (sync_idx != 0xFF) {
                        Chunk *sync_body = (Chunk *)frame->chunk->constants[sync_idx].as.closure.native_fn;
                        LatValue ns_result;
                        VMResult nsr = vm_run(vm, sync_body, &ns_result);
                        if (nsr != VM_OK) {
                            first_error = vm->error ? strdup(vm->error) : strdup("scope stmt error");
                            free(vm->error);
                            vm->error = NULL;
                        } else {
                            value_free(&ns_result);
                        }
                    }

                    /* Create child VMs for each spawn */
                    VMSpawnTask *tasks = calloc(spawn_count, sizeof(VMSpawnTask));
                    for (uint8_t i = 0; i < spawn_count && !first_error; i++) {
                        Chunk *sp_chunk = (Chunk *)frame->chunk->constants[spawn_indices[i]].as.closure.native_fn;
                        tasks[i].chunk = sp_chunk;
                        tasks[i].child_vm = vm_clone_for_thread(vm);
                        vm_export_locals_to_env(vm, tasks[i].child_vm);
                        tasks[i].error = NULL;
                    }

                    /* Launch all spawn threads */
                    for (uint8_t i = 0; i < spawn_count; i++) {
                        if (!tasks[i].child_vm) continue;
                        pthread_create(&tasks[i].thread, NULL, vm_spawn_thread_fn, &tasks[i]);
                    }

                    /* Join all threads */
                    for (uint8_t i = 0; i < spawn_count; i++) {
                        if (!tasks[i].child_vm) continue;
                        pthread_join(tasks[i].thread, NULL);
                    }

                    /* Restore parent TLS state */
                    current_vm = vm;

                    /* Collect first error from child threads */
                    for (uint8_t i = 0; i < spawn_count; i++) {
                        if (tasks[i].error && !first_error) {
                            first_error = tasks[i].error;
                        } else if (tasks[i].error) {
                            free(tasks[i].error);
                        }
                        if (tasks[i].child_vm)
                            vm_free_child(tasks[i].child_vm);
                    }

                    env_pop_scope(vm->env);
                    free(tasks);

                    if (first_error) {
                        VMResult err = runtime_error(vm, "%s", first_error);
                        free(first_error);
                        return err;
                    }
                    push(vm, value_unit());
                }
#endif
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_SELECT:
#endif
            case OP_SELECT: {
                /* Read all inline arm data upfront */
                uint8_t arm_count = READ_BYTE();
                typedef struct { uint8_t flags, chan_idx, body_idx, binding_idx; } SelArmInfo;
                SelArmInfo *arm_info = malloc(arm_count * sizeof(SelArmInfo));
                for (uint8_t i = 0; i < arm_count; i++) {
                    arm_info[i].flags = READ_BYTE();
                    arm_info[i].chan_idx = READ_BYTE();
                    arm_info[i].body_idx = READ_BYTE();
                    arm_info[i].binding_idx = READ_BYTE();
                }

#ifdef __EMSCRIPTEN__
                free(arm_info);
                push(vm, value_nil());
#else
                /* Find default and timeout arms */
                int default_arm = -1;
                int timeout_arm = -1;
                for (uint8_t i = 0; i < arm_count; i++) {
                    if (arm_info[i].flags & 0x01) default_arm = (int)i;
                    if (arm_info[i].flags & 0x02) timeout_arm = (int)i;
                }

                /* Export locals to env for sub-chunk visibility */
                env_push_scope(vm->env);
                for (size_t fi2 = 0; fi2 < vm->frame_count; fi2++) {
                    CallFrame *f2 = &vm->frames[fi2];
                    if (!f2->chunk) continue;
                    size_t lc = (fi2 + 1 < vm->frame_count)
                        ? (size_t)(vm->frames[fi2 + 1].slots - f2->slots)
                        : (size_t)(vm->stack_top - f2->slots);
                    for (size_t sl = 0; sl < lc; sl++) {
                        if (sl < f2->chunk->local_name_cap && f2->chunk->local_names[sl])
                            env_define(vm->env, f2->chunk->local_names[sl],
                                       value_deep_clone(&f2->slots[sl]));
                    }
                }

                /* Evaluate all channel expressions upfront */
                LatChannel **channels = calloc(arm_count, sizeof(LatChannel *));
                for (uint8_t i = 0; i < arm_count; i++) {
                    if (arm_info[i].flags & 0x03) continue; /* skip default/timeout */
                    Chunk *ch_chunk = (Chunk *)frame->chunk->constants[arm_info[i].chan_idx].as.closure.native_fn;
                    LatValue ch_val;
                    VMResult cr = vm_run(vm, ch_chunk, &ch_val);
                    if (cr != VM_OK) {
                        env_pop_scope(vm->env);
                        for (uint8_t j = 0; j < i; j++)
                            if (channels[j]) channel_release(channels[j]);
                        free(channels);
                        free(arm_info);
                        return runtime_error(vm, "%s", vm->error ? vm->error : "select channel error");
                    }
                    if (ch_val.type != VAL_CHANNEL) {
                        value_free(&ch_val);
                        env_pop_scope(vm->env);
                        for (uint8_t j = 0; j < i; j++)
                            if (channels[j]) channel_release(channels[j]);
                        free(channels);
                        free(arm_info);
                        return runtime_error(vm, "select arm: expression is not a Channel");
                    }
                    channels[i] = ch_val.as.channel.ch;
                    channel_retain(channels[i]);
                    value_free(&ch_val);
                }

                /* Evaluate timeout if present */
                long timeout_ms = -1;
                if (timeout_arm >= 0) {
                    Chunk *to_chunk = (Chunk *)frame->chunk->constants[arm_info[timeout_arm].chan_idx].as.closure.native_fn;
                    LatValue to_val;
                    VMResult tr = vm_run(vm, to_chunk, &to_val);
                    if (tr != VM_OK) {
                        env_pop_scope(vm->env);
                        for (uint8_t i = 0; i < arm_count; i++)
                            if (channels[i]) channel_release(channels[i]);
                        free(channels);
                        free(arm_info);
                        return runtime_error(vm, "%s", vm->error ? vm->error : "select timeout error");
                    }
                    if (to_val.type != VAL_INT) {
                        value_free(&to_val);
                        env_pop_scope(vm->env);
                        for (uint8_t i = 0; i < arm_count; i++)
                            if (channels[i]) channel_release(channels[i]);
                        free(channels);
                        free(arm_info);
                        return runtime_error(vm, "select timeout must be an integer (milliseconds)");
                    }
                    timeout_ms = (long)to_val.as.int_val;
                    value_free(&to_val);
                }

                /* Build shuffled index array for fairness */
                size_t ch_arm_count = 0;
                size_t *indices = malloc(arm_count * sizeof(size_t));
                for (uint8_t i = 0; i < arm_count; i++) {
                    if (!(arm_info[i].flags & 0x03))
                        indices[ch_arm_count++] = i;
                }
                /* Fisher-Yates shuffle */
                for (size_t i = ch_arm_count; i > 1; i--) {
                    size_t j = (size_t)rand() % i;
                    size_t tmp = indices[i-1];
                    indices[i-1] = indices[j];
                    indices[j] = tmp;
                }

                /* Set up waiter for blocking */
                pthread_mutex_t sel_mutex = PTHREAD_MUTEX_INITIALIZER;
                pthread_cond_t  sel_cond  = PTHREAD_COND_INITIALIZER;
                LatSelectWaiter waiter = {
                    .mutex = &sel_mutex,
                    .cond  = &sel_cond,
                    .next  = NULL,
                };

                LatValue select_result = value_unit();
                bool select_found = false;
                bool select_error = false;

                /* Compute deadline for timeout */
                struct timespec deadline;
                if (timeout_ms >= 0) {
                    clock_gettime(CLOCK_REALTIME, &deadline);
                    deadline.tv_sec  += timeout_ms / 1000;
                    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
                    if (deadline.tv_nsec >= 1000000000L) {
                        deadline.tv_sec++;
                        deadline.tv_nsec -= 1000000000L;
                    }
                }

                for (;;) {
                    /* Try non-blocking recv on each channel arm (shuffled order) */
                    bool all_closed = true;
                    for (size_t k = 0; k < ch_arm_count; k++) {
                        size_t i = indices[k];
                        LatChannel *ch = channels[i];
                        LatValue recv_val;
                        bool closed = false;
                        if (channel_try_recv(ch, &recv_val, &closed)) {
                            /* Got a value — bind in env, run body */
                            env_push_scope(vm->env);
                            const char *binding = (arm_info[i].flags & 0x04)
                                ? frame->chunk->constants[arm_info[i].binding_idx].as.str_val
                                : NULL;
                            if (binding)
                                env_define(vm->env, binding, recv_val);
                            else
                                value_free(&recv_val);

                            Chunk *arm_chunk = (Chunk *)frame->chunk->constants[arm_info[i].body_idx].as.closure.native_fn;
                            LatValue arm_result;
                            VMResult ar = vm_run(vm, arm_chunk, &arm_result);
                            env_pop_scope(vm->env);
                            if (ar != VM_OK) {
                                select_error = true;
                            } else {
                                value_free(&select_result);
                                select_result = arm_result;
                            }
                            select_found = true;
                            break;
                        }
                        if (!closed) all_closed = false;
                    }
                    if (select_found || select_error) break;

                    if (all_closed && ch_arm_count > 0) {
                        /* All channels closed — execute default if present */
                        if (default_arm >= 0) {
                            env_push_scope(vm->env);
                            Chunk *def_chunk = (Chunk *)frame->chunk->constants[arm_info[default_arm].body_idx].as.closure.native_fn;
                            LatValue def_result;
                            VMResult dr = vm_run(vm, def_chunk, &def_result);
                            if (dr == VM_OK) {
                                value_free(&select_result);
                                select_result = def_result;
                            } else {
                                select_error = true;
                            }
                            env_pop_scope(vm->env);
                        }
                        break;
                    }

                    /* If there's a default arm, execute it immediately */
                    if (default_arm >= 0) {
                        env_push_scope(vm->env);
                        Chunk *def_chunk = (Chunk *)frame->chunk->constants[arm_info[default_arm].body_idx].as.closure.native_fn;
                        LatValue def_result;
                        VMResult dr = vm_run(vm, def_chunk, &def_result);
                        if (dr == VM_OK) {
                            value_free(&select_result);
                            select_result = def_result;
                        } else {
                            select_error = true;
                        }
                        env_pop_scope(vm->env);
                        break;
                    }

                    /* Block: register waiter on all channels, then wait */
                    for (size_t k = 0; k < ch_arm_count; k++)
                        channel_add_waiter(channels[indices[k]], &waiter);

                    pthread_mutex_lock(&sel_mutex);
                    if (timeout_ms >= 0) {
                        int rc = pthread_cond_timedwait(&sel_cond, &sel_mutex, &deadline);
                        if (rc != 0) {
                            /* Timeout expired */
                            pthread_mutex_unlock(&sel_mutex);
                            for (size_t k = 0; k < ch_arm_count; k++)
                                channel_remove_waiter(channels[indices[k]], &waiter);
                            if (timeout_arm >= 0) {
                                env_push_scope(vm->env);
                                Chunk *to_body = (Chunk *)frame->chunk->constants[arm_info[timeout_arm].body_idx].as.closure.native_fn;
                                LatValue to_result;
                                VMResult tor = vm_run(vm, to_body, &to_result);
                                if (tor == VM_OK) {
                                    value_free(&select_result);
                                    select_result = to_result;
                                } else {
                                    select_error = true;
                                }
                                env_pop_scope(vm->env);
                            }
                            break;
                        }
                    } else {
                        pthread_cond_wait(&sel_cond, &sel_mutex);
                    }
                    pthread_mutex_unlock(&sel_mutex);

                    /* Remove waiters and retry */
                    for (size_t k = 0; k < ch_arm_count; k++)
                        channel_remove_waiter(channels[indices[k]], &waiter);
                }

                pthread_mutex_destroy(&sel_mutex);
                pthread_cond_destroy(&sel_cond);
                free(indices);
                for (uint8_t i = 0; i < arm_count; i++)
                    if (channels[i]) channel_release(channels[i]);
                free(channels);
                env_pop_scope(vm->env);

                if (select_error) {
                    value_free(&select_result);
                    char *err_msg = vm->error ? strdup(vm->error) : strdup("select error");
                    free(vm->error);
                    vm->error = NULL;
                    free(arm_info);
                    VMResult err = runtime_error(vm, "%s", err_msg);
                    free(err_msg);
                    return err;
                }
                free(arm_info);
                push(vm, select_result);
#endif
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_LOAD_INT8:
#endif
            case OP_LOAD_INT8: {
                int8_t val = (int8_t)READ_BYTE();
                push(vm, value_int((int64_t)val));
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_INC_LOCAL:
#endif
            case OP_INC_LOCAL: {
                uint8_t slot = READ_BYTE();
                LatValue *lv = &frame->slots[slot];
                if (lv->type == VAL_INT) {
                    lv->as.int_val++;
                } else {
                    VM_ERROR("OP_INC_LOCAL: expected Int");
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_DEC_LOCAL:
#endif
            case OP_DEC_LOCAL: {
                uint8_t slot = READ_BYTE();
                LatValue *lv = &frame->slots[slot];
                if (lv->type == VAL_INT) {
                    lv->as.int_val--;
                } else {
                    VM_ERROR("OP_DEC_LOCAL: expected Int");
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_ADD_INT:
#endif
            case OP_ADD_INT: {
                vm->stack_top--;
                vm->stack_top[-1].as.int_val += vm->stack_top[0].as.int_val;
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_SUB_INT:
#endif
            case OP_SUB_INT: {
                vm->stack_top--;
                vm->stack_top[-1].as.int_val -= vm->stack_top[0].as.int_val;
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_MUL_INT:
#endif
            case OP_MUL_INT: {
                vm->stack_top--;
                vm->stack_top[-1].as.int_val *= vm->stack_top[0].as.int_val;
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_LT_INT:
#endif
            case OP_LT_INT: {
                vm->stack_top--;
                int64_t a = vm->stack_top[-1].as.int_val;
                vm->stack_top[-1].type = VAL_BOOL;
                vm->stack_top[-1].as.bool_val = a < vm->stack_top[0].as.int_val;
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_LTEQ_INT:
#endif
            case OP_LTEQ_INT: {
                vm->stack_top--;
                int64_t a = vm->stack_top[-1].as.int_val;
                vm->stack_top[-1].type = VAL_BOOL;
                vm->stack_top[-1].as.bool_val = a <= vm->stack_top[0].as.int_val;
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_RESET_EPHEMERAL:
#endif
            case OP_RESET_EPHEMERAL: {
                /* Promote all ephemeral values on the entire stack before
                 * resetting.  This covers local bindings (which stay on the
                 * stack via add_local without an explicit clone) as well as
                 * expression temporaries in parent frames that could be
                 * invalidated when a callee resets the shared arena. */
                if (vm->ephemeral_on_stack) {
                    for (LatValue *slot = vm->stack; slot < vm->stack_top; slot++) {
                        vm_promote_value(slot);
                    }
                    vm->ephemeral_on_stack = false;
                }
                bump_arena_reset(vm->ephemeral);
                break;
            }

            /* ── Runtime type checking ── */
#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_CHECK_TYPE:
#endif
            case OP_CHECK_TYPE: {
                uint8_t slot = READ_BYTE();
                uint8_t type_idx = READ_BYTE();
                uint8_t err_idx = READ_BYTE();
                LatValue *val = &frame->slots[slot];
                const char *type_name = frame->chunk->constants[type_idx].as.str_val;
                if (!vm_type_matches(val, type_name)) {
                    const char *err_fmt = frame->chunk->constants[err_idx].as.str_val;
                    const char *actual = vm_value_type_display(val);
                    /* err_fmt has a %s placeholder for the actual type */
                    VM_ERROR(err_fmt, actual);
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_CHECK_RETURN_TYPE:
#endif
            case OP_CHECK_RETURN_TYPE: {
                uint8_t type_idx = READ_BYTE();
                uint8_t err_idx = READ_BYTE();
                LatValue *val = vm_peek(vm, 0);
                const char *type_name = frame->chunk->constants[type_idx].as.str_val;
                if (!vm_type_matches(val, type_name)) {
                    const char *err_fmt = frame->chunk->constants[err_idx].as.str_val;
                    const char *actual = vm_value_type_display(val);
                    VM_ERROR(err_fmt, actual);
                }
                break;
            }

#ifdef VM_USE_COMPUTED_GOTO
            lbl_OP_HALT:
#endif
            case OP_HALT:
                *result = value_unit();
                return VM_OK;

            default:
                VM_ERROR("unknown opcode %d", op); break;
        }
    }
}

#undef READ_BYTE
#undef READ_U16
