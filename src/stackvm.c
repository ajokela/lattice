#include "stackvm.h"
#include "runtime.h"
#include "stackopcode.h"
#include "stackcompiler.h" /* for AstPhase (PHASE_FLUID, PHASE_CRYSTAL, PHASE_UNSPECIFIED) */
#include "intern.h"
#include "builtins.h"
#include "lattice.h"
#include "channel.h"
#include "ext.h"
#include "lexer.h"
#include "parser.h"
#include "latc.h"
#include "string_ops.h"
#include "array_ops.h"
#include "builtin_methods.h"
#include "package.h"
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#ifndef __EMSCRIPTEN__
#include <pthread.h>
#endif
#include "memory.h"

/* In the StackVM dispatch loop, use the inline fast-path for value_free
 * to avoid function call overhead on primitives (int, float, bool, etc.) */
#define value_free(v) value_free_inline(v)

/* Native function pointer for StackVM builtins.
 * Args array is arg_count elements. Returns a LatValue result. */
typedef LatValue (*VMNativeFn)(LatValue *args, int arg_count);

/* Sentinel to distinguish native C functions from compiled closures. */
#define VM_NATIVE_MARKER ((struct Expr **)(uintptr_t)0x1)
#define VM_EXT_MARKER    ((struct Expr **)(uintptr_t)0x2)

/* String interning threshold: strings <= this length are interned after
 * concatenation or when loaded from the constant pool. */
#define INTERN_THRESHOLD 64

/* Try to intern a string value.  If the string is short enough (<= INTERN_THRESHOLD),
 * intern it and return a value with REGION_INTERNED.  Otherwise return the value
 * unchanged.  The original buffer is freed if interning succeeds. */
static inline LatValue stackvm_try_intern(LatValue v) {
    if (v.type != VAL_STR || v.region_id == REGION_INTERNED) return v;
    size_t len = v.as.str_len ? v.as.str_len : strlen(v.as.str_val);
    if (len > INTERN_THRESHOLD) return v;
    const char *interned = intern(v.as.str_val);
    /* Free the original if it was heap-allocated (REGION_NONE) */
    if (v.region_id == REGION_NONE) free(v.as.str_val);
    v.as.str_val = (char *)interned;
    v.region_id = REGION_INTERNED;
    return v;
}

/* ── Stack operations ── */

static void push(StackVM *vm, LatValue val) {
    if (vm->stack_top - vm->stack >= STACKVM_STACK_MAX) {
        fprintf(stderr, "fatal: StackVM stack overflow\n");
        exit(1);
    }
    *vm->stack_top = val;
    vm->stack_top++;
}

static LatValue pop(StackVM *vm) {
    vm->stack_top--;
    return *vm->stack_top;
}

static LatValue *stackvm_peek(StackVM *vm, int distance) { return &vm->stack_top[-1 - distance]; }

/* Get the source line for the current instruction in the topmost frame. */
static int stackvm_current_line(StackVM *vm) {
    if (vm->frame_count == 0) return 0;
    StackCallFrame *f = &vm->frames[vm->frame_count - 1];
    if (!f->chunk || !f->chunk->lines || f->chunk->lines_len == 0) return 0;
    size_t offset = (size_t)(f->ip - f->chunk->code);
    if (offset > 0) offset--; /* ip already advanced past the opcode */
    if (offset >= f->chunk->lines_len) offset = f->chunk->lines_len - 1;
    return f->chunk->lines[offset];
}

static StackVMResult runtime_error(StackVM *vm, const char *fmt, ...) {
    char *inner = NULL;
    va_list args;
    va_start(args, fmt);
    lat_vasprintf(&inner, fmt, args);
    va_end(args);
    vm->error = inner;
    return STACKVM_RUNTIME_ERROR;
}

/* Try to route a runtime error through exception handlers.
 * If a handler exists, unwinds to it, pushes the error string, and returns STACKVM_OK
 * (caller should `break` to continue the StackVM loop).
 * If no handler, returns STACKVM_RUNTIME_ERROR (caller should `return` the result). */
static StackVMResult stackvm_handle_error(StackVM *vm, StackCallFrame **frame_ptr, const char *fmt, ...) {
    char *inner = NULL;
    va_list args;
    va_start(args, fmt);
    lat_vasprintf(&inner, fmt, args);
    va_end(args);

    if (vm->handler_count > 0) {
        /* Caught by try/catch — pass raw error message without line prefix */
        StackExceptionHandler h = vm->handlers[--vm->handler_count];
        while (vm->frame_count - 1 > h.frame_index) vm->frame_count--;
        *frame_ptr = &vm->frames[vm->frame_count - 1];
        vm->stack_top = h.stack_top;
        (*frame_ptr)->ip = h.ip;
        push(vm, value_string(inner));
        free(inner);
        return STACKVM_OK;
    }

    /* Uncaught — store raw error (stack trace provides line info) */
    vm->error = inner;
    return STACKVM_RUNTIME_ERROR;
}

/* Like stackvm_handle_error but for native function errors that already have
 * a message in vm->error.  Does NOT prepend [line N] to match tree-walker
 * behaviour (native errors carry their own descriptive messages). */
static StackVMResult stackvm_handle_native_error(StackVM *vm, StackCallFrame **frame_ptr) {
    if (vm->handler_count > 0) {
        StackExceptionHandler h = vm->handlers[--vm->handler_count];
        while (vm->frame_count - 1 > h.frame_index) vm->frame_count--;
        *frame_ptr = &vm->frames[vm->frame_count - 1];
        vm->stack_top = h.stack_top;
        (*frame_ptr)->ip = h.ip;
        push(vm, value_string(vm->error));
        free(vm->error);
        vm->error = NULL;
        return STACKVM_OK;
    }
    /* Uncaught — vm->error already set by native function, no line prefix */
    return STACKVM_RUNTIME_ERROR;
}

static bool is_falsy(LatValue *v) {
    return v->type == VAL_NIL || (v->type == VAL_BOOL && !v->as.bool_val) || v->type == VAL_UNIT;
}

/* ── Upvalue management ── */

static ObjUpvalue *new_upvalue(LatValue *slot) {
    ObjUpvalue *uv = calloc(1, sizeof(ObjUpvalue));
    if (!uv) return NULL;
    uv->location = slot;
    uv->closed = value_nil();
    uv->next = NULL;
    return uv;
}

static ObjUpvalue *capture_upvalue(StackVM *vm, LatValue *local) {
    ObjUpvalue *prev = NULL;
    ObjUpvalue *uv = vm->open_upvalues;

    while (uv != NULL && uv->location > local) {
        prev = uv;
        uv = uv->next;
    }

    if (uv != NULL && uv->location == local) return uv;

    ObjUpvalue *created = new_upvalue(local);
    created->next = uv;

    if (prev == NULL) vm->open_upvalues = created;
    else prev->next = created;

    return created;
}

/* VM_NATIVE_MARKER and VM_EXT_MARKER defined at top of file */

/* Fast-path clone: flat copy for primitives, strdup for strings,
 * full deep clone only for compound types. */
static inline LatValue value_clone_fast(const LatValue *src) {
    switch (src->type) {
        case VAL_INT:
        case VAL_FLOAT:
        case VAL_BOOL:
        case VAL_UNIT:
        case VAL_NIL:
        case VAL_RANGE: {
            LatValue v = *src;
            v.region_id = REGION_NONE;
            return v;
        }
        case VAL_STR: {
            if (src->region_id == REGION_INTERNED) return *src;
            /* Use cached length when available to avoid strlen */
            size_t slen = src->as.str_len ? src->as.str_len : strlen(src->as.str_val);
            /* Intern short strings on clone to avoid strdup and enable
             * pointer-equality comparisons. */
            if (slen <= INTERN_THRESHOLD) return value_string_interned(src->as.str_val);
            LatValue v = *src;
            v.as.str_val = strdup(src->as.str_val);
            v.as.str_len = slen; /* preserve cached length */
            v.region_id = REGION_NONE;
            return v;
        }
        case VAL_BUFFER: {
            LatValue v = *src;
            v.as.buffer.data = malloc(src->as.buffer.cap);
            if (!v.as.buffer.data) return value_unit();
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
                src->as.closure.default_values != VM_NATIVE_MARKER && src->as.closure.default_values != VM_EXT_MARKER) {
                /* Bytecode closure: shallow copy + strdup param_names */
                LatValue v = *src;
                if (src->as.closure.param_names) {
                    v.as.closure.param_names = malloc(src->as.closure.param_count * sizeof(char *));
                    if (!v.as.closure.param_names) return value_unit();
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
            if (!v.as.strct.field_names) return value_unit();
            v.as.strct.field_values = malloc(fc * sizeof(LatValue));
            if (!v.as.strct.field_values) return value_unit();
            for (size_t i = 0; i < fc; i++) {
                v.as.strct.field_names[i] = src->as.strct.field_names[i]; /* interned, shared */
                v.as.strct.field_values[i] = value_clone_fast(&src->as.strct.field_values[i]);
            }
            if (src->as.strct.field_phases) {
                v.as.strct.field_phases = malloc(fc * sizeof(PhaseTag));
                if (v.as.strct.field_phases)
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
            if (!v.as.array.elems) return value_unit();
            for (size_t i = 0; i < len; i++) v.as.array.elems[i] = value_clone_fast(&src->as.array.elems[i]);
            v.region_id = REGION_NONE;
            return v;
        }
        case VAL_TUPLE: {
            LatValue v = *src;
            size_t len = src->as.tuple.len;
            v.as.tuple.elems = malloc(len * sizeof(LatValue));
            if (!v.as.tuple.elems) return value_unit();
            for (size_t i = 0; i < len; i++) v.as.tuple.elems[i] = value_clone_fast(&src->as.tuple.elems[i]);
            v.region_id = REGION_NONE;
            return v;
        }
        case VAL_MAP: {
            LatValue v = value_map_new();
            v.phase = src->phase; /* Preserve phase tag */
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
                if (!v.as.map.key_phases) {
                    v.region_id = REGION_NONE;
                    return v;
                }
                *v.as.map.key_phases = lat_map_new(sizeof(PhaseTag));
                for (size_t i = 0; i < ksrc->cap; i++) {
                    if (ksrc->entries[i].state == MAP_OCCUPIED)
                        lat_map_set(v.as.map.key_phases, ksrc->entries[i].key, ksrc->entries[i].value);
                }
            }
            v.region_id = REGION_NONE;
            return v;
        }
        default: return value_deep_clone(src);
    }
}

static void close_upvalues(StackVM *vm, LatValue *last) {
    while (vm->open_upvalues != NULL && vm->open_upvalues->location >= last) {
        ObjUpvalue *uv = vm->open_upvalues;
        uv->closed = value_clone_fast(uv->location);
        uv->location = &uv->closed;
        vm->open_upvalues = uv->next;
    }
}

/* Create a string value allocated in the ephemeral arena.
 * The caller passes a malloc'd string; it's copied into the arena and the original is freed. */
__attribute__((unused)) static inline LatValue stackvm_ephemeral_string(StackVM *vm, char *s) {
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
static inline LatValue stackvm_ephemeral_concat(StackVM *vm, const char *a, size_t la, const char *b, size_t lb) {
    size_t total = la + lb + 1;
    size_t result_len = la + lb;
    if (vm->ephemeral) {
        char *buf = bump_alloc(vm->ephemeral, total);
        memcpy(buf, a, la);
        memcpy(buf + la, b, lb);
        buf[result_len] = '\0';
        vm->ephemeral_on_stack = true;
        LatValue v;
        v.type = VAL_STR;
        v.phase = VTAG_UNPHASED;
        v.region_id = REGION_EPHEMERAL;
        v.as.str_val = buf;
        v.as.str_len = result_len;
        return v;
    }
    char *buf = malloc(total);
    if (!buf) return value_unit();
    memcpy(buf, a, la);
    memcpy(buf + la, b, lb);
    buf[result_len] = '\0';
    return value_string_owned_len(buf, result_len);
}

/* If value is ephemeral, promote to malloc (or intern if short string). */
static inline void stackvm_promote_value(LatValue *v) {
    if (v->region_id == REGION_EPHEMERAL) {
        /* Try interning short strings to avoid a full deep-clone.
         * Interned strings are long-lived (owned by intern table). */
        size_t slen = (v->type == VAL_STR && v->as.str_len) ? v->as.str_len : 0;
        if (v->type == VAL_STR && (slen ? slen : strlen(v->as.str_val)) <= INTERN_THRESHOLD) {
            const char *interned = intern(v->as.str_val);
            v->as.str_val = (char *)interned;
            v->region_id = REGION_INTERNED;
        } else {
            *v = value_deep_clone(v);
        }
    }
}

/* Promote all ephemeral values in the current frame before entering a new
 * bytecode frame, so the callee's OP_RESET_EPHEMERAL won't invalidate
 * anything in the caller's frame. */
static inline void stackvm_promote_frame_ephemerals(StackVM *vm, StackCallFrame *frame) {
    if (vm->ephemeral_on_stack) {
        for (LatValue *slot = frame->slots; slot < vm->stack_top; slot++) stackvm_promote_value(slot);
        vm->ephemeral_on_stack = false;
    }
}

/* ── Closure invocation helper for builtins ──
 * Calls a compiled closure from within the StackVM using a temporary wrapper chunk.
 * Returns the closure's return value. */
static LatValue stackvm_call_closure(StackVM *vm, LatValue *closure, LatValue *args, int arg_count) {
    if (closure->type != VAL_CLOSURE || closure->as.closure.native_fn == NULL ||
        closure->as.closure.default_values == VM_NATIVE_MARKER) {
        return value_nil();
    }

    /* Reuse the pre-built wrapper chunk, patching the arg count */
    vm->call_wrapper.code[1] = (uint8_t)arg_count;

    /* Push closure + args onto the stack for the wrapper to invoke */
    push(vm, value_clone_fast(closure));
    for (int i = 0; i < arg_count; i++) push(vm, value_clone_fast(&args[i]));

    LatValue result;
    stackvm_run(vm, &vm->call_wrapper, &result);
    return result;
}

/* BuiltinCallback adapter for stackvm: closure is a LatValue*, ctx is a StackVM* */
static LatValue stackvm_builtin_callback(void *closure, LatValue *args, int arg_count, void *ctx) {
    return stackvm_call_closure((StackVM *)ctx, (LatValue *)closure, args, arg_count);
}

static bool stackvm_find_local_value(StackVM *vm, const char *name, LatValue *out) {
    if (vm->frame_count == 0) return false;
    StackCallFrame *frame = &vm->frames[vm->frame_count - 1];
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

/* Thin wrapper: delegate to runtime's record_history */
static inline void stackvm_record_history(StackVM *vm, const char *name, LatValue *val) {
    rt_record_history(vm->rt, name, val);
}
/* ── Phase system: variable access by name helpers ── */

static bool stackvm_get_var_by_name(StackVM *vm, const char *name, LatValue *out) {
    /* Check current frame's locals first */
    if (vm->frame_count > 0) {
        StackCallFrame *frame = &vm->frames[vm->frame_count - 1];
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

static bool stackvm_set_var_by_name(StackVM *vm, const char *name, LatValue val) {
    /* Check current frame's locals first */
    if (vm->frame_count > 0) {
        StackCallFrame *frame = &vm->frames[vm->frame_count - 1];
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
static void stackvm_write_back(StackVM *vm, StackCallFrame *frame, uint8_t loc_type, uint8_t loc_slot, const char *name,
                               LatValue val) {
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
        case 2: /* global */ env_set(vm->env, name, value_deep_clone(&val)); break;
    }
    stackvm_record_history(vm, name, &val);
}

/* ── Phase system wrappers: delegate to runtime, bridge errors to StackVM ── */

static StackVMResult stackvm_fire_reactions(StackVM *vm, StackCallFrame **frame_ptr, const char *name,
                                            const char *phase) {
    (void)frame_ptr;
    rt_fire_reactions(vm->rt, name, phase);
    if (vm->rt->error) {
        vm->error = vm->rt->error;
        vm->rt->error = NULL;
        return STACKVM_RUNTIME_ERROR;
    }
    return STACKVM_OK;
}

static StackVMResult stackvm_freeze_cascade(StackVM *vm, StackCallFrame **frame_ptr, const char *target_name) {
    (void)frame_ptr;
    rt_freeze_cascade(vm->rt, target_name);
    if (vm->rt->error) {
        vm->error = vm->rt->error;
        vm->rt->error = NULL;
        return STACKVM_RUNTIME_ERROR;
    }
    return STACKVM_OK;
}

static char *stackvm_validate_seeds(StackVM *vm, const char *name, LatValue *val, bool consume) {
    return rt_validate_seeds(vm->rt, name, val, consume);
}
/* ── StackVM lifecycle ── */

void stackvm_init(StackVM *vm, LatRuntime *rt) {
    memset(vm, 0, sizeof(StackVM));
    vm->rt = rt;
    vm->stack_top = vm->stack;
    vm->env = rt->env; /* cached pointer — runtime owns the env */
    vm->error = NULL;
    vm->open_upvalues = NULL;
    vm->handler_count = 0;
    vm->defer_count = 0;
    vm->struct_meta = rt->struct_meta; /* cached pointer — runtime owns struct_meta */
    vm->fn_chunks = NULL;
    vm->fn_chunk_count = 0;
    vm->fn_chunk_cap = 0;
    vm->module_cache = lat_map_new(sizeof(LatValue));
    vm->ephemeral = bump_arena_new();

    /* Pre-build the call wrapper chunk: [OP_CALL, 0, OP_RETURN] */
    memset(&vm->call_wrapper, 0, sizeof(Chunk));
    vm->call_wrapper.code = malloc(3);
    if (!vm->call_wrapper.code) return;
    vm->call_wrapper.code[0] = OP_CALL;
    vm->call_wrapper.code[1] = 0;
    vm->call_wrapper.code[2] = OP_RETURN;
    vm->call_wrapper.code_len = 3;
    vm->call_wrapper.code_cap = 3;
    vm->call_wrapper.lines = calloc(3, sizeof(int));
    if (!vm->call_wrapper.lines) return;
    vm->call_wrapper.lines_len = 3;
    vm->call_wrapper.lines_cap = 3;

    /* Initialize GC (disabled by default; caller enables via gc.enabled) */
    gc_init(&vm->gc);
}

void stackvm_free(StackVM *vm) {
    /* Clear thread-local runtime pointer if it still refers to this VM's runtime,
     * preventing dangling pointer after the caller's stack-allocated LatRuntime dies. */
    if (lat_runtime_current() == vm->rt) lat_runtime_set_current(NULL);

    /* Free any remaining stack values */
    while (vm->stack_top > vm->stack) {
        vm->stack_top--;
        value_free(vm->stack_top);
    }
    /* env and struct_meta are owned by the runtime — don't free here */
    free(vm->error);

    /* Free open upvalues */
    ObjUpvalue *uv = vm->open_upvalues;
    while (uv) {
        ObjUpvalue *next = uv->next;
        if (uv->location == &uv->closed) value_free(&uv->closed);
        free(uv);
        uv = next;
    }

    /* Free upvalue arrays in frames */
    for (size_t i = 0; i < vm->frame_count; i++) {
        StackCallFrame *f = &vm->frames[i];
        for (size_t j = 0; j < f->upvalue_count; j++) {
            if (f->upvalues[j] && f->upvalues[j]->location == &f->upvalues[j]->closed)
                value_free(&f->upvalues[j]->closed);
            free(f->upvalues[j]);
        }
        free(f->upvalues);
    }

    /* Free function chunks */
    for (size_t i = 0; i < vm->fn_chunk_count; i++) chunk_free(vm->fn_chunks[i]);
    free(vm->fn_chunks);

    /* Free per-StackVM module cache */
    for (size_t i = 0; i < vm->module_cache.cap; i++) {
        if (vm->module_cache.entries[i].state == MAP_OCCUPIED) {
            LatValue *v = (LatValue *)vm->module_cache.entries[i].value;
            value_free(v);
        }
    }
    lat_map_free(&vm->module_cache);

    /* Phase system arrays are owned by the runtime — don't free here */

    bump_arena_free(vm->ephemeral);

    /* Free call wrapper (inline Chunk, not heap-allocated) */
    free(vm->call_wrapper.code);
    free(vm->call_wrapper.lines);
    pic_table_free(&vm->call_wrapper.pic);

    /* Free GC-tracked objects */
    gc_free(&vm->gc);
}

void stackvm_print_stack_trace(StackVM *vm) {
    if (vm->frame_count <= 1) return; /* No trace for top-level errors */
    fprintf(stderr, "stack trace (most recent call last):\n");
    for (size_t i = 0; i < vm->frame_count; i++) {
        StackCallFrame *f = &vm->frames[i];
        if (!f->chunk) continue;
        size_t offset = (size_t)(f->ip - f->chunk->code);
        if (offset > 0) offset--;
        int line = 0;
        if (f->chunk->lines && offset < f->chunk->lines_len) line = f->chunk->lines[offset];
        const char *name = f->chunk->name;
        if (name && name[0]) fprintf(stderr, "  [line %d] in %s()\n", line, name);
        else if (i == 0) fprintf(stderr, "  [line %d] in <script>\n", line);
        else fprintf(stderr, "  [line %d] in <closure>\n", line);
    }
}

/* ── Concurrency infrastructure ── */

void stackvm_track_chunk(StackVM *vm, Chunk *ch) {
    if (vm->fn_chunk_count >= vm->fn_chunk_cap) {
        vm->fn_chunk_cap = vm->fn_chunk_cap ? vm->fn_chunk_cap * 2 : 8;
        vm->fn_chunks = realloc(vm->fn_chunks, vm->fn_chunk_cap * sizeof(Chunk *));
    }
    vm->fn_chunks[vm->fn_chunk_count++] = ch;
}

#ifndef __EMSCRIPTEN__

typedef struct {
    Chunk *chunk;      /* compiled spawn body (parent owns via fn_chunks) */
    StackVM *child_vm; /* independent StackVM for thread */
    char *error;       /* NULL on success */
    pthread_t thread;
} VMSpawnTask;

StackVM *stackvm_clone_for_thread(StackVM *parent) {
    /* Create a child runtime with cloned env + fresh phase arrays */
    LatRuntime *child_rt = calloc(1, sizeof(LatRuntime));
    if (!child_rt) return NULL;
    child_rt->env = env_clone(parent->rt->env);
    child_rt->struct_meta = parent->rt->struct_meta; /* shared read-only */
    child_rt->script_dir = parent->rt->script_dir ? strdup(parent->rt->script_dir) : NULL;
    child_rt->prog_argc = parent->rt->prog_argc;
    child_rt->prog_argv = parent->rt->prog_argv;
    child_rt->module_cache = lat_map_new(sizeof(LatValue));
    child_rt->required_files = lat_map_new(sizeof(bool));
    child_rt->loaded_extensions = lat_map_new(sizeof(LatValue));

    StackVM *child = calloc(1, sizeof(StackVM));
    if (!child) return NULL;
    child->rt = child_rt;
    child->stack_top = child->stack;
    child->env = child_rt->env;
    child->error = NULL;
    child->open_upvalues = NULL;
    child->handler_count = 0;
    child->defer_count = 0;
    child->struct_meta = child_rt->struct_meta;
    child->fn_chunks = NULL;
    child->fn_chunk_count = 0;
    child->fn_chunk_cap = 0;
    child->module_cache = lat_map_new(sizeof(LatValue));
    child->ephemeral = bump_arena_new();

    /* Pre-build the call wrapper chunk */
    memset(&child->call_wrapper, 0, sizeof(Chunk));
    child->call_wrapper.code = malloc(3);
    if (!child->call_wrapper.code) return NULL;
    child->call_wrapper.code[0] = OP_CALL;
    child->call_wrapper.code[1] = 0;
    child->call_wrapper.code[2] = OP_RETURN;
    child->call_wrapper.code_len = 3;
    child->call_wrapper.code_cap = 3;
    child->call_wrapper.lines = calloc(3, sizeof(int));
    if (!child->call_wrapper.lines) return NULL;
    child->call_wrapper.lines_len = 3;
    child->call_wrapper.lines_cap = 3;

    return child;
}

void stackvm_free_child(StackVM *child) {
    /* Free stack values */
    while (child->stack_top > child->stack) {
        child->stack_top--;
        value_free(child->stack_top);
    }
    /* env is a cached pointer to child->rt->env — freed by runtime below */
    free(child->error);

    /* Free open upvalues */
    ObjUpvalue *uv = child->open_upvalues;
    while (uv) {
        ObjUpvalue *next = uv->next;
        if (uv->location == &uv->closed) value_free(&uv->closed);
        free(uv);
        uv = next;
    }

    /* Free upvalue arrays in frames */
    for (size_t i = 0; i < child->frame_count; i++) {
        StackCallFrame *f = &child->frames[i];
        for (size_t j = 0; j < f->upvalue_count; j++) {
            if (f->upvalues[j] && f->upvalues[j]->location == &f->upvalues[j]->closed)
                value_free(&f->upvalues[j]->closed);
            free(f->upvalues[j]);
        }
        free(f->upvalues);
    }

    /* Free child-owned fn_chunks */
    for (size_t i = 0; i < child->fn_chunk_count; i++) chunk_free(child->fn_chunks[i]);
    free(child->fn_chunks);

    /* Free per-StackVM module cache */
    lat_map_free(&child->module_cache);
    /* struct_meta is shared — parent runtime owns it */
    bump_arena_free(child->ephemeral);

    /* Free call wrapper */
    free(child->call_wrapper.code);
    free(child->call_wrapper.lines);

    /* Free child runtime (env + caches) */
    LatRuntime *crt = child->rt;
    if (crt) {
        if (crt->env) env_free(crt->env);
        lat_map_free(&crt->module_cache);
        lat_map_free(&crt->required_files);
        lat_map_free(&crt->loaded_extensions);
        free(crt->script_dir);
        free(crt);
    }

    free(child);
}

/* Export current frame's live locals into child's env as globals,
 * so re-compiled code can access them via OP_GET_GLOBAL. */
static void stackvm_export_locals_to_env(StackVM *parent, StackVM *child) {
    for (size_t fi = 0; fi < parent->frame_count; fi++) {
        StackCallFrame *f = &parent->frames[fi];
        if (!f->chunk) continue;
        size_t local_count = (size_t)(parent->stack_top - f->slots);
        if (fi + 1 < parent->frame_count) local_count = (size_t)(parent->frames[fi + 1].slots - f->slots);
        for (size_t slot = 0; slot < local_count; slot++) {
            if (slot < f->chunk->local_name_cap && f->chunk->local_names[slot]) {
                env_define(child->env, f->chunk->local_names[slot], value_deep_clone(&f->slots[slot]));
            }
        }
    }
}

static void *stackvm_spawn_thread_fn(void *arg) {
    VMSpawnTask *task = arg;
    lat_runtime_set_current(task->child_vm->rt);
    task->child_vm->rt->active_vm = task->child_vm;

    /* Set up thread-local heap */
    DualHeap *heap = dual_heap_new();
    value_set_heap(heap);
    value_set_arena(NULL);

    LatValue result;
    StackVMResult r = stackvm_run(task->child_vm, task->chunk, &result);
    if (r != STACKVM_OK) {
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
static const char *stackvm_find_pressure(StackVM *vm, const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < vm->rt->pressure_count; i++) {
        if (strcmp(vm->rt->pressures[i].name, name) == 0) return vm->rt->pressures[i].mode;
    }
    return NULL;
}

/* ── Pre-computed djb2 hashes for builtin method names ── */
#define MHASH_all          0x0b885ddeu
#define MHASH_any          0x0b885e2du
#define MHASH_bytes        0x0f30b64cu
#define MHASH_chars        0x0f392d36u
#define MHASH_chunk        0x0f3981beu
#define MHASH_close        0x0f3b9a5bu
#define MHASH_contains     0x42aa8264u
#define MHASH_count        0x0f3d586eu
#define MHASH_difference   0x52a92470u
#define MHASH_drop         0x7c95d91au
#define MHASH_each         0x7c961b96u
#define MHASH_ends_with    0x9079bb6au
#define MHASH_entries      0x6b84747fu
#define MHASH_enum_name    0x9f13be1au
#define MHASH_enumerate    0x9f82838bu
#define MHASH_filter       0xfd7675abu
#define MHASH_find         0x7c96cb66u
#define MHASH_first        0x0f704b8du
#define MHASH_flat         0x7c96d68cu
#define MHASH_flat_map     0x022d3129u
#define MHASH_flatten      0xb27dd5f3u
#define MHASH_for_each     0x0f4aaefcu
#define MHASH_get          0x0b887685u
#define MHASH_group_by     0xdd0fdaecu
#define MHASH_has          0x0b887a41u
#define MHASH_index_of     0x66e4af51u
#define MHASH_insert       0x04d4029au
#define MHASH_intersection 0x40c04d3cu
#define MHASH_is_empty     0xdc1854cfu
#define MHASH_is_subset    0x805437d6u
#define MHASH_is_superset  0x05f3913bu
#define MHASH_is_variant   0x443eb735u
#define MHASH_join         0x7c9915d5u
#define MHASH_keys         0x7c9979c1u
#define MHASH_last         0x7c99f459u
#define MHASH_len          0x0b888bc4u
#define MHASH_length       0x0b2deac7u
#define MHASH_map          0x0b888f83u
#define MHASH_max          0x0b888f8bu
#define MHASH_merge        0x0fecc3f5u
#define MHASH_min          0x0b889089u
#define MHASH_pad_left     0xf3895c84u
#define MHASH_pad_right    0x6523b4b7u
#define MHASH_payload      0x9c4949cfu
#define MHASH_pop          0x0b889e14u
#define MHASH_push         0x7c9c7ae5u
#define MHASH_recv         0x7c9d4d95u
#define MHASH_reduce       0x19279c1du
#define MHASH_add          0x0b885cceu
#define MHASH_remove       0x192c7473u
#define MHASH_remove_at    0xd988a4a7u
#define MHASH_repeat       0x192dec66u
#define MHASH_replace      0x3eef4e01u
#define MHASH_reverse      0x3f5854c1u
#define MHASH_send         0x7c9ddb4fu
#define MHASH_set          0x0b88a991u
#define MHASH_slice        0x105d06d5u
#define MHASH_sort         0x7c9e066du
#define MHASH_sort_by      0xa365ac87u
#define MHASH_split        0x105f45f1u
#define MHASH_starts_with  0xf5ef8361u
#define MHASH_substring    0xcc998606u
#define MHASH_sum          0x0b88ab9au
#define MHASH_tag          0x0b88ad41u
#define MHASH_take         0x7c9e564au
#define MHASH_to_array     0xcebde966u
#define MHASH_to_lower     0xcf836790u
#define MHASH_to_upper     0xd026b2b3u
#define MHASH_trim         0x7c9e9e61u
#define MHASH_trim_end     0xcdcebb17u
#define MHASH_trim_start   0x7d6a808eu
#define MHASH_union        0x1082522eu
#define MHASH_unique       0x20cca1bcu
#define MHASH_values       0x22383ff5u
#define MHASH_variant_name 0xb2b2b8bau
#define MHASH_zip          0x0b88c7d8u
/* Ref methods */
#define MHASH_deref      0x0f49e72bu
#define MHASH_inner_type 0xdf644222u
/* Buffer methods */
#define MHASH_push_u16   0x1aaf75a0u
#define MHASH_push_u32   0x1aaf75deu
#define MHASH_read_u8    0x3ddb750du
#define MHASH_write_u8   0x931616bcu
#define MHASH_read_u16   0xf94a15fcu
#define MHASH_write_u16  0xf5d8ed8bu
#define MHASH_read_u32   0xf94a163au
#define MHASH_write_u32  0xf5d8edc9u
#define MHASH_capitalize 0xee09978bu
#define MHASH_title_case 0x4b7027c2u
#define MHASH_snake_case 0xb7f6c232u
#define MHASH_camel_case 0xe2889d82u
#define MHASH_kebab_case 0x62be3b95u
#define MHASH_read_i8    0x3ddb7381u
#define MHASH_read_i16   0xf949e2f0u
#define MHASH_read_i32   0xf949e32eu
#define MHASH_read_f32   0xf949d66bu
#define MHASH_read_f64   0xf949d6d0u
#define MHASH_clear      0x0f3b6d8cu
#define MHASH_fill       0x7c96cb2cu
#define MHASH_resize     0x192fa5b7u
#define MHASH_to_string  0xd09c437eu
#define MHASH_to_hex     0x1e83ed8cu
#define MHASH_capacity   0x104ec913u

static inline uint32_t method_hash(const char *s) {
    uint32_t h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

/* Returns true if the builtin method is "simple" — no user closures executed,
 * safe for direct-pointer mutation without clone-mutate-writeback. */
static inline bool stackvm_invoke_builtin_is_simple(uint32_t mhash) {
    return !(mhash == MHASH_map || mhash == MHASH_filter || mhash == MHASH_reduce || mhash == MHASH_each ||
             mhash == MHASH_sort || mhash == MHASH_find || mhash == MHASH_any || mhash == MHASH_all);
}

/* Resolve the PIC handler ID for a given (type, method_hash) pair.
 * Returns 0 if no builtin matches, or a PIC_xxx handler ID.
 * This is a pure lookup: no execution, no side effects. */
static uint16_t pic_resolve_builtin_id(uint8_t type_tag, uint32_t mhash) {
    switch (type_tag) {
        case VAL_ARRAY:
            if (mhash == MHASH_len) return PIC_ARRAY_LEN;
            if (mhash == MHASH_length) return PIC_ARRAY_LENGTH;
            if (mhash == MHASH_push) return PIC_ARRAY_PUSH;
            if (mhash == MHASH_pop) return PIC_ARRAY_POP;
            if (mhash == MHASH_contains) return PIC_ARRAY_CONTAINS;
            if (mhash == MHASH_enumerate) return PIC_ARRAY_ENUMERATE;
            if (mhash == MHASH_reverse) return PIC_ARRAY_REVERSE;
            if (mhash == MHASH_join) return PIC_ARRAY_JOIN;
            if (mhash == MHASH_map) return PIC_ARRAY_MAP;
            if (mhash == MHASH_filter) return PIC_ARRAY_FILTER;
            if (mhash == MHASH_reduce) return PIC_ARRAY_REDUCE;
            if (mhash == MHASH_each) return PIC_ARRAY_EACH;
            if (mhash == MHASH_sort) return PIC_ARRAY_SORT;
            if (mhash == MHASH_for_each) return PIC_ARRAY_FOR_EACH;
            if (mhash == MHASH_find) return PIC_ARRAY_FIND;
            if (mhash == MHASH_any) return PIC_ARRAY_ANY;
            if (mhash == MHASH_all) return PIC_ARRAY_ALL;
            if (mhash == MHASH_flat) return PIC_ARRAY_FLAT;
            if (mhash == MHASH_flatten) return PIC_ARRAY_FLATTEN;
            if (mhash == MHASH_slice) return PIC_ARRAY_SLICE;
            if (mhash == MHASH_take) return PIC_ARRAY_TAKE;
            if (mhash == MHASH_drop) return PIC_ARRAY_DROP;
            if (mhash == MHASH_index_of) return PIC_ARRAY_INDEX_OF;
            if (mhash == MHASH_zip) return PIC_ARRAY_ZIP;
            if (mhash == MHASH_unique) return PIC_ARRAY_UNIQUE;
            if (mhash == MHASH_remove_at) return PIC_ARRAY_REMOVE_AT;
            if (mhash == MHASH_insert) return PIC_ARRAY_INSERT;
            if (mhash == MHASH_first) return PIC_ARRAY_FIRST;
            if (mhash == MHASH_last) return PIC_ARRAY_LAST;
            if (mhash == MHASH_sum) return PIC_ARRAY_SUM;
            if (mhash == MHASH_min) return PIC_ARRAY_MIN;
            if (mhash == MHASH_max) return PIC_ARRAY_MAX;
            if (mhash == MHASH_chunk) return PIC_ARRAY_CHUNK;
            if (mhash == MHASH_flat_map) return PIC_ARRAY_FLAT_MAP;
            if (mhash == MHASH_sort_by) return PIC_ARRAY_SORT_BY;
            if (mhash == MHASH_group_by) return PIC_ARRAY_GROUP_BY;
            break;
        case VAL_STR:
            if (mhash == MHASH_len) return PIC_STRING_LEN;
            if (mhash == MHASH_length) return PIC_STRING_LENGTH;
            if (mhash == MHASH_split) return PIC_STRING_SPLIT;
            if (mhash == MHASH_trim) return PIC_STRING_TRIM;
            if (mhash == MHASH_to_upper) return PIC_STRING_TO_UPPER;
            if (mhash == MHASH_to_lower) return PIC_STRING_TO_LOWER;
            if (mhash == MHASH_starts_with) return PIC_STRING_STARTS_WITH;
            if (mhash == MHASH_ends_with) return PIC_STRING_ENDS_WITH;
            if (mhash == MHASH_replace) return PIC_STRING_REPLACE;
            if (mhash == MHASH_contains) return PIC_STRING_CONTAINS;
            if (mhash == MHASH_chars) return PIC_STRING_CHARS;
            if (mhash == MHASH_bytes) return PIC_STRING_BYTES;
            if (mhash == MHASH_reverse) return PIC_STRING_REVERSE;
            if (mhash == MHASH_repeat) return PIC_STRING_REPEAT;
            if (mhash == MHASH_pad_left) return PIC_STRING_PAD_LEFT;
            if (mhash == MHASH_pad_right) return PIC_STRING_PAD_RIGHT;
            if (mhash == MHASH_count) return PIC_STRING_COUNT;
            if (mhash == MHASH_is_empty) return PIC_STRING_IS_EMPTY;
            if (mhash == MHASH_index_of) return PIC_STRING_INDEX_OF;
            if (mhash == MHASH_substring) return PIC_STRING_SUBSTRING;
            if (mhash == MHASH_trim_start) return PIC_STRING_TRIM_START;
            if (mhash == MHASH_trim_end) return PIC_STRING_TRIM_END;
            if (mhash == MHASH_capitalize) return PIC_STRING_CAPITALIZE;
            if (mhash == MHASH_title_case) return PIC_STRING_TITLE_CASE;
            if (mhash == MHASH_snake_case) return PIC_STRING_SNAKE_CASE;
            if (mhash == MHASH_camel_case) return PIC_STRING_CAMEL_CASE;
            if (mhash == MHASH_kebab_case) return PIC_STRING_KEBAB_CASE;
            break;
        case VAL_MAP:
            if (mhash == MHASH_len) return PIC_MAP_LEN;
            if (mhash == MHASH_length) return PIC_MAP_LENGTH;
            if (mhash == MHASH_keys) return PIC_MAP_KEYS;
            if (mhash == MHASH_values) return PIC_MAP_VALUES;
            if (mhash == MHASH_entries) return PIC_MAP_ENTRIES;
            if (mhash == MHASH_get) return PIC_MAP_GET;
            if (mhash == MHASH_has) return PIC_MAP_HAS;
            if (mhash == MHASH_remove) return PIC_MAP_REMOVE;
            if (mhash == MHASH_merge) return PIC_MAP_MERGE;
            if (mhash == MHASH_set) return PIC_MAP_SET;
            if (mhash == MHASH_contains) return PIC_MAP_CONTAINS;
            if (mhash == MHASH_for_each) return PIC_ARRAY_FOR_EACH;
            if (mhash == MHASH_filter) return PIC_ARRAY_FILTER;
            if (mhash == MHASH_map) return PIC_ARRAY_MAP;
            break;
        case VAL_SET:
            if (mhash == MHASH_has) return PIC_SET_HAS;
            if (mhash == MHASH_add) return PIC_SET_ADD;
            if (mhash == MHASH_remove) return PIC_SET_REMOVE;
            if (mhash == MHASH_len) return PIC_SET_LEN;
            if (mhash == MHASH_length) return PIC_SET_LENGTH;
            if (mhash == MHASH_to_array) return PIC_SET_TO_ARRAY;
            if (mhash == MHASH_union) return PIC_SET_UNION;
            if (mhash == MHASH_intersection) return PIC_SET_INTERSECTION;
            if (mhash == MHASH_difference) return PIC_SET_DIFFERENCE;
            if (mhash == MHASH_is_subset) return PIC_SET_IS_SUBSET;
            if (mhash == MHASH_is_superset) return PIC_SET_IS_SUPERSET;
            if (mhash == MHASH_contains) return PIC_SET_CONTAINS;
            break;
        case VAL_ENUM:
            if (mhash == MHASH_tag) return PIC_ENUM_TAG;
            if (mhash == MHASH_payload) return PIC_ENUM_PAYLOAD;
            if (mhash == MHASH_variant_name) return PIC_ENUM_VARIANT_NAME;
            if (mhash == MHASH_enum_name) return PIC_ENUM_NAME;
            if (mhash == MHASH_is_variant) return PIC_ENUM_IS_VARIANT;
            break;
        case VAL_CHANNEL:
            if (mhash == MHASH_send) return PIC_CHANNEL_SEND;
            if (mhash == MHASH_recv) return PIC_CHANNEL_RECV;
            if (mhash == MHASH_close) return PIC_CHANNEL_CLOSE;
            break;
        case VAL_BUFFER:
            if (mhash == MHASH_len) return PIC_BUFFER_LEN;
            if (mhash == MHASH_length) return PIC_BUFFER_LENGTH;
            if (mhash == MHASH_push) return PIC_BUFFER_PUSH;
            if (mhash == MHASH_capacity) return PIC_BUFFER_CAPACITY;
            if (mhash == MHASH_push_u16) return PIC_BUFFER_PUSH_U16;
            if (mhash == MHASH_push_u32) return PIC_BUFFER_PUSH_U32;
            if (mhash == MHASH_read_u8) return PIC_BUFFER_READ_U8;
            if (mhash == MHASH_write_u8) return PIC_BUFFER_WRITE_U8;
            if (mhash == MHASH_read_u16) return PIC_BUFFER_READ_U16;
            if (mhash == MHASH_write_u16) return PIC_BUFFER_WRITE_U16;
            if (mhash == MHASH_read_u32) return PIC_BUFFER_READ_U32;
            if (mhash == MHASH_write_u32) return PIC_BUFFER_WRITE_U32;
            if (mhash == MHASH_slice) return PIC_BUFFER_SLICE;
            if (mhash == MHASH_clear) return PIC_BUFFER_CLEAR;
            if (mhash == MHASH_fill) return PIC_BUFFER_FILL;
            if (mhash == MHASH_resize) return PIC_BUFFER_RESIZE;
            if (mhash == MHASH_to_string) return PIC_BUFFER_TO_STRING;
            if (mhash == MHASH_to_array) return PIC_BUFFER_TO_ARRAY;
            if (mhash == MHASH_to_hex) return PIC_BUFFER_TO_HEX;
            if (mhash == MHASH_read_i8) return PIC_BUFFER_READ_I8;
            if (mhash == MHASH_read_i16) return PIC_BUFFER_READ_I16;
            if (mhash == MHASH_read_i32) return PIC_BUFFER_READ_I32;
            if (mhash == MHASH_read_f32) return PIC_BUFFER_READ_F32;
            if (mhash == MHASH_read_f64) return PIC_BUFFER_READ_F64;
            break;
        case VAL_RANGE:
            if (mhash == MHASH_len) return PIC_RANGE_CONTAINS;
            if (mhash == MHASH_length) return PIC_RANGE_CONTAINS;
            if (mhash == MHASH_contains) return PIC_RANGE_CONTAINS;
            if (mhash == MHASH_to_array) return PIC_RANGE_TO_ARRAY;
            break;
        case VAL_REF:
            if (mhash == MHASH_deref) return PIC_REF_DEREF;
            if (mhash == MHASH_inner_type) return PIC_REF_INNER_TYPE;
            /* Ref also proxies to inner type, so don't cache PIC_NOT_BUILTIN */
            return 0;
        default: break;
    }
    return 0;
}

static bool stackvm_invoke_builtin(StackVM *vm, LatValue *obj, const char *method, int arg_count,
                                   const char *var_name) {
    uint32_t mhash = method_hash(method);

    switch (obj->type) {
        /* Array methods */
        case VAL_ARRAY: {
            if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
                 (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
                arg_count == 0) {
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
                    if (var_name && obj->phase == VTAG_CRYSTAL)
                        lat_asprintf(&err, "cannot push to %s array '%s' (use thaw(%s) to make it mutable)", phase_name,
                                     var_name, var_name);
                    else lat_asprintf(&err, "cannot push to %s array", phase_name);
                    vm->error = err;
                    push(vm, value_unit());
                    return true;
                }
                /* Check pressure constraint */
                const char *pmode = stackvm_find_pressure(vm, var_name);
                if (pressure_blocks_grow(pmode)) {
                    LatValue val = pop(vm);
                    value_free(&val);
                    char *err = NULL;
                    lat_asprintf(&err, "pressurized (%s): cannot push to '%s'", pmode, var_name);
                    vm->error = err;
                    push(vm, value_unit());
                    return true;
                }
                LatValue val = pop(vm);
                stackvm_promote_value(&val);
                /* Mutate the array in-place */
                if (obj->as.array.len >= obj->as.array.cap) {
                    obj->as.array.cap = obj->as.array.cap ? obj->as.array.cap * 2 : 4;
                    obj->as.array.elems = realloc(obj->as.array.elems, obj->as.array.cap * sizeof(LatValue));
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
                    if (var_name && obj->phase == VTAG_CRYSTAL)
                        lat_asprintf(&err, "cannot pop from %s array '%s' (use thaw(%s) to make it mutable)",
                                     phase_name, var_name, var_name);
                    else lat_asprintf(&err, "cannot pop from %s array", phase_name);
                    vm->error = err;
                    push(vm, value_unit());
                    return true;
                }
                /* Check pressure constraint */
                const char *pmode = stackvm_find_pressure(vm, var_name);
                if (pressure_blocks_shrink(pmode)) {
                    char *err = NULL;
                    lat_asprintf(&err, "pressurized (%s): cannot pop from '%s'", pmode, var_name);
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
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_array_contains(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_enumerate && strcmp(method, "enumerate") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_array_enumerate(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_reverse && strcmp(method, "reverse") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_array_reverse(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_join && strcmp(method, "join") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_array_join(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_map && strcmp(method, "map") == 0 && arg_count == 1) {
                LatValue closure = pop(vm);
                char *err = NULL;
                LatValue result = builtin_array_map(obj, &closure, stackvm_builtin_callback, vm, &err);
                value_free(&closure);
                push(vm, result);
                return true;
            }
            if (mhash == MHASH_filter && strcmp(method, "filter") == 0 && arg_count == 1) {
                LatValue closure = pop(vm);
                char *err = NULL;
                LatValue result = builtin_array_filter(obj, &closure, stackvm_builtin_callback, vm, &err);
                value_free(&closure);
                push(vm, result);
                return true;
            }
            if (mhash == MHASH_reduce && strcmp(method, "reduce") == 0 && arg_count == 2) {
                LatValue acc = pop(vm);     /* second arg: initial value (TOS) */
                LatValue closure = pop(vm); /* first arg: closure */
                char *err = NULL;
                LatValue result = builtin_array_reduce(obj, &acc, true, &closure, stackvm_builtin_callback, vm, &err);
                value_free(&acc);
                value_free(&closure);
                push(vm, result);
                return true;
            }
            if (mhash == MHASH_each && strcmp(method, "each") == 0 && arg_count == 1) {
                LatValue closure = pop(vm);
                char *err = NULL;
                builtin_array_each(obj, &closure, stackvm_builtin_callback, vm, &err);
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
                if (!elems) return false;
                for (size_t i = 0; i < len; i++) elems[i] = value_deep_clone(&obj->as.array.elems[i]);

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
                            LatValue cmp = stackvm_call_closure(vm, &closure, args, 2);
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
                                double a_d = elems[j - 1].type == VAL_INT ? (double)elems[j - 1].as.int_val
                                                                          : elems[j - 1].as.float_val;
                                double b_d = key.type == VAL_INT ? (double)key.as.int_val : key.as.float_val;
                                should_swap = a_d > b_d;
                            } else if (elems[j - 1].type == VAL_STR && key.type == VAL_STR) {
                                should_swap = strcmp(elems[j - 1].as.str_val, key.as.str_val) > 0;
                            } else {
                                /* Mixed non-numeric types — error */
                                for (size_t fi = 0; fi < len; fi++) value_free(&elems[fi]);
                                free(elems);
                                vm->error = strdup("sort: cannot compare mixed types");
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
                char *err = NULL;
                builtin_array_each(obj, &closure, stackvm_builtin_callback, vm, &err);
                value_free(&closure);
                push(vm, value_unit());
                return true;
            }
            if (mhash == MHASH_find && strcmp(method, "find") == 0 && arg_count == 1) {
                LatValue closure = pop(vm);
                char *err = NULL;
                LatValue result = builtin_array_find(obj, &closure, stackvm_builtin_callback, vm, &err);
                value_free(&closure);
                push(vm, result);
                return true;
            }
            if (mhash == MHASH_any && strcmp(method, "any") == 0 && arg_count == 1) {
                LatValue closure = pop(vm);
                char *err = NULL;
                LatValue result = builtin_array_any(obj, &closure, stackvm_builtin_callback, vm, &err);
                value_free(&closure);
                push(vm, result);
                return true;
            }
            if (mhash == MHASH_all && strcmp(method, "all") == 0 && arg_count == 1) {
                LatValue closure = pop(vm);
                char *err = NULL;
                LatValue result = builtin_array_all(obj, &closure, stackvm_builtin_callback, vm, &err);
                value_free(&closure);
                push(vm, result);
                return true;
            }
            if (mhash == MHASH_flat && strcmp(method, "flat") == 0 && arg_count == 0) {
                push(vm, array_flat(obj));
                return true;
            }
            if (mhash == MHASH_flatten && strcmp(method, "flatten") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_array_flatten(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_slice && strcmp(method, "slice") == 0 && arg_count == 2) {
                LatValue end_v = pop(vm);
                LatValue start_v = pop(vm);
                char *err = NULL;
                LatValue r = array_slice(obj, start_v.as.int_val, end_v.as.int_val, &err);
                value_free(&start_v);
                value_free(&end_v);
                if (err) {
                    free(err);
                    push(vm, value_array(NULL, 0));
                } else push(vm, r);
                return true;
            }
            if (mhash == MHASH_take && strcmp(method, "take") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_array_take(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_drop && strcmp(method, "drop") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_array_drop(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_index_of && strcmp(method, "index_of") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_array_index_of(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_zip && strcmp(method, "zip") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_array_zip(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_unique && strcmp(method, "unique") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_array_unique(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_remove_at && strcmp(method, "remove_at") == 0 && arg_count == 1) {
                const char *pmode = stackvm_find_pressure(vm, var_name);
                if (pressure_blocks_shrink(pmode)) {
                    LatValue idx_v = pop(vm);
                    value_free(&idx_v);
                    char *err = NULL;
                    lat_asprintf(&err, "pressurized (%s): cannot remove_at from '%s'", pmode, var_name);
                    vm->error = err;
                    push(vm, value_unit());
                    return true;
                }
                LatValue idx_v = pop(vm);
                int64_t idx = idx_v.as.int_val;
                value_free(&idx_v);
                if (idx < 0 || (size_t)idx >= obj->as.array.len) {
                    push(vm, value_nil());
                    return true;
                }
                LatValue removed = obj->as.array.elems[(size_t)idx];
                memmove(&obj->as.array.elems[(size_t)idx], &obj->as.array.elems[(size_t)idx + 1],
                        (obj->as.array.len - (size_t)idx - 1) * sizeof(LatValue));
                obj->as.array.len--;
                push(vm, removed);
                return true;
            }
            if (mhash == MHASH_chunk && strcmp(method, "chunk") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_array_chunk(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_sum && strcmp(method, "sum") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_array_sum(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_min && strcmp(method, "min") == 0 && arg_count == 0) {
                char *err = NULL;
                LatValue r = builtin_array_min(obj, NULL, 0, &err);
                if (err) {
                    vm->error = err;
                    push(vm, value_nil());
                } else push(vm, r);
                return true;
            }
            if (mhash == MHASH_max && strcmp(method, "max") == 0 && arg_count == 0) {
                char *err = NULL;
                LatValue r = builtin_array_max(obj, NULL, 0, &err);
                if (err) {
                    vm->error = err;
                    push(vm, value_nil());
                } else push(vm, r);
                return true;
            }
            if (mhash == MHASH_first && strcmp(method, "first") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_array_first(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_last && strcmp(method, "last") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_array_last(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_flat_map && strcmp(method, "flat_map") == 0 && arg_count == 1) {
                LatValue closure = pop(vm);
                char *err = NULL;
                LatValue result = builtin_array_flat_map(obj, &closure, stackvm_builtin_callback, vm, &err);
                value_free(&closure);
                push(vm, result);
                return true;
            }
            if (mhash == MHASH_sort_by && strcmp(method, "sort_by") == 0 && arg_count == 1) {
                LatValue closure = pop(vm);
                char *err = NULL;
                LatValue result = builtin_array_sort_by(obj, &closure, stackvm_builtin_callback, vm, &err);
                value_free(&closure);
                push(vm, result);
                return true;
            }
            if (mhash == MHASH_group_by && strcmp(method, "group_by") == 0 && arg_count == 1) {
                LatValue closure = pop(vm);
                char *err = NULL;
                LatValue result = builtin_array_group_by(obj, &closure, stackvm_builtin_callback, vm, &err);
                value_free(&closure);
                push(vm, result);
                return true;
            }
            if (mhash == MHASH_insert && strcmp(method, "insert") == 0 && arg_count == 2) {
                const char *pmode = stackvm_find_pressure(vm, var_name);
                if (pressure_blocks_grow(pmode)) {
                    LatValue val = pop(vm);
                    LatValue idx_v = pop(vm);
                    value_free(&val);
                    value_free(&idx_v);
                    char *err = NULL;
                    lat_asprintf(&err, "pressurized (%s): cannot insert into '%s'", pmode, var_name);
                    vm->error = err;
                    push(vm, value_unit());
                    return true;
                }
                LatValue val = pop(vm);
                LatValue idx_v = pop(vm);
                int64_t idx = idx_v.as.int_val;
                value_free(&idx_v);
                if (idx < 0 || (size_t)idx > obj->as.array.len) {
                    value_free(&val);
                    vm->error = strdup(".insert() index out of bounds");
                    push(vm, value_unit());
                    return true;
                }
                /* Grow if needed */
                if (obj->as.array.len >= obj->as.array.cap) {
                    size_t new_cap = obj->as.array.cap < 4 ? 4 : obj->as.array.cap * 2;
                    obj->as.array.elems = realloc(obj->as.array.elems, new_cap * sizeof(LatValue));
                    obj->as.array.cap = new_cap;
                }
                /* Shift elements right */
                memmove(&obj->as.array.elems[(size_t)idx + 1], &obj->as.array.elems[(size_t)idx],
                        (obj->as.array.len - (size_t)idx) * sizeof(LatValue));
                obj->as.array.elems[(size_t)idx] = val;
                obj->as.array.len++;
                push(vm, value_unit());
                return true;
            }
        } break;

        /* String methods */
        case VAL_STR: {
            if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
                 (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
                arg_count == 0) {
                push(vm, value_int((int64_t)strlen(obj->as.str_val)));
                return true;
            }
            if (mhash == MHASH_contains && strcmp(method, "contains") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_string_contains(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_split && strcmp(method, "split") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_string_split(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_trim && strcmp(method, "trim") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_string_trim(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_to_upper && strcmp(method, "to_upper") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_string_to_upper(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_to_lower && strcmp(method, "to_lower") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_string_to_lower(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_capitalize && strcmp(method, "capitalize") == 0 && arg_count == 0) {
                push(vm, value_string_owned(lat_str_capitalize(obj->as.str_val)));
                return true;
            }
            if (mhash == MHASH_title_case && strcmp(method, "title_case") == 0 && arg_count == 0) {
                push(vm, value_string_owned(lat_str_title_case(obj->as.str_val)));
                return true;
            }
            if (mhash == MHASH_snake_case && strcmp(method, "snake_case") == 0 && arg_count == 0) {
                push(vm, value_string_owned(lat_str_snake_case(obj->as.str_val)));
                return true;
            }
            if (mhash == MHASH_camel_case && strcmp(method, "camel_case") == 0 && arg_count == 0) {
                push(vm, value_string_owned(lat_str_camel_case(obj->as.str_val)));
                return true;
            }
            if (mhash == MHASH_kebab_case && strcmp(method, "kebab_case") == 0 && arg_count == 0) {
                push(vm, value_string_owned(lat_str_kebab_case(obj->as.str_val)));
                return true;
            }
            if (mhash == MHASH_starts_with && strcmp(method, "starts_with") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_string_starts_with(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_ends_with && strcmp(method, "ends_with") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_string_ends_with(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_replace && strcmp(method, "replace") == 0 && arg_count == 2) {
                LatValue args[2];
                args[1] = pop(vm);
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_string_replace(obj, args, 2, &err);
                value_free(&args[0]);
                value_free(&args[1]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_index_of && strcmp(method, "index_of") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_string_index_of(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_substring && strcmp(method, "substring") == 0 && arg_count == 2) {
                LatValue args[2];
                args[1] = pop(vm);
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_string_substring(obj, args, 2, &err);
                value_free(&args[0]);
                value_free(&args[1]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_chars && strcmp(method, "chars") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_string_chars(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_bytes && strcmp(method, "bytes") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_string_bytes(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_reverse && strcmp(method, "reverse") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_string_reverse(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_repeat && strcmp(method, "repeat") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_string_repeat(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_trim_start && strcmp(method, "trim_start") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_string_trim_start(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_trim_end && strcmp(method, "trim_end") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_string_trim_end(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_pad_left && strcmp(method, "pad_left") == 0 && arg_count == 2) {
                LatValue args[2];
                args[1] = pop(vm);
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_string_pad_left(obj, args, 2, &err);
                value_free(&args[0]);
                value_free(&args[1]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_pad_right && strcmp(method, "pad_right") == 0 && arg_count == 2) {
                LatValue args[2];
                args[1] = pop(vm);
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_string_pad_right(obj, args, 2, &err);
                value_free(&args[0]);
                value_free(&args[1]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_count && strcmp(method, "count") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_string_count(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_is_empty && strcmp(method, "is_empty") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_string_is_empty(obj, NULL, 0, &err));
                return true;
            }
        } break;

        /* Map methods */
        case VAL_MAP: {
            if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
                 (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
                arg_count == 0) {
                push(vm, value_int((int64_t)lat_map_len(obj->as.map.map)));
                return true;
            }
            if (mhash == MHASH_get && strcmp(method, "get") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_map_get(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_keys && strcmp(method, "keys") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_map_keys(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_values && strcmp(method, "values") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_map_values(obj, NULL, 0, &err));
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
                    if (var_name && obj->phase == VTAG_CRYSTAL)
                        lat_asprintf(&err, "cannot set on %s map '%s' (use thaw(%s) to make it mutable)", phase_name,
                                     var_name, var_name);
                    else lat_asprintf(&err, "cannot set on %s map", phase_name);
                    vm->error = err;
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
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_map_has(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_has && strcmp(method, "has") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_map_has(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_remove && strcmp(method, "remove") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_map_remove(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_entries && strcmp(method, "entries") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_map_entries(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_merge && strcmp(method, "merge") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_map_merge(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_for_each && strcmp(method, "for_each") == 0 && arg_count == 1) {
                LatValue closure = pop(vm);
                for (size_t i = 0; i < obj->as.map.map->cap; i++) {
                    if (obj->as.map.map->entries[i].state == MAP_OCCUPIED) {
                        LatValue ca[2];
                        ca[0] = value_string(obj->as.map.map->entries[i].key);
                        ca[1] = value_deep_clone((LatValue *)obj->as.map.map->entries[i].value);
                        LatValue r = stackvm_call_closure(vm, &closure, ca, 2);
                        value_free(&ca[0]);
                        value_free(&ca[1]);
                        value_free(&r);
                    }
                }
                value_free(&closure);
                push(vm, value_unit());
                return true;
            }
            if (mhash == MHASH_filter && strcmp(method, "filter") == 0 && arg_count == 1) {
                LatValue closure = pop(vm);
                LatValue result = value_map_new();
                for (size_t i = 0; i < obj->as.map.map->cap; i++) {
                    if (obj->as.map.map->entries[i].state == MAP_OCCUPIED) {
                        LatValue ca[2];
                        ca[0] = value_string(obj->as.map.map->entries[i].key);
                        ca[1] = value_deep_clone((LatValue *)obj->as.map.map->entries[i].value);
                        LatValue r = stackvm_call_closure(vm, &closure, ca, 2);
                        bool keep = (r.type == VAL_BOOL && r.as.bool_val);
                        value_free(&ca[0]);
                        value_free(&r);
                        if (keep) {
                            lat_map_set(result.as.map.map, obj->as.map.map->entries[i].key, &ca[1]);
                        } else {
                            value_free(&ca[1]);
                        }
                    }
                }
                value_free(&closure);
                push(vm, result);
                return true;
            }
            if (mhash == MHASH_map && strcmp(method, "map") == 0 && arg_count == 1) {
                LatValue closure = pop(vm);
                LatValue result = value_map_new();
                for (size_t i = 0; i < obj->as.map.map->cap; i++) {
                    if (obj->as.map.map->entries[i].state == MAP_OCCUPIED) {
                        LatValue ca[2];
                        ca[0] = value_string(obj->as.map.map->entries[i].key);
                        ca[1] = value_deep_clone((LatValue *)obj->as.map.map->entries[i].value);
                        LatValue r = stackvm_call_closure(vm, &closure, ca, 2);
                        value_free(&ca[0]);
                        value_free(&ca[1]);
                        lat_map_set(result.as.map.map, obj->as.map.map->entries[i].key, &r);
                    }
                }
                value_free(&closure);
                push(vm, result);
                return true;
            }
        } break;

        /* Struct methods */
        case VAL_STRUCT: {
            if (mhash == MHASH_get && strcmp(method, "get") == 0 && arg_count == 1) {
                LatValue key = pop(vm);
                if (key.type == VAL_STR) {
                    bool found = false;
                    for (size_t i = 0; i < obj->as.strct.field_count; i++) {
                        if (obj->as.strct.field_names[i] == intern(key.as.str_val)) {
                            push(vm, value_deep_clone(&obj->as.strct.field_values[i]));
                            found = true;
                            break;
                        }
                    }
                    if (!found) push(vm, value_nil());
                } else {
                    push(vm, value_nil());
                }
                value_free(&key);
                return true;
            }
            /* Struct field that is callable */
            for (size_t i = 0; i < obj->as.strct.field_count; i++) {
                if (obj->as.strct.field_names[i] == intern(method)) {
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
            if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
                 (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
                arg_count == 0) {
                int64_t len = obj->as.range.end - obj->as.range.start;
                push(vm, value_int(len > 0 ? len : 0));
                return true;
            }
            if (mhash == MHASH_contains && strcmp(method, "contains") == 0 && arg_count == 1) {
                LatValue val = pop(vm);
                if (val.type == VAL_INT) {
                    push(vm, value_bool(val.as.int_val >= obj->as.range.start && val.as.int_val < obj->as.range.end));
                } else {
                    push(vm, value_bool(false));
                }
                value_free(&val);
                return true;
            }
        } break;

        /* Tuple methods */
        case VAL_TUPLE: {
            if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
                 (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
                arg_count == 0) {
                push(vm, value_int((int64_t)obj->as.tuple.len));
                return true;
            }
        } break;

        /* Enum methods */
        case VAL_ENUM: {
            if (mhash == MHASH_tag && strcmp(method, "tag") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_enum_tag(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_payload && strcmp(method, "payload") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_enum_payload(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_variant_name && strcmp(method, "variant_name") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_enum_tag(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_enum_name && strcmp(method, "enum_name") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_enum_enum_name(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_is_variant && strcmp(method, "is_variant") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_enum_is_variant(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
        } break;

        /* ── Set methods ── */
        case VAL_SET: {
            if (mhash == MHASH_has && strcmp(method, "has") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_set_has(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_add && strcmp(method, "add") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_set_add(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_remove && strcmp(method, "remove") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_set_remove(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
                 (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
                arg_count == 0) {
                push(vm, value_int((int64_t)lat_map_len(obj->as.set.map)));
                return true;
            }
            if (mhash == MHASH_to_array && strcmp(method, "to_array") == 0 && arg_count == 0) {
                char *err = NULL;
                push(vm, builtin_set_to_array(obj, NULL, 0, &err));
                return true;
            }
            if (mhash == MHASH_union && strcmp(method, "union") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_set_union(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_intersection && strcmp(method, "intersection") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_set_intersection(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_difference && strcmp(method, "difference") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_set_difference(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_is_subset && strcmp(method, "is_subset") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_set_is_subset(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_is_superset && strcmp(method, "is_superset") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_set_is_superset(obj, args, 1, &err);
                value_free(&args[0]);
                push(vm, r);
                return true;
            }
        } break;

        /* ── Channel methods ── */
        case VAL_CHANNEL: {
            if (mhash == MHASH_send && strcmp(method, "send") == 0 && arg_count == 1) {
                LatValue val = pop(vm);
                if (val.phase == VTAG_FLUID) {
                    value_free(&val);
                    vm->error = strdup("channel.send: can only send crystal (immutable) values");
                    push(vm, value_unit());
                    return true;
                }
                channel_send(obj->as.channel.ch, val);
                push(vm, value_unit());
                return true;
            }
            if (mhash == MHASH_recv && strcmp(method, "recv") == 0 && arg_count == 0) {
                bool ok;
                LatValue val = channel_recv(obj->as.channel.ch, &ok);
                if (!ok) {
                    push(vm, value_unit());
                    return true;
                }
                push(vm, val);
                return true;
            }
            if (mhash == MHASH_close && strcmp(method, "close") == 0 && arg_count == 0) {
                channel_close(obj->as.channel.ch);
                push(vm, value_unit());
                return true;
            }
        } break;

        /* ── Buffer methods ── */
        case VAL_BUFFER: {
            if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
                 (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
                arg_count == 0) {
                push(vm, value_int((int64_t)obj->as.buffer.len));
                return true;
            }
            if (mhash == MHASH_capacity && strcmp(method, "capacity") == 0 && arg_count == 0) {
                push(vm, value_int((int64_t)obj->as.buffer.cap));
                return true;
            }
            if (mhash == MHASH_push && strcmp(method, "push") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                push(vm, builtin_buffer_push(obj, args, 1, NULL));
                return true;
            }
            if (mhash == MHASH_push_u16 && strcmp(method, "push_u16") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                push(vm, builtin_buffer_push_u16(obj, args, 1, NULL));
                return true;
            }
            if (mhash == MHASH_push_u32 && strcmp(method, "push_u32") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                push(vm, builtin_buffer_push_u32(obj, args, 1, NULL));
                return true;
            }
            if (mhash == MHASH_read_u8 && strcmp(method, "read_u8") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_buffer_read_u8(obj, args, 1, &err);
                if (err) vm->error = err;
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_write_u8 && strcmp(method, "write_u8") == 0 && arg_count == 2) {
                LatValue args[2];
                args[1] = pop(vm);
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_buffer_write_u8(obj, args, 2, &err);
                if (err) vm->error = err;
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_read_u16 && strcmp(method, "read_u16") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_buffer_read_u16(obj, args, 1, &err);
                if (err) vm->error = err;
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_write_u16 && strcmp(method, "write_u16") == 0 && arg_count == 2) {
                LatValue args[2];
                args[1] = pop(vm);
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_buffer_write_u16(obj, args, 2, &err);
                if (err) vm->error = err;
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_read_u32 && strcmp(method, "read_u32") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_buffer_read_u32(obj, args, 1, &err);
                if (err) vm->error = err;
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_write_u32 && strcmp(method, "write_u32") == 0 && arg_count == 2) {
                LatValue args[2];
                args[1] = pop(vm);
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_buffer_write_u32(obj, args, 2, &err);
                if (err) vm->error = err;
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_read_i8 && strcmp(method, "read_i8") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_buffer_read_i8(obj, args, 1, &err);
                if (err) vm->error = err;
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_read_i16 && strcmp(method, "read_i16") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_buffer_read_i16(obj, args, 1, &err);
                if (err) vm->error = err;
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_read_i32 && strcmp(method, "read_i32") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_buffer_read_i32(obj, args, 1, &err);
                if (err) vm->error = err;
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_read_f32 && strcmp(method, "read_f32") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_buffer_read_f32(obj, args, 1, &err);
                if (err) vm->error = err;
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_read_f64 && strcmp(method, "read_f64") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_buffer_read_f64(obj, args, 1, &err);
                if (err) vm->error = err;
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_slice && strcmp(method, "slice") == 0 && arg_count == 2) {
                LatValue args[2];
                args[1] = pop(vm);
                args[0] = pop(vm);
                char *err = NULL;
                LatValue r = builtin_buffer_slice(obj, args, 2, &err);
                if (err) vm->error = err;
                push(vm, r);
                return true;
            }
            if (mhash == MHASH_clear && strcmp(method, "clear") == 0 && arg_count == 0) {
                push(vm, builtin_buffer_clear(obj, NULL, 0, NULL));
                return true;
            }
            if (mhash == MHASH_fill && strcmp(method, "fill") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                push(vm, builtin_buffer_fill(obj, args, 1, NULL));
                return true;
            }
            if (mhash == MHASH_resize && strcmp(method, "resize") == 0 && arg_count == 1) {
                LatValue args[1];
                args[0] = pop(vm);
                push(vm, builtin_buffer_resize(obj, args, 1, NULL));
                return true;
            }
            if (mhash == MHASH_to_string && strcmp(method, "to_string") == 0 && arg_count == 0) {
                push(vm, builtin_buffer_to_string(obj, NULL, 0, NULL));
                return true;
            }
            if (mhash == MHASH_to_array && strcmp(method, "to_array") == 0 && arg_count == 0) {
                push(vm, builtin_buffer_to_array(obj, NULL, 0, NULL));
                return true;
            }
            if (mhash == MHASH_to_hex && strcmp(method, "to_hex") == 0 && arg_count == 0) {
                push(vm, builtin_buffer_to_hex(obj, NULL, 0, NULL));
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
                *inner = stackvm_peek(vm, 0)[0];
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
                    LatValue key = stackvm_peek(vm, 0)[0];
                    if (key.type != VAL_STR) {
                        push(vm, value_nil());
                        return true;
                    }
                    LatValue *found = lat_map_get(inner->as.map.map, key.as.str_val);
                    value_free(&key);
                    vm->stack_top--;
                    push(vm, found ? value_deep_clone(found) : value_nil());
                    return true;
                }
                /* set(k, v) with 2 args -> Map proxy */
                if (mhash == MHASH_set && strcmp(method, "set") == 0 && arg_count == 2) {
                    if (obj->phase == VTAG_CRYSTAL) {
                        runtime_error(vm, "cannot set on a frozen Ref");
                        return true;
                    }
                    LatValue val2 = stackvm_peek(vm, 0)[0];
                    vm->stack_top--;
                    LatValue key = stackvm_peek(vm, 0)[0];
                    vm->stack_top--;
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
                    LatValue key = stackvm_peek(vm, 0)[0];
                    vm->stack_top--;
                    bool found = key.type == VAL_STR && lat_map_contains(inner->as.map.map, key.as.str_val);
                    value_free(&key);
                    push(vm, value_bool(found));
                    return true;
                }
                if (mhash == MHASH_contains && strcmp(method, "contains") == 0 && arg_count == 1) {
                    LatValue needle = stackvm_peek(vm, 0)[0];
                    vm->stack_top--;
                    bool found = false;
                    for (size_t i = 0; i < inner->as.map.map->cap; i++) {
                        if (inner->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                        LatValue *mv = (LatValue *)inner->as.map.map->entries[i].value;
                        if (value_eq(mv, &needle)) {
                            found = true;
                            break;
                        }
                    }
                    value_free(&needle);
                    push(vm, value_bool(found));
                    return true;
                }
                if (mhash == MHASH_keys && strcmp(method, "keys") == 0 && arg_count == 0) {
                    size_t n = lat_map_len(inner->as.map.map);
                    LatValue *elems = malloc((n > 0 ? n : 1) * sizeof(LatValue));
                    if (!elems) return false;
                    size_t ei = 0;
                    for (size_t i = 0; i < inner->as.map.map->cap; i++) {
                        if (inner->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                        elems[ei++] = value_string(inner->as.map.map->entries[i].key);
                    }
                    LatValue arr = value_array(elems, ei);
                    free(elems);
                    push(vm, arr);
                    return true;
                }
                if (mhash == MHASH_values && strcmp(method, "values") == 0 && arg_count == 0) {
                    size_t n = lat_map_len(inner->as.map.map);
                    LatValue *elems = malloc((n > 0 ? n : 1) * sizeof(LatValue));
                    if (!elems) return false;
                    size_t ei = 0;
                    for (size_t i = 0; i < inner->as.map.map->cap; i++) {
                        if (inner->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                        LatValue *mv = (LatValue *)inner->as.map.map->entries[i].value;
                        elems[ei++] = value_deep_clone(mv);
                    }
                    LatValue arr = value_array(elems, ei);
                    free(elems);
                    push(vm, arr);
                    return true;
                }
                if (mhash == MHASH_entries && strcmp(method, "entries") == 0 && arg_count == 0) {
                    size_t n = lat_map_len(inner->as.map.map);
                    LatValue *elems = malloc((n > 0 ? n : 1) * sizeof(LatValue));
                    if (!elems) return false;
                    size_t ei = 0;
                    for (size_t i = 0; i < inner->as.map.map->cap; i++) {
                        if (inner->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                        LatValue pair[2];
                        pair[0] = value_string(inner->as.map.map->entries[i].key);
                        pair[1] = value_deep_clone((LatValue *)inner->as.map.map->entries[i].value);
                        elems[ei++] = value_array(pair, 2);
                    }
                    LatValue arr = value_array(elems, ei);
                    free(elems);
                    push(vm, arr);
                    return true;
                }
                if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
                     (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
                    arg_count == 0) {
                    push(vm, value_int((int64_t)lat_map_len(inner->as.map.map)));
                    return true;
                }
                if (mhash == MHASH_merge && strcmp(method, "merge") == 0 && arg_count == 1) {
                    if (obj->phase == VTAG_CRYSTAL) {
                        runtime_error(vm, "cannot merge into a frozen Ref");
                        return true;
                    }
                    LatValue other = stackvm_peek(vm, 0)[0];
                    vm->stack_top--;
                    if (other.type == VAL_MAP) {
                        for (size_t i = 0; i < other.as.map.map->cap; i++) {
                            if (other.as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                            LatValue cloned = value_deep_clone((LatValue *)other.as.map.map->entries[i].value);
                            LatValue *old =
                                (LatValue *)lat_map_get(inner->as.map.map, other.as.map.map->entries[i].key);
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
                    LatValue val2 = stackvm_peek(vm, 0)[0];
                    vm->stack_top--;
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
                if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
                     (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
                    arg_count == 0) {
                    push(vm, value_int((int64_t)inner->as.array.len));
                    return true;
                }
                if (mhash == MHASH_contains && strcmp(method, "contains") == 0 && arg_count == 1) {
                    LatValue needle = stackvm_peek(vm, 0)[0];
                    vm->stack_top--;
                    bool found = false;
                    for (size_t i = 0; i < inner->as.array.len; i++) {
                        if (value_eq(&inner->as.array.elems[i], &needle)) {
                            found = true;
                            break;
                        }
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

static bool stackvm_type_matches(const LatValue *val, const char *type_name) {
    if (!type_name || strcmp(type_name, "Any") == 0 || strcmp(type_name, "any") == 0) return true;
    if (strcmp(type_name, "Int") == 0) return val->type == VAL_INT;
    if (strcmp(type_name, "Float") == 0) return val->type == VAL_FLOAT;
    if (strcmp(type_name, "String") == 0) return val->type == VAL_STR;
    if (strcmp(type_name, "Bool") == 0) return val->type == VAL_BOOL;
    if (strcmp(type_name, "Nil") == 0) return val->type == VAL_NIL;
    if (strcmp(type_name, "Map") == 0) return val->type == VAL_MAP;
    if (strcmp(type_name, "Array") == 0) return val->type == VAL_ARRAY;
    if (strcmp(type_name, "Fn") == 0 || strcmp(type_name, "Closure") == 0) return val->type == VAL_CLOSURE;
    if (strcmp(type_name, "Channel") == 0) return val->type == VAL_CHANNEL;
    if (strcmp(type_name, "Range") == 0) return val->type == VAL_RANGE;
    if (strcmp(type_name, "Set") == 0) return val->type == VAL_SET;
    if (strcmp(type_name, "Tuple") == 0) return val->type == VAL_TUPLE;
    if (strcmp(type_name, "Buffer") == 0) return val->type == VAL_BUFFER;
    if (strcmp(type_name, "Ref") == 0) return val->type == VAL_REF;
    if (strcmp(type_name, "Number") == 0) return val->type == VAL_INT || val->type == VAL_FLOAT;
    /* Struct name check */
    if (val->type == VAL_STRUCT && val->as.strct.name) return strcmp(val->as.strct.name, type_name) == 0;
    /* Enum name check */
    if (val->type == VAL_ENUM && val->as.enm.enum_name) return strcmp(val->as.enm.enum_name, type_name) == 0;
    return false;
}

static const char *stackvm_value_type_display(const LatValue *val) {
    switch (val->type) {
        case VAL_INT: return "Int";
        case VAL_FLOAT: return "Float";
        case VAL_BOOL: return "Bool";
        case VAL_STR: return "String";
        case VAL_ARRAY: return "Array";
        case VAL_STRUCT: return val->as.strct.name ? val->as.strct.name : "Struct";
        case VAL_CLOSURE: return "Fn";
        case VAL_UNIT: return "Unit";
        case VAL_NIL: return "Nil";
        case VAL_RANGE: return "Range";
        case VAL_MAP: return "Map";
        case VAL_CHANNEL: return "Channel";
        case VAL_ENUM: return val->as.enm.enum_name ? val->as.enm.enum_name : "Enum";
        case VAL_SET: return "Set";
        case VAL_TUPLE: return "Tuple";
        case VAL_BUFFER: return "Buffer";
        case VAL_REF: return "Ref";
    }
    return "Unknown";
}

/* ── Execution ── */

#define READ_BYTE() (*frame->ip++)
#define READ_U16()  (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))

/* Route runtime errors through exception handlers when possible.
 * Use in place of 'return runtime_error(vm, ...)' inside the dispatch loop. */
#define VM_ERROR(...)                                                       \
    do {                                                                    \
        StackVMResult _err = stackvm_handle_error(vm, &frame, __VA_ARGS__); \
        if (_err != STACKVM_OK) return _err;                                \
    } while (0)

/* Adjust stack for default parameters and variadic arguments before a compiled
 * closure call. Returns the adjusted arg_count, or -1 on error. On error,
 * vm->error is set with a heap-allocated message. */
/* Convert a Map or Set on the stack (pointed to by iter) to a key/value array in-place. */
static void stackvm_iter_convert_to_array(LatValue *iter) {
    bool is_map = (iter->type == VAL_MAP);
    LatMap *hm = is_map ? iter->as.map.map : iter->as.set.map;
    size_t len = lat_map_len(hm);
    LatValue *elms = malloc((len ? len : 1) * sizeof(LatValue));
    if (!elms) { return; }
    size_t ei = 0;
    for (size_t i = 0; i < hm->cap; i++) {
        if (hm->entries[i].state == MAP_OCCUPIED) {
            if (is_map) elms[ei++] = value_string(hm->entries[i].key);
            else elms[ei++] = value_deep_clone((LatValue *)hm->entries[i].value);
        }
    }
    LatValue arr = value_array(elms, ei);
    free(elms);
    value_free(iter);
    *iter = arr;
}

static int stackvm_adjust_call_args(StackVM *vm, Chunk *fn_chunk, int arity, int arg_count) {
    int dc = fn_chunk->default_count;
    bool vd = fn_chunk->fn_has_variadic;
    if (dc == 0 && !vd) {
        if (arg_count != arity) {
            lat_asprintf(&vm->error, "expected %d arguments but got %d", arity, arg_count);
            return -1;
        }
        return arg_count;
    }
    int required = arity - dc - (vd ? 1 : 0);
    int non_variadic = vd ? arity - 1 : arity;

    if (arg_count < required || (!vd && arg_count > arity)) {
        if (vd) lat_asprintf(&vm->error, "expected at least %d arguments but got %d", required, arg_count);
        else if (dc > 0) lat_asprintf(&vm->error, "expected %d to %d arguments but got %d", required, arity, arg_count);
        else lat_asprintf(&vm->error, "expected %d arguments but got %d", arity, arg_count);
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
            if (!elems) return 0;
            for (int i = extra - 1; i >= 0; i--) elems[i] = pop(vm);
        }
        push(vm, value_array(elems, extra));
        free(elems);
        arg_count = arity;
    }

    return arg_count;
}

/* Dispatch pointer adapters for LatRuntime */
static LatValue stackvm_dispatch_call_closure(void *vm_ptr, LatValue *closure, LatValue *args, int argc) {
    StackVM *vm = (StackVM *)vm_ptr;
    LatValue result = stackvm_call_closure(vm, closure, args, argc);
    /* Bridge StackVM errors to runtime for phase system functions */
    if (vm->error) {
        vm->rt->error = vm->error;
        vm->error = NULL;
    }
    return result;
}

StackVMResult stackvm_run(StackVM *vm, Chunk *chunk, LatValue *result) {
    /* Set up runtime dispatch pointers so native functions can call back */
    vm->rt->backend = RT_BACKEND_STACK_VM;
    vm->rt->active_vm = vm;
    vm->rt->call_closure = stackvm_dispatch_call_closure;
    vm->rt->find_local_value = (bool (*)(void *, const char *, LatValue *))stackvm_find_local_value;
    vm->rt->current_line = (int (*)(void *))stackvm_current_line;
    vm->rt->get_var_by_name = (bool (*)(void *, const char *, LatValue *))stackvm_get_var_by_name;
    vm->rt->set_var_by_name = (bool (*)(void *, const char *, LatValue))stackvm_set_var_by_name;
    lat_runtime_set_current(vm->rt);
    size_t base_frame = vm->frame_count;
    StackCallFrame *frame = &vm->frames[vm->frame_count++];
    frame->chunk = chunk;
    frame->ip = chunk->code;
    if (vm->next_frame_slots) {
        frame->slots = vm->next_frame_slots;
        frame->cleanup_base = vm->stack_top; /* Only free above this point on return */
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
        [OP_NIL] = &&lbl_OP_NIL,
        [OP_TRUE] = &&lbl_OP_TRUE,
        [OP_FALSE] = &&lbl_OP_FALSE,
        [OP_UNIT] = &&lbl_OP_UNIT,
        [OP_POP] = &&lbl_OP_POP,
        [OP_DUP] = &&lbl_OP_DUP,
        [OP_SWAP] = &&lbl_OP_SWAP,
        [OP_ADD] = &&lbl_OP_ADD,
        [OP_SUB] = &&lbl_OP_SUB,
        [OP_MUL] = &&lbl_OP_MUL,
        [OP_DIV] = &&lbl_OP_DIV,
        [OP_MOD] = &&lbl_OP_MOD,
        [OP_NEG] = &&lbl_OP_NEG,
        [OP_NOT] = &&lbl_OP_NOT,
        [OP_BIT_AND] = &&lbl_OP_BIT_AND,
        [OP_BIT_OR] = &&lbl_OP_BIT_OR,
        [OP_BIT_XOR] = &&lbl_OP_BIT_XOR,
        [OP_BIT_NOT] = &&lbl_OP_BIT_NOT,
        [OP_LSHIFT] = &&lbl_OP_LSHIFT,
        [OP_RSHIFT] = &&lbl_OP_RSHIFT,
        [OP_EQ] = &&lbl_OP_EQ,
        [OP_NEQ] = &&lbl_OP_NEQ,
        [OP_LT] = &&lbl_OP_LT,
        [OP_GT] = &&lbl_OP_GT,
        [OP_LTEQ] = &&lbl_OP_LTEQ,
        [OP_GTEQ] = &&lbl_OP_GTEQ,
        [OP_CONCAT] = &&lbl_OP_CONCAT,
        [OP_GET_LOCAL] = &&lbl_OP_GET_LOCAL,
        [OP_SET_LOCAL] = &&lbl_OP_SET_LOCAL,
        [OP_GET_GLOBAL] = &&lbl_OP_GET_GLOBAL,
        [OP_SET_GLOBAL] = &&lbl_OP_SET_GLOBAL,
        [OP_DEFINE_GLOBAL] = &&lbl_OP_DEFINE_GLOBAL,
        [OP_GET_UPVALUE] = &&lbl_OP_GET_UPVALUE,
        [OP_SET_UPVALUE] = &&lbl_OP_SET_UPVALUE,
        [OP_CLOSE_UPVALUE] = &&lbl_OP_CLOSE_UPVALUE,
        [OP_JUMP] = &&lbl_OP_JUMP,
        [OP_JUMP_IF_FALSE] = &&lbl_OP_JUMP_IF_FALSE,
        [OP_JUMP_IF_TRUE] = &&lbl_OP_JUMP_IF_TRUE,
        [OP_JUMP_IF_NOT_NIL] = &&lbl_OP_JUMP_IF_NOT_NIL,
        [OP_LOOP] = &&lbl_OP_LOOP,
        [OP_CALL] = &&lbl_OP_CALL,
        [OP_CLOSURE] = &&lbl_OP_CLOSURE,
        [OP_RETURN] = &&lbl_OP_RETURN,
        [OP_ITER_INIT] = &&lbl_OP_ITER_INIT,
        [OP_ITER_NEXT] = &&lbl_OP_ITER_NEXT,
        [OP_BUILD_ARRAY] = &&lbl_OP_BUILD_ARRAY,
        [OP_ARRAY_FLATTEN] = &&lbl_OP_ARRAY_FLATTEN,
        [OP_BUILD_MAP] = &&lbl_OP_BUILD_MAP,
        [OP_BUILD_TUPLE] = &&lbl_OP_BUILD_TUPLE,
        [OP_BUILD_STRUCT] = &&lbl_OP_BUILD_STRUCT,
        [OP_BUILD_RANGE] = &&lbl_OP_BUILD_RANGE,
        [OP_BUILD_ENUM] = &&lbl_OP_BUILD_ENUM,
        [OP_INDEX] = &&lbl_OP_INDEX,
        [OP_SET_INDEX] = &&lbl_OP_SET_INDEX,
        [OP_GET_FIELD] = &&lbl_OP_GET_FIELD,
        [OP_SET_FIELD] = &&lbl_OP_SET_FIELD,
        [OP_INVOKE] = &&lbl_OP_INVOKE,
        [OP_INVOKE_LOCAL] = &&lbl_OP_INVOKE_LOCAL,
        [OP_INVOKE_GLOBAL] = &&lbl_OP_INVOKE_GLOBAL,
        [OP_SET_INDEX_LOCAL] = &&lbl_OP_SET_INDEX_LOCAL,
        [OP_PUSH_EXCEPTION_HANDLER] = &&lbl_OP_PUSH_EXCEPTION_HANDLER,
        [OP_POP_EXCEPTION_HANDLER] = &&lbl_OP_POP_EXCEPTION_HANDLER,
        [OP_THROW] = &&lbl_OP_THROW,
        [OP_TRY_UNWRAP] = &&lbl_OP_TRY_UNWRAP,
        [OP_DEFER_PUSH] = &&lbl_OP_DEFER_PUSH,
        [OP_DEFER_RUN] = &&lbl_OP_DEFER_RUN,
        [OP_FREEZE] = &&lbl_OP_FREEZE,
        [OP_THAW] = &&lbl_OP_THAW,
        [OP_CLONE] = &&lbl_OP_CLONE,
        [OP_MARK_FLUID] = &&lbl_OP_MARK_FLUID,
        [OP_REACT] = &&lbl_OP_REACT,
        [OP_UNREACT] = &&lbl_OP_UNREACT,
        [OP_BOND] = &&lbl_OP_BOND,
        [OP_UNBOND] = &&lbl_OP_UNBOND,
        [OP_SEED] = &&lbl_OP_SEED,
        [OP_UNSEED] = &&lbl_OP_UNSEED,
        [OP_FREEZE_VAR] = &&lbl_OP_FREEZE_VAR,
        [OP_THAW_VAR] = &&lbl_OP_THAW_VAR,
        [OP_SUBLIMATE_VAR] = &&lbl_OP_SUBLIMATE_VAR,
        [OP_SUBLIMATE] = &&lbl_OP_SUBLIMATE,
        [OP_PRINT] = &&lbl_OP_PRINT,
        [OP_IMPORT] = &&lbl_OP_IMPORT,
        [OP_SCOPE] = &&lbl_OP_SCOPE,
        [OP_SELECT] = &&lbl_OP_SELECT,
        [OP_INC_LOCAL] = &&lbl_OP_INC_LOCAL,
        [OP_DEC_LOCAL] = &&lbl_OP_DEC_LOCAL,
        [OP_ADD_INT] = &&lbl_OP_ADD_INT,
        [OP_SUB_INT] = &&lbl_OP_SUB_INT,
        [OP_MUL_INT] = &&lbl_OP_MUL_INT,
        [OP_LT_INT] = &&lbl_OP_LT_INT,
        [OP_LTEQ_INT] = &&lbl_OP_LTEQ_INT,
        [OP_LOAD_INT8] = &&lbl_OP_LOAD_INT8,
        [OP_CONSTANT_16] = &&lbl_OP_CONSTANT_16,
        [OP_GET_GLOBAL_16] = &&lbl_OP_GET_GLOBAL_16,
        [OP_SET_GLOBAL_16] = &&lbl_OP_SET_GLOBAL_16,
        [OP_DEFINE_GLOBAL_16] = &&lbl_OP_DEFINE_GLOBAL_16,
        [OP_CLOSURE_16] = &&lbl_OP_CLOSURE_16,
        [OP_INVOKE_LOCAL_16] = &&lbl_OP_INVOKE_LOCAL_16,
        [OP_INVOKE_GLOBAL_16] = &&lbl_OP_INVOKE_GLOBAL_16,
        [OP_RESET_EPHEMERAL] = &&lbl_OP_RESET_EPHEMERAL,
        [OP_SET_LOCAL_POP] = &&lbl_OP_SET_LOCAL_POP,
        [OP_CHECK_TYPE] = &&lbl_OP_CHECK_TYPE,
        [OP_CHECK_RETURN_TYPE] = &&lbl_OP_CHECK_RETURN_TYPE,
        [OP_IS_CRYSTAL] = &&lbl_OP_IS_CRYSTAL,
        [OP_IS_FLUID] = &&lbl_OP_IS_FLUID,
        [OP_FREEZE_EXCEPT] = &&lbl_OP_FREEZE_EXCEPT,
        [OP_FREEZE_FIELD] = &&lbl_OP_FREEZE_FIELD,
        [OP_APPEND_STR_LOCAL] = &&lbl_OP_APPEND_STR_LOCAL,
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
            LatValue kv = frame->chunk->constants[idx];
            if (kv.type == VAL_STR && kv.region_id != REGION_INTERNED) {
                /* Intern string constants: avoids strdup on every load and
                 * enables pointer equality for string comparisons. */
                push(vm, value_string_interned(kv.as.str_val));
            } else {
                push(vm, value_clone_fast(&kv));
            }
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_CONSTANT_16:
#endif
        case OP_CONSTANT_16: {
            uint16_t idx = READ_U16();
            LatValue kv = frame->chunk->constants[idx];
            if (kv.type == VAL_STR && kv.region_id != REGION_INTERNED) {
                push(vm, value_string_interned(kv.as.str_val));
            } else {
                push(vm, value_clone_fast(&kv));
            }
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_NIL:
#endif
        case OP_NIL: push(vm, value_nil()); break;
#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_TRUE:
#endif
        case OP_TRUE: push(vm, value_bool(true)); break;
#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_FALSE:
#endif
        case OP_FALSE: push(vm, value_bool(false)); break;
#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_UNIT:
#endif
        case OP_UNIT: push(vm, value_unit()); break;

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
            push(vm, value_clone_fast(stackvm_peek(vm, 0)));
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
                /* Use cached str_len when available */
                size_t la = (!ra && a.as.str_len) ? a.as.str_len : strlen(pa);
                size_t lb = (!rb && b.as.str_len) ? b.as.str_len : strlen(pb);
                /* Optimization: when the left operand is a malloc'd string
                 * (REGION_NONE), realloc in-place to avoid copying the left
                 * side.  This turns the common s += "x" pattern from O(n)
                 * per append into amortized O(1) (realloc often extends). */
                char *buf;
                if (a.type == VAL_STR && a.region_id == REGION_NONE && !ra) {
                    buf = realloc(a.as.str_val, la + lb + 1);
                    memcpy(buf + la, pb, lb);
                    buf[la + lb] = '\0';
                    /* a's string has been consumed by realloc; prevent double-free */
                    a.as.str_val = NULL;
                    a.type = VAL_NIL;
                } else {
                    buf = malloc(la + lb + 1);
                    if (!buf) return STACKVM_RUNTIME_ERROR;
                    memcpy(buf, pa, la);
                    memcpy(buf + la, pb, lb);
                    buf[la + lb] = '\0';
                }
                LatValue result = value_string_owned_len(buf, la + lb);
                free(ra);
                free(rb);
                value_free(&a);
                value_free(&b);
                /* Intern short concat results for pointer-equality comparisons */
                push(vm, stackvm_try_intern(result));
                break;
            } else {
                value_free(&a);
                value_free(&b);
                VM_ERROR("operands must be numbers for '+'");
                break;
            }
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_SUB:
#endif
        case OP_SUB: {
            LatValue b = pop(vm), a = pop(vm);
            if (a.type == VAL_INT && b.type == VAL_INT) push(vm, value_int(a.as.int_val - b.as.int_val));
            else if (a.type == VAL_FLOAT && b.type == VAL_FLOAT) push(vm, value_float(a.as.float_val - b.as.float_val));
            else if (a.type == VAL_INT && b.type == VAL_FLOAT)
                push(vm, value_float((double)a.as.int_val - b.as.float_val));
            else if (a.type == VAL_FLOAT && b.type == VAL_INT)
                push(vm, value_float(a.as.float_val - (double)b.as.int_val));
            else {
                value_free(&a);
                value_free(&b);
                VM_ERROR("operands must be numbers for '-'");
                break;
            }
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_MUL:
#endif
        case OP_MUL: {
            LatValue b = pop(vm), a = pop(vm);
            if (a.type == VAL_INT && b.type == VAL_INT) push(vm, value_int(a.as.int_val * b.as.int_val));
            else if (a.type == VAL_FLOAT && b.type == VAL_FLOAT) push(vm, value_float(a.as.float_val * b.as.float_val));
            else if (a.type == VAL_INT && b.type == VAL_FLOAT)
                push(vm, value_float((double)a.as.int_val * b.as.float_val));
            else if (a.type == VAL_FLOAT && b.type == VAL_INT)
                push(vm, value_float(a.as.float_val * (double)b.as.int_val));
            else {
                value_free(&a);
                value_free(&b);
                VM_ERROR("operands must be numbers for '*'");
                break;
            }
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_DIV:
#endif
        case OP_DIV: {
            LatValue b = pop(vm), a = pop(vm);
            if (a.type == VAL_INT && b.type == VAL_INT) {
                if (b.as.int_val == 0) {
                    VM_ERROR("division by zero");
                    break;
                }
                push(vm, value_int(a.as.int_val / b.as.int_val));
            } else if (a.type == VAL_FLOAT || b.type == VAL_FLOAT) {
                double fa = a.type == VAL_INT ? (double)a.as.int_val : a.as.float_val;
                double fb = b.type == VAL_INT ? (double)b.as.int_val : b.as.float_val;
                /* Let IEEE 754 produce NaN/Inf for float division by zero */
                push(vm, value_float(fa / fb));
            } else {
                value_free(&a);
                value_free(&b);
                VM_ERROR("operands must be numbers for '/'");
                break;
            }
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_MOD:
#endif
        case OP_MOD: {
            LatValue b = pop(vm), a = pop(vm);
            if (a.type == VAL_INT && b.type == VAL_INT) {
                if (b.as.int_val == 0) {
                    VM_ERROR("modulo by zero");
                    break;
                }
                push(vm, value_int(a.as.int_val % b.as.int_val));
            } else {
                value_free(&a);
                value_free(&b);
                VM_ERROR("operands must be integers for '%%'");
                break;
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
                VM_ERROR("operand must be a number for unary '-'");
                break;
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
            value_free(&a);
            value_free(&b);
            push(vm, value_bool(eq));
            break;
        }
#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_NEQ:
#endif
        case OP_NEQ: {
            LatValue b = pop(vm), a = pop(vm);
            bool eq = value_eq(&a, &b);
            value_free(&a);
            value_free(&b);
            push(vm, value_bool(!eq));
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_LT:
#endif
        case OP_LT: {
            LatValue b = pop(vm), a = pop(vm);
            if (a.type == VAL_INT && b.type == VAL_INT) push(vm, value_bool(a.as.int_val < b.as.int_val));
            else if (a.type == VAL_FLOAT || b.type == VAL_FLOAT) {
                double fa = a.type == VAL_INT ? (double)a.as.int_val : a.as.float_val;
                double fb = b.type == VAL_INT ? (double)b.as.int_val : b.as.float_val;
                push(vm, value_bool(fa < fb));
            } else {
                value_free(&a);
                value_free(&b);
                VM_ERROR("operands must be numbers for '<'");
                break;
            }
            break;
        }
#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_GT:
#endif
        case OP_GT: {
            LatValue b = pop(vm), a = pop(vm);
            if (a.type == VAL_INT && b.type == VAL_INT) push(vm, value_bool(a.as.int_val > b.as.int_val));
            else if (a.type == VAL_FLOAT || b.type == VAL_FLOAT) {
                double fa = a.type == VAL_INT ? (double)a.as.int_val : a.as.float_val;
                double fb = b.type == VAL_INT ? (double)b.as.int_val : b.as.float_val;
                push(vm, value_bool(fa > fb));
            } else {
                value_free(&a);
                value_free(&b);
                VM_ERROR("operands must be numbers for '>'");
                break;
            }
            break;
        }
#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_LTEQ:
#endif
        case OP_LTEQ: {
            LatValue b = pop(vm), a = pop(vm);
            if (a.type == VAL_INT && b.type == VAL_INT) push(vm, value_bool(a.as.int_val <= b.as.int_val));
            else if (a.type == VAL_FLOAT || b.type == VAL_FLOAT) {
                double fa = a.type == VAL_INT ? (double)a.as.int_val : a.as.float_val;
                double fb = b.type == VAL_INT ? (double)b.as.int_val : b.as.float_val;
                push(vm, value_bool(fa <= fb));
            } else {
                value_free(&a);
                value_free(&b);
                VM_ERROR("operands must be numbers for '<='");
                break;
            }
            break;
        }
#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_GTEQ:
#endif
        case OP_GTEQ: {
            LatValue b = pop(vm), a = pop(vm);
            if (a.type == VAL_INT && b.type == VAL_INT) push(vm, value_bool(a.as.int_val >= b.as.int_val));
            else if (a.type == VAL_FLOAT || b.type == VAL_FLOAT) {
                double fa = a.type == VAL_INT ? (double)a.as.int_val : a.as.float_val;
                double fb = b.type == VAL_INT ? (double)b.as.int_val : b.as.float_val;
                push(vm, value_bool(fa >= fb));
            } else {
                value_free(&a);
                value_free(&b);
                VM_ERROR("operands must be numbers for '>='");
                break;
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
                value_free(&a);
                value_free(&b);
                VM_ERROR("operands must be integers for '&'");
                break;
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
                value_free(&a);
                value_free(&b);
                VM_ERROR("operands must be integers for '|'");
                break;
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
                value_free(&a);
                value_free(&b);
                VM_ERROR("operands must be integers for '^'");
                break;
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
                VM_ERROR("operand must be an integer for '~'");
                break;
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
                    VM_ERROR("shift amount out of range (0..63)");
                    break;
                }
                push(vm, value_int(a.as.int_val << b.as.int_val));
            } else {
                value_free(&a);
                value_free(&b);
                VM_ERROR("operands must be integers for '<<'");
                break;
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
                    VM_ERROR("shift amount out of range (0..63)");
                    break;
                }
                push(vm, value_int(a.as.int_val >> b.as.int_val));
            } else {
                value_free(&a);
                value_free(&b);
                VM_ERROR("operands must be integers for '>>'");
                break;
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
            LatValue result = stackvm_ephemeral_concat(vm, pa, la, pb, lb);
            free(ra);
            free(rb);
            value_free(&a);
            value_free(&b);
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
            frame->slots[slot] = value_clone_fast(stackvm_peek(vm, 0));
            /* Record history for tracked variables */
            if (vm->rt->tracking_active && frame->chunk->local_names && slot < frame->chunk->local_name_cap &&
                frame->chunk->local_names[slot]) {
                stackvm_record_history(vm, frame->chunk->local_names[slot], &frame->slots[slot]);
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
            /* Move from stack instead of cloning — avoids redundant strdup */
            vm->stack_top--;
            *dest = *vm->stack_top;
            if (dest->region_id == REGION_EPHEMERAL) stackvm_promote_value(dest);
            if (vm->rt->tracking_active && frame->chunk->local_names && slot < frame->chunk->local_name_cap &&
                frame->chunk->local_names[slot]) {
                stackvm_record_history(vm, frame->chunk->local_names[slot], dest);
            }
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
                const char *sug = env_find_similar_name(vm->env, name);
                if (sug) {
                    VM_ERROR("undefined variable '%s' (did you mean '%s'?)", name, sug);
                } else {
                    VM_ERROR("undefined variable '%s'", name);
                }
                break;
            }
            if (ref->type == VAL_CLOSURE && ref->as.closure.native_fn != NULL &&
                ref->as.closure.default_values == VM_NATIVE_MARKER) {
                /* VM natives have no owned allocations — safe to borrow. */
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
                const char *sug = env_find_similar_name(vm->env, name);
                if (sug) {
                    VM_ERROR("undefined variable '%s' (did you mean '%s'?)", name, sug);
                } else {
                    VM_ERROR("undefined variable '%s'", name);
                }
                break;
            }
            if (ref->type == VAL_CLOSURE && ref->as.closure.native_fn != NULL &&
                ref->as.closure.default_values == VM_NATIVE_MARKER) {
                /* VM natives have no owned allocations — safe to borrow. */
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
            LatValue *val = stackvm_peek(vm, 0);
            env_set(vm->env, name, value_clone_fast(val));
            if (vm->rt->tracking_active) { stackvm_record_history(vm, name, val); }
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_SET_GLOBAL_16:
#endif
        case OP_SET_GLOBAL_16: {
            uint16_t idx = READ_U16();
            const char *name = frame->chunk->constants[idx].as.str_val;
            LatValue *val = stackvm_peek(vm, 0);
            env_set(vm->env, name, value_clone_fast(val));
            if (vm->rt->tracking_active) { stackvm_record_history(vm, name, val); }
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_DEFINE_GLOBAL:
#endif
        case OP_DEFINE_GLOBAL: {
            uint8_t idx = READ_BYTE();
            const char *name = frame->chunk->constants[idx].as.str_val;
            LatValue val = pop(vm);
            stackvm_promote_value(&val);

            /* Phase-dispatch overloading: if defining a phase-constrained
             * closure and one already exists, create an overload array */
            if (val.type == VAL_CLOSURE && val.as.closure.native_fn != NULL &&
                val.as.closure.default_values != VM_NATIVE_MARKER && val.as.closure.default_values != VM_EXT_MARKER) {
                Chunk *ch = (Chunk *)val.as.closure.native_fn;
                if (ch->param_phases) {
                    LatValue existing;
                    if (env_get(vm->env, name, &existing)) {
                        if (existing.type == VAL_CLOSURE && existing.as.closure.native_fn != NULL &&
                            existing.as.closure.default_values != VM_NATIVE_MARKER &&
                            existing.as.closure.default_values != VM_EXT_MARKER) {
                            Chunk *ech = (Chunk *)existing.as.closure.native_fn;
                            if (ech->param_phases) {
                                LatValue elems[2] = {existing, val};
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
                            if (!new_elems) return STACKVM_RUNTIME_ERROR;
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
            stackvm_promote_value(&val);
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
                *frame->upvalues[slot]->location = value_clone_fast(stackvm_peek(vm, 0));
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
            if (is_falsy(stackvm_peek(vm, 0))) frame->ip += offset;
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_JUMP_IF_TRUE:
#endif
        case OP_JUMP_IF_TRUE: {
            uint16_t offset = READ_U16();
            if (!is_falsy(stackvm_peek(vm, 0))) frame->ip += offset;
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_JUMP_IF_NOT_NIL:
#endif
        case OP_JUMP_IF_NOT_NIL: {
            uint16_t offset = READ_U16();
            if (stackvm_peek(vm, 0)->type != VAL_NIL) frame->ip += offset;
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
            LatValue *callee = stackvm_peek(vm, arg_count);

            if (callee->type == VAL_CLOSURE && callee->as.closure.native_fn != NULL &&
                callee->as.closure.default_values == VM_NATIVE_MARKER) {
                /* StackVM native builtin function */
                VMNativeFn native = (VMNativeFn)callee->as.closure.native_fn;
                LatValue *args = (arg_count <= 16) ? vm->fast_args : malloc(arg_count * sizeof(LatValue));
                for (int i = arg_count - 1; i >= 0; i--) args[i] = pop(vm);
                LatValue callee_val = pop(vm); /* pop callee */
                LatValue ret = native(args, arg_count);
                /* Bridge: native errors from runtime to StackVM */
                if (vm->rt->error) {
                    vm->error = vm->rt->error;
                    vm->rt->error = NULL;
                }
                for (int i = 0; i < arg_count; i++) value_free(&args[i]);
                if (arg_count > 16) free(args);
                value_free(&callee_val);
                /* Check if native set an error (e.g. grow() seed failure) */
                if (vm->error) {
                    value_free(&ret);
                    StackVMResult r = stackvm_handle_native_error(vm, &frame);
                    if (r != STACKVM_OK) return r;
                    break;
                }
                push(vm, ret);
                break;
            }

            if (callee->type == VAL_CLOSURE && callee->as.closure.native_fn != NULL &&
                callee->as.closure.default_values == VM_EXT_MARKER) {
                /* Extension native function — call via ext_call_native() */
                LatValue *args = (arg_count <= 16) ? vm->fast_args : malloc(arg_count * sizeof(LatValue));
                for (int i = arg_count - 1; i >= 0; i--) args[i] = pop(vm);
                LatValue callee_val = pop(vm);
                LatValue ret = ext_call_native(callee_val.as.closure.native_fn, args, (size_t)arg_count);
                for (int i = 0; i < arg_count; i++) value_free(&args[i]);
                if (arg_count > 16) free(args);
                value_free(&callee_val);
                /* Extension errors return strings prefixed with "EVAL_ERROR:" */
                if (ret.type == VAL_STR && strncmp(ret.as.str_val, "EVAL_ERROR:", 11) == 0) {
                    vm->error = strdup(ret.as.str_val + 11);
                    value_free(&ret);
                    StackVMResult r = stackvm_handle_native_error(vm, &frame);
                    if (r != STACKVM_OK) return r;
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
                        LatValue *arg = stackvm_peek(vm, arg_count - 1 - j);
                        if (pp == PHASE_FLUID) {
                            if (arg->phase == VTAG_CRYSTAL) {
                                compatible = false;
                                break;
                            }
                            if (arg->phase == VTAG_FLUID) score += 3;
                            else score += 1;
                        } else if (pp == PHASE_CRYSTAL) {
                            if (arg->phase == VTAG_FLUID) {
                                compatible = false;
                                break;
                            }
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
                    VM_ERROR("no matching overload for given argument phases");
                    break;
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
                        LatValue *arg = stackvm_peek(vm, arg_count - 1 - i);
                        if (pp == PHASE_FLUID && arg->phase == VTAG_CRYSTAL) {
                            phase_mismatch = true;
                            break;
                        }
                        if (pp == PHASE_CRYSTAL && arg->phase == VTAG_FLUID) {
                            phase_mismatch = true;
                            break;
                        }
                    }
                    if (phase_mismatch) {
                        VM_ERROR("phase constraint violation in function '%s'",
                                 fn_chunk->name ? fn_chunk->name : "<anonymous>");
                        break;
                    }
                }

                int adjusted = stackvm_adjust_call_args(vm, fn_chunk, arity, (int)arg_count);
                if (adjusted < 0) {
                    char *err = vm->error;
                    vm->error = NULL;
                    VM_ERROR("%s", err);
                    free(err);
                    break;
                }
                (void)adjusted;

                if (vm->frame_count >= STACKVM_FRAMES_MAX) {
                    VM_ERROR("stack overflow (too many nested calls)");
                    break;
                }

                stackvm_promote_frame_ephemerals(vm, frame);

                /* Get upvalues from the callee closure */
                ObjUpvalue **callee_upvalues = (ObjUpvalue **)callee->as.closure.captured_env;
                size_t callee_upvalue_count = callee->region_id != REGION_NONE ? callee->region_id : 0;

                StackCallFrame *new_frame = &vm->frames[vm->frame_count++];
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
                if (!upvalues) return STACKVM_RUNTIME_ERROR;
                for (uint8_t i = 0; i < upvalue_count; i++) {
                    uint8_t is_local = READ_BYTE();
                    uint8_t index = READ_BYTE();
                    if (is_local) {
                        upvalues[i] = capture_upvalue(vm, &frame->slots[index]);
                    } else {
                        if (frame->upvalues && index < frame->upvalue_count) upvalues[i] = frame->upvalues[index];
                        else upvalues[i] = new_upvalue(&frame->slots[0]); /* fallback */
                    }
                }
            }

            /* Store upvalues in the closure value for later OP_CALL.
             * We pack them into the closure's captured_env as a hack. */
            fn_val.as.closure.captured_env = (Env *)upvalues;
            fn_val.as.closure.has_variadic = (upvalue_count > 0);
            fn_val.region_id = (size_t)upvalue_count;

            /* Track the chunk so it gets freed */
            if (fn_val.as.closure.native_fn) { /* Don't double-track - the chunk is in the constant pool already */
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
                if (!upvalues) return STACKVM_RUNTIME_ERROR;
                for (uint8_t i = 0; i < upvalue_count; i++) {
                    uint8_t is_local = READ_BYTE();
                    uint8_t index = READ_BYTE();
                    if (is_local) {
                        upvalues[i] = capture_upvalue(vm, &frame->slots[index]);
                    } else {
                        if (frame->upvalues && index < frame->upvalue_count) upvalues[i] = frame->upvalues[index];
                        else upvalues[i] = new_upvalue(&frame->slots[0]);
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
                /* Returned to the entry frame of this stackvm_run invocation.
                 * Free remaining stack values down to our entry point. */
                while (vm->stack_top > base) {
                    vm->stack_top--;
                    value_free(vm->stack_top);
                }
                *result = ret;
                return STACKVM_OK;
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
            LatValue *iter = stackvm_peek(vm, 0);
            if (iter->type == VAL_MAP || iter->type == VAL_SET) {
                /* Convert map/set to array for iteration */
                stackvm_iter_convert_to_array(iter);
            }
            if (iter->type != VAL_RANGE && iter->type != VAL_ARRAY) {
                VM_ERROR("cannot iterate over %s", value_type_name(iter));
                break;
            }
            push(vm, value_int(0)); /* index */
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_ITER_NEXT:
#endif
        case OP_ITER_NEXT: {
            uint16_t offset = READ_U16();
            LatValue *idx_val = stackvm_peek(vm, 0); /* index */
            LatValue *iter = stackvm_peek(vm, 1);    /* collection */
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
                for (int i = count - 1; i >= 0; i--) elems[i] = pop(vm);
                for (int i = 0; i < count; i++) stackvm_promote_value(&elems[i]);
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
                if (arr.as.array.elems[i].type == VAL_ARRAY) total += arr.as.array.elems[i].as.array.len;
                else total++;
            }
            LatValue *flat = malloc(total * sizeof(LatValue));
            if (!flat) return STACKVM_RUNTIME_ERROR;
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
                if (!pairs) return STACKVM_RUNTIME_ERROR;
                for (int i = pair_count * 2 - 1; i >= 0; i--) pairs[i] = pop(vm);
                for (uint8_t i = 0; i < pair_count; i++) {
                    LatValue key = pairs[i * 2];
                    LatValue val = pairs[i * 2 + 1];
                    stackvm_promote_value(&val);
                    if (key.type == VAL_STR) { lat_map_set(map.as.map.map, key.as.str_val, &val); }
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
                for (int i = count - 1; i >= 0; i--) elems[i] = pop(vm);
                for (int i = 0; i < count; i++) stackvm_promote_value(&elems[i]);
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
            for (int i = field_count - 1; i >= 0; i--) field_values[i] = pop(vm);
            for (int i = 0; i < field_count; i++) stackvm_promote_value(&field_values[i]);

            LatValue s = value_struct_vm(struct_name, field_names, field_values, field_count);

            /* Alloy enforcement: apply per-field phase from struct declaration */
            char phase_key[256];
            snprintf(phase_key, sizeof(phase_key), "__struct_phases_%s", struct_name);
            LatValue *phase_ref = env_get_ref(vm->env, phase_key);
            if (phase_ref && phase_ref->type == VAL_ARRAY && phase_ref->as.array.len == field_count) {
                s.as.strct.field_phases = calloc(field_count, sizeof(PhaseTag));
                for (uint8_t i = 0; s.as.strct.field_phases && i < field_count; i++) {
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

            if (field_count > 16) {
                free(field_names);
                free(field_values);
            }
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
                value_free(&start);
                value_free(&end);
                VM_ERROR("range bounds must be integers");
                break;
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
                if (!payload) return STACKVM_RUNTIME_ERROR;
                for (int i = payload_count - 1; i >= 0; i--) payload[i] = pop(vm);
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
                        VM_ERROR("array index out of bounds: %lld (len %zu)", (long long)i, inner->as.array.len);
                        break;
                    }
                    LatValue elem = value_deep_clone(&inner->as.array.elems[i]);
                    value_free(&obj);
                    push(vm, elem);
                    break;
                }
                if (inner->type == VAL_MAP && idx.type == VAL_STR) {
                    LatValue *found = lat_map_get(inner->as.map.map, idx.as.str_val);
                    if (found) push(vm, value_deep_clone(found));
                    else push(vm, value_nil());
                    value_free(&obj);
                    value_free(&idx);
                    break;
                }
                const char *it = value_type_name(&idx);
                const char *innert = value_type_name(inner);
                value_free(&obj);
                value_free(&idx);
                VM_ERROR("invalid index operation: Ref<%s>[%s]", innert, it);
                break;
            }
            if (obj.type == VAL_ARRAY && idx.type == VAL_INT) {
                int64_t i = idx.as.int_val;
                if (i < 0 || (size_t)i >= obj.as.array.len) {
                    value_free(&obj);
                    VM_ERROR("array index out of bounds: %lld (len %zu)", (long long)i, obj.as.array.len);
                    break;
                }
                LatValue elem = value_deep_clone(&obj.as.array.elems[i]);
                value_free(&obj);
                push(vm, elem);
            } else if (obj.type == VAL_MAP && idx.type == VAL_STR) {
                LatValue *found = lat_map_get(obj.as.map.map, idx.as.str_val);
                if (found) push(vm, value_deep_clone(found));
                else push(vm, value_nil());
                value_free(&obj);
                value_free(&idx);
            } else if (obj.type == VAL_STR && idx.type == VAL_INT) {
                int64_t i = idx.as.int_val;
                size_t len = strlen(obj.as.str_val);
                if (i < 0 || (size_t)i >= len) {
                    value_free(&obj);
                    VM_ERROR("string index out of bounds");
                    break;
                }
                char ch[2] = {obj.as.str_val[i], '\0'};
                value_free(&obj);
                push(vm, value_string(ch));
            } else if (obj.type == VAL_TUPLE && idx.type == VAL_INT) {
                int64_t i = idx.as.int_val;
                if (i < 0 || (size_t)i >= obj.as.tuple.len) {
                    value_free(&obj);
                    VM_ERROR("tuple index out of bounds");
                    break;
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
                    if (!slice) return STACKVM_RUNTIME_ERROR;
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
                    if (!elems) return STACKVM_RUNTIME_ERROR;
                    for (size_t i = 0; i < slice_len; i++) elems[i] = value_deep_clone(&obj.as.array.elems[start + i]);
                    value_free(&obj);
                    push(vm, value_array(elems, slice_len));
                    free(elems);
                }
            } else if (obj.type == VAL_BUFFER && idx.type == VAL_INT) {
                int64_t i = idx.as.int_val;
                if (i < 0 || (size_t)i >= obj.as.buffer.len) {
                    value_free(&obj);
                    VM_ERROR("buffer index out of bounds: %lld (len %zu)", (long long)i, obj.as.buffer.len);
                    break;
                }
                LatValue elem = value_int(obj.as.buffer.data[i]);
                value_free(&obj);
                push(vm, elem);
            } else {
                const char *ot = value_type_name(&obj);
                const char *it = value_type_name(&idx);
                value_free(&obj);
                value_free(&idx);
                VM_ERROR("invalid index operation: %s[%s]", ot, it);
                break;
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
                    value_free(&obj);
                    value_free(&idx);
                    value_free(&val);
                    VM_ERROR("cannot assign index on a frozen Ref");
                    break;
                }
                LatValue *inner = &obj.as.ref.ref->value;
                if (inner->type == VAL_ARRAY && idx.type == VAL_INT) {
                    int64_t i = idx.as.int_val;
                    if (i < 0 || (size_t)i >= inner->as.array.len) {
                        value_free(&obj);
                        value_free(&val);
                        VM_ERROR("array index out of bounds in assignment");
                        break;
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
                value_free(&obj);
                value_free(&idx);
                value_free(&val);
                VM_ERROR("invalid index assignment on Ref");
                break;
            }
            if (obj.type == VAL_ARRAY && idx.type == VAL_INT) {
                int64_t i = idx.as.int_val;
                if (i < 0 || (size_t)i >= obj.as.array.len) {
                    value_free(&obj);
                    value_free(&val);
                    VM_ERROR("array index out of bounds in assignment");
                    break;
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
                    value_free(&obj);
                    value_free(&val);
                    VM_ERROR("buffer index out of bounds in assignment");
                    break;
                }
                obj.as.buffer.data[i] = (uint8_t)(val.as.int_val & 0xFF);
                value_free(&val);
                push(vm, obj);
            } else {
                value_free(&obj);
                value_free(&idx);
                value_free(&val);
                VM_ERROR("invalid index assignment");
                break;
            }
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_GET_FIELD:
#endif
        case OP_GET_FIELD: {
            uint8_t name_idx = READ_BYTE();
            const char *field_name = frame->chunk->constants[name_idx].as.str_val;
            const char *interned_name = intern(field_name);
            LatValue obj = pop(vm);

            if (obj.type == VAL_STRUCT) {
                bool found = false;
                for (size_t i = 0; i < obj.as.strct.field_count; i++) {
                    if (obj.as.strct.field_names[i] == interned_name) {
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
                    VM_ERROR("struct has no field '%s'", field_name);
                    break;
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
                    VM_ERROR("tuple has no field '%s'", field_name);
                    break;
                }
                value_free(&obj);
            } else if (obj.type == VAL_ENUM) {
                if (strcmp(field_name, "tag") == 0) {
                    push(vm, value_string(obj.as.enm.variant_name));
                } else if (strcmp(field_name, "payload") == 0) {
                    /* Always return an array of all payloads */
                    if (obj.as.enm.payload_count > 0) {
                        LatValue *elems = malloc(obj.as.enm.payload_count * sizeof(LatValue));
                        if (!elems) return STACKVM_RUNTIME_ERROR;
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
                VM_ERROR("cannot access field '%s' on %s", field_name, tn);
                break;
            }
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_SET_FIELD:
#endif
        case OP_SET_FIELD: {
            uint8_t name_idx = READ_BYTE();
            const char *field_name = frame->chunk->constants[name_idx].as.str_val;
            const char *interned_fname = intern(field_name);
            LatValue obj = pop(vm);
            LatValue val = pop(vm);
            stackvm_promote_value(&val);

            if (obj.type == VAL_STRUCT) {
                /* Check overall struct phase (only if no per-field phases set) */
                if ((obj.phase == VTAG_CRYSTAL || obj.phase == VTAG_SUBLIMATED) && !obj.as.strct.field_phases) {
                    value_free(&obj);
                    value_free(&val);
                    VM_ERROR("cannot assign to field '%s' on a %s struct", field_name,
                             obj.phase == VTAG_CRYSTAL ? "frozen" : "sublimated");
                    break;
                }
                /* Check per-field phase constraints (alloy types) */
                bool field_frozen = false;
                if (obj.as.strct.field_phases) {
                    for (size_t i = 0; i < obj.as.strct.field_count; i++) {
                        if (obj.as.strct.field_names[i] == interned_fname) {
                            if (obj.as.strct.field_phases[i] == VTAG_CRYSTAL) field_frozen = true;
                            break;
                        }
                    }
                }
                if (field_frozen) {
                    value_free(&obj);
                    value_free(&val);
                    VM_ERROR("cannot assign to frozen field '%s'", field_name);
                    break;
                }
                bool found = false;
                for (size_t i = 0; i < obj.as.strct.field_count; i++) {
                    if (obj.as.strct.field_names[i] == interned_fname) {
                        value_free(&obj.as.strct.field_values[i]);
                        obj.as.strct.field_values[i] = val;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    value_free(&obj);
                    value_free(&val);
                    VM_ERROR("struct has no field '%s'", field_name);
                    break;
                }
                push(vm, obj);
            } else if (obj.type == VAL_MAP) {
                lat_map_set(obj.as.map.map, field_name, &val);
                push(vm, obj);
            } else {
                value_free(&obj);
                value_free(&val);
                VM_ERROR("cannot set field on non-struct/map value");
                break;
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
            LatValue *obj = stackvm_peek(vm, arg_count);

            if (stackvm_invoke_builtin(vm, obj, method_name, arg_count, NULL)) {
                if (vm->error) {
                    StackVMResult r = stackvm_handle_native_error(vm, &frame);
                    if (r != STACKVM_OK) return r;
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
                        int adjusted = stackvm_adjust_call_args(vm, fn_chunk, arity, (int)arg_count);
                        if (adjusted < 0) {
                            char *err = vm->error;
                            vm->error = NULL;
                            VM_ERROR("%s", err);
                            free(err);
                            break;
                        }
                        (void)adjusted;
                        ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                        size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;
                        if (vm->frame_count >= STACKVM_FRAMES_MAX) {
                            VM_ERROR("stack overflow (too many nested calls)");
                            break;
                        }
                        stackvm_promote_frame_ephemerals(vm, frame);
                        /* Replace the map on the stack with a closure placeholder
                         * so OP_RETURN can clean up properly. */
                        LatValue closure_copy = value_deep_clone(field);
                        value_free(obj);
                        *obj = closure_copy;
                        StackCallFrame *new_frame = &vm->frames[vm->frame_count++];
                        new_frame->chunk = fn_chunk;
                        new_frame->ip = fn_chunk->code;
                        new_frame->slots = obj;
                        new_frame->upvalues = upvals;
                        new_frame->upvalue_count = uv_count;
                        frame = new_frame;
                        break;
                    }
                    if (field && field->type == VAL_CLOSURE && field->as.closure.default_values == VM_NATIVE_MARKER) {
                        /* StackVM native function stored in map */
                        VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                        LatValue *args = (arg_count <= 16) ? vm->fast_args : malloc(arg_count * sizeof(LatValue));
                        for (int i = arg_count - 1; i >= 0; i--) args[i] = pop(vm);
                        LatValue obj_val = pop(vm);
                        LatValue ret = native(args, arg_count);
                        /* Bridge: native errors from runtime to StackVM */
                        if (vm->rt->error) {
                            vm->error = vm->rt->error;
                            vm->rt->error = NULL;
                        }
                        for (int i = 0; i < arg_count; i++) value_free(&args[i]);
                        if (arg_count > 16) free(args);
                        value_free(&obj_val);
                        push(vm, ret);
                        break;
                    }
                }

                /* Check if struct has a callable closure field */
                if (obj->type == VAL_STRUCT) {
                    const char *imethod = intern(method_name);
                    bool handled = false;
                    for (size_t fi = 0; fi < obj->as.strct.field_count; fi++) {
                        if (obj->as.strct.field_names[fi] != imethod) continue;
                        LatValue *field = &obj->as.strct.field_values[fi];
                        if (field->type == VAL_CLOSURE && field->as.closure.native_fn &&
                            field->as.closure.default_values != VM_NATIVE_MARKER) {
                            /* Bytecode closure in struct field — inject self */
                            Chunk *fn_chunk = (Chunk *)field->as.closure.native_fn;
                            ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                            size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;
                            if (vm->frame_count >= STACKVM_FRAMES_MAX) {
                                VM_ERROR("stack overflow (too many nested calls)");
                                break;
                            }
                            stackvm_promote_frame_ephemerals(vm, frame);
                            LatValue self_copy = value_deep_clone(obj);
                            LatValue closure_copy = value_deep_clone(field);
                            /* Shift args up by 1 to make room for self */
                            push(vm, value_nil());
                            for (int si = arg_count; si >= 1; si--) obj[si + 1] = obj[si];
                            obj[1] = self_copy;
                            value_free(obj);
                            *obj = closure_copy;
                            StackCallFrame *new_frame = &vm->frames[vm->frame_count++];
                            new_frame->chunk = fn_chunk;
                            new_frame->ip = fn_chunk->code;
                            new_frame->slots = obj;
                            new_frame->upvalues = upvals;
                            new_frame->upvalue_count = uv_count;
                            frame = new_frame;
                            handled = true;
                            break;
                        }
                        if (field->type == VAL_CLOSURE && field->as.closure.default_values == VM_NATIVE_MARKER) {
                            /* StackVM native in struct field — inject self */
                            VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                            LatValue self_copy = value_deep_clone(obj);
                            int total_args = arg_count + 1;
                            LatValue *args = malloc(total_args * sizeof(LatValue));
                            if (!args) return STACKVM_RUNTIME_ERROR;
                            args[0] = self_copy;
                            for (int ai = arg_count - 1; ai >= 0; ai--) args[ai + 1] = pop(vm);
                            LatValue obj_val = pop(vm);
                            LatValue ret = native(args, total_args);
                            for (int ai = 0; ai < total_args; ai++) value_free(&args[ai]);
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
                const char *type_name = (obj->type == VAL_STRUCT) ? obj->as.strct.name
                                        : (obj->type == VAL_ENUM) ? obj->as.enm.enum_name
                                                                  : value_type_name(obj);
                char key[256];
                snprintf(key, sizeof(key), "%s::%s", type_name, method_name);
                LatValue *method_ref = env_get_ref(vm->env, key);
                if (method_ref && method_ref->type == VAL_CLOSURE && method_ref->as.closure.native_fn) {
                    /* Found a compiled method - call it with self + args */
                    Chunk *fn_chunk = (Chunk *)method_ref->as.closure.native_fn;
                    /* Rearrange stack: obj is already below args, use as slot 0 */
                    if (vm->frame_count >= STACKVM_FRAMES_MAX) {
                        VM_ERROR("stack overflow (too many nested calls)");
                        break;
                    }
                    stackvm_promote_frame_ephemerals(vm, frame);
                    StackCallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->chunk = fn_chunk;
                    new_frame->ip = fn_chunk->code;
                    new_frame->slots = obj; /* self is in slot 0 */
                    new_frame->upvalues = NULL;
                    new_frame->upvalue_count = 0;
                    frame = new_frame;
                } else {
                    /* Method not found - error with suggestion */
                    const char *tname = value_type_name(obj);
                    int otype = obj->type;
                    for (int i = 0; i < arg_count; i++) {
                        LatValue v = pop(vm);
                        value_free(&v);
                    }
                    LatValue obj_val = pop(vm);
                    value_free(&obj_val);
                    const char *msug = builtin_find_similar_method(otype, method_name);
                    if (msug) {
                        VM_ERROR("type '%s' has no method '%s' (did you mean '%s'?)", tname, method_name, msug);
                    } else {
                        VM_ERROR("type '%s' has no method '%s'", tname, method_name);
                    }
                    break;
                }
            }
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_INVOKE_LOCAL:
#endif
        case OP_INVOKE_LOCAL: {
            size_t _pic_off = (size_t)(frame->ip - frame->chunk->code - 1);
            uint8_t slot = READ_BYTE();
            uint8_t method_idx = READ_BYTE();
            uint8_t arg_count = READ_BYTE();
            const char *method_name = frame->chunk->constants[method_idx].as.str_val;
            LatValue *obj = &frame->slots[slot]; /* Direct pointer to local */

            /* ── PIC fast path ── */
            uint8_t _obj_type = (uint8_t)obj->type;
            uint32_t _mhash = method_hash(method_name);
            PICSlot *_pic = pic_slot_for(&frame->chunk->pic, _pic_off);
            uint16_t _pic_id = _pic ? pic_lookup(_pic, _obj_type, _mhash) : 0;
            if (_pic_id == PIC_NOT_BUILTIN) goto invoke_local_not_builtin;

            const char *local_var_name = (frame->chunk->local_names && slot < frame->chunk->local_name_cap)
                                             ? frame->chunk->local_names[slot]
                                             : NULL;
            if (stackvm_invoke_builtin(vm, obj, method_name, arg_count, local_var_name)) {
                if (vm->error) {
                    StackVMResult r = stackvm_handle_native_error(vm, &frame);
                    if (r != STACKVM_OK) return r;
                    break;
                }
                /* Cache this as a builtin hit */
                if (!_pic) {
                    pic_table_ensure(&frame->chunk->pic);
                    _pic = pic_slot_for(&frame->chunk->pic, _pic_off);
                }
                if (_pic && _pic_id == 0) {
                    uint16_t _rid = pic_resolve_builtin_id(_obj_type, _mhash);
                    if (_rid) pic_update(_pic, _obj_type, _mhash, _rid);
                }
                /* Builtin popped args and pushed result.
                 * obj was mutated in-place (e.g. push modified the array). */
                break;
            }
            /* Cache as NOT_BUILTIN for this type+method */
            if (!_pic) {
                pic_table_ensure(&frame->chunk->pic);
                _pic = pic_slot_for(&frame->chunk->pic, _pic_off);
            }
            if (_pic) pic_update(_pic, _obj_type, _mhash, PIC_NOT_BUILTIN);
        invoke_local_not_builtin:

            /* Check if map has a callable closure field */
            if (obj->type == VAL_MAP) {
                LatValue *field = lat_map_get(obj->as.map.map, method_name);
                if (field && field->type == VAL_CLOSURE && field->as.closure.native_fn &&
                    field->as.closure.default_values != VM_NATIVE_MARKER) {
                    /* Bytecode closure stored in local map - call it */
                    Chunk *fn_chunk = (Chunk *)field->as.closure.native_fn;
                    int arity = (int)field->as.closure.param_count;
                    int adjusted = stackvm_adjust_call_args(vm, fn_chunk, arity, (int)arg_count);
                    if (adjusted < 0) {
                        char *err = vm->error;
                        vm->error = NULL;
                        VM_ERROR("%s", err);
                        free(err);
                        break;
                    }
                    arg_count = (uint8_t)adjusted;
                    ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                    size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;
                    if (vm->frame_count >= STACKVM_FRAMES_MAX) {
                        VM_ERROR("stack overflow (too many nested calls)");
                        break;
                    }
                    stackvm_promote_frame_ephemerals(vm, frame);
                    /* Set up frame: closure in slot 0, args follow */
                    LatValue closure_copy = value_deep_clone(field);
                    LatValue *arg_base = vm->stack_top - arg_count;
                    push(vm, value_nil()); /* make room */
                    for (int si = arg_count - 1; si >= 0; si--) arg_base[si + 1] = arg_base[si];
                    arg_base[0] = closure_copy;
                    StackCallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->chunk = fn_chunk;
                    new_frame->ip = fn_chunk->code;
                    new_frame->slots = arg_base;
                    new_frame->upvalues = upvals;
                    new_frame->upvalue_count = uv_count;
                    frame = new_frame;
                    break;
                }
                if (field && field->type == VAL_CLOSURE && field->as.closure.default_values == VM_NATIVE_MARKER) {
                    /* StackVM native function stored in local map */
                    VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                    LatValue *args = (arg_count <= 16) ? vm->fast_args : malloc(arg_count * sizeof(LatValue));
                    for (int i = arg_count - 1; i >= 0; i--) args[i] = pop(vm);
                    LatValue ret = native(args, arg_count);
                    /* Bridge: native errors from runtime to StackVM */
                    if (vm->rt->error) {
                        vm->error = vm->rt->error;
                        vm->rt->error = NULL;
                    }
                    for (int i = 0; i < arg_count; i++) value_free(&args[i]);
                    if (arg_count > 16) free(args);
                    if (vm->error) {
                        value_free(&ret);
                        char *err = vm->error;
                        vm->error = NULL;
                        VM_ERROR("%s", err);
                        free(err);
                        break;
                    }
                    push(vm, ret);
                    break;
                }
            }

            /* Check if struct has a callable closure field */
            if (obj->type == VAL_STRUCT) {
                const char *imethod2 = intern(method_name);
                bool handled = false;
                for (size_t fi = 0; fi < obj->as.strct.field_count; fi++) {
                    if (obj->as.strct.field_names[fi] != imethod2) continue;
                    LatValue *field = &obj->as.strct.field_values[fi];
                    if (field->type == VAL_CLOSURE && field->as.closure.native_fn &&
                        field->as.closure.default_values != VM_NATIVE_MARKER) {
                        /* Bytecode closure — inject [closure, self] below args */
                        Chunk *fn_chunk = (Chunk *)field->as.closure.native_fn;
                        ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                        size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;
                        if (vm->frame_count >= STACKVM_FRAMES_MAX) {
                            VM_ERROR("stack overflow (too many nested calls)");
                            break;
                        }
                        stackvm_promote_frame_ephemerals(vm, frame);
                        LatValue self_copy = value_deep_clone(obj);
                        LatValue closure_copy = value_deep_clone(field);
                        LatValue *arg_base = vm->stack_top - arg_count;
                        push(vm, value_nil());
                        push(vm, value_nil());
                        for (int si = arg_count - 1; si >= 0; si--) arg_base[si + 2] = arg_base[si];
                        arg_base[0] = closure_copy;
                        arg_base[1] = self_copy;
                        StackCallFrame *new_frame = &vm->frames[vm->frame_count++];
                        new_frame->chunk = fn_chunk;
                        new_frame->ip = fn_chunk->code;
                        new_frame->slots = arg_base;
                        new_frame->upvalues = upvals;
                        new_frame->upvalue_count = uv_count;
                        frame = new_frame;
                        handled = true;
                        break;
                    }
                    if (field->type == VAL_CLOSURE && field->as.closure.default_values == VM_NATIVE_MARKER) {
                        /* StackVM native — inject self */
                        VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                        LatValue self_copy = value_deep_clone(obj);
                        int total_args = arg_count + 1;
                        LatValue *args = malloc(total_args * sizeof(LatValue));
                        if (!args) return STACKVM_RUNTIME_ERROR;
                        args[0] = self_copy;
                        for (int ai = arg_count - 1; ai >= 0; ai--) args[ai + 1] = pop(vm);
                        LatValue ret = native(args, total_args);
                        for (int ai = 0; ai < total_args; ai++) value_free(&args[ai]);
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
                const char *type_name = (obj->type == VAL_STRUCT) ? obj->as.strct.name
                                        : (obj->type == VAL_ENUM) ? obj->as.enm.enum_name
                                                                  : value_type_name(obj);
                char key[256];
                snprintf(key, sizeof(key), "%s::%s", type_name, method_name);
                LatValue *method_ref = env_get_ref(vm->env, key);
                if (method_ref && method_ref->type == VAL_CLOSURE && method_ref->as.closure.native_fn) {
                    Chunk *fn_chunk = (Chunk *)method_ref->as.closure.native_fn;
                    if (vm->frame_count >= STACKVM_FRAMES_MAX) {
                        VM_ERROR("stack overflow (too many nested calls)");
                        break;
                    }
                    stackvm_promote_frame_ephemerals(vm, frame);
                    /* Push self (deep clone of local) below args for the new frame. */
                    LatValue *arg_base = vm->stack_top - arg_count;
                    push(vm, value_nil());
                    for (int i = arg_count; i > 0; i--) vm->stack_top[-1 - (arg_count - i)] = arg_base[i - 1];
                    *arg_base = value_deep_clone(obj);
                    StackCallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->chunk = fn_chunk;
                    new_frame->ip = fn_chunk->code;
                    new_frame->slots = arg_base;
                    new_frame->upvalues = NULL;
                    new_frame->upvalue_count = 0;
                    frame = new_frame;
                } else {
                    /* Method not found - error with suggestion */
                    const char *tname = value_type_name(obj);
                    int otype = obj->type;
                    for (int i = 0; i < arg_count; i++) {
                        LatValue v = pop(vm);
                        value_free(&v);
                    }
                    const char *msug = builtin_find_similar_method(otype, method_name);
                    if (msug) {
                        VM_ERROR("type '%s' has no method '%s' (did you mean '%s'?)", tname, method_name, msug);
                    } else {
                        VM_ERROR("type '%s' has no method '%s'", tname, method_name);
                    }
                    break;
                }
            }
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_INVOKE_GLOBAL:
#endif
        case OP_INVOKE_GLOBAL: {
            size_t _gpic_off = (size_t)(frame->ip - frame->chunk->code - 1);
            uint8_t name_idx = READ_BYTE();
            uint8_t method_idx = READ_BYTE();
            uint8_t arg_count = READ_BYTE();
            const char *global_name = frame->chunk->constants[name_idx].as.str_val;
            const char *method_name = frame->chunk->constants[method_idx].as.str_val;

            /* Fast path: simple builtins (no closures) can mutate in place */
            uint32_t mhash_g = method_hash(method_name);

            /* ── PIC: skip builtin dispatch if cached as not-builtin ── */
            PICSlot *_gpic = pic_slot_for(&frame->chunk->pic, _gpic_off);
            /* For INVOKE_GLOBAL we need to know the type of the global to use PIC.
             * We defer the check — the PIC is populated after the first call. */
            bool _gpic_skip_builtin = false;
            {
                LatValue *_gref = env_get_ref(vm->env, global_name);
                if (_gref && _gpic) {
                    uint16_t _gid = pic_lookup(_gpic, (uint8_t)_gref->type, mhash_g);
                    if (_gid == PIC_NOT_BUILTIN) _gpic_skip_builtin = true;
                }
            }

            if (!_gpic_skip_builtin && stackvm_invoke_builtin_is_simple(mhash_g)) {
                LatValue *ref = env_get_ref(vm->env, global_name);
                if (!ref) {
                    const char *sug = env_find_similar_name(vm->env, global_name);
                    if (sug) {
                        VM_ERROR("undefined variable '%s' (did you mean '%s'?)", global_name, sug);
                    } else {
                        VM_ERROR("undefined variable '%s'", global_name);
                    }
                    break;
                }
                if (stackvm_invoke_builtin(vm, ref, method_name, arg_count, global_name)) {
                    if (vm->error) {
                        StackVMResult r = stackvm_handle_native_error(vm, &frame);
                        if (r != STACKVM_OK) return r;
                        break;
                    }
                    /* Update PIC on builtin hit */
                    if (!_gpic) {
                        pic_table_ensure(&frame->chunk->pic);
                        _gpic = pic_slot_for(&frame->chunk->pic, _gpic_off);
                    }
                    if (_gpic) {
                        uint16_t _rid = pic_resolve_builtin_id((uint8_t)ref->type, mhash_g);
                        if (_rid) pic_update(_gpic, (uint8_t)ref->type, mhash_g, _rid);
                    }
                    /* Record history for tracked variables */
                    if (vm->rt->tracking_active) { stackvm_record_history(vm, global_name, ref); }
                    break;
                }
            }

            /* Slow path: closure-invoking builtins or non-builtin dispatch */
            LatValue obj_val;
            if (!env_get(vm->env, global_name, &obj_val)) {
                const char *sug = env_find_similar_name(vm->env, global_name);
                if (sug) {
                    VM_ERROR("undefined variable '%s' (did you mean '%s'?)", global_name, sug);
                } else {
                    VM_ERROR("undefined variable '%s'", global_name);
                }
                break;
            }

            if (!_gpic_skip_builtin && stackvm_invoke_builtin(vm, &obj_val, method_name, arg_count, global_name)) {
                if (vm->error) {
                    value_free(&obj_val);
                    StackVMResult r = stackvm_handle_native_error(vm, &frame);
                    if (r != STACKVM_OK) return r;
                    break;
                }
                /* Update PIC on builtin hit */
                if (!_gpic) {
                    pic_table_ensure(&frame->chunk->pic);
                    _gpic = pic_slot_for(&frame->chunk->pic, _gpic_off);
                }
                if (_gpic) {
                    uint16_t _rid = pic_resolve_builtin_id((uint8_t)obj_val.type, mhash_g);
                    if (_rid) pic_update(_gpic, (uint8_t)obj_val.type, mhash_g, _rid);
                }
                /* Write back the mutated object to the global env */
                env_set(vm->env, global_name, obj_val);
                /* Record history for tracked variables */
                if (vm->rt->tracking_active) {
                    LatValue cur;
                    if (env_get(vm->env, global_name, &cur)) {
                        stackvm_record_history(vm, global_name, &cur);
                        value_free(&cur);
                    }
                }
                break;
            }

            /* Not a builtin — cache PIC_NOT_BUILTIN and fall through */
            if (!_gpic) {
                pic_table_ensure(&frame->chunk->pic);
                _gpic = pic_slot_for(&frame->chunk->pic, _gpic_off);
            }
            if (_gpic) pic_update(_gpic, (uint8_t)obj_val.type, mhash_g, PIC_NOT_BUILTIN);

            /* insert object below args on stack and
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
                    int adjusted = stackvm_adjust_call_args(vm, fn_chunk, arity, (int)arg_count);
                    if (adjusted < 0) {
                        char *err = vm->error;
                        vm->error = NULL;
                        value_free(&obj_val);
                        VM_ERROR("%s", err);
                        free(err);
                        break;
                    }
                    arg_count = (uint8_t)adjusted;
                    /* Recalculate obj pointer — vm_adjust may have pushed defaults */
                    obj = vm->stack_top - arg_count - 1;
                    ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                    size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;
                    if (vm->frame_count >= STACKVM_FRAMES_MAX) {
                        VM_ERROR("stack overflow (too many nested calls)");
                        break;
                    }
                    stackvm_promote_frame_ephemerals(vm, frame);
                    LatValue closure_copy = value_deep_clone(field);
                    value_free(obj);
                    *obj = closure_copy;
                    StackCallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->chunk = fn_chunk;
                    new_frame->ip = fn_chunk->code;
                    new_frame->slots = obj;
                    new_frame->upvalues = upvals;
                    new_frame->upvalue_count = uv_count;
                    frame = new_frame;
                    break;
                }
                if (field && field->type == VAL_CLOSURE && field->as.closure.default_values == VM_NATIVE_MARKER) {
                    VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                    LatValue *args = (arg_count <= 16) ? vm->fast_args : malloc(arg_count * sizeof(LatValue));
                    for (int i = arg_count - 1; i >= 0; i--) args[i] = pop(vm);
                    LatValue obj_popped = pop(vm);
                    LatValue ret = native(args, arg_count);
                    /* Bridge: native errors from runtime to StackVM */
                    if (vm->rt->error) {
                        vm->error = vm->rt->error;
                        vm->rt->error = NULL;
                    }
                    for (int i = 0; i < arg_count; i++) value_free(&args[i]);
                    if (arg_count > 16) free(args);
                    value_free(&obj_popped);
                    push(vm, ret);
                    break;
                }
            }

            if (obj->type == VAL_STRUCT) {
                const char *imethod3 = intern(method_name);
                bool handled = false;
                for (size_t fi = 0; fi < obj->as.strct.field_count; fi++) {
                    if (obj->as.strct.field_names[fi] != imethod3) continue;
                    LatValue *field = &obj->as.strct.field_values[fi];
                    if (field->type == VAL_CLOSURE && field->as.closure.native_fn &&
                        field->as.closure.default_values != VM_NATIVE_MARKER) {
                        Chunk *fn_chunk = (Chunk *)field->as.closure.native_fn;
                        ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                        size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;
                        if (vm->frame_count >= STACKVM_FRAMES_MAX) {
                            VM_ERROR("stack overflow (too many nested calls)");
                            break;
                        }
                        stackvm_promote_frame_ephemerals(vm, frame);
                        LatValue self_copy = value_deep_clone(obj);
                        LatValue closure_copy = value_deep_clone(field);
                        push(vm, value_nil());
                        for (int si = arg_count; si >= 1; si--) obj[si + 1] = obj[si];
                        obj[1] = self_copy;
                        value_free(obj);
                        *obj = closure_copy;
                        StackCallFrame *new_frame = &vm->frames[vm->frame_count++];
                        new_frame->chunk = fn_chunk;
                        new_frame->ip = fn_chunk->code;
                        new_frame->slots = obj;
                        new_frame->upvalues = upvals;
                        new_frame->upvalue_count = uv_count;
                        frame = new_frame;
                        handled = true;
                        break;
                    }
                    if (field->type == VAL_CLOSURE && field->as.closure.default_values == VM_NATIVE_MARKER) {
                        VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                        LatValue self_copy = value_deep_clone(obj);
                        int total_args = arg_count + 1;
                        LatValue *args = malloc(total_args * sizeof(LatValue));
                        if (!args) return STACKVM_RUNTIME_ERROR;
                        args[0] = self_copy;
                        for (int ai = arg_count - 1; ai >= 0; ai--) args[ai + 1] = pop(vm);
                        LatValue obj_popped = pop(vm);
                        LatValue ret = native(args, total_args);
                        for (int ai = 0; ai < total_args; ai++) value_free(&args[ai]);
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
                const char *type_name = (obj->type == VAL_STRUCT) ? obj->as.strct.name
                                        : (obj->type == VAL_ENUM) ? obj->as.enm.enum_name
                                                                  : value_type_name(obj);
                char key[256];
                snprintf(key, sizeof(key), "%s::%s", type_name, method_name);
                LatValue *method_ref = env_get_ref(vm->env, key);
                if (method_ref && method_ref->type == VAL_CLOSURE && method_ref->as.closure.native_fn) {
                    Chunk *fn_chunk = (Chunk *)method_ref->as.closure.native_fn;
                    if (vm->frame_count >= STACKVM_FRAMES_MAX) {
                        VM_ERROR("stack overflow (too many nested calls)");
                        break;
                    }
                    stackvm_promote_frame_ephemerals(vm, frame);
                    /* Replace obj with self clone, shift args */
                    LatValue self_copy = value_deep_clone(obj);
                    value_free(obj);
                    *obj = self_copy;
                    StackCallFrame *new_frame = &vm->frames[vm->frame_count++];
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
        lbl_OP_INVOKE_LOCAL_16:
#endif
        case OP_INVOKE_LOCAL_16: {
            size_t _pic16_off = (size_t)(frame->ip - frame->chunk->code - 1);
            uint8_t slot = READ_BYTE();
            uint16_t method_idx = READ_U16();
            uint8_t arg_count = READ_BYTE();
            const char *method_name = frame->chunk->constants[method_idx].as.str_val;
            LatValue *obj = &frame->slots[slot]; /* Direct pointer to local */

            /* ── PIC fast path ── */
            uint8_t _obj16_type = (uint8_t)obj->type;
            uint32_t _mhash16 = method_hash(method_name);
            PICSlot *_pic16 = pic_slot_for(&frame->chunk->pic, _pic16_off);
            uint16_t _pic16_id = _pic16 ? pic_lookup(_pic16, _obj16_type, _mhash16) : 0;
            if (_pic16_id == PIC_NOT_BUILTIN) goto invoke_local16_not_builtin;

            const char *local_var_name = (frame->chunk->local_names && slot < frame->chunk->local_name_cap)
                                             ? frame->chunk->local_names[slot]
                                             : NULL;
            if (stackvm_invoke_builtin(vm, obj, method_name, arg_count, local_var_name)) {
                if (vm->error) {
                    StackVMResult r = stackvm_handle_native_error(vm, &frame);
                    if (r != STACKVM_OK) return r;
                    break;
                }
                /* Cache builtin hit */
                if (!_pic16) {
                    pic_table_ensure(&frame->chunk->pic);
                    _pic16 = pic_slot_for(&frame->chunk->pic, _pic16_off);
                }
                if (_pic16 && _pic16_id == 0) {
                    uint16_t _rid16 = pic_resolve_builtin_id(_obj16_type, _mhash16);
                    if (_rid16) pic_update(_pic16, _obj16_type, _mhash16, _rid16);
                }
                break;
            }
            /* Cache as NOT_BUILTIN */
            if (!_pic16) {
                pic_table_ensure(&frame->chunk->pic);
                _pic16 = pic_slot_for(&frame->chunk->pic, _pic16_off);
            }
            if (_pic16) pic_update(_pic16, _obj16_type, _mhash16, PIC_NOT_BUILTIN);
        invoke_local16_not_builtin:

            if (obj->type == VAL_MAP) {
                LatValue *field = lat_map_get(obj->as.map.map, method_name);
                if (field && field->type == VAL_CLOSURE && field->as.closure.native_fn &&
                    field->as.closure.default_values != VM_NATIVE_MARKER) {
                    Chunk *fn_chunk = (Chunk *)field->as.closure.native_fn;
                    int arity = (int)field->as.closure.param_count;
                    int adjusted = stackvm_adjust_call_args(vm, fn_chunk, arity, (int)arg_count);
                    if (adjusted < 0) {
                        char *err = vm->error;
                        vm->error = NULL;
                        VM_ERROR("%s", err);
                        free(err);
                        break;
                    }
                    arg_count = (uint8_t)adjusted;
                    ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                    size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;
                    if (vm->frame_count >= STACKVM_FRAMES_MAX) {
                        VM_ERROR("stack overflow (too many nested calls)");
                        break;
                    }
                    stackvm_promote_frame_ephemerals(vm, frame);
                    LatValue closure_copy = value_deep_clone(field);
                    LatValue *arg_base = vm->stack_top - arg_count;
                    push(vm, value_nil());
                    for (int si = arg_count - 1; si >= 0; si--) arg_base[si + 1] = arg_base[si];
                    arg_base[0] = closure_copy;
                    StackCallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->chunk = fn_chunk;
                    new_frame->ip = fn_chunk->code;
                    new_frame->slots = arg_base;
                    new_frame->upvalues = upvals;
                    new_frame->upvalue_count = uv_count;
                    frame = new_frame;
                    break;
                }
                if (field && field->type == VAL_CLOSURE && field->as.closure.default_values == VM_NATIVE_MARKER) {
                    VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                    LatValue *args = (arg_count <= 16) ? vm->fast_args : malloc(arg_count * sizeof(LatValue));
                    for (int i = arg_count - 1; i >= 0; i--) args[i] = pop(vm);
                    LatValue ret = native(args, arg_count);
                    if (vm->rt->error) {
                        vm->error = vm->rt->error;
                        vm->rt->error = NULL;
                    }
                    for (int i = 0; i < arg_count; i++) value_free(&args[i]);
                    if (arg_count > 16) free(args);
                    if (vm->error) {
                        value_free(&ret);
                        char *err = vm->error;
                        vm->error = NULL;
                        VM_ERROR("%s", err);
                        free(err);
                        break;
                    }
                    push(vm, ret);
                    break;
                }
            }

            if (obj->type == VAL_STRUCT) {
                const char *imethod2 = intern(method_name);
                bool handled = false;
                for (size_t fi = 0; fi < obj->as.strct.field_count; fi++) {
                    if (obj->as.strct.field_names[fi] != imethod2) continue;
                    LatValue *field = &obj->as.strct.field_values[fi];
                    if (field->type == VAL_CLOSURE && field->as.closure.native_fn &&
                        field->as.closure.default_values != VM_NATIVE_MARKER) {
                        Chunk *fn_chunk = (Chunk *)field->as.closure.native_fn;
                        ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                        size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;
                        if (vm->frame_count >= STACKVM_FRAMES_MAX) {
                            VM_ERROR("stack overflow (too many nested calls)");
                            break;
                        }
                        stackvm_promote_frame_ephemerals(vm, frame);
                        LatValue self_copy = value_deep_clone(obj);
                        LatValue closure_copy = value_deep_clone(field);
                        LatValue *arg_base = vm->stack_top - arg_count;
                        push(vm, value_nil());
                        push(vm, value_nil());
                        for (int si = arg_count - 1; si >= 0; si--) arg_base[si + 2] = arg_base[si];
                        arg_base[0] = closure_copy;
                        arg_base[1] = self_copy;
                        StackCallFrame *new_frame = &vm->frames[vm->frame_count++];
                        new_frame->chunk = fn_chunk;
                        new_frame->ip = fn_chunk->code;
                        new_frame->slots = arg_base;
                        new_frame->upvalues = upvals;
                        new_frame->upvalue_count = uv_count;
                        frame = new_frame;
                        handled = true;
                        break;
                    }
                    if (field->type == VAL_CLOSURE && field->as.closure.default_values == VM_NATIVE_MARKER) {
                        VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                        LatValue self_copy = value_deep_clone(obj);
                        int total_args = arg_count + 1;
                        LatValue *args = malloc(total_args * sizeof(LatValue));
                        if (!args) return STACKVM_RUNTIME_ERROR;
                        args[0] = self_copy;
                        for (int ai = arg_count - 1; ai >= 0; ai--) args[ai + 1] = pop(vm);
                        LatValue ret = native(args, total_args);
                        for (int ai = 0; ai < total_args; ai++) value_free(&args[ai]);
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
                const char *type_name = (obj->type == VAL_STRUCT) ? obj->as.strct.name
                                        : (obj->type == VAL_ENUM) ? obj->as.enm.enum_name
                                                                  : value_type_name(obj);
                char key[256];
                snprintf(key, sizeof(key), "%s::%s", type_name, method_name);
                LatValue *method_ref = env_get_ref(vm->env, key);
                if (method_ref && method_ref->type == VAL_CLOSURE && method_ref->as.closure.native_fn) {
                    Chunk *fn_chunk = (Chunk *)method_ref->as.closure.native_fn;
                    if (vm->frame_count >= STACKVM_FRAMES_MAX) {
                        VM_ERROR("stack overflow (too many nested calls)");
                        break;
                    }
                    stackvm_promote_frame_ephemerals(vm, frame);
                    LatValue *arg_base = vm->stack_top - arg_count;
                    push(vm, value_nil());
                    for (int i = arg_count; i > 0; i--) vm->stack_top[-1 - (arg_count - i)] = arg_base[i - 1];
                    *arg_base = value_deep_clone(obj);
                    StackCallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->chunk = fn_chunk;
                    new_frame->ip = fn_chunk->code;
                    new_frame->slots = arg_base;
                    new_frame->upvalues = NULL;
                    new_frame->upvalue_count = 0;
                    frame = new_frame;
                } else {
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
        lbl_OP_INVOKE_GLOBAL_16:
#endif
        case OP_INVOKE_GLOBAL_16: {
            size_t _gpic16_off = (size_t)(frame->ip - frame->chunk->code - 1);
            uint16_t name_idx = READ_U16();
            uint16_t method_idx = READ_U16();
            uint8_t arg_count = READ_BYTE();
            const char *global_name = frame->chunk->constants[name_idx].as.str_val;
            const char *method_name = frame->chunk->constants[method_idx].as.str_val;

            /* Fast path: simple builtins (no closures) can mutate in place */
            uint32_t mhash_g = method_hash(method_name);

            /* ── PIC: skip builtin dispatch if cached as not-builtin ── */
            PICSlot *_gpic16 = pic_slot_for(&frame->chunk->pic, _gpic16_off);
            bool _gpic16_skip = false;
            {
                LatValue *_gref16 = env_get_ref(vm->env, global_name);
                if (_gref16 && _gpic16) {
                    uint16_t _gid16 = pic_lookup(_gpic16, (uint8_t)_gref16->type, mhash_g);
                    if (_gid16 == PIC_NOT_BUILTIN) _gpic16_skip = true;
                }
            }

            if (!_gpic16_skip && stackvm_invoke_builtin_is_simple(mhash_g)) {
                LatValue *ref = env_get_ref(vm->env, global_name);
                if (!ref) {
                    const char *sug = env_find_similar_name(vm->env, global_name);
                    if (sug) {
                        VM_ERROR("undefined variable '%s' (did you mean '%s'?)", global_name, sug);
                    } else {
                        VM_ERROR("undefined variable '%s'", global_name);
                    }
                    break;
                }
                if (stackvm_invoke_builtin(vm, ref, method_name, arg_count, global_name)) {
                    if (vm->error) {
                        StackVMResult r = stackvm_handle_native_error(vm, &frame);
                        if (r != STACKVM_OK) return r;
                        break;
                    }
                    /* Update PIC */
                    if (!_gpic16) {
                        pic_table_ensure(&frame->chunk->pic);
                        _gpic16 = pic_slot_for(&frame->chunk->pic, _gpic16_off);
                    }
                    if (_gpic16) {
                        uint16_t _rid = pic_resolve_builtin_id((uint8_t)ref->type, mhash_g);
                        if (_rid) pic_update(_gpic16, (uint8_t)ref->type, mhash_g, _rid);
                    }
                    if (vm->rt->tracking_active) { stackvm_record_history(vm, global_name, ref); }
                    break;
                }
            }

            /* Slow path: closure-invoking builtins or non-builtin dispatch */
            LatValue obj_val;
            if (!env_get(vm->env, global_name, &obj_val)) {
                const char *sug = env_find_similar_name(vm->env, global_name);
                if (sug) {
                    VM_ERROR("undefined variable '%s' (did you mean '%s'?)", global_name, sug);
                } else {
                    VM_ERROR("undefined variable '%s'", global_name);
                }
                break;
            }

            if (!_gpic16_skip && stackvm_invoke_builtin(vm, &obj_val, method_name, arg_count, global_name)) {
                if (vm->error) {
                    value_free(&obj_val);
                    StackVMResult r = stackvm_handle_native_error(vm, &frame);
                    if (r != STACKVM_OK) return r;
                    break;
                }
                /* Update PIC */
                if (!_gpic16) {
                    pic_table_ensure(&frame->chunk->pic);
                    _gpic16 = pic_slot_for(&frame->chunk->pic, _gpic16_off);
                }
                if (_gpic16) {
                    uint16_t _rid = pic_resolve_builtin_id((uint8_t)obj_val.type, mhash_g);
                    if (_rid) pic_update(_gpic16, (uint8_t)obj_val.type, mhash_g, _rid);
                }
                env_set(vm->env, global_name, obj_val);
                if (vm->rt->tracking_active) {
                    LatValue cur;
                    if (env_get(vm->env, global_name, &cur)) {
                        stackvm_record_history(vm, global_name, &cur);
                        value_free(&cur);
                    }
                }
                break;
            }

            /* Not a builtin — cache PIC_NOT_BUILTIN */
            if (!_gpic16) {
                pic_table_ensure(&frame->chunk->pic);
                _gpic16 = pic_slot_for(&frame->chunk->pic, _gpic16_off);
            }
            if (_gpic16) pic_update(_gpic16, (uint8_t)obj_val.type, mhash_g, PIC_NOT_BUILTIN);

            /* insert object below args on stack and
             * dispatch like OP_INVOKE (struct closures, impl methods, etc.) */
            push(vm, value_nil()); /* make room */
            LatValue *base = vm->stack_top - arg_count - 1;
            memmove(base + 1, base, arg_count * sizeof(LatValue));
            *base = obj_val;
            LatValue *obj = base;

            if (obj->type == VAL_MAP) {
                LatValue *field = lat_map_get(obj->as.map.map, method_name);
                if (field && field->type == VAL_CLOSURE && field->as.closure.native_fn &&
                    field->as.closure.default_values != VM_NATIVE_MARKER) {
                    Chunk *fn_chunk = (Chunk *)field->as.closure.native_fn;
                    int arity = (int)field->as.closure.param_count;
                    int adjusted = stackvm_adjust_call_args(vm, fn_chunk, arity, (int)arg_count);
                    if (adjusted < 0) {
                        char *err = vm->error;
                        vm->error = NULL;
                        value_free(&obj_val);
                        VM_ERROR("%s", err);
                        free(err);
                        break;
                    }
                    arg_count = (uint8_t)adjusted;
                    obj = vm->stack_top - arg_count - 1;
                    ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                    size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;
                    if (vm->frame_count >= STACKVM_FRAMES_MAX) {
                        VM_ERROR("stack overflow (too many nested calls)");
                        break;
                    }
                    stackvm_promote_frame_ephemerals(vm, frame);
                    LatValue closure_copy = value_deep_clone(field);
                    value_free(obj);
                    *obj = closure_copy;
                    StackCallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->chunk = fn_chunk;
                    new_frame->ip = fn_chunk->code;
                    new_frame->slots = obj;
                    new_frame->upvalues = upvals;
                    new_frame->upvalue_count = uv_count;
                    frame = new_frame;
                    break;
                }
                if (field && field->type == VAL_CLOSURE && field->as.closure.default_values == VM_NATIVE_MARKER) {
                    VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                    LatValue *args = (arg_count <= 16) ? vm->fast_args : malloc(arg_count * sizeof(LatValue));
                    for (int i = arg_count - 1; i >= 0; i--) args[i] = pop(vm);
                    LatValue obj_popped = pop(vm);
                    LatValue ret = native(args, arg_count);
                    if (vm->rt->error) {
                        vm->error = vm->rt->error;
                        vm->rt->error = NULL;
                    }
                    for (int i = 0; i < arg_count; i++) value_free(&args[i]);
                    if (arg_count > 16) free(args);
                    value_free(&obj_popped);
                    push(vm, ret);
                    break;
                }
            }

            if (obj->type == VAL_STRUCT) {
                const char *imethod3 = intern(method_name);
                bool handled = false;
                for (size_t fi = 0; fi < obj->as.strct.field_count; fi++) {
                    if (obj->as.strct.field_names[fi] != imethod3) continue;
                    LatValue *field = &obj->as.strct.field_values[fi];
                    if (field->type == VAL_CLOSURE && field->as.closure.native_fn &&
                        field->as.closure.default_values != VM_NATIVE_MARKER) {
                        Chunk *fn_chunk = (Chunk *)field->as.closure.native_fn;
                        ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                        size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;
                        if (vm->frame_count >= STACKVM_FRAMES_MAX) {
                            VM_ERROR("stack overflow (too many nested calls)");
                            break;
                        }
                        stackvm_promote_frame_ephemerals(vm, frame);
                        LatValue self_copy = value_deep_clone(obj);
                        LatValue closure_copy = value_deep_clone(field);
                        push(vm, value_nil());
                        for (int si = arg_count; si >= 1; si--) obj[si + 1] = obj[si];
                        obj[1] = self_copy;
                        value_free(obj);
                        *obj = closure_copy;
                        StackCallFrame *new_frame = &vm->frames[vm->frame_count++];
                        new_frame->chunk = fn_chunk;
                        new_frame->ip = fn_chunk->code;
                        new_frame->slots = obj;
                        new_frame->upvalues = upvals;
                        new_frame->upvalue_count = uv_count;
                        frame = new_frame;
                        handled = true;
                        break;
                    }
                    if (field->type == VAL_CLOSURE && field->as.closure.default_values == VM_NATIVE_MARKER) {
                        VMNativeFn native = (VMNativeFn)field->as.closure.native_fn;
                        LatValue self_copy = value_deep_clone(obj);
                        int total_args = arg_count + 1;
                        LatValue *args = malloc(total_args * sizeof(LatValue));
                        if (!args) return STACKVM_RUNTIME_ERROR;
                        args[0] = self_copy;
                        for (int ai = arg_count - 1; ai >= 0; ai--) args[ai + 1] = pop(vm);
                        LatValue obj_popped = pop(vm);
                        LatValue ret = native(args, total_args);
                        for (int ai = 0; ai < total_args; ai++) value_free(&args[ai]);
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
                const char *type_name = (obj->type == VAL_STRUCT) ? obj->as.strct.name
                                        : (obj->type == VAL_ENUM) ? obj->as.enm.enum_name
                                                                  : value_type_name(obj);
                char key[256];
                snprintf(key, sizeof(key), "%s::%s", type_name, method_name);
                LatValue *method_ref = env_get_ref(vm->env, key);
                if (method_ref && method_ref->type == VAL_CLOSURE && method_ref->as.closure.native_fn) {
                    Chunk *fn_chunk = (Chunk *)method_ref->as.closure.native_fn;
                    if (vm->frame_count >= STACKVM_FRAMES_MAX) {
                        VM_ERROR("stack overflow (too many nested calls)");
                        break;
                    }
                    stackvm_promote_frame_ephemerals(vm, frame);
                    LatValue self_copy = value_deep_clone(obj);
                    value_free(obj);
                    *obj = self_copy;
                    StackCallFrame *new_frame = &vm->frames[vm->frame_count++];
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
            stackvm_promote_value(&val);
            LatValue *obj = &frame->slots[slot]; /* Direct pointer to local */

            /* Ref proxy: delegate set-index to inner value */
            if (obj->type == VAL_REF) {
                if (obj->phase == VTAG_CRYSTAL) {
                    value_free(&val);
                    VM_ERROR("cannot assign index on a frozen Ref");
                    break;
                }
                LatValue *inner = &obj->as.ref.ref->value;
                if (inner->type == VAL_ARRAY && idx.type == VAL_INT) {
                    int64_t i = idx.as.int_val;
                    if (i < 0 || (size_t)i >= inner->as.array.len) {
                        value_free(&val);
                        VM_ERROR("array index out of bounds: %lld (len %zu)", (long long)i, inner->as.array.len);
                        break;
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
                value_free(&val);
                value_free(&idx);
                VM_ERROR("invalid index assignment on Ref");
                break;
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
                    value_free(&val);
                    value_free(&idx);
                    VM_ERROR("cannot modify a %s value", obj->phase == VTAG_CRYSTAL ? "frozen" : "sublimated");
                    break;
                }
            }
            /* Check per-key phase on non-frozen maps */
            if (obj->type == VAL_MAP && idx.type == VAL_STR && obj->as.map.key_phases) {
                PhaseTag *kp = lat_map_get(obj->as.map.key_phases, idx.as.str_val);
                if (kp && *kp == VTAG_CRYSTAL) {
                    value_free(&val);
                    value_free(&idx);
                    VM_ERROR("cannot modify frozen key '%s'", idx.as.str_val);
                    break;
                }
            }
            if (obj->type == VAL_ARRAY && idx.type == VAL_INT) {
                int64_t i = idx.as.int_val;
                if (i < 0 || (size_t)i >= obj->as.array.len) {
                    value_free(&val);
                    VM_ERROR("array index out of bounds: %lld (len %zu)", (long long)i, obj->as.array.len);
                    break;
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
                    VM_ERROR("buffer index out of bounds: %lld (len %zu)", (long long)i, obj->as.buffer.len);
                    break;
                }
                obj->as.buffer.data[i] = (uint8_t)(val.as.int_val & 0xFF);
                value_free(&val);
            } else {
                value_free(&val);
                value_free(&idx);
                VM_ERROR("invalid index assignment");
                break;
            }
            break;
        }

        /* ── Exception handling ── */
#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_PUSH_EXCEPTION_HANDLER:
#endif
        case OP_PUSH_EXCEPTION_HANDLER: {
            uint16_t offset = READ_U16();
            if (vm->handler_count >= STACKVM_HANDLER_MAX) {
                VM_ERROR("too many nested exception handlers");
                break;
            }
            StackExceptionHandler *h = &vm->handlers[vm->handler_count++];
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
            if (vm->handler_count > 0) vm->handler_count--;
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_THROW:
#endif
        case OP_THROW: {
            LatValue err = pop(vm);
            if (vm->handler_count > 0) {
                StackExceptionHandler h = vm->handlers[--vm->handler_count];
                /* Unwind stack */
                while (vm->frame_count - 1 > h.frame_index) { vm->frame_count--; }
                frame = &vm->frames[vm->frame_count - 1];
                vm->stack_top = h.stack_top;
                frame->ip = h.ip;
                push(vm, err);
            } else {
                StackVMResult res;
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
            LatValue *val = stackvm_peek(vm, 0);
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
                            return STACKVM_OK;
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
            (void)pop(vm); /* we already freed the peeked value */
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
            if (vm->defer_count < STACKVM_DEFER_MAX) {
                StackDeferEntry *d = &vm->defers[vm->defer_count++];
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
                StackDeferEntry *d = &vm->defers[vm->defer_count - 1];
                if (d->frame_index != current_frame_idx) break;
                if (d->scope_depth < min_depth) break;
                vm->defer_count--;

                /* Save return value (TOS) — we push it back after defer */
                LatValue ret_val = pop(vm);

                /* Create a view chunk over the defer body's bytecode.
                 * The body starts at d->ip and ends with OP_RETURN.
                 * stackvm_run will push a new frame and execute until OP_RETURN.
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
                stackvm_run(vm, &wrapper, &defer_result);
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
                VM_ERROR("cannot freeze a channel");
                break;
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
            stackvm_peek(vm, 0)->phase = VTAG_FLUID;
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
            size_t ri = vm->rt->reaction_count;
            for (size_t i = 0; i < vm->rt->reaction_count; i++) {
                if (strcmp(vm->rt->reactions[i].var_name, var_name) == 0) {
                    ri = i;
                    break;
                }
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
                vm->rt->reactions[ri].callbacks =
                    realloc(vm->rt->reactions[ri].callbacks, vm->rt->reactions[ri].cb_cap * sizeof(LatValue));
            }
            vm->rt->reactions[ri].callbacks[vm->rt->reactions[ri].cb_count++] = value_deep_clone(&callback);
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
            for (size_t i = 0; i < vm->rt->reaction_count; i++) {
                if (strcmp(vm->rt->reactions[i].var_name, var_name) != 0) continue;
                free(vm->rt->reactions[i].var_name);
                for (size_t j = 0; j < vm->rt->reactions[i].cb_count; j++)
                    value_free(&vm->rt->reactions[i].callbacks[j]);
                free(vm->rt->reactions[i].callbacks);
                vm->rt->reactions[i] = vm->rt->reactions[--vm->rt->reaction_count];
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
                value_free(&dep_v);
                value_free(&strategy_v);
                vm->error = strdup("bond() requires variable names for dependencies");
                StackVMResult r = stackvm_handle_native_error(vm, &frame);
                if (r != STACKVM_OK) return r;
                break;
            }
            /* Validate: target must not be already frozen */
            {
                LatValue target_val;
                bool target_found = env_get(vm->env, target_name, &target_val);
                if (!target_found) target_found = stackvm_find_local_value(vm, target_name, &target_val);
                if (target_found) {
                    if (target_val.phase == VTAG_CRYSTAL) {
                        value_free(&target_val);
                        value_free(&dep_v);
                        value_free(&strategy_v);
                        char *msg = NULL;
                        lat_asprintf(&msg, "cannot bond already-frozen variable '%s'", target_name);
                        vm->error = msg;
                        StackVMResult r = stackvm_handle_native_error(vm, &frame);
                        if (r != STACKVM_OK) return r;
                        break;
                    }
                    value_free(&target_val);
                }
            }
            /* Validate: dep variable must exist */
            {
                LatValue dep_val;
                bool dep_found = env_get(vm->env, dep_name, &dep_val);
                if (!dep_found) dep_found = stackvm_find_local_value(vm, dep_name, &dep_val);
                if (!dep_found) {
                    char *msg = NULL;
                    lat_asprintf(&msg, "cannot bond undefined variable '%s'", dep_name);
                    value_free(&dep_v);
                    value_free(&strategy_v);
                    vm->error = msg;
                    StackVMResult r = stackvm_handle_native_error(vm, &frame);
                    if (r != STACKVM_OK) return r;
                    break;
                }
                value_free(&dep_val);
            }
            /* Find or create bond entry */
            size_t bi = vm->rt->bond_count;
            for (size_t i = 0; i < vm->rt->bond_count; i++) {
                if (strcmp(vm->rt->bonds[i].target, target_name) == 0) {
                    bi = i;
                    break;
                }
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
                vm->rt->bonds[bi].deps = realloc(vm->rt->bonds[bi].deps, vm->rt->bonds[bi].dep_cap * sizeof(char *));
                vm->rt->bonds[bi].dep_strategies =
                    realloc(vm->rt->bonds[bi].dep_strategies, vm->rt->bonds[bi].dep_cap * sizeof(char *));
            }
            vm->rt->bonds[bi].deps[vm->rt->bonds[bi].dep_count] = strdup(dep_name);
            vm->rt->bonds[bi].dep_strategies[vm->rt->bonds[bi].dep_count] = strdup(strategy);
            vm->rt->bonds[bi].dep_count++;
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
            for (size_t i = 0; i < vm->rt->bond_count; i++) {
                if (strcmp(vm->rt->bonds[i].target, target_name) != 0) continue;
                for (size_t j = 0; j < vm->rt->bonds[i].dep_count; j++) {
                    if (strcmp(vm->rt->bonds[i].deps[j], dep_name) != 0) continue;
                    free(vm->rt->bonds[i].deps[j]);
                    if (vm->rt->bonds[i].dep_strategies) free(vm->rt->bonds[i].dep_strategies[j]);
                    /* Swap-remove */
                    vm->rt->bonds[i].deps[j] = vm->rt->bonds[i].deps[vm->rt->bonds[i].dep_count - 1];
                    if (vm->rt->bonds[i].dep_strategies)
                        vm->rt->bonds[i].dep_strategies[j] =
                            vm->rt->bonds[i].dep_strategies[vm->rt->bonds[i].dep_count - 1];
                    vm->rt->bonds[i].dep_count--;
                    break;
                }
                /* If empty, remove the bond entry */
                if (vm->rt->bonds[i].dep_count == 0) {
                    free(vm->rt->bonds[i].target);
                    free(vm->rt->bonds[i].deps);
                    free(vm->rt->bonds[i].dep_strategies);
                    vm->rt->bonds[i] = vm->rt->bonds[--vm->rt->bond_count];
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
            if (vm->rt->seed_count >= vm->rt->seed_cap) {
                vm->rt->seed_cap = vm->rt->seed_cap ? vm->rt->seed_cap * 2 : 4;
                vm->rt->seeds = realloc(vm->rt->seeds, vm->rt->seed_cap * sizeof(*vm->rt->seeds));
            }
            vm->rt->seeds[vm->rt->seed_count].var_name = strdup(var_name);
            vm->rt->seeds[vm->rt->seed_count].contract = value_deep_clone(&contract);
            vm->rt->seed_count++;
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
            for (size_t i = 0; i < vm->rt->seed_count; i++) {
                if (strcmp(vm->rt->seeds[i].var_name, var_name) != 0) continue;
                free(vm->rt->seeds[i].var_name);
                value_free(&vm->rt->seeds[i].contract);
                vm->rt->seeds[i] = vm->rt->seeds[--vm->rt->seed_count];
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
                VM_ERROR("cannot freeze a channel");
                break;
            }
            /* Validate seed contracts (don't consume — matches tree-walker freeze behavior) */
            char *seed_err = stackvm_validate_seeds(vm, var_name, &val, false);
            if (seed_err) {
                value_free(&val);
                vm->error = seed_err;
                StackVMResult r = stackvm_handle_native_error(vm, &frame);
                if (r != STACKVM_OK) return r;
                break;
            }
            LatValue frozen = value_freeze(val);
            LatValue ret = value_deep_clone(&frozen);
            stackvm_write_back(vm, frame, loc_type, loc_slot, var_name, frozen);
            value_free(&frozen);
            StackVMResult cr = stackvm_freeze_cascade(vm, &frame, var_name);
            if (cr != STACKVM_OK) {
                value_free(&ret);
                return cr;
            }
            StackVMResult rr = stackvm_fire_reactions(vm, &frame, var_name, "crystal");
            if (rr != STACKVM_OK) {
                value_free(&ret);
                return rr;
            }
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
            stackvm_write_back(vm, frame, loc_type, loc_slot, var_name, thawed);
            value_free(&thawed);
            StackVMResult rr = stackvm_fire_reactions(vm, &frame, var_name, "fluid");
            if (rr != STACKVM_OK) {
                value_free(&ret);
                return rr;
            }
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
            stackvm_write_back(vm, frame, loc_type, loc_slot, var_name, val);
            value_free(&val);
            StackVMResult rr = stackvm_fire_reactions(vm, &frame, var_name, "sublimated");
            if (rr != STACKVM_OK) {
                value_free(&ret);
                return rr;
            }
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
        lbl_OP_IS_FLUID:
#endif
        case OP_IS_FLUID: {
            LatValue val = pop(vm);
            bool is_fluid = (val.phase == VTAG_FLUID);
            value_free(&val);
            push(vm, value_bool(is_fluid));
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
            if (!except_names) return STACKVM_RUNTIME_ERROR;
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
                    if (!val.as.strct.field_phases) {
                        VM_ERROR("out of memory");
                        break;
                    }
                    for (size_t i = 0; i < val.as.strct.field_count; i++) val.as.strct.field_phases[i] = val.phase;
                }
                for (size_t i = 0; i < val.as.strct.field_count; i++) {
                    bool exempted = false;
                    for (uint8_t j = 0; j < except_count; j++) {
                        if (val.as.strct.field_names[i] == intern(except_names[j])) {
                            exempted = true;
                            break;
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
                    if (!val.as.map.key_phases) {
                        VM_ERROR("out of memory");
                        break;
                    }
                    *val.as.map.key_phases = lat_map_new(sizeof(PhaseTag));
                }
                for (size_t i = 0; i < val.as.map.map->cap; i++) {
                    if (val.as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                    const char *key = val.as.map.map->entries[i].key;
                    bool exempted = false;
                    for (uint8_t j = 0; j < except_count; j++) {
                        if (strcmp(key, except_names[j]) == 0) {
                            exempted = true;
                            break;
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
            stackvm_write_back(vm, frame, loc_type, loc_slot, var_name, val);
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
                    if (parent.as.strct.field_names[i] == intern(fname)) {
                        fi = i;
                        break;
                    }
                }
                if (fi == (size_t)-1) {
                    value_free(&parent);
                    value_free(&field_name);
                    VM_ERROR("struct has no field '%s'", fname);
                    break;
                }
                parent.as.strct.field_values[fi] = value_freeze(parent.as.strct.field_values[fi]);
                if (!parent.as.strct.field_phases)
                    parent.as.strct.field_phases = calloc(parent.as.strct.field_count, sizeof(PhaseTag));
                parent.as.strct.field_phases[fi] = VTAG_CRYSTAL;
                LatValue ret = value_deep_clone(&parent.as.strct.field_values[fi]);
                stackvm_write_back(vm, frame, loc_type, loc_slot, parent_name, parent);
                value_free(&parent);
                value_free(&field_name);
                push(vm, ret);
            } else if (parent.type == VAL_MAP && field_name.type == VAL_STR) {
                const char *key = field_name.as.str_val;
                LatValue *val_ptr = (LatValue *)lat_map_get(parent.as.map.map, key);
                if (!val_ptr) {
                    value_free(&parent);
                    value_free(&field_name);
                    VM_ERROR("map has no key '%s'", key);
                    break;
                }
                *val_ptr = value_freeze(*val_ptr);
                if (!parent.as.map.key_phases) {
                    parent.as.map.key_phases = calloc(1, sizeof(LatMap));
                    *parent.as.map.key_phases = lat_map_new(sizeof(PhaseTag));
                }
                PhaseTag crystal = VTAG_CRYSTAL;
                lat_map_set(parent.as.map.key_phases, key, &crystal);
                LatValue ret = value_deep_clone(val_ptr);
                stackvm_write_back(vm, frame, loc_type, loc_slot, parent_name, parent);
                value_free(&parent);
                value_free(&field_name);
                push(vm, ret);
            } else {
                value_free(&parent);
                value_free(&field_name);
                VM_ERROR("freeze field requires a struct or map");
                break;
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
            if (!vals) return STACKVM_RUNTIME_ERROR;
            for (int i = argc - 1; i >= 0; i--) vals[i] = pop(vm);
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

            /* Check for built-in stdlib module */
            LatValue builtin_mod;
            if (rt_try_builtin_import(raw_path, &builtin_mod)) {
                push(vm, builtin_mod);
                break;
            }

            /* Try lat_modules/ resolution for bare module names */
            char *pkg_resolved = pkg_resolve_module(raw_path, vm->rt->script_dir);

            /* Resolve file path: append .lat if not present */
            size_t plen = strlen(raw_path);
            char *file_path;
            if (pkg_resolved) {
                file_path = pkg_resolved; /* already absolute */
            } else if (plen >= 4 && strcmp(raw_path + plen - 4, ".lat") == 0) {
                file_path = strdup(raw_path);
            } else {
                file_path = malloc(plen + 5);
                if (!file_path) return STACKVM_RUNTIME_ERROR;
                memcpy(file_path, raw_path, plen);
                memcpy(file_path + plen, ".lat", 5);
            }

            /* Resolve to absolute path */
            char resolved[PATH_MAX];
            if (pkg_resolved) {
                /* pkg_resolve_module already returned an absolute path */
                strncpy(resolved, file_path, PATH_MAX - 1);
                resolved[PATH_MAX - 1] = '\0';
                free(file_path);
            } else if (!realpath(file_path, resolved)) {
                char errbuf[512];
                snprintf(errbuf, sizeof(errbuf), "import: cannot find '%s'", file_path);
                free(file_path);
                VM_ERROR("%s", errbuf);
                break;
            } else {
                free(file_path);
            }

            /* Check module cache */
            LatValue *cached = lat_map_get(&vm->module_cache, resolved);
            if (cached) {
                push(vm, value_deep_clone(cached));
                break;
            }

            /* Read the file */
            char *source = builtin_read_file(resolved);
            if (!source) {
                VM_ERROR("import: cannot read '%s'", resolved);
                break;
            }

            /* Lex */
            Lexer mod_lex = lexer_new(source);
            char *lex_err = NULL;
            LatVec mod_toks = lexer_tokenize(&mod_lex, &lex_err);
            free(source);
            if (lex_err) {
                char *errmsg = NULL;
                lat_asprintf(&errmsg, "import '%s': %s", resolved, lex_err);
                free(lex_err);
                lat_vec_free(&mod_toks);
                VM_ERROR("%s", errmsg ? errmsg : "import lex error");
                free(errmsg);
                break;
            }

            /* Parse */
            Parser mod_parser = parser_new(&mod_toks);
            char *parse_err = NULL;
            Program mod_prog = parser_parse(&mod_parser, &parse_err);
            if (parse_err) {
                char *errmsg = NULL;
                lat_asprintf(&errmsg, "import '%s': %s", resolved, parse_err);
                free(parse_err);
                program_free(&mod_prog);
                for (size_t ti = 0; ti < mod_toks.len; ti++) token_free(lat_vec_get(&mod_toks, ti));
                lat_vec_free(&mod_toks);
                VM_ERROR("%s", errmsg ? errmsg : "import parse error");
                free(errmsg);
                break;
            }

            /* Compile as module (no auto-call of main) */
            char *comp_err = NULL;
            Chunk *mod_chunk = stack_compile_module(&mod_prog, &comp_err);

            /* Free parse artifacts */
            program_free(&mod_prog);
            for (size_t ti = 0; ti < mod_toks.len; ti++) token_free(lat_vec_get(&mod_toks, ti));
            lat_vec_free(&mod_toks);

            if (!mod_chunk) {
                char *errmsg = NULL;
                lat_asprintf(&errmsg, "import '%s': %s", resolved, comp_err ? comp_err : "compile error");
                free(comp_err);
                VM_ERROR("%s", errmsg ? errmsg : "import compile error");
                free(errmsg);
                break;
            }

            /* Track the chunk for proper lifetime management */
            if (vm->fn_chunk_count >= vm->fn_chunk_cap) {
                vm->fn_chunk_cap = vm->fn_chunk_cap ? vm->fn_chunk_cap * 2 : 8;
                vm->fn_chunks = realloc(vm->fn_chunks, vm->fn_chunk_cap * sizeof(Chunk *));
            }
            vm->fn_chunks[vm->fn_chunk_count++] = mod_chunk;

            /* Push a module scope so module globals are isolated */
            env_push_scope(vm->env);

            /* Run the module chunk */
            LatValue mod_result;
            StackVMResult mod_r = stackvm_run(vm, mod_chunk, &mod_result);
            if (mod_r != STACKVM_OK) {
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

                /* Filter based on export declarations */
                if (!module_should_export(name, (const char **)mod_chunk->export_names, mod_chunk->export_count,
                                          mod_chunk->has_exports))
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
            for (uint8_t i = 0; i < spawn_count; i++) spawn_indices[i] = READ_BYTE();

#ifdef __EMSCRIPTEN__
            (void)sync_idx;
            push(vm, value_unit());
#else
                /* Export current locals so sub-chunks can see them via env */
                env_push_scope(vm->env);
                for (size_t fi2 = 0; fi2 < vm->frame_count; fi2++) {
                    StackCallFrame *f2 = &vm->frames[fi2];
                    if (!f2->chunk) continue;
                    size_t lc = (fi2 + 1 < vm->frame_count) ? (size_t)(vm->frames[fi2 + 1].slots - f2->slots)
                                                            : (size_t)(vm->stack_top - f2->slots);
                    for (size_t sl = 0; sl < lc; sl++) {
                        if (sl < f2->chunk->local_name_cap && f2->chunk->local_names[sl])
                            env_define(vm->env, f2->chunk->local_names[sl], value_deep_clone(&f2->slots[sl]));
                    }
                }

                if (spawn_count == 0) {
                    /* No spawns — run sync body */
                    if (sync_idx != 0xFF) {
                        Chunk *body = (Chunk *)frame->chunk->constants[sync_idx].as.closure.native_fn;
                        LatValue scope_result;
                        StackVMResult sr = stackvm_run(vm, body, &scope_result);
                        env_pop_scope(vm->env);
                        if (sr != STACKVM_OK) { return runtime_error(vm, "%s", vm->error ? vm->error : "scope error"); }
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
                        StackVMResult nsr = stackvm_run(vm, sync_body, &ns_result);
                        if (nsr != STACKVM_OK) {
                            first_error = vm->error ? strdup(vm->error) : strdup("scope stmt error");
                            free(vm->error);
                            vm->error = NULL;
                        } else {
                            value_free(&ns_result);
                        }
                    }

                    /* Create child VMs for each spawn */
                    VMSpawnTask *tasks = calloc(spawn_count, sizeof(VMSpawnTask));
                    if (!tasks) return STACKVM_RUNTIME_ERROR;
                    for (uint8_t i = 0; i < spawn_count && !first_error; i++) {
                        Chunk *sp_chunk = (Chunk *)frame->chunk->constants[spawn_indices[i]].as.closure.native_fn;
                        tasks[i].chunk = sp_chunk;
                        tasks[i].child_vm = stackvm_clone_for_thread(vm);
                        stackvm_export_locals_to_env(vm, tasks[i].child_vm);
                        tasks[i].error = NULL;
                    }

                    /* Launch all spawn threads */
                    for (uint8_t i = 0; i < spawn_count; i++) {
                        if (!tasks[i].child_vm) continue;
                        pthread_create(&tasks[i].thread, NULL, stackvm_spawn_thread_fn, &tasks[i]);
                    }

                    /* Join all threads */
                    for (uint8_t i = 0; i < spawn_count; i++) {
                        if (!tasks[i].child_vm) continue;
                        pthread_join(tasks[i].thread, NULL);
                    }

                    /* Restore parent TLS state */
                    lat_runtime_set_current(vm->rt);
                    vm->rt->active_vm = vm;

                    /* Collect first error from child threads */
                    for (uint8_t i = 0; i < spawn_count; i++) {
                        if (tasks[i].error && !first_error) {
                            first_error = tasks[i].error;
                        } else if (tasks[i].error) {
                            free(tasks[i].error);
                        }
                        if (tasks[i].child_vm) stackvm_free_child(tasks[i].child_vm);
                    }

                    env_pop_scope(vm->env);
                    free(tasks);

                    if (first_error) {
                        StackVMResult err = runtime_error(vm, "%s", first_error);
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
            typedef struct {
                uint8_t flags, chan_idx, body_idx, binding_idx;
            } SelArmInfo;
            SelArmInfo *arm_info = malloc(arm_count * sizeof(SelArmInfo));
            if (!arm_info) return STACKVM_RUNTIME_ERROR;
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
                    StackCallFrame *f2 = &vm->frames[fi2];
                    if (!f2->chunk) continue;
                    size_t lc = (fi2 + 1 < vm->frame_count) ? (size_t)(vm->frames[fi2 + 1].slots - f2->slots)
                                                            : (size_t)(vm->stack_top - f2->slots);
                    for (size_t sl = 0; sl < lc; sl++) {
                        if (sl < f2->chunk->local_name_cap && f2->chunk->local_names[sl])
                            env_define(vm->env, f2->chunk->local_names[sl], value_deep_clone(&f2->slots[sl]));
                    }
                }

                /* Evaluate all channel expressions upfront */
                LatChannel **channels = calloc(arm_count, sizeof(LatChannel *));
                if (!channels) return STACKVM_RUNTIME_ERROR;
                for (uint8_t i = 0; i < arm_count; i++) {
                    if (arm_info[i].flags & 0x03) continue; /* skip default/timeout */
                    Chunk *ch_chunk = (Chunk *)frame->chunk->constants[arm_info[i].chan_idx].as.closure.native_fn;
                    LatValue ch_val;
                    StackVMResult cr = stackvm_run(vm, ch_chunk, &ch_val);
                    if (cr != STACKVM_OK) {
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
                    Chunk *to_chunk =
                        (Chunk *)frame->chunk->constants[arm_info[timeout_arm].chan_idx].as.closure.native_fn;
                    LatValue to_val;
                    StackVMResult tr = stackvm_run(vm, to_chunk, &to_val);
                    if (tr != STACKVM_OK) {
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
                if (!indices) return STACKVM_RUNTIME_ERROR;
                for (uint8_t i = 0; i < arm_count; i++) {
                    if (!(arm_info[i].flags & 0x03)) indices[ch_arm_count++] = i;
                }
                /* Fisher-Yates shuffle */
                for (size_t i = ch_arm_count; i > 1; i--) {
                    size_t j = (size_t)rand() % i;
                    size_t tmp = indices[i - 1];
                    indices[i - 1] = indices[j];
                    indices[j] = tmp;
                }

                /* Set up waiter for blocking */
                pthread_mutex_t sel_mutex = PTHREAD_MUTEX_INITIALIZER;
                pthread_cond_t sel_cond = PTHREAD_COND_INITIALIZER;
                LatSelectWaiter waiter = {
                    .mutex = &sel_mutex,
                    .cond = &sel_cond,
                    .next = NULL,
                };

                LatValue select_result = value_unit();
                bool select_found = false;
                bool select_error = false;

                /* Compute deadline for timeout */
                struct timespec deadline;
                if (timeout_ms >= 0) {
                    clock_gettime(CLOCK_REALTIME, &deadline);
                    deadline.tv_sec += timeout_ms / 1000;
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
                            if (binding) env_define(vm->env, binding, recv_val);
                            else value_free(&recv_val);

                            Chunk *arm_chunk =
                                (Chunk *)frame->chunk->constants[arm_info[i].body_idx].as.closure.native_fn;
                            LatValue arm_result;
                            StackVMResult ar = stackvm_run(vm, arm_chunk, &arm_result);
                            env_pop_scope(vm->env);
                            if (ar != STACKVM_OK) {
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
                            Chunk *def_chunk =
                                (Chunk *)frame->chunk->constants[arm_info[default_arm].body_idx].as.closure.native_fn;
                            LatValue def_result;
                            StackVMResult dr = stackvm_run(vm, def_chunk, &def_result);
                            if (dr == STACKVM_OK) {
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
                        Chunk *def_chunk =
                            (Chunk *)frame->chunk->constants[arm_info[default_arm].body_idx].as.closure.native_fn;
                        LatValue def_result;
                        StackVMResult dr = stackvm_run(vm, def_chunk, &def_result);
                        if (dr == STACKVM_OK) {
                            value_free(&select_result);
                            select_result = def_result;
                        } else {
                            select_error = true;
                        }
                        env_pop_scope(vm->env);
                        break;
                    }

                    /* Block: register waiter on all channels, then wait */
                    for (size_t k = 0; k < ch_arm_count; k++) channel_add_waiter(channels[indices[k]], &waiter);

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
                                Chunk *to_body = (Chunk *)frame->chunk->constants[arm_info[timeout_arm].body_idx]
                                                     .as.closure.native_fn;
                                LatValue to_result;
                                StackVMResult tor = stackvm_run(vm, to_body, &to_result);
                                if (tor == STACKVM_OK) {
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
                    for (size_t k = 0; k < ch_arm_count; k++) channel_remove_waiter(channels[indices[k]], &waiter);
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
                    StackVMResult err = runtime_error(vm, "%s", err_msg);
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
                for (LatValue *slot = vm->stack; slot < vm->stack_top; slot++) { stackvm_promote_value(slot); }
                vm->ephemeral_on_stack = false;
            }
            bump_arena_reset(vm->ephemeral);
            /* GC safe point: collect at statement boundaries */
            gc_maybe_collect(&vm->gc, vm);
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
            if (!stackvm_type_matches(val, type_name)) {
                const char *err_fmt = frame->chunk->constants[err_idx].as.str_val;
                const char *actual = stackvm_value_type_display(val);
                /* Only suggest if the type name is NOT a known built-in */
                if (!lat_is_known_type(type_name)) {
                    const char *tsug = lat_find_similar_type(type_name, NULL, NULL);
                    if (tsug) {
                        char *base = NULL;
                        lat_asprintf(&base, err_fmt, actual);
                        VM_ERROR("%s (did you mean '%s'?)", base, tsug);
                        free(base);
                    }
                }
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
            LatValue *val = stackvm_peek(vm, 0);
            const char *type_name = frame->chunk->constants[type_idx].as.str_val;
            if (!stackvm_type_matches(val, type_name)) {
                const char *err_fmt = frame->chunk->constants[err_idx].as.str_val;
                const char *actual = stackvm_value_type_display(val);
                if (!lat_is_known_type(type_name)) {
                    const char *tsug = lat_find_similar_type(type_name, NULL, NULL);
                    if (tsug) {
                        char *base = NULL;
                        lat_asprintf(&base, err_fmt, actual);
                        VM_ERROR("%s (did you mean '%s'?)", base, tsug);
                        free(base);
                    }
                }
                VM_ERROR(err_fmt, actual);
            }
            break;
        }

        /* ── String append fast path ── */
#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_APPEND_STR_LOCAL:
#endif
        case OP_APPEND_STR_LOCAL: {
            /* Append TOS (string) to local[slot] in-place via realloc.
             * Avoids the GET_LOCAL clone + ADD copy + SET_LOCAL_POP cycle
             * that would otherwise make repeated s += "x" O(n^2).
             * Uses cached str_len to avoid O(n) strlen on the accumulator. */
            uint8_t slot = READ_BYTE();
            LatValue rhs = pop(vm);
            LatValue *local = &frame->slots[slot];
            if (local->type == VAL_STR && rhs.type == VAL_STR) {
                const char *rp = rhs.as.str_val;
                size_t rl = rhs.as.str_len ? rhs.as.str_len : strlen(rp);
                if (rl == 0) {
                    /* Appending empty string is a no-op */
                    value_free(&rhs);
                    break;
                }
                /* Use cached length if available, otherwise compute and cache */
                size_t ll = local->as.str_len ? local->as.str_len : strlen(local->as.str_val);
                if (local->region_id == REGION_NONE) {
                    /* Direct realloc — no clone, no full copy */
                    char *buf = realloc(local->as.str_val, ll + rl + 1);
                    memcpy(buf + ll, rp, rl);
                    buf[ll + rl] = '\0';
                    local->as.str_val = buf;
                } else if (local->region_id == REGION_INTERNED || local->region_id == REGION_CONST) {
                    /* Can't mutate interned/const — make a fresh copy */
                    char *buf = malloc(ll + rl + 1);
                    if (!buf) return STACKVM_RUNTIME_ERROR;
                    memcpy(buf, local->as.str_val, ll);
                    memcpy(buf + ll, rp, rl);
                    buf[ll + rl] = '\0';
                    /* Don't free interned/const strings */
                    local->as.str_val = buf;
                    local->region_id = REGION_NONE;
                } else {
                    /* Ephemeral or region-owned — clone to malloc, then append */
                    char *buf = malloc(ll + rl + 1);
                    if (!buf) return STACKVM_RUNTIME_ERROR;
                    memcpy(buf, local->as.str_val, ll);
                    memcpy(buf + ll, rp, rl);
                    buf[ll + rl] = '\0';
                    local->as.str_val = buf;
                    local->region_id = REGION_NONE;
                }
                /* Update cached length */
                local->as.str_len = ll + rl;
                value_free(&rhs);
                /* Record history for tracked variables */
                if (vm->rt->tracking_active && frame->chunk->local_names && slot < frame->chunk->local_name_cap &&
                    frame->chunk->local_names[slot]) {
                    stackvm_record_history(vm, frame->chunk->local_names[slot], local);
                }
            } else {
                /* Fallback for non-string types: inline ADD + store back.
                 * Handles int += int, float += float, and mixed-type string concat. */
                LatValue a2 = value_clone_fast(local);
                LatValue result;
                if (a2.type == VAL_INT && rhs.type == VAL_INT) {
                    result = value_int(a2.as.int_val + rhs.as.int_val);
                } else if (a2.type == VAL_FLOAT && rhs.type == VAL_FLOAT) {
                    result = value_float(a2.as.float_val + rhs.as.float_val);
                } else if (a2.type == VAL_INT && rhs.type == VAL_FLOAT) {
                    result = value_float((double)a2.as.int_val + rhs.as.float_val);
                } else if (a2.type == VAL_FLOAT && rhs.type == VAL_INT) {
                    result = value_float(a2.as.float_val + (double)rhs.as.int_val);
                } else if (a2.type == VAL_STR || rhs.type == VAL_STR) {
                    const char *pa2 = (a2.type == VAL_STR) ? a2.as.str_val : NULL;
                    const char *pb2 = (rhs.type == VAL_STR) ? rhs.as.str_val : NULL;
                    char *ra2 = pa2 ? NULL : value_repr(&a2);
                    char *rb2 = pb2 ? NULL : value_repr(&rhs);
                    if (!pa2) pa2 = ra2;
                    if (!pb2) pb2 = rb2;
                    size_t la2 = strlen(pa2), lb2 = strlen(pb2);
                    char *buf = malloc(la2 + lb2 + 1);
                    if (!buf) return STACKVM_RUNTIME_ERROR;
                    memcpy(buf, pa2, la2);
                    memcpy(buf + la2, pb2, lb2);
                    buf[la2 + lb2] = '\0';
                    result = value_string_owned(buf);
                    free(ra2);
                    free(rb2);
                } else {
                    value_free(&a2);
                    value_free(&rhs);
                    VM_ERROR("operands must be numbers for '+'");
                    break;
                }
                value_free(&a2);
                value_free(&rhs);
                value_free(local);
                *local = stackvm_try_intern(result);
                /* Record history for tracked variables */
                if (vm->rt->tracking_active && frame->chunk->local_names && slot < frame->chunk->local_name_cap &&
                    frame->chunk->local_names[slot]) {
                    stackvm_record_history(vm, frame->chunk->local_names[slot], local);
                }
            }
            break;
        }

#ifdef VM_USE_COMPUTED_GOTO
        lbl_OP_HALT:
#endif
        case OP_HALT: *result = value_unit(); return STACKVM_OK;

            default: VM_ERROR("unknown opcode %d", op); break;
        }
    }
}

#undef READ_BYTE
#undef READ_U16
