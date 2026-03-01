#include "lattice.h"
#include "regvm.h"
#include "regopcode.h"
#include "runtime.h"
#include "value.h"
#include "env.h"
#include "stackvm.h" /* For ObjUpvalue */
#include "channel.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "builtins.h"
#include "ext.h"
#include "memory.h"
#include "string_ops.h"
#include "builtin_methods.h"
#include "iterator.h"
#include "intern.h"
#include "package.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#ifdef _WIN32
#include "win32_compat.h"
#endif
#ifndef __EMSCRIPTEN__
#include <pthread.h>
#endif

/* Native function marker — same sentinel as stack VM (defined in vm.c) */
#define VM_NATIVE_MARKER ((struct Expr **)(uintptr_t)0x1)
/* Extension function marker — same sentinel as stack VM */
#define VM_EXT_MARKER ((struct Expr **)(uintptr_t)0x2)

/* String interning threshold: strings <= this length are interned after
 * concatenation or when loaded from the constant pool. */
#define INTERN_THRESHOLD 64

/* ── RegChunk implementation ── */

RegChunk *regchunk_new(void) {
    RegChunk *c = calloc(1, sizeof(RegChunk));
    if (!c) return NULL;
    c->magic = REGCHUNK_MAGIC;
    c->code_cap = 128;
    c->code = malloc(c->code_cap * sizeof(RegInstr));
    if (!c->code) {
        free(c);
        return NULL;
    }
    c->const_cap = 32;
    c->constants = malloc(c->const_cap * sizeof(LatValue));
    if (!c->constants) {
        free(c->code);
        free(c);
        return NULL;
    }
    c->lines_cap = 128;
    c->lines = malloc(c->lines_cap * sizeof(int));
    if (!c->lines) {
        free(c->constants);
        free(c->code);
        free(c);
        return NULL;
    }
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
        if (v->type == VAL_CLOSURE && v->as.closure.body == NULL && v->as.closure.native_fn != NULL &&
            v->as.closure.default_values != VM_NATIVE_MARKER && v->as.closure.default_values != VM_EXT_MARKER) {
            /* Free prototype-owned param_names (runtime closures borrow, not own) */
            if (v->as.closure.param_names) {
                for (size_t pi = 0; pi < v->as.closure.param_count; pi++) free(v->as.closure.param_names[pi]);
                free(v->as.closure.param_names);
                v->as.closure.param_names = NULL;
            }
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
        for (size_t i = 0; i < c->local_name_cap; i++) free(c->local_names[i]);
        free(c->local_names);
    }
    free(c->name);
    free(c->param_phases);
    if (c->export_names) {
        for (size_t i = 0; i < c->export_count; i++) free(c->export_names[i]);
        free(c->export_names);
    }
    pic_table_free(&c->pic);
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
    /* Deduplicate string constants */
    if (val.type == VAL_STR && val.as.str_val) {
        for (size_t i = 0; i < c->const_len; i++) {
            if (c->constants[i].type == VAL_STR && c->constants[i].as.str_val &&
                strcmp(c->constants[i].as.str_val, val.as.str_val) == 0) {
                free(val.as.str_val);
                return i;
            }
        }
    }
    /* Deduplicate integer constants */
    if (val.type == VAL_INT) {
        for (size_t i = 0; i < c->const_len; i++) {
            if (c->constants[i].type == VAL_INT && c->constants[i].as.int_val == val.as.int_val) { return i; }
        }
    }
    /* Deduplicate float constants */
    if (val.type == VAL_FLOAT) {
        for (size_t i = 0; i < c->const_len; i++) {
            if (c->constants[i].type == VAL_FLOAT && c->constants[i].as.float_val == val.as.float_val) { return i; }
        }
    }
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
        for (size_t i = old_cap; i < c->local_name_cap; i++) c->local_names[i] = NULL;
    }
    free(c->local_names[reg]);
    c->local_names[reg] = name ? strdup(name) : NULL;
}

/* ── Register VM ── */

void regvm_init(RegVM *vm, LatRuntime *rt) {
    memset(vm, 0, sizeof(RegVM));
    vm->fn_chunk_cap = 16;
    vm->fn_chunks = malloc(vm->fn_chunk_cap * sizeof(RegChunk *));
    if (!vm->fn_chunks) return;
    vm->module_cache = NULL;
    vm->ephemeral = bump_arena_new();
    vm->rt = rt;
    vm->env = rt->env;
    vm->struct_meta = rt->struct_meta;
    /* Initialize register stack to nil */
    for (size_t i = 0; i < REGVM_REG_MAX * REGVM_FRAMES_MAX; i++) { vm->reg_stack[i] = value_nil(); }
}

void regvm_free(RegVM *vm) {
    /* Clear thread-local runtime pointer if it still refers to this VM's runtime,
     * preventing dangling pointer after the caller's stack-allocated LatRuntime dies. */
    if (lat_runtime_current() == vm->rt) lat_runtime_set_current(NULL);

    /* Don't free env/struct_meta — runtime owns them */
    for (size_t i = 0; i < vm->fn_chunk_count; i++) regchunk_free(vm->fn_chunks[i]);
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
    for (size_t i = 0; i < vm->reg_stack_top; i++) value_free_inline(&vm->reg_stack[i]);
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

/* ── Threaded spawn support ── */

#ifndef __EMSCRIPTEN__

typedef struct {
    RegChunk *chunk; /* compiled spawn body (parent owns) */
    RegVM *child_vm; /* independent RegVM for thread */
    char *error;     /* NULL on success */
    pthread_t thread;
} RegVMSpawnTask;

/* Create an independent RegVM clone for running a spawn in its own thread. */
RegVM *regvm_clone_for_thread(RegVM *parent) {
    /* Create a child runtime with cloned env + fresh caches */
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

    RegVM *child = calloc(1, sizeof(RegVM));
    if (!child) return NULL;
    child->rt = child_rt;
    child->env = child_rt->env;
    child->struct_meta = child_rt->struct_meta;
    child->error = NULL;
    child->open_upvalues = NULL;
    child->handler_count = 0;
    child->defer_count = 0;
    child->fn_chunks = NULL;
    child->fn_chunk_count = 0;
    child->fn_chunk_cap = 0;
    child->module_cache = NULL;
    child->ephemeral = bump_arena_new();
    child->frame_count = 0;
    child->reg_stack_top = 0;
    /* Initialize register stack to nil */
    for (size_t i = 0; i < REGVM_REG_MAX * REGVM_FRAMES_MAX; i++) child->reg_stack[i] = value_nil();
    return child;
}

/* Free a child RegVM created by regvm_clone_for_thread. */
void regvm_free_child(RegVM *child) {
    /* Free register values */
    for (size_t i = 0; i < child->reg_stack_top; i++) value_free_inline(&child->reg_stack[i]);
    /* Free open upvalues */
    ObjUpvalue *uv = child->open_upvalues;
    while (uv) {
        ObjUpvalue *next = uv->next;
        value_free(&uv->closed);
        free(uv);
        uv = next;
    }
    free(child->error);
    /* Free child-owned fn_chunks */
    for (size_t i = 0; i < child->fn_chunk_count; i++) regchunk_free(child->fn_chunks[i]);
    free(child->fn_chunks);
    /* Free per-VM module cache */
    if (child->module_cache) {
        for (size_t i = 0; i < child->module_cache->cap; i++) {
            if (child->module_cache->entries[i].state == MAP_OCCUPIED) {
                LatValue *v = (LatValue *)child->module_cache->entries[i].value;
                value_free(v);
            }
        }
        lat_map_free(child->module_cache);
        free(child->module_cache);
    }
    if (child->ephemeral) { bump_arena_free(child->ephemeral); }
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
 * so re-compiled sub-chunks can access them via OP_GET_GLOBAL. */
static void regvm_export_locals_to_env(RegVM *parent, RegVM *child) {
    for (int fi = 0; fi < parent->frame_count; fi++) {
        RegCallFrame *f = &parent->frames[fi];
        if (!f->chunk) continue;
        for (size_t sl = 0; sl < f->chunk->local_name_cap; sl++) {
            if (f->chunk->local_names[sl])
                env_define(child->env, f->chunk->local_names[sl], value_deep_clone(&f->regs[sl]));
        }
    }
}

/* Thread function: runs a RegVM sub-chunk in its own thread. */
static void *regvm_spawn_thread_fn(void *arg) {
    RegVMSpawnTask *task = arg;
    lat_runtime_set_current(task->child_vm->rt);
    task->child_vm->rt->active_vm = task->child_vm;

    /* Set up thread-local heap */
    DualHeap *heap = dual_heap_new();
    value_set_heap(heap);
    value_set_arena(NULL);

    LatValue result;
    RegVMResult r = regvm_run(task->child_vm, task->chunk, &result);
    if (r != REGVM_OK) {
        task->error = task->child_vm->error;
        task->child_vm->error = NULL;
    } else {
        value_free(&result);
    }

    dual_heap_free(heap);
    return NULL;
}

#endif /* __EMSCRIPTEN__ */

/* ── Value cloning (same fast-path as stack VM) ── */

/* Primitive type check: types that have no heap data and can be bitwise-copied.
 * VAL_INT=0, VAL_FLOAT=1, VAL_BOOL=2, so type<=VAL_BOOL covers all three.
 * VAL_UNIT, VAL_NIL, VAL_RANGE are also primitive (no heap pointers).
 * Safe because value_free_inline already short-circuits for these types,
 * so region_id is irrelevant — no memory is freed. */
#define RVM_IS_PRIMITIVE(v) \
    ((v).type <= VAL_BOOL || (v).type == VAL_UNIT || (v).type == VAL_NIL || (v).type == VAL_RANGE)

/* Borrowed-string check: REGION_CONST and REGION_INTERNED strings can be
 * bitwise-copied between registers without strdup.  value_free skips
 * non-REGION_NONE values, so the register doesn't own the pointer.
 * Clone-on-escape (globals, arrays, upvalues) still uses rvm_clone(). */
#define RVM_IS_BORROWED_STR(v) \
    ((v).type == VAL_STR && ((v).region_id == REGION_CONST || (v).region_id == REGION_INTERNED))

/* Forward declaration for rvm_clone (used by rvm_clone_or_borrow) */
static inline LatValue rvm_clone(const LatValue *src);

/* Fast inline clone that avoids the full rvm_clone switch for common cases.
 * Handles primitives (bitwise copy) and borrowed strings (bitwise copy).
 * Falls through to rvm_clone() only for heap-owning types. */
static inline LatValue rvm_clone_or_borrow(const LatValue *src) {
    if (RVM_IS_PRIMITIVE(*src)) return *src;
    if (RVM_IS_BORROWED_STR(*src)) return *src;
    return rvm_clone(src);
}

static inline LatValue rvm_clone(const LatValue *src) {
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
            LatValue v = *src;
            if (src->region_id == REGION_INTERNED) return v; /* interned strings are never freed — no need to strdup */
            /* Use cached length when available to avoid strlen */
            size_t slen = src->as.str_len ? src->as.str_len : strlen(src->as.str_val);
            /* Try interning short strings on escape (e.g. ephemeral → global).
             * Avoids strdup and enables pointer-equality comparisons. */
            if (slen <= INTERN_THRESHOLD) return value_string_interned(src->as.str_val);
            v.as.str_val = strdup(src->as.str_val);
            v.as.str_len = slen; /* preserve cached length */
            v.region_id = REGION_NONE;
            return v;
        }
        case VAL_CLOSURE: {
            if (src->as.closure.body == NULL && src->as.closure.native_fn != NULL &&
                src->as.closure.default_values != VM_NATIVE_MARKER && src->as.closure.default_values != VM_EXT_MARKER) {
                LatValue v = *src;
                /* Bytecode closures don't own param_names — the prototype
                 * (in the RegChunk constant pool) owns them.  Setting NULL
                 * here prevents a heap-use-after-free where two register
                 * clones could end up sharing the same param_names pointer. */
                v.as.closure.param_names = NULL;
                return v;
            }
            return value_deep_clone(src);
        }
        case VAL_ARRAY: {
            LatValue v = *src;
            size_t len = src->as.array.len;
            size_t cap = src->as.array.cap > 0 ? src->as.array.cap : (len > 0 ? len : 1);
            v.as.array.elems = malloc(cap * sizeof(LatValue));
            if (!v.as.array.elems) return value_unit();
            v.as.array.cap = cap;
            for (size_t i = 0; i < len; i++) v.as.array.elems[i] = rvm_clone(&src->as.array.elems[i]);
            v.region_id = REGION_NONE;
            return v;
        }
        default: return value_deep_clone(src);
    }
}

/* ── Runtime error ── */

/* Basic error (no exception handler check, for use outside dispatch loop) */
static RegVMResult rvm_error(RegVM *vm, const char *fmt, ...) {
    char *msg = NULL;
    va_list args;
    va_start(args, fmt);
    lat_vasprintf(&msg, fmt, args);
    va_end(args);

    vm->error = msg;
    return REGVM_RUNTIME_ERROR;
}

/* Forward declaration needed by rvm_handle_error */
static inline void reg_set(LatValue *r, LatValue val);

/* Build a structured error Map from the current RegVM state.
 * Must be called BEFORE unwinding frames so the stack trace is accurate. */
static LatValue regvm_build_error_map(RegVM *vm, const char *message) {
    LatValue err_map = value_map_new();

    /* message */
    LatValue msg_val = value_string(message);
    lat_map_set(err_map.as.map.map, "message", &msg_val);

    /* line — from the topmost frame */
    int line = 0;
    if (vm->frame_count > 0) {
        RegCallFrame *f = &vm->frames[vm->frame_count - 1];
        if (f->chunk && f->chunk->lines) {
            size_t offset = (size_t)(f->ip - f->chunk->code);
            if (offset > 0) offset--;
            if (offset < f->chunk->lines_len) line = f->chunk->lines[offset];
        }
    }
    LatValue line_val = value_int(line);
    lat_map_set(err_map.as.map.map, "line", &line_val);

    /* stack — array of strings */
    LatValue *stack_elems = NULL;
    size_t stack_len = 0;
    if (vm->frame_count > 0) {
        stack_elems = malloc((size_t)vm->frame_count * sizeof(LatValue));
        for (int i = vm->frame_count; i > 0; i--) {
            RegCallFrame *f = &vm->frames[i - 1];
            if (!f->chunk) continue;
            size_t offset = (size_t)(f->ip - f->chunk->code);
            if (offset > 0) offset--;
            int fline = 0;
            if (f->chunk->lines && offset < f->chunk->lines_len) fline = f->chunk->lines[offset];
            const char *name = f->chunk->name;
            char buf[256];
            if (name && name[0]) snprintf(buf, sizeof(buf), "%s() at line %d", name, fline);
            else if (i == 1) snprintf(buf, sizeof(buf), "<script> at line %d", fline);
            else snprintf(buf, sizeof(buf), "<closure> at line %d", fline);
            stack_elems[stack_len++] = value_string(buf);
        }
    }
    LatValue stack_arr = value_array(stack_elems, stack_len);
    lat_map_set(err_map.as.map.map, "stack", &stack_arr);
    free(stack_elems);

    return err_map;
}

/* Error handler that routes through exception handlers when available.
 * Used inside the dispatch loop via RVM_ERROR macro.
 * Returns REGVM_OK if handled (execution continues), error otherwise. */
static RegVMResult rvm_handle_error(RegVM *vm, RegCallFrame **frame_ptr, LatValue **R_ptr, const char *fmt, ...) {
    char *inner = NULL;
    va_list args;
    va_start(args, fmt);
    lat_vasprintf(&inner, fmt, args);
    va_end(args);

    /* If there's an active handler, build structured error map before unwinding */
    if (vm->handler_count > 0) {
        LatValue err_map = regvm_build_error_map(vm, inner);
        free(inner);
        RegHandler h = vm->handlers[--vm->handler_count];

        /* Unwind frames */
        while (vm->frame_count - 1 > (int)h.frame_index) {
            RegCallFrame *uf = &vm->frames[vm->frame_count - 1];
            for (int i = 0; i < REGVM_REG_MAX; i++) value_free_inline(&uf->regs[i]);
            vm->frame_count--;
            vm->reg_stack_top -= REGVM_REG_MAX;
        }

        *frame_ptr = &vm->frames[vm->frame_count - 1];
        *R_ptr = (*frame_ptr)->regs;
        (*frame_ptr)->ip = h.ip;
        reg_set(&(*R_ptr)[h.error_reg], err_map);
        return REGVM_OK;
    }

    /* Uncaught — store raw error (line info provided separately via stack trace) */
    vm->error = inner;
    return REGVM_RUNTIME_ERROR;
}

/* ── Set a register (save old, assign new, free old) ──
 * This order prevents use-after-free when the new value aliases the old
 * register's memory (e.g., via shallow clone or shared struct fields). */

static inline void reg_set(LatValue *r, LatValue val) {
    LatValue old = *r;
    *r = val;
    value_free_inline(&old);
}

/* Forward declarations for recursive closure calls */
static RegVMResult regvm_dispatch(RegVM *vm, int base_frame, LatValue *result);
static LatValue regvm_call_closure(RegVM *vm, LatValue *closure, LatValue *args, int argc);

/* BuiltinCallback adapter for regvm: closure is a LatValue*, ctx is a RegVM* */
static LatValue regvm_builtin_callback(void *closure, LatValue *args, int arg_count, void *ctx) {
    return regvm_call_closure((RegVM *)ctx, (LatValue *)closure, args, arg_count);
}

/* Iterator callback adapter: ctx is RegVM*, closure is LatValue* */
static LatValue regvm_iter_callback(void *ctx, LatValue *closure, LatValue *args, int argc) {
    return regvm_call_closure((RegVM *)ctx, closure, args, argc);
}

/* Run a sub-chunk within the current VM (pushes a new frame, doesn't reset state) */
static RegVMResult regvm_run_sub(RegVM *vm, RegChunk *chunk, LatValue *result) {
    if (vm->frame_count >= REGVM_FRAMES_MAX) return rvm_error(vm, "call stack overflow");
    size_t new_base = vm->reg_stack_top;
    if (new_base + REGVM_REG_MAX > REGVM_REG_MAX * REGVM_FRAMES_MAX) return rvm_error(vm, "register stack overflow");
    LatValue *new_regs = &vm->reg_stack[new_base];
    vm->reg_stack_top += REGVM_REG_MAX;
    int mr = chunk->max_reg ? chunk->max_reg : REGVM_REG_MAX;
    for (int i = 0; i < mr; i++) new_regs[i] = value_nil();

    int saved_base = vm->frame_count;
    RegCallFrame *new_frame = &vm->frames[vm->frame_count++];
    new_frame->chunk = chunk;
    new_frame->ip = chunk->code;
    new_frame->regs = new_regs;
    new_frame->reg_count = mr;
    new_frame->upvalues = NULL;
    new_frame->upvalue_count = 0;
    new_frame->caller_result_reg = 0;

    RegVMResult res = regvm_dispatch(vm, saved_base, result);

    /* Clean up any frames left by HALT (which doesn't pop the frame) */
    while (vm->frame_count > saved_base) {
        RegCallFrame *f = &vm->frames[vm->frame_count - 1];
        for (int i = 0; i < (int)f->reg_count; i++) value_free_inline(&f->regs[i]);
        vm->frame_count--;
        vm->reg_stack_top -= REGVM_REG_MAX;
    }

    return res;
}

/* ── Pre-computed djb2 hashes for builtin method names ── */
#define MHASH_add                  0x0b885cceu
#define MHASH_all                  0x0b885ddeu
#define MHASH_any                  0x0b885e2du
#define MHASH_bytes                0x0f30b64cu
#define MHASH_camel_case           0xe2889d82u
#define MHASH_capacity             0x104ec913u
#define MHASH_capitalize           0xee09978bu
#define MHASH_chars                0x0f392d36u
#define MHASH_chunk                0x0f3981beu
#define MHASH_clear                0x0f3b6d8cu
#define MHASH_close                0x0f3b9a5bu
#define MHASH_contains             0x42aa8264u
#define MHASH_count                0x0f3d586eu
#define MHASH_delete               0xf8838478u
#define MHASH_deref                0x0f49e72bu
#define MHASH_difference           0x52a92470u
#define MHASH_drop                 0x7c95d91au
#define MHASH_each                 0x7c961b96u
#define MHASH_ends_with            0x9079bb6au
#define MHASH_entries              0x6b84747fu
#define MHASH_enum_name            0x9f13be1au
#define MHASH_enumerate            0x9f82838bu
#define MHASH_fill                 0x7c96cb2cu
#define MHASH_filter               0xfd7675abu
#define MHASH_find                 0x7c96cb66u
#define MHASH_first                0x0f704b8du
#define MHASH_flat                 0x7c96d68cu
#define MHASH_flat_map             0x022d3129u
#define MHASH_flatten              0xb27dd5f3u
#define MHASH_for_each             0x0f4aaefcu
#define MHASH_get                  0x0b887685u
#define MHASH_group_by             0xdd0fdaecu
#define MHASH_has                  0x0b887a41u
#define MHASH_index_of             0x66e4af51u
#define MHASH_inner_type           0xdf644222u
#define MHASH_insert               0x04d4029au
#define MHASH_intersection         0x40c04d3cu
#define MHASH_is_empty             0xdc1854cfu
#define MHASH_is_subset            0x805437d6u
#define MHASH_is_superset          0x05f3913bu
#define MHASH_is_variant           0x443eb735u
#define MHASH_join                 0x7c9915d5u
#define MHASH_kebab_case           0x62be3b95u
#define MHASH_keys                 0x7c9979c1u
#define MHASH_last                 0x7c99f459u
#define MHASH_len                  0x0b888bc4u
#define MHASH_length               0x0b2deac7u
#define MHASH_map                  0x0b888f83u
#define MHASH_max                  0x0b888f8bu
#define MHASH_merge                0x0fecc3f5u
#define MHASH_min                  0x0b889089u
#define MHASH_pad_left             0xf3895c84u
#define MHASH_pad_right            0x6523b4b7u
#define MHASH_payload              0x9c4949cfu
#define MHASH_pop                  0x0b889e14u
#define MHASH_push                 0x7c9c7ae5u
#define MHASH_push_u16             0x1aaf75a0u
#define MHASH_push_u32             0x1aaf75deu
#define MHASH_read_f32             0xf949d66bu
#define MHASH_read_f64             0xf949d6d0u
#define MHASH_read_i8              0x3ddb7381u
#define MHASH_read_i16             0xf949e2f0u
#define MHASH_read_i32             0xf949e32eu
#define MHASH_read_u8              0x3ddb750du
#define MHASH_read_u16             0xf94a15fcu
#define MHASH_read_u32             0xf94a163au
#define MHASH_recv                 0x7c9d4d95u
#define MHASH_reduce               0x19279c1du
#define MHASH_remove               0x192c7473u
#define MHASH_remove_at            0xd988a4a7u
#define MHASH_repeat               0x192dec66u
#define MHASH_replace              0x3eef4e01u
#define MHASH_resize               0x192fa5b7u
#define MHASH_reverse              0x3f5854c1u
#define MHASH_send                 0x7c9ddb4fu
#define MHASH_set                  0x0b88a991u
#define MHASH_slice                0x105d06d5u
#define MHASH_snake_case           0xb7f6c232u
#define MHASH_sort                 0x7c9e066du
#define MHASH_sort_by              0xa365ac87u
#define MHASH_split                0x105f45f1u
#define MHASH_starts_with          0xf5ef8361u
#define MHASH_substring            0xcc998606u
#define MHASH_sum                  0x0b88ab9au
#define MHASH_symmetric_difference 0x1f3d47ecu
#define MHASH_tag                  0x0b88ad41u
#define MHASH_take                 0x7c9e564au
#define MHASH_title_case           0x4b7027c2u
#define MHASH_to_array             0xcebde966u
#define MHASH_to_hex               0x1e83ed8cu
#define MHASH_to_lower             0xcf836790u
#define MHASH_to_string            0xd09c437eu
#define MHASH_to_upper             0xd026b2b3u
#define MHASH_trim                 0x7c9e9e61u
#define MHASH_trim_end             0xcdcebb17u
#define MHASH_trim_start           0x7d6a808eu
#define MHASH_union                0x1082522eu
#define MHASH_unique               0x20cca1bcu
#define MHASH_values               0x22383ff5u
#define MHASH_variant_name         0xb2b2b8bau
#define MHASH_write_u8             0x931616bcu
#define MHASH_write_u16            0xf5d8ed8bu
#define MHASH_write_u32            0xf5d8edc9u
#define MHASH_zip                  0x0b88c7d8u

static inline uint32_t method_hash(const char *s) {
    uint32_t h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

/* Resolve the PIC handler ID for a given (type, method_hash) pair.
 * Returns 0 if no builtin matches, or a PIC_xxx handler ID. */
static uint16_t rvm_pic_resolve(uint8_t type_tag, uint32_t mhash) {
    switch (type_tag) {
        case VAL_ARRAY:
            if (mhash == MHASH_len) return PIC_ARRAY_LEN;
            if (mhash == MHASH_length) return PIC_ARRAY_LENGTH;
            if (mhash == MHASH_push) return PIC_ARRAY_PUSH;
            if (mhash == MHASH_pop) return PIC_ARRAY_POP;
            if (mhash == MHASH_contains) return PIC_ARRAY_CONTAINS;
            if (mhash == MHASH_reverse) return PIC_ARRAY_REVERSE;
            if (mhash == MHASH_join) return PIC_ARRAY_JOIN;
            if (mhash == MHASH_slice) return PIC_ARRAY_SLICE;
            if (mhash == MHASH_take) return PIC_ARRAY_TAKE;
            if (mhash == MHASH_drop) return PIC_ARRAY_DROP;
            if (mhash == MHASH_unique) return PIC_ARRAY_UNIQUE;
            if (mhash == MHASH_first) return PIC_ARRAY_FIRST;
            if (mhash == MHASH_last) return PIC_ARRAY_LAST;
            if (mhash == MHASH_sum) return PIC_ARRAY_SUM;
            if (mhash == MHASH_min) return PIC_ARRAY_MIN;
            if (mhash == MHASH_max) return PIC_ARRAY_MAX;
            if (mhash == MHASH_enumerate) return PIC_ARRAY_ENUMERATE;
            if (mhash == MHASH_index_of) return PIC_ARRAY_INDEX_OF;
            if (mhash == MHASH_zip) return PIC_ARRAY_ZIP;
            if (mhash == MHASH_chunk) return PIC_ARRAY_CHUNK;
            if (mhash == MHASH_flatten) return PIC_ARRAY_FLATTEN;
            if (mhash == MHASH_flat) return PIC_ARRAY_FLAT;
            if (mhash == MHASH_remove_at) return PIC_ARRAY_REMOVE_AT;
            if (mhash == MHASH_insert) return PIC_ARRAY_INSERT;
            if (mhash == MHASH_map) return PIC_ARRAY_MAP;
            if (mhash == MHASH_filter) return PIC_ARRAY_FILTER;
            if (mhash == MHASH_reduce) return PIC_ARRAY_REDUCE;
            if (mhash == MHASH_each) return PIC_ARRAY_EACH;
            if (mhash == MHASH_sort) return PIC_ARRAY_SORT;
            if (mhash == MHASH_find) return PIC_ARRAY_FIND;
            if (mhash == MHASH_any) return PIC_ARRAY_ANY;
            if (mhash == MHASH_all) return PIC_ARRAY_ALL;
            if (mhash == MHASH_for_each) return PIC_ARRAY_FOR_EACH;
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
            if (mhash == MHASH_symmetric_difference) return PIC_SET_SYMMETRIC_DIFFERENCE;
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
            return 0; /* Ref proxies inner type, don't cache NOT_BUILTIN */
        default: break;
    }
    return 0;
}

/* ── Invoke builtin method ── */
/* Returns true if handled, false if not a builtin */

static bool rvm_invoke_builtin(RegVM *vm, LatValue *obj, const char *method, LatValue *args, int arg_count,
                               LatValue *result, const char *var_name) {
    uint32_t mhash = method_hash(method);
    if (obj->type == VAL_ARRAY) {
        if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
             (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
            arg_count == 0) {
            *result = value_int((int64_t)obj->as.array.len);
            return true;
        }
        if (mhash == MHASH_push && strcmp(method, "push") == 0 && arg_count == 1) {
            if (value_is_crystal(obj)) {
                if (var_name)
                    lat_asprintf(&vm->error, "cannot push to crystal array '%s' (use thaw(%s) to make it mutable)",
                                 var_name, var_name);
                else vm->error = strdup("cannot push to a crystal array");
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
                                            lat_asprintf(&vm->error, "pressurized (%s): cannot push to '%s'", mode,
                                                         cf->chunk->local_names[r]);
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
            /* Fast path: primitives skip rvm_clone overhead in push.
             * Values escape into the array, but primitives have no heap data. */
            LatValue val = rvm_clone_or_borrow(&args[0]);
            if (obj->as.array.len >= obj->as.array.cap) {
                obj->as.array.cap = obj->as.array.cap ? obj->as.array.cap * 2 : 4;
                obj->as.array.elems = realloc(obj->as.array.elems, obj->as.array.cap * sizeof(LatValue));
            }
            obj->as.array.elems[obj->as.array.len++] = val;
            *result = value_unit();
            return true;
        }
        if (mhash == MHASH_pop && strcmp(method, "pop") == 0 && arg_count == 0) {
            if (value_is_crystal(obj)) {
                if (var_name)
                    lat_asprintf(&vm->error, "cannot pop from crystal array '%s' (use thaw(%s) to make it mutable)",
                                 var_name, var_name);
                else vm->error = strdup("cannot pop from a crystal array");
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
                                            lat_asprintf(&vm->error, "pressurized (%s): cannot pop from '%s'", mode,
                                                         cf->chunk->local_names[r]);
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
        if (mhash == MHASH_contains && strcmp(method, "contains") == 0 && arg_count == 1) {
            char *err = NULL;
            *result = builtin_array_contains(obj, args, 1, &err);
            return true;
        }
        if (mhash == MHASH_reverse && strcmp(method, "reverse") == 0 && arg_count == 0) {
            char *err = NULL;
            *result = builtin_array_reverse(obj, NULL, 0, &err);
            return true;
        }
        if (mhash == MHASH_map && strcmp(method, "map") == 0 && arg_count == 1) {
            char *err = NULL;
            *result = builtin_array_map(obj, &args[0], regvm_builtin_callback, vm, &err);
            return true;
        }
        if (mhash == MHASH_filter && strcmp(method, "filter") == 0 && arg_count == 1) {
            char *err = NULL;
            *result = builtin_array_filter(obj, &args[0], regvm_builtin_callback, vm, &err);
            return true;
        }
        if (mhash == MHASH_join && strcmp(method, "join") == 0 && arg_count == 1) {
            char *err = NULL;
            *result = builtin_array_join(obj, args, 1, &err);
            return true;
        }
    }

    if (obj->type == VAL_STR) {
        if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
             (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
            arg_count == 0) {
            *result = value_int((int64_t)strlen(obj->as.str_val));
            return true;
        }
        if (mhash == MHASH_contains && strcmp(method, "contains") == 0 && arg_count == 1) {
            if (args[0].type == VAL_STR) {
                *result = value_bool(strstr(obj->as.str_val, args[0].as.str_val) != NULL);
            } else {
                *result = value_bool(false);
            }
            return true;
        }
    }

    if (obj->type == VAL_MAP) {
        if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
             (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
            arg_count == 0) {
            size_t count = 0;
            for (size_t i = 0; i < obj->as.map.map->cap; i++) {
                if (obj->as.map.map->entries[i].state == MAP_OCCUPIED) count++;
            }
            *result = value_int((int64_t)count);
            return true;
        }
        if (mhash == MHASH_keys && strcmp(method, "keys") == 0 && arg_count == 0) {
            size_t cap = obj->as.map.map->cap;
            LatValue *keys = malloc(cap * sizeof(LatValue));
            if (!keys) return 0;
            size_t count = 0;
            for (size_t i = 0; i < cap; i++) {
                if (obj->as.map.map->entries[i].state == MAP_OCCUPIED)
                    keys[count++] = value_string(obj->as.map.map->entries[i].key);
            }
            *result = value_array(keys, count);
            free(keys);
            return true;
        }
        if (mhash == MHASH_values && strcmp(method, "values") == 0 && arg_count == 0) {
            size_t cap = obj->as.map.map->cap;
            LatValue *vals = malloc(cap * sizeof(LatValue));
            if (!vals) return 0;
            size_t count = 0;
            for (size_t i = 0; i < cap; i++) {
                if (obj->as.map.map->entries[i].state == MAP_OCCUPIED)
                    vals[count++] = rvm_clone((LatValue *)obj->as.map.map->entries[i].value);
            }
            *result = value_array(vals, count);
            free(vals);
            return true;
        }
        if (mhash == MHASH_get && strcmp(method, "get") == 0 && arg_count == 1) {
            if (args[0].type == VAL_STR) {
                LatValue *val = lat_map_get(obj->as.map.map, args[0].as.str_val);
                *result = val ? rvm_clone(val) : value_nil();
            } else {
                *result = value_nil();
            }
            return true;
        }
        if (mhash == MHASH_set && strcmp(method, "set") == 0 && arg_count == 2) {
            if (args[0].type == VAL_STR) {
                LatValue cloned = rvm_clone(&args[1]);
                lat_map_set(obj->as.map.map, args[0].as.str_val, &cloned);
            }
            *result = value_unit();
            return true;
        }
        if (((mhash == MHASH_has && strcmp(method, "has") == 0) ||
             (mhash == MHASH_contains && strcmp(method, "contains") == 0)) &&
            arg_count == 1) {
            if (args[0].type == VAL_STR) *result = value_bool(lat_map_get(obj->as.map.map, args[0].as.str_val) != NULL);
            else *result = value_bool(false);
            return true;
        }
        if (mhash == MHASH_entries && strcmp(method, "entries") == 0 && arg_count == 0) {
            size_t cap = obj->as.map.map->cap;
            LatValue *entries = malloc(cap * sizeof(LatValue));
            if (!entries) return 0;
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
        if (mhash == MHASH_merge && strcmp(method, "merge") == 0 && arg_count == 1) {
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
        if (mhash == MHASH_for_each && strcmp(method, "for_each") == 0 && arg_count == 1) {
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
        if (mhash == MHASH_filter && strcmp(method, "filter") == 0 && arg_count == 1) {
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
        if (mhash == MHASH_enumerate && strcmp(method, "enumerate") == 0 && arg_count == 0) {
            char *err = NULL;
            *result = builtin_array_enumerate(obj, NULL, 0, &err);
            return true;
        }
        if (mhash == MHASH_reduce && strcmp(method, "reduce") == 0 && (arg_count == 1 || arg_count == 2)) {
            char *err = NULL;
            bool has_init = (arg_count == 2);
            *result = builtin_array_reduce(obj, has_init ? &args[1] : NULL, has_init, &args[0], regvm_builtin_callback,
                                           vm, &err);
            return true;
        }
        if (((mhash == MHASH_each && strcmp(method, "each") == 0) ||
             (mhash == MHASH_for_each && strcmp(method, "for_each") == 0)) &&
            arg_count == 1) {
            char *err = NULL;
            *result = builtin_array_each(obj, &args[0], regvm_builtin_callback, vm, &err);
            return true;
        }
        if (mhash == MHASH_sort && strcmp(method, "sort") == 0 && arg_count <= 1) {
            size_t len = obj->as.array.len;
            LatValue *sorted = malloc(len * sizeof(LatValue));
            if (!sorted) return false;
            for (size_t i = 0; i < len; i++) sorted[i] = rvm_clone(&obj->as.array.elems[i]);
            /* Insertion sort */
            for (size_t i = 1; i < len; i++) {
                LatValue key = sorted[i];
                int64_t j = (int64_t)i - 1;
                while (j >= 0) {
                    bool swap = false;
                    if (arg_count == 1) {
                        LatValue cb_args[2] = {rvm_clone(&sorted[j]), rvm_clone(&key)};
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
                            double a =
                                sorted[j].type == VAL_FLOAT ? sorted[j].as.float_val : (double)sorted[j].as.int_val;
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
        if (mhash == MHASH_sort_by && strcmp(method, "sort_by") == 0 && arg_count == 1) {
            char *err = NULL;
            *result = builtin_array_sort_by(obj, &args[0], regvm_builtin_callback, vm, &err);
            return true;
        }
        if (mhash == MHASH_find && strcmp(method, "find") == 0 && arg_count == 1) {
            char *err = NULL;
            *result = builtin_array_find(obj, &args[0], regvm_builtin_callback, vm, &err);
            return true;
        }
        if (mhash == MHASH_any && strcmp(method, "any") == 0 && arg_count == 1) {
            char *err = NULL;
            *result = builtin_array_any(obj, &args[0], regvm_builtin_callback, vm, &err);
            return true;
        }
        if (mhash == MHASH_all && strcmp(method, "all") == 0 && arg_count == 1) {
            char *err = NULL;
            *result = builtin_array_all(obj, &args[0], regvm_builtin_callback, vm, &err);
            return true;
        }
        if (mhash == MHASH_flat_map && strcmp(method, "flat_map") == 0 && arg_count == 1) {
            char *err = NULL;
            *result = builtin_array_flat_map(obj, &args[0], regvm_builtin_callback, vm, &err);
            return true;
        }
        if (mhash == MHASH_unique && strcmp(method, "unique") == 0 && arg_count == 0) {
            char *err = NULL;
            *result = builtin_array_unique(obj, NULL, 0, &err);
            return true;
        }
        if (mhash == MHASH_index_of && strcmp(method, "index_of") == 0 && arg_count == 1) {
            char *err = NULL;
            *result = builtin_array_index_of(obj, args, 1, &err);
            return true;
        }
        if (mhash == MHASH_first && strcmp(method, "first") == 0 && arg_count == 0) {
            char *err = NULL;
            *result = builtin_array_first(obj, NULL, 0, &err);
            return true;
        }
        if (mhash == MHASH_last && strcmp(method, "last") == 0 && arg_count == 0) {
            char *err = NULL;
            *result = builtin_array_last(obj, NULL, 0, &err);
            return true;
        }
        if (mhash == MHASH_slice && strcmp(method, "slice") == 0 && (arg_count == 1 || arg_count == 2)) {
            int64_t start = args[0].type == VAL_INT ? args[0].as.int_val : 0;
            int64_t end = arg_count == 2 && args[1].type == VAL_INT ? args[1].as.int_val : (int64_t)obj->as.array.len;
            if (start < 0) start += (int64_t)obj->as.array.len;
            if (end < 0) end += (int64_t)obj->as.array.len;
            if (start < 0) start = 0;
            if (end > (int64_t)obj->as.array.len) end = (int64_t)obj->as.array.len;
            if (start >= end) {
                *result = value_array(NULL, 0);
                return true;
            }
            size_t count = (size_t)(end - start);
            LatValue *elems = malloc(count * sizeof(LatValue));
            if (!elems) return 0;
            for (size_t i = 0; i < count; i++) elems[i] = rvm_clone(&obj->as.array.elems[start + (int64_t)i]);
            *result = value_array(elems, count);
            free(elems);
            return true;
        }
        if (mhash == MHASH_take && strcmp(method, "take") == 0 && arg_count == 1) {
            int64_t n = args[0].type == VAL_INT ? args[0].as.int_val : 0;
            if (n < 0) n = 0;
            if (n > (int64_t)obj->as.array.len) n = (int64_t)obj->as.array.len;
            LatValue *elems = malloc((size_t)n * sizeof(LatValue));
            if (!elems) return 0;
            for (int64_t i = 0; i < n; i++) elems[i] = rvm_clone(&obj->as.array.elems[i]);
            *result = value_array(elems, (size_t)n);
            free(elems);
            return true;
        }
        if (mhash == MHASH_drop && strcmp(method, "drop") == 0 && arg_count == 1) {
            int64_t n = args[0].type == VAL_INT ? args[0].as.int_val : 0;
            if (n < 0) n = 0;
            if (n > (int64_t)obj->as.array.len) n = (int64_t)obj->as.array.len;
            size_t count = obj->as.array.len - (size_t)n;
            LatValue *elems = malloc(count * sizeof(LatValue));
            if (!elems) return 0;
            for (size_t i = 0; i < count; i++) elems[i] = rvm_clone(&obj->as.array.elems[n + (int64_t)i]);
            *result = value_array(elems, count);
            free(elems);
            return true;
        }
        if (mhash == MHASH_flatten && strcmp(method, "flatten") == 0 && arg_count == 0) {
            char *err = NULL;
            *result = builtin_array_flatten(obj, NULL, 0, &err);
            return true;
        }
        if (mhash == MHASH_zip && strcmp(method, "zip") == 0 && arg_count == 1) {
            char *err = NULL;
            *result = builtin_array_zip(obj, args, 1, &err);
            return true;
        }
        if (mhash == MHASH_sum && strcmp(method, "sum") == 0 && arg_count == 0) {
            char *err = NULL;
            *result = builtin_array_sum(obj, NULL, 0, &err);
            return true;
        }
        if (mhash == MHASH_min && strcmp(method, "min") == 0 && arg_count == 0) {
            char *err = NULL;
            *result = builtin_array_min(obj, NULL, 0, &err);
            if (err) { vm->error = err; }
            return true;
        }
        if (mhash == MHASH_max && strcmp(method, "max") == 0 && arg_count == 0) {
            char *err = NULL;
            *result = builtin_array_max(obj, NULL, 0, &err);
            if (err) { vm->error = err; }
            return true;
        }
        if (mhash == MHASH_insert && strcmp(method, "insert") == 0 && arg_count == 2) {
            if (value_is_crystal(obj)) {
                vm->error = strdup("cannot insert into a crystal array");
                *result = value_unit();
                return true;
            }
            if (obj->phase == VTAG_SUBLIMATED) {
                vm->error = strdup("cannot insert into a sublimated array");
                *result = value_unit();
                return true;
            }
            /* Check pressure constraints */
            if (vm->rt && vm->rt->pressure_count > 0) {
                RegCallFrame *cf = &vm->frames[vm->frame_count - 1];
                if (cf->chunk && cf->chunk->local_names) {
                    for (size_t r = 0; r < cf->chunk->local_name_cap; r++) {
                        if (&cf->regs[r] == obj && cf->chunk->local_names[r] && cf->chunk->local_names[r][0]) {
                            for (size_t pi = 0; pi < vm->rt->pressure_count; pi++) {
                                if (strcmp(vm->rt->pressures[pi].name, cf->chunk->local_names[r]) == 0) {
                                    const char *mode = vm->rt->pressures[pi].mode;
                                    if (strcmp(mode, "no_grow") == 0 || strcmp(mode, "no_resize") == 0) {
                                        lat_asprintf(&vm->error, "pressurized (%s): cannot insert into '%s'", mode,
                                                     cf->chunk->local_names[r]);
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
            if (args[0].type != VAL_INT) {
                *result = value_unit();
                return true;
            }
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
        if (mhash == MHASH_remove_at && strcmp(method, "remove_at") == 0 && arg_count == 1) {
            if (value_is_crystal(obj)) {
                vm->error = strdup("cannot remove from a crystal array");
                *result = value_unit();
                return true;
            }
            if (obj->phase == VTAG_SUBLIMATED) {
                vm->error = strdup("cannot remove from a sublimated array");
                *result = value_unit();
                return true;
            }
            /* Check pressure constraints */
            if (vm->rt && vm->rt->pressure_count > 0) {
                RegCallFrame *cf = &vm->frames[vm->frame_count - 1];
                if (cf->chunk && cf->chunk->local_names) {
                    for (size_t r = 0; r < cf->chunk->local_name_cap; r++) {
                        if (&cf->regs[r] == obj && cf->chunk->local_names[r] && cf->chunk->local_names[r][0]) {
                            for (size_t pi = 0; pi < vm->rt->pressure_count; pi++) {
                                if (strcmp(vm->rt->pressures[pi].name, cf->chunk->local_names[r]) == 0) {
                                    const char *mode = vm->rt->pressures[pi].mode;
                                    if (strcmp(mode, "no_shrink") == 0 || strcmp(mode, "no_resize") == 0) {
                                        lat_asprintf(&vm->error, "pressurized (%s): cannot remove from '%s'", mode,
                                                     cf->chunk->local_names[r]);
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
            if (args[0].type != VAL_INT) {
                *result = value_nil();
                return true;
            }
            int64_t idx = args[0].as.int_val;
            if (idx < 0) idx += (int64_t)obj->as.array.len;
            if (idx < 0 || (size_t)idx >= obj->as.array.len) {
                *result = value_nil();
                return true;
            }
            *result = obj->as.array.elems[idx];
            memmove(&obj->as.array.elems[idx], &obj->as.array.elems[idx + 1],
                    (obj->as.array.len - (size_t)idx - 1) * sizeof(LatValue));
            obj->as.array.len--;
            return true;
        }
        if (mhash == MHASH_chunk && strcmp(method, "chunk") == 0 && arg_count == 1) {
            char *err = NULL;
            *result = builtin_array_chunk(obj, args, 1, &err);
            return true;
        }
        if (mhash == MHASH_group_by && strcmp(method, "group_by") == 0 && arg_count == 1) {
            char *err = NULL;
            *result = builtin_array_group_by(obj, &args[0], regvm_builtin_callback, vm, &err);
            return true;
        }
    }

    /* ── Array additional methods ── */
    if (obj->type == VAL_ARRAY) {
        if (mhash == MHASH_flat && strcmp(method, "flat") == 0 && arg_count == 0) {
            char *err = NULL;
            *result = builtin_array_flatten(obj, NULL, 0, &err);
            return true;
        }
        if (mhash == MHASH_first && strcmp(method, "first") == 0 && arg_count == 0) {
            char *err = NULL;
            *result = builtin_array_first(obj, NULL, 0, &err);
            return true;
        }
        if (mhash == MHASH_last && strcmp(method, "last") == 0 && arg_count == 0) {
            char *err = NULL;
            *result = builtin_array_last(obj, NULL, 0, &err);
            return true;
        }
        if (mhash == MHASH_min && strcmp(method, "min") == 0 && arg_count == 0) {
            char *err = NULL;
            *result = builtin_array_min(obj, NULL, 0, &err);
            if (err) { vm->error = err; }
            return true;
        }
        if (mhash == MHASH_max && strcmp(method, "max") == 0 && arg_count == 0) {
            char *err = NULL;
            *result = builtin_array_max(obj, NULL, 0, &err);
            if (err) { vm->error = err; }
            return true;
        }
    }

    /* ── String additional methods ── */
    if (obj->type == VAL_STR) {
        if (mhash == MHASH_split && strcmp(method, "split") == 0 && arg_count == 1) {
            if (args[0].type != VAL_STR) {
                *result = value_array(NULL, 0);
                return true;
            }
            const char *s = obj->as.str_val;
            const char *sep = args[0].as.str_val;
            size_t sep_len = strlen(sep);
            size_t cap = 8;
            LatValue *parts = malloc(cap * sizeof(LatValue));
            if (!parts) return 0;
            size_t count = 0;
            if (sep_len == 0) {
                for (size_t i = 0; s[i]; i++) {
                    if (count >= cap) {
                        cap *= 2;
                        parts = realloc(parts, cap * sizeof(LatValue));
                    }
                    char c[2] = {s[i], '\0'};
                    parts[count++] = value_string(c);
                }
            } else {
                const char *p = s;
                while (*p) {
                    const char *found = strstr(p, sep);
                    if (!found) {
                        if (count >= cap) {
                            cap *= 2;
                            parts = realloc(parts, cap * sizeof(LatValue));
                        }
                        parts[count++] = value_string(p);
                        break;
                    }
                    if (count >= cap) {
                        cap *= 2;
                        parts = realloc(parts, cap * sizeof(LatValue));
                    }
                    char *part = strndup(p, (size_t)(found - p));
                    parts[count++] = value_string_owned(part);
                    p = found + sep_len;
                }
            }
            *result = value_array(parts, count);
            free(parts);
            return true;
        }
        if (mhash == MHASH_trim && strcmp(method, "trim") == 0 && arg_count == 0) {
            const char *s = obj->as.str_val;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            const char *e = obj->as.str_val + strlen(obj->as.str_val);
            while (e > s && (*(e - 1) == ' ' || *(e - 1) == '\t' || *(e - 1) == '\n' || *(e - 1) == '\r')) e--;
            *result = value_string_owned(strndup(s, (size_t)(e - s)));
            return true;
        }
        if (mhash == MHASH_trim_start && strcmp(method, "trim_start") == 0 && arg_count == 0) {
            const char *s = obj->as.str_val;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            *result = value_string(s);
            return true;
        }
        if (mhash == MHASH_trim_end && strcmp(method, "trim_end") == 0 && arg_count == 0) {
            size_t len = strlen(obj->as.str_val);
            const char *e = obj->as.str_val + len;
            while (e > obj->as.str_val && (*(e - 1) == ' ' || *(e - 1) == '\t' || *(e - 1) == '\n' || *(e - 1) == '\r'))
                e--;
            *result = value_string_owned(strndup(obj->as.str_val, (size_t)(e - obj->as.str_val)));
            return true;
        }
        if (mhash == MHASH_to_upper && strcmp(method, "to_upper") == 0 && arg_count == 0) {
            char *s = strdup(obj->as.str_val);
            for (char *p = s; *p; p++)
                if (*p >= 'a' && *p <= 'z') *p -= 32;
            *result = value_string_owned(s);
            return true;
        }
        if (mhash == MHASH_to_lower && strcmp(method, "to_lower") == 0 && arg_count == 0) {
            char *s = strdup(obj->as.str_val);
            for (char *p = s; *p; p++)
                if (*p >= 'A' && *p <= 'Z') *p += 32;
            *result = value_string_owned(s);
            return true;
        }
        if (mhash == MHASH_capitalize && strcmp(method, "capitalize") == 0 && arg_count == 0) {
            *result = value_string_owned(lat_str_capitalize(obj->as.str_val));
            return true;
        }
        if (mhash == MHASH_title_case && strcmp(method, "title_case") == 0 && arg_count == 0) {
            *result = value_string_owned(lat_str_title_case(obj->as.str_val));
            return true;
        }
        if (mhash == MHASH_snake_case && strcmp(method, "snake_case") == 0 && arg_count == 0) {
            *result = value_string_owned(lat_str_snake_case(obj->as.str_val));
            return true;
        }
        if (mhash == MHASH_camel_case && strcmp(method, "camel_case") == 0 && arg_count == 0) {
            *result = value_string_owned(lat_str_camel_case(obj->as.str_val));
            return true;
        }
        if (mhash == MHASH_kebab_case && strcmp(method, "kebab_case") == 0 && arg_count == 0) {
            *result = value_string_owned(lat_str_kebab_case(obj->as.str_val));
            return true;
        }
        if (mhash == MHASH_starts_with && strcmp(method, "starts_with") == 0 && arg_count == 1) {
            if (args[0].type == VAL_STR)
                *result = value_bool(strncmp(obj->as.str_val, args[0].as.str_val, strlen(args[0].as.str_val)) == 0);
            else *result = value_bool(false);
            return true;
        }
        if (mhash == MHASH_ends_with && strcmp(method, "ends_with") == 0 && arg_count == 1) {
            if (args[0].type == VAL_STR) {
                size_t slen = strlen(obj->as.str_val);
                size_t plen = strlen(args[0].as.str_val);
                *result = value_bool(plen <= slen && strcmp(obj->as.str_val + slen - plen, args[0].as.str_val) == 0);
            } else {
                *result = value_bool(false);
            }
            return true;
        }
        if (mhash == MHASH_replace && strcmp(method, "replace") == 0 && arg_count == 2) {
            if (args[0].type != VAL_STR || args[1].type != VAL_STR) {
                *result = rvm_clone(obj);
                return true;
            }
            const char *s = obj->as.str_val;
            const char *from = args[0].as.str_val;
            const char *to = args[1].as.str_val;
            size_t from_len = strlen(from), to_len = strlen(to);
            if (from_len == 0) {
                *result = rvm_clone(obj);
                return true;
            }
            size_t cap = strlen(s) + 64;
            char *buf = malloc(cap);
            if (!buf) return 0;
            size_t pos = 0;
            while (*s) {
                if (strncmp(s, from, from_len) == 0) {
                    while (pos + to_len >= cap) {
                        cap *= 2;
                        buf = realloc(buf, cap);
                    }
                    memcpy(buf + pos, to, to_len);
                    pos += to_len;
                    s += from_len;
                } else {
                    if (pos + 1 >= cap) {
                        cap *= 2;
                        buf = realloc(buf, cap);
                    }
                    buf[pos++] = *s++;
                }
            }
            buf[pos] = '\0';
            *result = value_string_owned(buf);
            return true;
        }
        if (mhash == MHASH_index_of && strcmp(method, "index_of") == 0 && arg_count == 1) {
            if (args[0].type == VAL_STR) {
                const char *found = strstr(obj->as.str_val, args[0].as.str_val);
                *result = found ? value_int((int64_t)(found - obj->as.str_val)) : value_int(-1);
            } else {
                *result = value_int(-1);
            }
            return true;
        }
        if (mhash == MHASH_substring && strcmp(method, "substring") == 0 && (arg_count == 1 || arg_count == 2)) {
            size_t slen = strlen(obj->as.str_val);
            int64_t start = args[0].type == VAL_INT ? args[0].as.int_val : 0;
            int64_t end = arg_count == 2 && args[1].type == VAL_INT ? args[1].as.int_val : (int64_t)slen;
            if (start < 0) start += (int64_t)slen;
            if (end < 0) end += (int64_t)slen;
            if (start < 0) start = 0;
            if (end > (int64_t)slen) end = (int64_t)slen;
            if (start >= end) {
                *result = value_string("");
                return true;
            }
            *result = value_string_owned(strndup(obj->as.str_val + start, (size_t)(end - start)));
            return true;
        }
        if (mhash == MHASH_repeat && strcmp(method, "repeat") == 0 && arg_count == 1) {
            if (args[0].type != VAL_INT || args[0].as.int_val < 0) {
                *result = value_string("");
                return true;
            }
            int64_t n = args[0].as.int_val;
            size_t slen = strlen(obj->as.str_val);
            char *buf = malloc(slen * (size_t)n + 1);
            if (!buf) return 0;
            for (int64_t i = 0; i < n; i++) memcpy(buf + i * (int64_t)slen, obj->as.str_val, slen);
            buf[slen * (size_t)n] = '\0';
            *result = value_string_owned(buf);
            return true;
        }
        if (mhash == MHASH_chars && strcmp(method, "chars") == 0 && arg_count == 0) {
            size_t len = strlen(obj->as.str_val);
            LatValue *elems = malloc(len * sizeof(LatValue));
            if (!elems) return false;
            for (size_t i = 0; i < len; i++) {
                char c[2] = {obj->as.str_val[i], '\0'};
                elems[i] = value_string(c);
            }
            *result = value_array(elems, len);
            free(elems);
            return true;
        }
        if (mhash == MHASH_bytes && strcmp(method, "bytes") == 0 && arg_count == 0) {
            size_t len = strlen(obj->as.str_val);
            LatValue *elems = malloc(len * sizeof(LatValue));
            if (!elems) return 0;
            for (size_t i = 0; i < len; i++) elems[i] = value_int((int64_t)(unsigned char)obj->as.str_val[i]);
            *result = value_array(elems, len);
            free(elems);
            return true;
        }
        if (mhash == MHASH_reverse && strcmp(method, "reverse") == 0 && arg_count == 0) {
            size_t len = strlen(obj->as.str_val);
            char *buf = malloc(len + 1);
            if (!buf) return 0;
            for (size_t i = 0; i < len; i++) buf[i] = obj->as.str_val[len - 1 - i];
            buf[len] = '\0';
            *result = value_string_owned(buf);
            return true;
        }
        if (mhash == MHASH_pad_left && strcmp(method, "pad_left") == 0 && (arg_count == 1 || arg_count == 2)) {
            int64_t n = args[0].type == VAL_INT ? args[0].as.int_val : 0;
            char pad =
                (arg_count == 2 && args[1].type == VAL_STR && args[1].as.str_val[0]) ? args[1].as.str_val[0] : ' ';
            size_t slen = strlen(obj->as.str_val);
            if ((int64_t)slen >= n) {
                *result = rvm_clone(obj);
                return true;
            }
            size_t plen = (size_t)n - slen;
            char *buf = malloc((size_t)n + 1);
            if (!buf) return 0;
            memset(buf, pad, plen);
            memcpy(buf + plen, obj->as.str_val, slen);
            buf[(size_t)n] = '\0';
            *result = value_string_owned(buf);
            return true;
        }
        if (mhash == MHASH_pad_right && strcmp(method, "pad_right") == 0 && (arg_count == 1 || arg_count == 2)) {
            int64_t n = args[0].type == VAL_INT ? args[0].as.int_val : 0;
            char pad =
                (arg_count == 2 && args[1].type == VAL_STR && args[1].as.str_val[0]) ? args[1].as.str_val[0] : ' ';
            size_t slen = strlen(obj->as.str_val);
            if ((int64_t)slen >= n) {
                *result = rvm_clone(obj);
                return true;
            }
            char *buf = malloc((size_t)n + 1);
            if (!buf) return 0;
            memcpy(buf, obj->as.str_val, slen);
            memset(buf + slen, pad, (size_t)n - slen);
            buf[(size_t)n] = '\0';
            *result = value_string_owned(buf);
            return true;
        }
    }

    /* ── Enum methods ── */
    if (obj->type == VAL_ENUM) {
        if ((mhash == MHASH_tag && strcmp(method, "tag") == 0) ||
            (mhash == MHASH_variant_name && strcmp(method, "variant_name") == 0)) {
            *result = value_string(obj->as.enm.variant_name);
            return true;
        }
        if (mhash == MHASH_enum_name && strcmp(method, "enum_name") == 0) {
            *result = value_string(obj->as.enm.enum_name);
            return true;
        }
        if (mhash == MHASH_payload && strcmp(method, "payload") == 0) {
            if (obj->as.enm.payload_count > 0) {
                LatValue *elems = malloc(obj->as.enm.payload_count * sizeof(LatValue));
                if (!elems) return 0;
                for (size_t pi = 0; pi < obj->as.enm.payload_count; pi++)
                    elems[pi] = rvm_clone(&obj->as.enm.payload[pi]);
                *result = value_array(elems, obj->as.enm.payload_count);
                free(elems);
            } else {
                *result = value_array(NULL, 0);
            }
            return true;
        }
        if (mhash == MHASH_is_variant && strcmp(method, "is_variant") == 0 && arg_count == 1) {
            if (args[0].type == VAL_STR)
                *result = value_bool(strcmp(obj->as.enm.variant_name, args[0].as.str_val) == 0);
            else *result = value_bool(false);
            return true;
        }
    }

    /* ── Tuple methods ── */
    if (obj->type == VAL_TUPLE) {
        if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
             (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
            arg_count == 0) {
            *result = value_int((int64_t)obj->as.tuple.len);
            return true;
        }
    }

    /* ── Range methods ── */
    if (obj->type == VAL_RANGE) {
        if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
             (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
            arg_count == 0) {
            int64_t len = obj->as.range.end - obj->as.range.start;
            *result = value_int(len > 0 ? len : 0);
            return true;
        }
        if (mhash == MHASH_contains && strcmp(method, "contains") == 0 && arg_count == 1) {
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
        if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
             (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
            arg_count == 0) {
            *result = value_int((int64_t)lat_map_len(obj->as.set.map));
            return true;
        }
        if (mhash == MHASH_has && strcmp(method, "has") == 0 && arg_count == 1) {
            char *key = value_hash_key(&args[0]);
            bool found = lat_map_contains(obj->as.set.map, key);
            free(key);
            *result = value_bool(found);
            return true;
        }
        if (mhash == MHASH_add && strcmp(method, "add") == 0 && arg_count == 1) {
            char *key = value_hash_key(&args[0]);
            LatValue val = rvm_clone(&args[0]);
            lat_map_set(obj->as.set.map, key, &val);
            free(key);
            *result = value_unit();
            return true;
        }
        if (mhash == MHASH_remove && strcmp(method, "remove") == 0 && arg_count == 1) {
            char *key = value_hash_key(&args[0]);
            lat_map_remove(obj->as.set.map, key);
            free(key);
            *result = value_unit();
            return true;
        }
        if (mhash == MHASH_to_array && strcmp(method, "to_array") == 0 && arg_count == 0) {
            size_t len = lat_map_len(obj->as.set.map);
            LatValue *elems = malloc((len > 0 ? len : 1) * sizeof(LatValue));
            if (!elems) return 0;
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
        if (mhash == MHASH_union && strcmp(method, "union") == 0 && arg_count == 1 && args[0].type == VAL_SET) {
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
        if (mhash == MHASH_intersection && strcmp(method, "intersection") == 0 && arg_count == 1 &&
            args[0].type == VAL_SET) {
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
        if (mhash == MHASH_difference && strcmp(method, "difference") == 0 && arg_count == 1 &&
            args[0].type == VAL_SET) {
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
        if (mhash == MHASH_symmetric_difference && strcmp(method, "symmetric_difference") == 0 && arg_count == 1 &&
            args[0].type == VAL_SET) {
            LatValue result_set = value_set_new();
            /* Add elements in self but not in other */
            for (size_t i = 0; i < obj->as.set.map->cap; i++) {
                if (obj->as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                if (!lat_map_contains(args[0].as.set.map, obj->as.set.map->entries[i].key)) {
                    LatValue v = rvm_clone((LatValue *)obj->as.set.map->entries[i].value);
                    lat_map_set(result_set.as.set.map, obj->as.set.map->entries[i].key, &v);
                }
            }
            /* Add elements in other but not in self */
            for (size_t i = 0; i < args[0].as.set.map->cap; i++) {
                if (args[0].as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                if (!lat_map_contains(obj->as.set.map, args[0].as.set.map->entries[i].key)) {
                    LatValue v = rvm_clone((LatValue *)args[0].as.set.map->entries[i].value);
                    lat_map_set(result_set.as.set.map, args[0].as.set.map->entries[i].key, &v);
                }
            }
            *result = result_set;
            return true;
        }
        if (mhash == MHASH_is_subset && strcmp(method, "is_subset") == 0 && arg_count == 1 && args[0].type == VAL_SET) {
            bool is = true;
            for (size_t i = 0; i < obj->as.set.map->cap; i++) {
                if (obj->as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                if (!lat_map_contains(args[0].as.set.map, obj->as.set.map->entries[i].key)) {
                    is = false;
                    break;
                }
            }
            *result = value_bool(is);
            return true;
        }
        if (mhash == MHASH_is_superset && strcmp(method, "is_superset") == 0 && arg_count == 1 &&
            args[0].type == VAL_SET) {
            bool is = true;
            for (size_t i = 0; i < args[0].as.set.map->cap; i++) {
                if (args[0].as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                if (!lat_map_contains(obj->as.set.map, args[0].as.set.map->entries[i].key)) {
                    is = false;
                    break;
                }
            }
            *result = value_bool(is);
            return true;
        }
    }

    /* ── String additional: count, is_empty ── */
    if (obj->type == VAL_STR) {
        if (mhash == MHASH_count && strcmp(method, "count") == 0 && arg_count == 1) {
            int64_t cnt = 0;
            if (args[0].type == VAL_STR && args[0].as.str_val[0]) {
                const char *p = obj->as.str_val;
                size_t nlen = strlen(args[0].as.str_val);
                while ((p = strstr(p, args[0].as.str_val)) != NULL) {
                    cnt++;
                    p += nlen;
                }
            }
            *result = value_int(cnt);
            return true;
        }
        if (mhash == MHASH_is_empty && strcmp(method, "is_empty") == 0 && arg_count == 0) {
            *result = value_bool(obj->as.str_val[0] == '\0');
            return true;
        }
    }

    /* ── Map additional: remove/delete ── */
    if (obj->type == VAL_MAP) {
        if (((mhash == MHASH_remove && strcmp(method, "remove") == 0) ||
             (mhash == MHASH_delete && strcmp(method, "delete") == 0)) &&
            arg_count == 1) {
            if (args[0].type == VAL_STR) lat_map_remove(obj->as.map.map, args[0].as.str_val);
            *result = value_unit();
            return true;
        }
        if (mhash == MHASH_map && strcmp(method, "map") == 0 && arg_count == 1) {
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
        if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
             (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
            arg_count == 0) {
            *result = value_int((int64_t)obj->as.buffer.len);
            return true;
        }
        if (mhash == MHASH_capacity && strcmp(method, "capacity") == 0 && arg_count == 0) {
            *result = value_int((int64_t)obj->as.buffer.cap);
            return true;
        }
        if (mhash == MHASH_push && strcmp(method, "push") == 0 && arg_count == 1) {
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
        if (mhash == MHASH_push_u16 && strcmp(method, "push_u16") == 0 && arg_count == 1) {
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
        if (mhash == MHASH_push_u32 && strcmp(method, "push_u32") == 0 && arg_count == 1) {
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
        if (mhash == MHASH_read_u8 && strcmp(method, "read_u8") == 0 && arg_count == 1) {
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
        if (mhash == MHASH_write_u8 && strcmp(method, "write_u8") == 0 && arg_count == 2) {
            if (args[0].type == VAL_INT && args[1].type == VAL_INT) {
                int64_t idx = args[0].as.int_val;
                if (idx >= 0 && (size_t)idx < obj->as.buffer.len)
                    obj->as.buffer.data[idx] = (uint8_t)args[1].as.int_val;
            }
            *result = value_unit();
            return true;
        }
        if (mhash == MHASH_read_u16 && strcmp(method, "read_u16") == 0 && arg_count == 1) {
            if (args[0].type == VAL_INT) {
                int64_t idx = args[0].as.int_val;
                if (idx < 0 || (size_t)idx + 2 > obj->as.buffer.len) {
                    *result = value_nil();
                } else {
                    uint16_t v = (uint16_t)(obj->as.buffer.data[idx]) | ((uint16_t)(obj->as.buffer.data[idx + 1]) << 8);
                    *result = value_int((int64_t)v);
                }
            } else {
                *result = value_nil();
            }
            return true;
        }
        if (mhash == MHASH_write_u16 && strcmp(method, "write_u16") == 0 && arg_count == 2) {
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
        if (mhash == MHASH_read_u32 && strcmp(method, "read_u32") == 0 && arg_count == 1) {
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
        if (mhash == MHASH_write_u32 && strcmp(method, "write_u32") == 0 && arg_count == 2) {
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
        if (mhash == MHASH_read_i8 && strcmp(method, "read_i8") == 0 && arg_count == 1) {
            if (args[0].type == VAL_INT) {
                int64_t idx = args[0].as.int_val;
                if (idx < 0 || (size_t)idx >= obj->as.buffer.len) {
                    *result = value_nil();
                } else {
                    *result = value_int((int8_t)obj->as.buffer.data[idx]);
                }
            } else {
                *result = value_nil();
            }
            return true;
        }
        if (mhash == MHASH_read_i16 && strcmp(method, "read_i16") == 0 && arg_count == 1) {
            if (args[0].type == VAL_INT) {
                int64_t idx = args[0].as.int_val;
                if (idx < 0 || (size_t)idx + 2 > obj->as.buffer.len) {
                    *result = value_nil();
                } else {
                    int16_t v;
                    memcpy(&v, obj->as.buffer.data + idx, 2);
                    *result = value_int(v);
                }
            } else {
                *result = value_nil();
            }
            return true;
        }
        if (mhash == MHASH_read_i32 && strcmp(method, "read_i32") == 0 && arg_count == 1) {
            if (args[0].type == VAL_INT) {
                int64_t idx = args[0].as.int_val;
                if (idx < 0 || (size_t)idx + 4 > obj->as.buffer.len) {
                    *result = value_nil();
                } else {
                    int32_t v;
                    memcpy(&v, obj->as.buffer.data + idx, 4);
                    *result = value_int(v);
                }
            } else {
                *result = value_nil();
            }
            return true;
        }
        if (mhash == MHASH_read_f32 && strcmp(method, "read_f32") == 0 && arg_count == 1) {
            if (args[0].type == VAL_INT) {
                int64_t idx = args[0].as.int_val;
                if (idx < 0 || (size_t)idx + 4 > obj->as.buffer.len) {
                    *result = value_nil();
                } else {
                    float v;
                    memcpy(&v, obj->as.buffer.data + idx, 4);
                    *result = value_float((double)v);
                }
            } else {
                *result = value_nil();
            }
            return true;
        }
        if (mhash == MHASH_read_f64 && strcmp(method, "read_f64") == 0 && arg_count == 1) {
            if (args[0].type == VAL_INT) {
                int64_t idx = args[0].as.int_val;
                if (idx < 0 || (size_t)idx + 8 > obj->as.buffer.len) {
                    *result = value_nil();
                } else {
                    double v;
                    memcpy(&v, obj->as.buffer.data + idx, 8);
                    *result = value_float(v);
                }
            } else {
                *result = value_nil();
            }
            return true;
        }
        if (mhash == MHASH_slice && strcmp(method, "slice") == 0 && (arg_count == 1 || arg_count == 2)) {
            int64_t start = args[0].type == VAL_INT ? args[0].as.int_val : 0;
            int64_t end = arg_count == 2 && args[1].type == VAL_INT ? args[1].as.int_val : (int64_t)obj->as.buffer.len;
            if (start < 0) start = 0;
            if (end > (int64_t)obj->as.buffer.len) end = (int64_t)obj->as.buffer.len;
            if (start >= end) {
                *result = value_buffer(NULL, 0);
                return true;
            }
            *result = value_buffer(obj->as.buffer.data + start, (size_t)(end - start));
            return true;
        }
        if (mhash == MHASH_clear && strcmp(method, "clear") == 0 && arg_count == 0) {
            obj->as.buffer.len = 0;
            *result = value_unit();
            return true;
        }
        if (mhash == MHASH_fill && strcmp(method, "fill") == 0 && arg_count == 1) {
            if (args[0].type == VAL_INT) memset(obj->as.buffer.data, (uint8_t)args[0].as.int_val, obj->as.buffer.len);
            *result = value_unit();
            return true;
        }
        if (mhash == MHASH_resize && strcmp(method, "resize") == 0 && arg_count == 1) {
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
        if (mhash == MHASH_to_string && strcmp(method, "to_string") == 0 && arg_count == 0) {
            char *s = malloc(obj->as.buffer.len + 1);
            if (!s) return 0;
            memcpy(s, obj->as.buffer.data, obj->as.buffer.len);
            s[obj->as.buffer.len] = '\0';
            *result = value_string_owned(s);
            return true;
        }
        if (mhash == MHASH_to_array && strcmp(method, "to_array") == 0 && arg_count == 0) {
            LatValue *elems = malloc((obj->as.buffer.len > 0 ? obj->as.buffer.len : 1) * sizeof(LatValue));
            if (!elems) return 0;
            for (size_t i = 0; i < obj->as.buffer.len; i++) elems[i] = value_int((int64_t)obj->as.buffer.data[i]);
            *result = value_array(elems, obj->as.buffer.len);
            free(elems);
            return true;
        }
        if (mhash == MHASH_to_hex && strcmp(method, "to_hex") == 0 && arg_count == 0) {
            char *hex = malloc(obj->as.buffer.len * 2 + 1);
            if (!hex) return 0;
            for (size_t i = 0; i < obj->as.buffer.len; i++) snprintf(hex + i * 2, 3, "%02x", obj->as.buffer.data[i]);
            hex[obj->as.buffer.len * 2] = '\0';
            *result = value_string_owned(hex);
            return true;
        }
    }

    /* ── Ref methods ── */
    if (obj->type == VAL_REF) {
        LatRef *ref = obj->as.ref.ref;
        if (((mhash == MHASH_get && strcmp(method, "get") == 0) ||
             (mhash == MHASH_deref && strcmp(method, "deref") == 0)) &&
            arg_count == 0) {
            *result = value_deep_clone(&ref->value);
            return true;
        }
        if (mhash == MHASH_set && strcmp(method, "set") == 0 && arg_count == 1 && arg_count == 1) {
            if (obj->phase == VTAG_CRYSTAL) {
                *result = value_unit();
                return true;
            }
            value_free(&ref->value);
            ref->value = rvm_clone(&args[0]);
            *result = value_unit();
            return true;
        }
        if (mhash == MHASH_inner_type && strcmp(method, "inner_type") == 0 && arg_count == 0) {
            *result = value_string(value_type_name(&ref->value));
            return true;
        }
        /* Proxy: delegate to inner value's methods if applicable */
        if (ref->value.type == VAL_MAP) {
            if (mhash == MHASH_get && strcmp(method, "get") == 0 && arg_count == 1 && args[0].type == VAL_STR) {
                LatValue *val = lat_map_get(ref->value.as.map.map, args[0].as.str_val);
                *result = val ? value_deep_clone(val) : value_nil();
                return true;
            }
            if (mhash == MHASH_set && strcmp(method, "set") == 0 && arg_count == 2 && args[0].type == VAL_STR) {
                if (obj->phase != VTAG_CRYSTAL) {
                    LatValue cloned = rvm_clone(&args[1]);
                    lat_map_set(ref->value.as.map.map, args[0].as.str_val, &cloned);
                }
                *result = value_unit();
                return true;
            }
            if (((mhash == MHASH_has && strcmp(method, "has") == 0) ||
                 (mhash == MHASH_contains && strcmp(method, "contains") == 0)) &&
                arg_count == 1 && args[0].type == VAL_STR) {
                *result = value_bool(lat_map_get(ref->value.as.map.map, args[0].as.str_val) != NULL);
                return true;
            }
            if (mhash == MHASH_keys && strcmp(method, "keys") == 0 && arg_count == 0) {
                size_t cap = ref->value.as.map.map->cap;
                LatValue *keys = malloc(cap * sizeof(LatValue));
                if (!keys) return 0;
                size_t cnt = 0;
                for (size_t i = 0; i < cap; i++) {
                    if (ref->value.as.map.map->entries[i].state == MAP_OCCUPIED)
                        keys[cnt++] = value_string(ref->value.as.map.map->entries[i].key);
                }
                *result = value_array(keys, cnt);
                free(keys);
                return true;
            }
            if (mhash == MHASH_values && strcmp(method, "values") == 0 && arg_count == 0) {
                size_t cap = ref->value.as.map.map->cap;
                LatValue *vals = malloc(cap * sizeof(LatValue));
                if (!vals) return 0;
                size_t cnt = 0;
                for (size_t i = 0; i < cap; i++) {
                    if (ref->value.as.map.map->entries[i].state == MAP_OCCUPIED)
                        vals[cnt++] = value_deep_clone((LatValue *)ref->value.as.map.map->entries[i].value);
                }
                *result = value_array(vals, cnt);
                free(vals);
                return true;
            }
            if (mhash == MHASH_entries && strcmp(method, "entries") == 0 && arg_count == 0) {
                size_t cap = ref->value.as.map.map->cap;
                LatValue *entries = malloc(cap * sizeof(LatValue));
                if (!entries) return 0;
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
            if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
                 (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
                arg_count == 0) {
                *result = value_int((int64_t)lat_map_len(ref->value.as.map.map));
                return true;
            }
            if (mhash == MHASH_merge && strcmp(method, "merge") == 0 && arg_count == 1 && args[0].type == VAL_MAP) {
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
            if (mhash == MHASH_push && strcmp(method, "push") == 0 && arg_count == 1) {
                LatValue val = rvm_clone(&args[0]);
                if (ref->value.as.array.len >= ref->value.as.array.cap) {
                    ref->value.as.array.cap = ref->value.as.array.cap ? ref->value.as.array.cap * 2 : 4;
                    ref->value.as.array.elems =
                        realloc(ref->value.as.array.elems, ref->value.as.array.cap * sizeof(LatValue));
                }
                ref->value.as.array.elems[ref->value.as.array.len++] = val;
                *result = value_unit();
                return true;
            }
            if (mhash == MHASH_pop && strcmp(method, "pop") == 0 && arg_count == 0) {
                if (ref->value.as.array.len == 0) {
                    *result = value_nil();
                } else {
                    *result = ref->value.as.array.elems[--ref->value.as.array.len];
                }
                return true;
            }
            if (((mhash == MHASH_len && strcmp(method, "len") == 0) ||
                 (mhash == MHASH_length && strcmp(method, "length") == 0)) &&
                arg_count == 0) {
                *result = value_int((int64_t)ref->value.as.array.len);
                return true;
            }
            if (mhash == MHASH_contains && strcmp(method, "contains") == 0 && arg_count == 1) {
                bool found = false;
                for (size_t i = 0; i < ref->value.as.array.len; i++) {
                    if (value_eq(&ref->value.as.array.elems[i], &args[0])) {
                        found = true;
                        break;
                    }
                }
                *result = value_bool(found);
                return true;
            }
        }
    }

    /* ── Channel methods ── */
    if (obj->type == VAL_CHANNEL) {
        if (mhash == MHASH_send && strcmp(method, "send") == 0 && arg_count == 1) {
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
        if (mhash == MHASH_recv && strcmp(method, "recv") == 0 && arg_count == 0) {
            bool ok = false;
            *result = channel_recv(obj->as.channel.ch, &ok);
            if (!ok) *result = value_unit();
            return true;
        }
        if (mhash == MHASH_close && strcmp(method, "close") == 0 && arg_count == 0) {
            channel_close(obj->as.channel.ch);
            *result = value_unit();
            return true;
        }
    }

    /* ── Iterator methods ── */
    if (obj->type == VAL_ITERATOR) {
        if (strcmp(method, "next") == 0 && arg_count == 0) {
            bool done = false;
            LatValue val = obj->as.iterator.next_fn(obj->as.iterator.state, &done);
            *result = done ? (value_free(&val), value_nil()) : val;
            return true;
        }
        if ((strcmp(method, "collect") == 0 || strcmp(method, "to_array") == 0) && arg_count == 0) {
            *result = iter_collect(obj);
            return true;
        }
        if (strcmp(method, "count") == 0 && arg_count == 0) {
            *result = value_int(iter_count(obj));
            return true;
        }
        if (strcmp(method, "take") == 0 && arg_count == 1) {
            if (args[0].type != VAL_INT) {
                vm->error = strdup(".take() expects an integer");
                return false;
            }
            LatValue it = *obj;
            obj->type = VAL_NIL;
            *result = iter_take(it, args[0].as.int_val);
            return true;
        }
        if (strcmp(method, "skip") == 0 && arg_count == 1) {
            if (args[0].type != VAL_INT) {
                vm->error = strdup(".skip() expects an integer");
                return false;
            }
            LatValue it = *obj;
            obj->type = VAL_NIL;
            *result = iter_skip(it, args[0].as.int_val);
            return true;
        }
        if (strcmp(method, "enumerate") == 0 && arg_count == 0) {
            LatValue it = *obj;
            obj->type = VAL_NIL;
            *result = iter_enumerate(it);
            return true;
        }
        if (strcmp(method, "zip") == 0 && arg_count == 1) {
            if (args[0].type != VAL_ITERATOR) {
                vm->error = strdup(".zip() expects an Iterator");
                return false;
            }
            LatValue left = *obj;
            obj->type = VAL_NIL;
            LatValue right = args[0];
            args[0].type = VAL_NIL;
            *result = iter_zip(left, right);
            return true;
        }
        if (strcmp(method, "map") == 0 && arg_count == 1) {
            if (args[0].type != VAL_CLOSURE) {
                vm->error = strdup(".map() expects a closure");
                return false;
            }
            LatValue it = *obj;
            obj->type = VAL_NIL;
            *result = iter_map_transform(it, args[0], vm, regvm_iter_callback);
            return true;
        }
        if (strcmp(method, "filter") == 0 && arg_count == 1) {
            if (args[0].type != VAL_CLOSURE) {
                vm->error = strdup(".filter() expects a closure");
                return false;
            }
            LatValue it = *obj;
            obj->type = VAL_NIL;
            *result = iter_filter(it, args[0], vm, regvm_iter_callback);
            return true;
        }
        if (strcmp(method, "reduce") == 0 && arg_count == 2) {
            if (args[0].type != VAL_CLOSURE) {
                vm->error = strdup(".reduce() expects (closure, initial_value)");
                return false;
            }
            *result = iter_reduce(obj, args[1], &args[0], vm, regvm_iter_callback);
            return true;
        }
        if (strcmp(method, "any") == 0 && arg_count == 1) {
            if (args[0].type != VAL_CLOSURE) {
                vm->error = strdup(".any() expects a closure");
                return false;
            }
            *result = value_bool(iter_any(obj, &args[0], vm, regvm_iter_callback));
            return true;
        }
        if (strcmp(method, "all") == 0 && arg_count == 1) {
            if (args[0].type != VAL_CLOSURE) {
                vm->error = strdup(".all() expects a closure");
                return false;
            }
            *result = value_bool(iter_all(obj, &args[0], vm, regvm_iter_callback));
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
        if (ret.type == VAL_STR && ret.as.str_val && strncmp(ret.as.str_val, "EVAL_ERROR:", 11) == 0) {
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
    if (new_base + REGVM_REG_MAX > REGVM_REG_MAX * REGVM_FRAMES_MAX) return value_nil();

    LatValue *new_regs = &vm->reg_stack[new_base];
    vm->reg_stack_top += REGVM_REG_MAX;
    int mr = fn_chunk->max_reg ? fn_chunk->max_reg : REGVM_REG_MAX;
    for (int i = 0; i < mr; i++) new_regs[i] = value_nil();

    new_regs[0] = value_unit();
    for (int i = 0; i < argc; i++) { new_regs[1 + i] = rvm_clone_or_borrow(&args[i]); }

    ObjUpvalue **upvals = (ObjUpvalue **)closure->as.closure.captured_env;
    size_t uv_count = closure->region_id != (size_t)-1 ? closure->region_id : 0;

    int saved_base = vm->frame_count;
    RegCallFrame *new_frame = &vm->frames[vm->frame_count++];
    new_frame->chunk = fn_chunk;
    new_frame->ip = fn_chunk->code;
    new_frame->regs = new_regs;
    new_frame->reg_count = mr;
    new_frame->upvalues = upvals;
    new_frame->upvalue_count = uv_count;
    new_frame->caller_result_reg = 0;

    LatValue ret;
    RegVMResult res = regvm_dispatch(vm, saved_base, &ret);
    if (res != REGVM_OK) {
        /* Unwind any frames left by the failed dispatch back to saved_base */
        while (vm->frame_count > saved_base) {
            RegCallFrame *uf = &vm->frames[vm->frame_count - 1];
            for (int i = 0; i < REGVM_REG_MAX; i++) value_free_inline(&uf->regs[i]);
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

    LatValue *R = frame->regs; /* Register base pointer */

/* Route runtime errors through exception handlers when possible */
#define RVM_ERROR(...)                                                    \
    do {                                                                  \
        RegVMResult _err = rvm_handle_error(vm, &frame, &R, __VA_ARGS__); \
        if (_err != REGVM_OK) return _err;                                \
        DISPATCH();                                                       \
    } while (0)

#define READ_INSTR() (*frame->ip++)
#define REGS         R

#ifdef VM_USE_COMPUTED_GOTO
    /* Computed goto dispatch table */
    static void *dispatch_table[ROP_COUNT] = {
        [ROP_MOVE] = &&L_MOVE,
        [ROP_LOADK] = &&L_LOADK,
        [ROP_LOADI] = &&L_LOADI,
        [ROP_LOADNIL] = &&L_LOADNIL,
        [ROP_LOADTRUE] = &&L_LOADTRUE,
        [ROP_LOADFALSE] = &&L_LOADFALSE,
        [ROP_LOADUNIT] = &&L_LOADUNIT,
        [ROP_ADD] = &&L_ADD,
        [ROP_SUB] = &&L_SUB,
        [ROP_MUL] = &&L_MUL,
        [ROP_DIV] = &&L_DIV,
        [ROP_MOD] = &&L_MOD,
        [ROP_NEG] = &&L_NEG,
        [ROP_ADDI] = &&L_ADDI,
        [ROP_CONCAT] = &&L_CONCAT,
        [ROP_EQ] = &&L_EQ,
        [ROP_NEQ] = &&L_NEQ,
        [ROP_LT] = &&L_LT,
        [ROP_LTEQ] = &&L_LTEQ,
        [ROP_GT] = &&L_GT,
        [ROP_GTEQ] = &&L_GTEQ,
        [ROP_NOT] = &&L_NOT,
        [ROP_JMP] = &&L_JMP,
        [ROP_JMPFALSE] = &&L_JMPFALSE,
        [ROP_JMPTRUE] = &&L_JMPTRUE,
        [ROP_GETGLOBAL] = &&L_GETGLOBAL,
        [ROP_SETGLOBAL] = &&L_SETGLOBAL,
        [ROP_DEFINEGLOBAL] = &&L_DEFINEGLOBAL,
        [ROP_GETFIELD] = &&L_GETFIELD,
        [ROP_SETFIELD] = &&L_SETFIELD,
        [ROP_GETINDEX] = &&L_GETINDEX,
        [ROP_SETINDEX] = &&L_SETINDEX,
        [ROP_GETUPVALUE] = &&L_GETUPVALUE,
        [ROP_SETUPVALUE] = &&L_SETUPVALUE,
        [ROP_CLOSEUPVALUE] = &&L_CLOSEUPVALUE,
        [ROP_CALL] = &&L_CALL,
        [ROP_RETURN] = &&L_RETURN,
        [ROP_CLOSURE] = &&L_CLOSURE,
        [ROP_NEWARRAY] = &&L_NEWARRAY,
        [ROP_NEWSTRUCT] = &&L_NEWSTRUCT,
        [ROP_BUILDRANGE] = &&L_BUILDRANGE,
        [ROP_LEN] = &&L_LEN,
        [ROP_PRINT] = &&L_PRINT,
        [ROP_INVOKE] = &&L_INVOKE,
        [ROP_FREEZE] = &&L_FREEZE,
        [ROP_THAW] = &&L_THAW,
        [ROP_CLONE] = &&L_CLONE,
        [ROP_ITERINIT] = &&L_ITERINIT,
        [ROP_ITERNEXT] = &&L_ITERNEXT,
        [ROP_MARKFLUID] = &&L_MARKFLUID,
        /* Bitwise */
        [ROP_BIT_AND] = &&L_BIT_AND,
        [ROP_BIT_OR] = &&L_BIT_OR,
        [ROP_BIT_XOR] = &&L_BIT_XOR,
        [ROP_BIT_NOT] = &&L_BIT_NOT,
        [ROP_LSHIFT] = &&L_LSHIFT,
        [ROP_RSHIFT] = &&L_RSHIFT,
        /* Tuple */
        [ROP_NEWTUPLE] = &&L_NEWTUPLE,
        /* Spread/Flatten */
        [ROP_ARRAY_FLATTEN] = &&L_ARRAY_FLATTEN,
        /* Enum */
        [ROP_NEWENUM] = &&L_NEWENUM,
        /* Optional chaining */
        [ROP_JMPNOTNIL] = &&L_JMPNOTNIL,
        /* Exception handling */
        [ROP_PUSH_HANDLER] = &&L_PUSH_HANDLER,
        [ROP_POP_HANDLER] = &&L_POP_HANDLER,
        [ROP_THROW] = &&L_THROW,
        [ROP_TRY_UNWRAP] = &&L_TRY_UNWRAP,
        /* Defer */
        [ROP_DEFER_PUSH] = &&L_DEFER_PUSH,
        [ROP_DEFER_RUN] = &&L_DEFER_RUN,
        /* Variadic */
        [ROP_COLLECT_VARARGS] = &&L_COLLECT_VARARGS,
        /* Advanced phase */
        [ROP_FREEZE_VAR] = &&L_FREEZE_VAR,
        [ROP_THAW_VAR] = &&L_THAW_VAR,
        [ROP_SUBLIMATE_VAR] = &&L_SUBLIMATE_VAR,
        [ROP_SUBLIMATE] = &&L_SUBLIMATE,
        [ROP_REACT] = &&L_REACT,
        [ROP_UNREACT] = &&L_UNREACT,
        [ROP_BOND] = &&L_BOND,
        [ROP_UNBOND] = &&L_UNBOND,
        [ROP_SEED] = &&L_SEED,
        [ROP_UNSEED] = &&L_UNSEED,
        /* Module/Import */
        [ROP_IMPORT] = &&L_IMPORT,
        /* Concurrency */
        [ROP_SCOPE] = &&L_SCOPE,
        [ROP_SELECT] = &&L_SELECT,
        /* Ephemeral arena */
        [ROP_RESET_EPHEMERAL] = &&L_RESET_EPHEMERAL,
        /* Optimization */
        [ROP_ADD_INT] = &&L_ADD_INT,
        [ROP_SUB_INT] = &&L_SUB_INT,
        [ROP_MUL_INT] = &&L_MUL_INT,
        [ROP_LT_INT] = &&L_LT_INT,
        [ROP_LTEQ_INT] = &&L_LTEQ_INT,
        [ROP_INC_REG] = &&L_INC_REG,
        [ROP_DEC_REG] = &&L_DEC_REG,
        [ROP_SETINDEX_LOCAL] = &&L_SETINDEX_LOCAL,
        [ROP_INVOKE_GLOBAL] = &&L_INVOKE_GLOBAL,
        [ROP_INVOKE_LOCAL] = &&L_INVOKE_LOCAL,
        /* Phase query */
        [ROP_IS_CRYSTAL] = &&L_IS_CRYSTAL,
        [ROP_IS_FLUID] = &&L_IS_FLUID,
        /* Type checking */
        [ROP_CHECK_TYPE] = &&L_CHECK_TYPE,
        [ROP_FREEZE_FIELD] = &&L_FREEZE_FIELD,
        [ROP_THAW_FIELD] = &&L_THAW_FIELD,
        [ROP_FREEZE_EXCEPT] = &&L_FREEZE_EXCEPT,
        /* Require */
        [ROP_REQUIRE] = &&L_REQUIRE,
        /* Slice assignment */
        [ROP_SETSLICE] = &&L_SETSLICE,
        [ROP_SETSLICE_LOCAL] = &&L_SETSLICE_LOCAL,
        /* Misc */
        [ROP_HALT] = &&L_HALT,
    };

#define DISPATCH()                            \
    do {                                      \
        RegInstr _i = READ_INSTR();           \
        goto *dispatch_table[REG_GET_OP(_i)]; \
    } while (0)

    /* We need the instruction available after goto. Use a local. */
#undef DISPATCH
#define DISPATCH()                               \
    do {                                         \
        instr = READ_INSTR();                    \
        goto *dispatch_table[REG_GET_OP(instr)]; \
    } while (0)

    RegInstr instr;
    DISPATCH();

#define CASE(label) L_##label:

#else
    /* Switch-based dispatch */
    for (;;) {
        RegInstr instr = READ_INSTR();
        switch (REG_GET_OP(instr)) {

#define CASE(label) case ROP_##label:
#define DISPATCH()  continue

#endif

    CASE(MOVE) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        /* Fast path: primitives (int/float/bool/nil/unit/range) and borrowed
         * strings (REGION_CONST/INTERNED) can be bitwise-copied between registers.
         * Primitives have no heap data; borrowed strings are owned by the
         * constant pool or intern table.  rvm_clone_or_borrow handles both. */
        reg_set(&R[a], rvm_clone_or_borrow(&R[b]));
        /* Record history for tracked variables */
        {
            if (vm->rt->tracking_active && frame->chunk->local_names && a < frame->chunk->local_name_cap &&
                frame->chunk->local_names[a] && frame->chunk->local_names[a][0]) {
                rt_record_history(vm->rt, frame->chunk->local_names[a], &R[a]);
            }
        }
        DISPATCH();
    }

    CASE(LOADK) {
        uint8_t a = REG_GET_A(instr);
        uint16_t bx = REG_GET_Bx(instr);
        LatValue kv = frame->chunk->constants[bx];
        /* Fast path: primitives (int/float/bool/etc) are bitwise-copied directly.
         * String constants: intern the string for pointer-equality comparisons
         * and zero-cost clones (REGION_INTERNED is never freed or strdup'd).
         * rvm_clone already skips strdup for REGION_INTERNED. */
        if (RVM_IS_PRIMITIVE(kv)) {
            reg_set(&R[a], kv);
        } else if (kv.type == VAL_STR) {
            reg_set(&R[a], value_string_interned(kv.as.str_val));
        } else {
            reg_set(&R[a], rvm_clone(&kv));
        }
        /* Record history for tracked variables */
        {
            if (vm->rt->tracking_active && frame->chunk->local_names && a < frame->chunk->local_name_cap &&
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
            if (vm->rt->tracking_active && frame->chunk->local_names && a < frame->chunk->local_name_cap &&
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
            if (vm->rt->tracking_active && frame->chunk->local_names && a < frame->chunk->local_name_cap &&
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
            if (vm->rt->tracking_active && frame->chunk->local_names && a < frame->chunk->local_name_cap &&
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
            /* Use cached str_len when available to avoid O(n) strlen */
            size_t lb = R[b].as.str_len ? R[b].as.str_len : strlen(R[b].as.str_val);
            size_t lc = R[c].as.str_len ? R[c].as.str_len : strlen(R[c].as.str_val);
            size_t total = lb + lc;
            /* Optimization: when dest == left operand (s = s + x pattern) and
             * the left string is a plain malloc'd buffer, realloc in-place to
             * avoid copying the entire left side + an extra free. */
            if (a == b && R[b].region_id == REGION_NONE && b != c) {
                char *buf = realloc(R[b].as.str_val, total + 1);
                memcpy(buf + lb, R[c].as.str_val, lc);
                buf[total] = '\0';
                R[a].as.str_val = buf;   /* update in-place (realloc may move) */
                R[a].as.str_len = total; /* update cached length */
                /* Intern short results for pointer-equality comparisons */
                if (total <= INTERN_THRESHOLD) {
                    const char *interned = intern(buf);
                    free(R[a].as.str_val);
                    R[a].as.str_val = (char *)interned;
                    R[a].region_id = REGION_INTERNED;
                    R[a].as.str_len = total;
                }
            } else {
                char *buf = malloc(total + 1);
                if (!buf) return REGVM_RUNTIME_ERROR;
                memcpy(buf, R[b].as.str_val, lb);
                memcpy(buf + lb, R[c].as.str_val, lc);
                buf[total] = '\0';
                LatValue v = value_string_owned(buf);
                v.as.str_len = total; /* cache length */
                /* Intern short concat results */
                if (total <= INTERN_THRESHOLD) {
                    const char *interned = intern(buf);
                    free(v.as.str_val);
                    v.as.str_val = (char *)interned;
                    v.region_id = REGION_INTERNED;
                    v.as.str_len = total;
                }
                reg_set(&R[a], v);
            }
        } else {
            RVM_ERROR("cannot add %s and %s", value_type_name(&R[b]), value_type_name(&R[c]));
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
            RVM_ERROR("cannot subtract %s from %s", value_type_name(&R[c]), value_type_name(&R[b]));
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
            RVM_ERROR("cannot multiply %s and %s", value_type_name(&R[b]), value_type_name(&R[c]));
        }
        DISPATCH();
    }

    CASE(DIV) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type == VAL_INT && R[c].type == VAL_INT) {
            if (R[c].as.int_val == 0) RVM_ERROR("division by zero");
            reg_set(&R[a], value_int(R[b].as.int_val / R[c].as.int_val));
        } else if (R[b].type == VAL_FLOAT || R[c].type == VAL_FLOAT) {
            double rv = R[c].type == VAL_FLOAT ? R[c].as.float_val : (double)R[c].as.int_val;
            double lv = R[b].type == VAL_FLOAT ? R[b].as.float_val : (double)R[b].as.int_val;
            reg_set(&R[a], value_float(lv / rv)); /* float div by zero → Inf/NaN */
        } else {
            RVM_ERROR("cannot divide %s by %s", value_type_name(&R[b]), value_type_name(&R[c]));
        }
        DISPATCH();
    }

    CASE(MOD) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type == VAL_INT && R[c].type == VAL_INT) {
            if (R[c].as.int_val == 0) RVM_ERROR("modulo by zero");
            reg_set(&R[a], value_int(R[b].as.int_val % R[c].as.int_val));
        } else {
            RVM_ERROR("cannot modulo %s by %s", value_type_name(&R[b]), value_type_name(&R[c]));
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
        int8_t c = (int8_t)REG_GET_C(instr);
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
        /* Fast path: skip value_display when operands are already strings */
        char *ls = (R[b].type == VAL_STR) ? NULL : value_display(&R[b]);
        char *rs = (R[c].type == VAL_STR) ? NULL : value_display(&R[c]);
        const char *lp = ls ? ls : R[b].as.str_val;
        const char *rp = rs ? rs : R[c].as.str_val;
        /* Use cached length when available (only for direct string operands) */
        size_t ll = (!ls && R[b].as.str_len) ? R[b].as.str_len : strlen(lp);
        size_t rl = (!rs && R[c].as.str_len) ? R[c].as.str_len : strlen(rp);
        char *buf = bump_alloc(vm->ephemeral, ll + rl + 1);
        memcpy(buf, lp, ll);
        memcpy(buf + ll, rp, rl);
        buf[ll + rl] = '\0';
        free(ls);
        free(rs); /* NULL-safe: free(NULL) is a no-op */
        LatValue v = {.type = VAL_STR, .phase = VTAG_UNPHASED, .region_id = REGION_EPHEMERAL};
        v.as.str_val = buf;
        v.as.str_len = ll + rl; /* cache result length */
        reg_set(&R[a], v);
        DISPATCH();
    }

    CASE(EQ) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        bool eq_result;
        /* Custom struct eq() method */
        if (R[b].type == VAL_STRUCT && R[c].type == VAL_STRUCT) {
            const char *eq_intern = intern("eq");
            bool found_eq = false;
            for (size_t i = 0; i < R[b].as.strct.field_count; i++) {
                if (R[b].as.strct.field_names[i] == eq_intern && R[b].as.strct.field_values[i].type == VAL_CLOSURE) {
                    /* Pass self (R[b]) and other (R[c]) as args */
                    LatValue eq_args[2] = {R[b], R[c]};
                    LatValue res = regvm_call_closure(vm, &R[b].as.strct.field_values[i], eq_args, 2);
                    eq_result = value_is_truthy(&res);
                    value_free(&res);
                    found_eq = true;
                    break;
                }
            }
            if (!found_eq) eq_result = value_eq(&R[b], &R[c]);
        } else {
            eq_result = value_eq(&R[b], &R[c]);
        }
        reg_set(&R[a], value_bool(eq_result));
        DISPATCH();
    }

    CASE(NEQ) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        bool eq_result;
        /* Custom struct eq() method */
        if (R[b].type == VAL_STRUCT && R[c].type == VAL_STRUCT) {
            const char *eq_intern = intern("eq");
            bool found_eq = false;
            for (size_t i = 0; i < R[b].as.strct.field_count; i++) {
                if (R[b].as.strct.field_names[i] == eq_intern && R[b].as.strct.field_values[i].type == VAL_CLOSURE) {
                    /* Pass self (R[b]) and other (R[c]) as args */
                    LatValue eq_args[2] = {R[b], R[c]};
                    LatValue res = regvm_call_closure(vm, &R[b].as.strct.field_values[i], eq_args, 2);
                    eq_result = value_is_truthy(&res);
                    value_free(&res);
                    found_eq = true;
                    break;
                }
            }
            if (!found_eq) eq_result = value_eq(&R[b], &R[c]);
        } else {
            eq_result = value_eq(&R[b], &R[c]);
        }
        reg_set(&R[a], value_bool(!eq_result));
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
            RVM_ERROR("cannot compare %s < %s", value_type_name(&R[b]), value_type_name(&R[c]));
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
            RVM_ERROR("cannot compare %s <= %s", value_type_name(&R[b]), value_type_name(&R[c]));
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
            RVM_ERROR("cannot compare %s > %s", value_type_name(&R[b]), value_type_name(&R[c]));
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
            RVM_ERROR("cannot compare %s >= %s", value_type_name(&R[b]), value_type_name(&R[c]));
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
        if (!value_is_truthy(&R[a])) frame->ip += offset;
        DISPATCH();
    }

    CASE(JMPTRUE) {
        uint8_t a = REG_GET_A(instr);
        int16_t offset = REG_GET_sBx(instr);
        if (value_is_truthy(&R[a])) frame->ip += offset;
        DISPATCH();
    }

    CASE(GETGLOBAL) {
        uint8_t a = REG_GET_A(instr);
        uint16_t bx = REG_GET_Bx(instr);
        const char *name = frame->chunk->constants[bx].as.str_val;
        LatValue val;
        if (!env_get(vm->env, name, &val)) {
            const char *sug = env_find_similar_name(vm->env, name);
            if (sug) RVM_ERROR("undefined variable '%s' (did you mean '%s'?)", name, sug);
            else RVM_ERROR("undefined variable '%s'", name);
        }
        /* env_get already returns a value_deep_clone'd copy — no need to
         * clone again.  Directly assign the owned value to the register. */
        reg_set(&R[a], val);
        DISPATCH();
    }

    CASE(SETGLOBAL) {
        uint8_t a = REG_GET_A(instr);
        uint16_t bx = REG_GET_Bx(instr);
        const char *name = frame->chunk->constants[bx].as.str_val;
        if (!env_set(vm->env, name, rvm_clone(&R[a]))) {
            const char *sug = env_find_similar_name(vm->env, name);
            if (sug) RVM_ERROR("undefined variable '%s' (did you mean '%s'?)", name, sug);
            else RVM_ERROR("undefined variable '%s'", name);
        }
        /* Record history for tracked globals */
        {
            if (vm->rt->tracking_active) rt_record_history(vm->rt, name, &R[a]);
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
                                    LatValue elems[2] = {value_deep_clone(&existing), val};
                                    LatValue arr = value_array(elems, 2);
                                    env_define(vm->env, name, arr);
                                    DISPATCH();
                                }
                            }
                        } else if (existing.type == VAL_ARRAY) {
                            size_t new_len = existing.as.array.len + 1;
                            LatValue *new_elems = malloc(new_len * sizeof(LatValue));
                            if (!new_elems) return REGVM_RUNTIME_ERROR;
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
                    /* Fast path: primitive/borrowed fields avoid rvm_clone overhead */
                    reg_set(&R[a], rvm_clone_or_borrow(&R[b].as.strct.field_values[i]));
                    found = true;
                    break;
                }
            }
            if (!found) RVM_ERROR("struct '%s' has no field '%s'", R[b].as.strct.name, field_name);
        } else if (R[b].type == VAL_MAP) {
            LatValue *val = lat_map_get(R[b].as.map.map, field_name);
            if (val) reg_set(&R[a], rvm_clone_or_borrow(val));
            else reg_set(&R[a], value_nil());
        } else if (R[b].type == VAL_TUPLE) {
            char *endp;
            long idx = strtol(field_name, &endp, 10);
            if (*endp == '\0' && idx >= 0 && (size_t)idx < R[b].as.tuple.len)
                reg_set(&R[a], rvm_clone_or_borrow(&R[b].as.tuple.elems[idx]));
            else RVM_ERROR("tuple has no field '%s'", field_name);
        } else if (R[b].type == VAL_ENUM) {
            if (strcmp(field_name, "tag") == 0 || strcmp(field_name, "variant_name") == 0)
                reg_set(&R[a], value_string(R[b].as.enm.variant_name));
            else if (strcmp(field_name, "enum_name") == 0) reg_set(&R[a], value_string(R[b].as.enm.enum_name));
            else if (strcmp(field_name, "payload") == 0) {
                if (R[b].as.enm.payload_count > 0) {
                    LatValue *elems = malloc(R[b].as.enm.payload_count * sizeof(LatValue));
                    if (!elems) return REGVM_RUNTIME_ERROR;
                    for (size_t pi = 0; pi < R[b].as.enm.payload_count; pi++)
                        elems[pi] = rvm_clone(&R[b].as.enm.payload[pi]);
                    reg_set(&R[a], value_array(elems, R[b].as.enm.payload_count));
                    free(elems);
                } else {
                    reg_set(&R[a], value_array(NULL, 0));
                }
            } else RVM_ERROR("enum has no field '%s'", field_name);
        } else {
            RVM_ERROR("cannot access field '%s' on %s", field_name, value_type_name(&R[b]));
        }
        DISPATCH();
    }

    CASE(SETFIELD) {
        uint8_t a = REG_GET_A(instr); /* object reg */
        uint8_t b = REG_GET_B(instr); /* field name constant */
        uint8_t c = REG_GET_C(instr); /* value reg */
        const char *field_name = frame->chunk->constants[b].as.str_val;

        /* Phase checks */
        if (R[a].phase == VTAG_CRYSTAL || R[a].phase == VTAG_SUBLIMATED) {
            /* Check per-field phases for structs with partial freeze (freeze except) */
            bool blocked = true;
            if (R[a].type == VAL_STRUCT && R[a].as.strct.field_phases && R[a].phase == VTAG_CRYSTAL) {
                for (size_t i = 0; i < R[a].as.strct.field_count; i++) {
                    if (strcmp(R[a].as.strct.field_names[i], field_name) == 0) {
                        if (R[a].as.strct.field_phases[i] != VTAG_CRYSTAL) blocked = false;
                        break;
                    }
                }
            }
            if (blocked) {
                const char *phase_name = R[a].phase == VTAG_CRYSTAL ? "frozen" : "sublimated";
                RVM_ERROR("cannot set field '%s' on a %s value", field_name, phase_name);
            }
        }
        /* Also check per-field phases (alloy types) even on non-frozen structs */
        if (R[a].type == VAL_STRUCT && R[a].as.strct.field_phases && R[a].phase != VTAG_CRYSTAL) {
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
                if (!elems) return REGVM_RUNTIME_ERROR;
                for (size_t i = 0; i < slice_len; i++) elems[i] = rvm_clone(&R[b].as.array.elems[start + (int64_t)i]);
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
                if (!slice) return REGVM_RUNTIME_ERROR;
                memcpy(slice, R[b].as.str_val + start, slice_len);
                slice[slice_len] = '\0';
                reg_set(&R[a], value_string_owned(slice));
            }
        } else if (R[b].type == VAL_ARRAY) {
            if (R[c].type != VAL_INT) RVM_ERROR("array index must be an integer");
            int64_t idx = R[c].as.int_val;
            if (idx < 0) idx += (int64_t)R[b].as.array.len;
            if (idx < 0 || (size_t)idx >= R[b].as.array.len)
                RVM_ERROR("array index %lld out of bounds (len %zu)", (long long)R[c].as.int_val, R[b].as.array.len);
            /* Fast path: primitive elements (int/float/bool/nil/unit/range)
             * are bitwise-copied without rvm_clone.  No heap data to manage.
             * This is the hot path for sieve/bubble_sort array reads. */
            reg_set(&R[a], rvm_clone_or_borrow(&R[b].as.array.elems[idx]));
        } else if (R[b].type == VAL_MAP) {
            if (R[c].type != VAL_STR) RVM_ERROR("map key must be a string");
            LatValue *val = lat_map_get(R[b].as.map.map, R[c].as.str_val);
            if (val) reg_set(&R[a], rvm_clone_or_borrow(val));
            else reg_set(&R[a], value_nil());
        } else if (R[b].type == VAL_STR) {
            if (R[c].type != VAL_INT) RVM_ERROR("string index must be an integer");
            int64_t idx = R[c].as.int_val;
            size_t len = strlen(R[b].as.str_val);
            if (idx < 0) idx += (int64_t)len;
            if (idx < 0 || (size_t)idx >= len) RVM_ERROR("string index out of bounds");
            char buf[2] = {R[b].as.str_val[idx], '\0'};
            reg_set(&R[a], value_string(buf));
        } else if (R[b].type == VAL_BUFFER) {
            if (R[c].type != VAL_INT) RVM_ERROR("buffer index must be an integer");
            int64_t idx = R[c].as.int_val;
            if (idx < 0 || (size_t)idx >= R[b].as.buffer.len) RVM_ERROR("buffer index out of bounds");
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
                if (idx < 0 || (size_t)idx >= ref->value.as.array.len) RVM_ERROR("array index out of bounds");
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
        uint8_t a = REG_GET_A(instr); /* object */
        uint8_t b = REG_GET_B(instr); /* index */
        uint8_t c = REG_GET_C(instr); /* value */

        /* Phase checks for mutation */
        if (R[a].phase == VTAG_CRYSTAL) {
            /* Allow mutation on maps with per-key phases (freeze except) if key is not frozen */
            bool blocked = true;
            if (R[a].type == VAL_MAP && R[b].type == VAL_STR && R[a].as.map.key_phases) {
                PhaseTag *kp = lat_map_get(R[a].as.map.key_phases, R[b].as.str_val);
                if (!kp || *kp != VTAG_CRYSTAL) blocked = false;
            }
            if (blocked) RVM_ERROR("cannot modify a frozen value");
        }
        if (R[a].phase == VTAG_SUBLIMATED) RVM_ERROR("cannot modify a sublimated value");
        /* Per-key phase check for non-frozen maps */
        if (R[a].type == VAL_MAP && R[b].type == VAL_STR && R[a].as.map.key_phases) {
            PhaseTag *kp = lat_map_get(R[a].as.map.key_phases, R[b].as.str_val);
            if (kp && *kp == VTAG_CRYSTAL) RVM_ERROR("cannot modify frozen key '%s'", R[b].as.str_val);
        }

        if (R[a].type == VAL_ARRAY) {
            if (R[b].type != VAL_INT) RVM_ERROR("array index must be an integer");
            int64_t idx = R[b].as.int_val;
            if (idx < 0) idx += (int64_t)R[a].as.array.len;
            if (idx < 0 || (size_t)idx >= R[a].as.array.len) RVM_ERROR("array index out of bounds");
            /* Fast path: when both old element and new value are primitives,
             * skip value_free (no-op for primitives) and rvm_clone (bitwise copy).
             * This is the hot path for sieve `flags[j] = 0` and bubble_sort swaps. */
            if (RVM_IS_PRIMITIVE(R[a].as.array.elems[idx]) && RVM_IS_PRIMITIVE(R[c])) {
                R[a].as.array.elems[idx] = R[c];
            } else {
                value_free(&R[a].as.array.elems[idx]);
                R[a].as.array.elems[idx] = rvm_clone(&R[c]);
            }
        } else if (R[a].type == VAL_MAP) {
            if (R[b].type != VAL_STR) RVM_ERROR("map key must be a string");
            LatValue cloned = rvm_clone(&R[c]);
            lat_map_set(R[a].as.map.map, R[b].as.str_val, &cloned);
        } else if (R[a].type == VAL_BUFFER) {
            if (R[b].type != VAL_INT) RVM_ERROR("buffer index must be an integer");
            int64_t idx = R[b].as.int_val;
            if (idx < 0 || (size_t)idx >= R[a].as.buffer.len) RVM_ERROR("buffer index out of bounds");
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
                if (idx < 0 || (size_t)idx >= ref->value.as.array.len) RVM_ERROR("array index out of bounds");
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
            /* Fast path: primitives/borrowed strings avoid rvm_clone overhead */
            reg_set(&R[a], rvm_clone_or_borrow(frame->upvalues[b]->location));
        }
        DISPATCH();
    }

    CASE(SETUPVALUE) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        if (frame->upvalues && b < frame->upvalue_count) {
            /* Upvalue locations must own their values (survive frame pops),
             * so we need real clones for heap types.  Primitives are safe
             * to bitwise-copy. */
            value_free(frame->upvalues[b]->location);
            *frame->upvalues[b]->location = rvm_clone_or_borrow(&R[a]);
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
        uint8_t a = REG_GET_A(instr); /* func register */
        uint8_t b = REG_GET_B(instr); /* arg count */
        uint8_t c = REG_GET_C(instr); /* return count (1 for now) */
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
                LatValue matched = value_deep_clone(&func->as.array.elems[best_idx]);
                reg_set(func, matched);
            } else {
                RVM_ERROR("no matching overload for given argument phases");
            }
        }

        if (func->type != VAL_CLOSURE) RVM_ERROR("attempt to call a non-function (%s)", value_type_name(func));

        /* Check for native function */
        if (func->as.closure.default_values == VM_NATIVE_MARKER) {
            VMNativeFn native = (VMNativeFn)func->as.closure.native_fn;

            /* Sync named locals to env only when reactive primitives are in use
             * (track, react, bond, pressurize, etc.). Skipping the sync for
             * regular natives like print/to_string avoids deep-cloning all locals
             * on every call — critical for performance with large maps/arrays. */
            if (vm->rt && (vm->rt->reaction_count > 0 || vm->rt->pressure_count > 0)) {
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
            /* Collect args — use rvm_clone_or_borrow for fast primitives.
             * Native functions receive owned copies; value_free after call
             * handles cleanup.  Primitives have no heap data so bitwise
             * copy + no-op free is safe. */
            LatValue args[16];
            for (int i = 0; i < b; i++) args[i] = rvm_clone_or_borrow(&R[a + 1 + i]);
            LatValue ret = native(args, b);
            for (int i = 0; i < b; i++) value_free(&args[i]);
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
            for (int i = 0; i < b; i++) args[i] = rvm_clone_or_borrow(&R[a + 1 + i]);
            LatValue ret = ext_call_native(func->as.closure.native_fn, args, (size_t)b);
            for (int i = 0; i < b; i++) value_free(&args[i]);
            /* Extension errors return strings prefixed with "EVAL_ERROR:" */
            if (ret.type == VAL_STR && ret.as.str_val && strncmp(ret.as.str_val, "EVAL_ERROR:", 11) == 0) {
                char *msg = strdup(ret.as.str_val + 11);
                value_free(&ret);
                RVM_ERROR("%s", msg);
            }
            reg_set(&R[a], ret);
            DISPATCH();
        }

        /* Compiled function call */
        RegChunk *fn_chunk = (RegChunk *)func->as.closure.native_fn;
        if (!fn_chunk) RVM_ERROR("attempt to call a closure with NULL chunk");

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

        if (vm->frame_count >= REGVM_FRAMES_MAX) RVM_ERROR("call stack overflow");

        /* Allocate new register window */
        size_t new_base = vm->reg_stack_top;
        if (new_base + REGVM_REG_MAX > REGVM_REG_MAX * REGVM_FRAMES_MAX) RVM_ERROR("register stack overflow");

        LatValue *new_regs = &vm->reg_stack[new_base];
        vm->reg_stack_top += REGVM_REG_MAX;

        /* Initialize registers to nil (bounded by max_reg) */
        int mr = fn_chunk->max_reg ? fn_chunk->max_reg : REGVM_REG_MAX;
        for (int i = 0; i < mr; i++) new_regs[i] = value_nil();

        /* Copy arguments: R[0] = reserved, R[1..n] = args.
         * Use rvm_clone_or_borrow for fast primitive/borrowed-string passthrough.
         * Hot path for fib_recursive passing ints. */
        new_regs[0] = value_unit(); /* Reserved slot */
        for (int i = 0; i < b; i++) { new_regs[1 + i] = rvm_clone_or_borrow(&R[a + 1 + i]); }

        /* Set up upvalues */
        ObjUpvalue **upvals = (ObjUpvalue **)func->as.closure.captured_env;
        size_t uv_count = func->region_id != (size_t)-1 ? func->region_id : 0;

        /* Push new frame */
        RegCallFrame *new_frame = &vm->frames[vm->frame_count++];
        new_frame->chunk = fn_chunk;
        new_frame->ip = fn_chunk->code;
        new_frame->regs = new_regs;
        new_frame->reg_count = mr;
        new_frame->upvalues = upvals;
        new_frame->upvalue_count = uv_count;
        new_frame->caller_result_reg = a; /* RETURN puts result here */
        frame = new_frame;
        R = new_regs;
        DISPATCH();
    }

    CASE(RETURN) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);

        /* Fast path: primitives can be bitwise-copied without rvm_clone.
         * This is the hot path for fib_recursive returning ints.
         * Borrowed strings also safe since the frame cleanup below
         * won't free them (value_free_inline skips non-REGION_NONE). */
        LatValue ret_val = (b > 0) ? rvm_clone_or_borrow(&R[a]) : value_unit();
        uint8_t dest_reg = frame->caller_result_reg;

        /* Close any open upvalues that point into this frame's registers */
        {
            LatValue *frame_base = frame->regs;
            LatValue *frame_end = frame_base + REGVM_REG_MAX;
            ObjUpvalue **prev = &vm->open_upvalues;
            while (*prev) {
                ObjUpvalue *uv = *prev;
                if (uv->location >= frame_base && uv->location < frame_end) {
                    /* Close this upvalue: clone value into uv->closed.
                     * Must use rvm_clone (not bitwise copy) so that
                     * uv->closed owns independent heap allocations.
                     * Otherwise frame cleanup frees shared pointers
                     * (e.g. closure param_names), leaving uv->closed
                     * with dangling references (heap-use-after-free). */
                    uv->closed = rvm_clone(uv->location);
                    *uv->location = value_nil(); /* prevent double-free */
                    uv->location = &uv->closed;
                    *prev = uv->next;
                } else {
                    prev = &uv->next;
                }
            }
        }

        /* Clean up current frame's registers (bounded by reg_count) */
        for (int i = 0; i < (int)frame->reg_count; i++) value_free_inline(&frame->regs[i]);

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
        /* RegVM bytecode closures never own param_names.  The prototype
         * in the constant pool owns them (freed by regchunk_free).
         * Keeping param_names NULL on all runtime closures eliminates an
         * entire class of UAF / double-free bugs. Trade-off: closures
         * display as <closure||> instead of <closure|param_name|>. */
        closure.as.closure.param_names = NULL;
        closure.as.closure.default_values = NULL;
        closure.as.closure.has_variadic = fn_proto.as.closure.has_variadic;
        closure.as.closure.captured_env = NULL;

        /* Process upvalue descriptors that follow the CLOSURE instruction */
        /* Each upvalue descriptor is encoded as a MOVE instruction:
         * A=1 means local, A=0 means upvalue; B=index */
        size_t uv_count = fn_proto.region_id; /* upvalue count stored by compiler */
        ObjUpvalue **upvals = NULL;

        if (uv_count > 0) {
            upvals = malloc(uv_count * sizeof(ObjUpvalue *));
            if (!upvals) return REGVM_RUNTIME_ERROR;
            for (size_t i = 0; i < uv_count; i++) {
                RegInstr desc = READ_INSTR();
                uint8_t is_local = REG_GET_A(desc);
                uint8_t index = REG_GET_B(desc);

                if (is_local) {
                    /* Capture from current frame's register — deduplicate
                     * so multiple closures sharing the same local share one
                     * ObjUpvalue (mirrors stack VM's capture_upvalue). */
                    LatValue *target = &R[index];
                    ObjUpvalue *existing = NULL;
                    for (ObjUpvalue *p = vm->open_upvalues; p; p = p->next) {
                        if (p->location == target) {
                            existing = p;
                            break;
                        }
                    }
                    if (existing) {
                        upvals[i] = existing;
                    } else {
                        ObjUpvalue *uv = malloc(sizeof(ObjUpvalue));
                        if (!uv) return REGVM_RUNTIME_ERROR;
                        uv->location = target;
                        uv->closed = value_nil();
                        uv->next = vm->open_upvalues;
                        vm->open_upvalues = uv;
                        upvals[i] = uv;
                    }
                } else {
                    /* Capture from enclosing upvalue */
                    if (frame->upvalues && index < frame->upvalue_count) upvals[i] = frame->upvalues[index];
                    else upvals[i] = NULL;
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
        uint8_t b = REG_GET_B(instr); /* base register */
        uint8_t c = REG_GET_C(instr); /* count */

        if (c == 0) {
            reg_set(&R[a], value_array(NULL, 0));
        } else {
            LatValue *elems = malloc(c * sizeof(LatValue));
            if (!elems) return REGVM_RUNTIME_ERROR;
            for (int i = 0; i < c; i++) elems[i] = rvm_clone(&R[b + i]);
            reg_set(&R[a], value_array(elems, c));
            free(elems);
        }
        DISPATCH();
    }

    CASE(NEWSTRUCT) {
        uint8_t a = REG_GET_A(instr);
        /* b is unused; name constant comes from follow-up LOADK instruction */
        uint8_t c = REG_GET_C(instr); /* field count */

        /* Read the follow-up LOADK instruction to get the full constant index */
        RegInstr name_instr = READ_INSTR();
        uint16_t name_ki = REG_GET_Bx(name_instr);
        const char *struct_name = frame->chunk->constants[name_ki].as.str_val;

        /* Look up field names from struct metadata */
        char meta_name[256];
        snprintf(meta_name, sizeof(meta_name), "__struct_%s", struct_name);
        LatValue meta;
        if (!env_get(vm->env, meta_name, &meta)) { RVM_ERROR("unknown struct '%s'", struct_name); }

        if (meta.type != VAL_ARRAY || (int)meta.as.array.len != c) {
            RVM_ERROR("struct '%s' field count mismatch", struct_name);
        }

        /* Build field names array */
        char **field_names = malloc(c * sizeof(char *));
        if (!field_names) return REGVM_RUNTIME_ERROR;
        LatValue *field_values = malloc(c * sizeof(LatValue));
        if (!field_values) return REGVM_RUNTIME_ERROR;
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
        for (int i = 0; i < c; i++) { field_values[i] = rvm_clone(&R[field_base + i]); }

        LatValue strct = value_struct(struct_name, field_names, field_values, c);
        for (int i = 0; i < c; i++) free(field_names[i]);
        free(field_names);
        free(field_values);

        /* Alloy enforcement: apply per-field phase from struct declaration */
        {
            char phase_key[256];
            snprintf(phase_key, sizeof(phase_key), "__struct_phases_%s", struct_name);
            LatValue *phase_ref = env_get_ref(vm->env, phase_key);
            if (phase_ref && phase_ref->type == VAL_ARRAY && (int)phase_ref->as.array.len == c) {
                strct.as.strct.field_phases = calloc(c, sizeof(PhaseTag));
                for (int i = 0; strct.as.strct.field_phases && i < c; i++) {
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
        if (R[b].type != VAL_INT || R[c].type != VAL_INT) RVM_ERROR("range bounds must be integers");
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
        uint8_t b = REG_GET_B(instr); /* count */
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
        if (rvm_invoke_builtin(vm, &R[obj_reg], method_name, invoke_args, argc, &invoke_result, NULL)) {
            if (vm->error) return REGVM_RUNTIME_ERROR;
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
                    LatValue ret = ext_call_native(field->as.closure.native_fn, call_args, (size_t)argc);
                    if (ret.type == VAL_STR && ret.as.str_val && strncmp(ret.as.str_val, "EVAL_ERROR:", 11) == 0) {
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
                    if (vm->frame_count >= REGVM_FRAMES_MAX) RVM_ERROR("call stack overflow");

                    size_t new_base = vm->reg_stack_top;
                    LatValue *new_regs = &vm->reg_stack[new_base];
                    vm->reg_stack_top += REGVM_REG_MAX;
                    int mr = fn_chunk->max_reg ? fn_chunk->max_reg : REGVM_REG_MAX;
                    for (int i = 0; i < mr; i++) new_regs[i] = value_nil();

                    /* Slot 0 = reserved, slots 1+ = args (no self for map closures) */
                    new_regs[0] = value_unit();
                    for (int i = 0; i < argc; i++) { new_regs[1 + i] = rvm_clone(&R[args_base + i]); }

                    ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                    size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;

                    RegCallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->chunk = fn_chunk;
                    new_frame->ip = fn_chunk->code;
                    new_frame->regs = new_regs;
                    new_frame->reg_count = mr;
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
                    if (vm->frame_count >= REGVM_FRAMES_MAX) RVM_ERROR("call stack overflow");

                    size_t new_base = vm->reg_stack_top;
                    LatValue *new_regs = &vm->reg_stack[new_base];
                    vm->reg_stack_top += REGVM_REG_MAX;
                    int mr = fn_chunk->max_reg ? fn_chunk->max_reg : REGVM_REG_MAX;
                    for (int i = 0; i < mr; i++) new_regs[i] = value_nil();

                    /* Slot 0 = reserved, slot 1 = self, slots 2+ = args */
                    new_regs[0] = value_unit();
                    new_regs[1] = rvm_clone(&R[obj_reg]); /* self = first param */
                    for (int i = 0; i < argc; i++) { new_regs[2 + i] = rvm_clone(&R[args_base + i]); }

                    ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                    size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;

                    RegCallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->chunk = fn_chunk;
                    new_frame->ip = fn_chunk->code;
                    new_frame->regs = new_regs;
                    new_frame->reg_count = mr;
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

                if (vm->frame_count >= REGVM_FRAMES_MAX) RVM_ERROR("call stack overflow");

                size_t new_base = vm->reg_stack_top;
                LatValue *new_regs = &vm->reg_stack[new_base];
                vm->reg_stack_top += REGVM_REG_MAX;
                int mr = fn_chunk->max_reg ? fn_chunk->max_reg : REGVM_REG_MAX;
                for (int i = 0; i < mr; i++) new_regs[i] = value_nil();

                /* ITEM_IMPL compiles self at slot 0, other params at slot 1+ */
                new_regs[0] = rvm_clone(&R[obj_reg]); /* self */
                for (int i = 0; i < argc; i++) { new_regs[1 + i] = rvm_clone(&R[args_base + i]); }

                ObjUpvalue **upvals = (ObjUpvalue **)impl_fn.as.closure.captured_env;
                size_t uv_count = impl_fn.region_id != (size_t)-1 ? impl_fn.region_id : 0;

                RegCallFrame *new_frame = &vm->frames[vm->frame_count++];
                new_frame->chunk = fn_chunk;
                new_frame->ip = fn_chunk->code;
                new_frame->regs = new_regs;
                new_frame->reg_count = mr;
                new_frame->upvalues = upvals;
                new_frame->upvalue_count = uv_count;
                new_frame->caller_result_reg = dst;
                frame = new_frame;
                R = new_regs;
                DISPATCH();
            }
        }

    invoke_fail: {
        const char *msug = builtin_find_similar_method(R[obj_reg].type, method_name);
        if (msug)
            RVM_ERROR("no method '%s' on %s (did you mean '%s'?)", method_name, value_type_name(&R[obj_reg]), msug);
        else RVM_ERROR("no method '%s' on %s", method_name, value_type_name(&R[obj_reg]));
    }
    }

    CASE(FREEZE) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        if (R[b].type == VAL_CHANNEL) RVM_ERROR("cannot freeze a channel");
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
        if (R[b].type == VAL_ITERATOR) {
            /* Collect the iterator into an array for uniform index-based iteration */
            LatValue collected = iter_collect(&R[b]);
            reg_set(&R[a], collected);
            DISPATCH();
        }
        if (R[b].type == VAL_MAP) {
            /* Convert map to array of [key, value] pairs for uniform iteration */
            LatMap *m = R[b].as.map.map;
            size_t cap = m->cap;
            size_t count = lat_map_len(m);
            LatValue *entries = malloc((count > 0 ? count : 1) * sizeof(LatValue));
            if (!entries) return REGVM_RUNTIME_ERROR;
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
            if (!elems) return REGVM_RUNTIME_ERROR;
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
            if (!chars) return REGVM_RUNTIME_ERROR;
            for (size_t i = 0; i < len; i++) {
                char ch[2] = {R[b].as.str_val[i], '\0'};
                chars[i] = value_string(ch);
            }
            reg_set(&R[a], value_array(chars, len));
            free(chars);
        } else {
            if (a != b) reg_set(&R[a], rvm_clone(&R[b]));
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
                /* Done — range iteration produces only primitives (nil/int),
                 * so skip reg_set overhead and assign directly. */
                R[a].type = VAL_NIL;
                R[a].region_id = REGION_NONE;
            } else {
                R[a].type = VAL_INT;
                R[a].as.int_val = current_val;
                R[a].region_id = REGION_NONE;
            }
        } else if (R[b].type == VAL_ARRAY) {
            int64_t idx = R[c].as.int_val;
            if ((size_t)idx >= R[b].as.array.len) {
                reg_set(&R[a], value_nil());
            } else {
                /* Fast path: primitive/borrowed array elements avoid rvm_clone */
                reg_set(&R[a], rvm_clone_or_borrow(&R[b].as.array.elems[idx]));
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
        if (R[b].type != VAL_INT || R[c].type != VAL_INT) RVM_ERROR("bitwise AND requires integers");
        reg_set(&R[a], value_int(R[b].as.int_val & R[c].as.int_val));
        DISPATCH();
    }

    CASE(BIT_OR) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type != VAL_INT || R[c].type != VAL_INT) RVM_ERROR("bitwise OR requires integers");
        reg_set(&R[a], value_int(R[b].as.int_val | R[c].as.int_val));
        DISPATCH();
    }

    CASE(BIT_XOR) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type != VAL_INT || R[c].type != VAL_INT) RVM_ERROR("bitwise XOR requires integers");
        reg_set(&R[a], value_int(R[b].as.int_val ^ R[c].as.int_val));
        DISPATCH();
    }

    CASE(BIT_NOT) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        if (R[b].type != VAL_INT) RVM_ERROR("bitwise NOT requires integer");
        reg_set(&R[a], value_int(~R[b].as.int_val));
        DISPATCH();
    }

    CASE(LSHIFT) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type != VAL_INT || R[c].type != VAL_INT) RVM_ERROR("left shift requires integers");
        if (R[c].as.int_val < 0 || R[c].as.int_val > 63) RVM_ERROR("shift amount out of range (0..63)");
        reg_set(&R[a], value_int(R[b].as.int_val << R[c].as.int_val));
        DISPATCH();
    }

    CASE(RSHIFT) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);
        if (R[b].type != VAL_INT || R[c].type != VAL_INT) RVM_ERROR("right shift requires integers");
        reg_set(&R[a], value_int(R[b].as.int_val >> R[c].as.int_val));
        DISPATCH();
    }

    /* ── Tuple ── */

    CASE(NEWTUPLE) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr); /* count */
        LatValue *elems = c > 0 ? malloc(c * sizeof(LatValue)) : NULL;
        for (int i = 0; i < c; i++) elems[i] = rvm_clone(&R[b + i]);
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
        if (!elems) return REGVM_RUNTIME_ERROR;
        size_t out = 0;
        for (size_t i = 0; i < R[b].as.array.len; i++) {
            if (R[b].as.array.elems[i].type == VAL_ARRAY) {
                LatValue *inner = &R[b].as.array.elems[i];
                for (size_t j = 0; j < inner->as.array.len; j++) {
                    if (out >= cap) {
                        cap *= 2;
                        elems = realloc(elems, cap * sizeof(LatValue));
                    }
                    elems[out++] = rvm_clone(&inner->as.array.elems[j]);
                }
            } else {
                if (out >= cap) {
                    cap *= 2;
                    elems = realloc(elems, cap * sizeof(LatValue));
                }
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
            if (!payload) return REGVM_RUNTIME_ERROR;
            for (int i = 0; i < argc; i++) payload[i] = rvm_clone(&R[base + i]);
            reg_set(&R[dst], value_enum(enum_name, variant_name, payload, argc));
            free(payload);
        }
        DISPATCH();
    }

    /* ── Optional chaining ── */

    CASE(JMPNOTNIL) {
        uint8_t a = REG_GET_A(instr);
        int16_t offset = REG_GET_sBx(instr);
        if (R[a].type != VAL_NIL) frame->ip += offset;
        DISPATCH();
    }

    /* ── Exception handling ── */

    CASE(PUSH_HANDLER) {
        uint8_t a = REG_GET_A(instr);
        int16_t offset = REG_GET_sBx(instr);
        if (vm->handler_count >= REGVM_HANDLER_MAX) RVM_ERROR("exception handler stack overflow");
        RegHandler *h = &vm->handlers[vm->handler_count++];
        h->ip = frame->ip + offset;
        h->chunk = frame->chunk;
        h->frame_index = (size_t)(vm->frame_count - 1);
        h->reg_stack_top = vm->reg_stack_top;
        h->error_reg = a;
        DISPATCH();
    }

    CASE(POP_HANDLER) {
        if (vm->handler_count > 0) vm->handler_count--;
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
                lat_asprintf(&vm->error, "unhandled exception: %s", repr);
                free(repr);
            }
            value_free(&thrown);
            return REGVM_RUNTIME_ERROR;
        }

        /* Build structured error map before unwinding */
        const char *msg = (thrown.type == VAL_STR) ? thrown.as.str_val : NULL;
        char *repr = msg ? NULL : value_repr(&thrown);
        LatValue err_map = regvm_build_error_map(vm, msg ? msg : repr);
        free(repr);
        value_free(&thrown);

        /* Unwind to handler */
        RegHandler h = vm->handlers[--vm->handler_count];

        /* Clean up frames between current and handler frame */
        while (vm->frame_count - 1 > (int)h.frame_index) {
            RegCallFrame *f = &vm->frames[vm->frame_count - 1];
            for (int i = 0; i < REGVM_REG_MAX; i++) value_free_inline(&f->regs[i]);
            vm->frame_count--;
            vm->reg_stack_top -= REGVM_REG_MAX;
        }

        frame = &vm->frames[vm->frame_count - 1];
        R = frame->regs;
        frame->ip = h.ip;

        reg_set(&R[h.error_reg], err_map);
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

                    for (int i = 0; i < REGVM_REG_MAX; i++) value_free_inline(&frame->regs[i]);
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
                LatValue unwrapped = R[a].as.enm.payload_count > 0 ? rvm_clone(&R[a].as.enm.payload[0]) : value_nil();
                reg_set(&R[a], unwrapped);
                DISPATCH();
            } else if (strcmp(R[a].as.enm.variant_name, "Err") == 0) {
                LatValue err_val = rvm_clone(&R[a]);
                uint8_t dest_reg = frame->caller_result_reg;
                for (int i = 0; i < REGVM_REG_MAX; i++) value_free_inline(&frame->regs[i]);
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
        if (vm->defer_count >= REGVM_DEFER_MAX) RVM_ERROR("defer stack overflow");
        RegDefer *d = &vm->defers[vm->defer_count++];
        d->ip = frame->ip; /* Points to start of defer body */
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
            for (int i = 0; i < REGVM_REG_MAX; i++) new_regs[i] = rvm_clone(&R[i]);

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
                for (int i = 0; i < REGVM_REG_MAX; i++) value_free_inline(&defer_frame->regs[i]);
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
        uint8_t a = REG_GET_A(instr); /* destination register */
        uint8_t b = REG_GET_B(instr); /* start position (declared_arity + 1) */
        /* Collect excess args into an array */
        size_t cap = 8;
        LatValue *elems = malloc(cap * sizeof(LatValue));
        if (!elems) return REGVM_RUNTIME_ERROR;
        size_t count = 0;
        for (int i = b; i < REGVM_REG_MAX; i++) {
            if (R[i].type == VAL_NIL || R[i].type == VAL_UNIT) break;
            if (count >= cap) {
                cap *= 2;
                elems = realloc(elems, cap * sizeof(LatValue));
            }
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
            if (R[slot].type == VAL_CHANNEL) RVM_ERROR("cannot freeze a channel");
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
                if (vm->rt->error) {
                    vm->error = vm->rt->error;
                    vm->rt->error = NULL;
                    return REGVM_RUNTIME_ERROR;
                }
                if (vm->error) return REGVM_RUNTIME_ERROR;
                rt_fire_reactions(vm->rt, var_name, "crystal");
                if (vm->rt->error) {
                    vm->error = vm->rt->error;
                    vm->rt->error = NULL;
                    return REGVM_RUNTIME_ERROR;
                }
                if (vm->error) return REGVM_RUNTIME_ERROR;
                /* Record history for tracked globals */
                {
                    if (vm->rt->tracking_active) rt_record_history(vm->rt, var_name, &frozen);
                }
            }
            DISPATCH();
        }
        if (target) {
            /* Validate seed contracts */
            char *seed_err = rt_validate_seeds(vm->rt, var_name, target, consume_seeds);
            if (seed_err) { RVM_ERROR("%s", seed_err); }
            LatValue frozen = value_freeze(rvm_clone(target));
            value_free(target);
            *target = frozen;
            /* Sync to env for cascade/reactions (locals aren't in env) */
            if (loc_type != 2) { /* not already global */
                if (!env_set(vm->env, var_name, value_deep_clone(&frozen)))
                    env_define(vm->env, var_name, value_deep_clone(&frozen));
            }
            rt_freeze_cascade(vm->rt, var_name);
            if (vm->rt->error) {
                vm->error = vm->rt->error;
                vm->rt->error = NULL;
                return REGVM_RUNTIME_ERROR;
            }
            if (vm->error) return REGVM_RUNTIME_ERROR;
            rt_fire_reactions(vm->rt, var_name, "crystal");
            if (vm->rt->error) {
                vm->error = vm->rt->error;
                vm->rt->error = NULL;
                return REGVM_RUNTIME_ERROR;
            }
            if (vm->error) return REGVM_RUNTIME_ERROR;
            /* Record history for tracked variables after phase change */
            {
                if (vm->rt->tracking_active) rt_record_history(vm->rt, var_name, target);
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
                    if (vm->rt->tracking_active) rt_record_history(vm->rt, var_name, &thawed);
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
                if (vm->rt->tracking_active) rt_record_history(vm->rt, var_name, target);
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

    CASE(SUBLIMATE) {
        uint8_t a = REG_GET_A(instr);
        R[a].phase = VTAG_SUBLIMATED;
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
            for (size_t j = 0; j < vm->rt->reactions[i].cb_count; j++) value_free(&vm->rt->reactions[i].callbacks[j]);
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
        if (dep_name[0] == '\0') { RVM_ERROR("bond() requires variable names for dependencies"); }
        /* Validate: check variables exist and target is not already frozen */
        {
            /* Find target variable's phase */
            PhaseTag target_phase = VTAG_UNPHASED;
            LatValue tval;
            bool t_env = env_get(vm->env, target_name, &tval);
            if (t_env) {
                target_phase = tval.phase;
                value_free(&tval);
            } else {
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
            if (target_phase == VTAG_CRYSTAL) RVM_ERROR("bond: variable '%s' is already frozen", target_name);

            /* Check dep variable exists */
            LatValue dep_val;
            bool dep_found = env_get(vm->env, dep_name, &dep_val);
            if (dep_found) {
                value_free(&dep_val);
            } else {
                bool found_local = false;
                for (int fi = 0; fi < (int)vm->frame_count; fi++) {
                    RegCallFrame *f = &vm->frames[fi];
                    if (!f->chunk || !f->chunk->local_names) continue;
                    for (size_t r = 0; r < f->chunk->local_name_cap; r++) {
                        if (f->chunk->local_names[r] && strcmp(f->chunk->local_names[r], dep_name) == 0) {
                            found_local = true;
                            break;
                        }
                    }
                    if (found_local) break;
                }
                if (!found_local) RVM_ERROR("bond: undefined variable '%s'", dep_name);
            }
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
                if (vm->rt->bonds[i].dep_strategies) free(vm->rt->bonds[i].dep_strategies[j]);
                vm->rt->bonds[i].deps[j] = vm->rt->bonds[i].deps[vm->rt->bonds[i].dep_count - 1];
                if (vm->rt->bonds[i].dep_strategies)
                    vm->rt->bonds[i].dep_strategies[j] =
                        vm->rt->bonds[i].dep_strategies[vm->rt->bonds[i].dep_count - 1];
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
            if (!file_path) return REGVM_RUNTIME_ERROR;
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
            char *emsg = NULL;
            lat_asprintf(&emsg, "import: cannot find '%s'", file_path);
            free(file_path);
            /* Set error directly without [line N] prefix for import errors */
            vm->error = emsg;
            return REGVM_RUNTIME_ERROR;
        } else {
            free(file_path);
        }

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
        if (!source) RVM_ERROR("import: cannot read '%s'", resolved);

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
            RVM_ERROR("%s", errmsg ? errmsg : "import lex error");
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
            RVM_ERROR("%s", errmsg ? errmsg : "import parse error");
        }

        /* Compile as module */
        char *comp_err = NULL;
        RegChunk *mod_chunk = reg_compile_module(&mod_prog, &comp_err);

        /* Free parse artifacts */
        program_free(&mod_prog);
        for (size_t ti = 0; ti < mod_toks.len; ti++) token_free(lat_vec_get(&mod_toks, ti));
        lat_vec_free(&mod_toks);

        if (!mod_chunk) {
            char *errmsg = NULL;
            lat_asprintf(&errmsg, "import '%s': %s", resolved, comp_err ? comp_err : "compile error");
            free(comp_err);
            RVM_ERROR("%s", errmsg ? errmsg : "import compile error");
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
            if (!module_should_export(name, (const char **)mod_chunk->export_names, mod_chunk->export_count,
                                      mod_chunk->has_exports))
                continue;

            LatValue exported = rvm_clone(val_ptr);
            lat_map_set(module_map.as.map.map, name, &exported);
        }

        env_pop_scope(vm->env);

        /* Cache */
        if (!vm->module_cache) {
            vm->module_cache = malloc(sizeof(LatMap));
            if (!vm->module_cache) return REGVM_RUNTIME_ERROR;
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
            if (!file_path) return REGVM_RUNTIME_ERROR;
            memcpy(file_path, raw_path, plen);
            memcpy(file_path + plen, ".lat", 5);
        }

        /* Resolve to absolute path: try CWD first, then script_dir */
        char resolved[PATH_MAX];
        bool found = (realpath(file_path, resolved) != NULL);
        if (!found && vm->rt->script_dir && file_path[0] != '/') {
            char script_rel[PATH_MAX];
            snprintf(script_rel, sizeof(script_rel), "%s/%s", vm->rt->script_dir, file_path);
            found = (realpath(script_rel, resolved) != NULL);
        }
        if (!found) {
            char *emsg = NULL;
            lat_asprintf(&emsg, "require: cannot find '%s'", raw_path);
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
        if (!source) RVM_ERROR("require: cannot read '%s'", resolved);

        /* Lex */
        Lexer req_lex = lexer_new(source);
        char *lex_err = NULL;
        LatVec req_toks = lexer_tokenize(&req_lex, &lex_err);
        free(source);
        if (lex_err) {
            char *errmsg = NULL;
            lat_asprintf(&errmsg, "require '%s': %s", resolved, lex_err);
            free(lex_err);
            lat_vec_free(&req_toks);
            RVM_ERROR("%s", errmsg ? errmsg : "require lex error");
        }

        /* Parse */
        Parser req_parser = parser_new(&req_toks);
        char *parse_err = NULL;
        Program req_prog = parser_parse(&req_parser, &parse_err);
        if (parse_err) {
            char *errmsg = NULL;
            lat_asprintf(&errmsg, "require '%s': %s", resolved, parse_err);
            free(parse_err);
            program_free(&req_prog);
            for (size_t ti = 0; ti < req_toks.len; ti++) token_free(lat_vec_get(&req_toks, ti));
            lat_vec_free(&req_toks);
            RVM_ERROR("%s", errmsg ? errmsg : "require parse error");
        }

        /* Compile as module (via regcompiler, not stack VM compiler) */
        char *comp_err = NULL;
        RegChunk *req_chunk = reg_compile_module(&req_prog, &comp_err);

        /* Free parse artifacts */
        program_free(&req_prog);
        for (size_t ti = 0; ti < req_toks.len; ti++) token_free(lat_vec_get(&req_toks, ti));
        lat_vec_free(&req_toks);

        if (!req_chunk) {
            char *errmsg = NULL;
            lat_asprintf(&errmsg, "require '%s': %s", resolved, comp_err ? comp_err : "compile error");
            free(comp_err);
            RVM_ERROR("%s", errmsg ? errmsg : "require compile error");
        }

        /* Track chunk */
        regvm_track_chunk(vm, req_chunk);

        /* Mark as loaded (for dedup) before execution */
        if (!vm->module_cache) {
            vm->module_cache = malloc(sizeof(LatMap));
            if (!vm->module_cache) return REGVM_RUNTIME_ERROR;
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

#ifdef __EMSCRIPTEN__
        /* WASM: no pthreads — run spawns sequentially */
        (void)sync_idx;
        reg_set(&R[dst_reg], value_unit());
#else
                /* Export locals to env for sub-chunk access */
                env_push_scope(vm->env);
                for (int fi2 = 0; fi2 < vm->frame_count; fi2++) {
                    RegCallFrame *f2 = &vm->frames[fi2];
                    if (!f2->chunk) continue;
                    for (size_t sl = 0; sl < f2->chunk->local_name_cap; sl++) {
                        if (f2->chunk->local_names[sl])
                            env_define(vm->env, f2->chunk->local_names[sl], rvm_clone(&f2->regs[sl]));
                    }
                }

                if (spawn_count == 0) {
                    /* No spawns — run sync body only */
                    if (sync_idx != 0xFF) {
                        RegChunk *sync_body = (RegChunk *)frame->chunk->constants[sync_idx].as.closure.native_fn;
                        LatValue scope_result = value_unit();
                        if (sync_body) {
                            RegVMResult sr = regvm_run_sub(vm, sync_body, &scope_result);
                            frame = &vm->frames[vm->frame_count - 1];
                            R = frame->regs;
                            if (sr != REGVM_OK) {
                                env_pop_scope(vm->env);
                                RVM_ERROR("%s", vm->error ? vm->error : "scope error");
                            }
                        }
                        env_pop_scope(vm->env);
                        reg_set(&R[dst_reg], scope_result);
                    } else {
                        env_pop_scope(vm->env);
                        reg_set(&R[dst_reg], value_unit());
                    }
                } else {
                    /* Has spawns — run concurrently with pthreads */
                    char *first_error = NULL;

                    /* Run sync body first (non-spawn statements) */
                    if (sync_idx != 0xFF) {
                        RegChunk *sync_body = (RegChunk *)frame->chunk->constants[sync_idx].as.closure.native_fn;
                        if (sync_body) {
                            LatValue ns_result;
                            RegVMResult nsr = regvm_run_sub(vm, sync_body, &ns_result);
                            frame = &vm->frames[vm->frame_count - 1];
                            R = frame->regs;
                            if (nsr != REGVM_OK) {
                                first_error = vm->error ? strdup(vm->error) : strdup("scope stmt error");
                                free(vm->error);
                                vm->error = NULL;
                            } else {
                                value_free(&ns_result);
                            }
                        }
                    }

                    /* Create child VMs for each spawn */
                    RegVMSpawnTask *tasks = calloc(spawn_count, sizeof(RegVMSpawnTask));
                    if (!tasks) return REGVM_RUNTIME_ERROR;
                    for (uint8_t i = 0; i < spawn_count && !first_error; i++) {
                        RegChunk *sp_chunk = (RegChunk *)frame->chunk->constants[spawn_indices[i]].as.closure.native_fn;
                        tasks[i].chunk = sp_chunk;
                        tasks[i].child_vm = regvm_clone_for_thread(vm);
                        regvm_export_locals_to_env(vm, tasks[i].child_vm);
                        tasks[i].error = NULL;
                    }

                    /* Launch all spawn threads */
                    for (uint8_t i = 0; i < spawn_count; i++) {
                        if (!tasks[i].child_vm) continue;
                        pthread_create(&tasks[i].thread, NULL, regvm_spawn_thread_fn, &tasks[i]);
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
                        if (tasks[i].child_vm) regvm_free_child(tasks[i].child_vm);
                    }

                    env_pop_scope(vm->env);
                    free(tasks);

                    if (first_error) {
                        /* rvm_handle_error copies the message via vasprintf, so we can
                         * free first_error afterward. Free old vm->error first. */
                        free(vm->error);
                        vm->error = NULL;
                        RegVMResult _serr = rvm_handle_error(vm, &frame, &R, "%s", first_error);
                        free(first_error);
                        if (_serr != REGVM_OK) return _serr;
                        DISPATCH();
                    }
                    reg_set(&R[dst_reg], value_unit());
                }
#endif
        DISPATCH();
    }

    CASE(SELECT) {
        uint8_t dst_reg = REG_GET_A(instr);
        /* Variable-length: arm_count, per-arm data */
        RegInstr data1 = READ_INSTR();
        uint8_t arm_count = REG_GET_A(data1);

        /* Read all arm descriptors (2 data words per arm) */
        typedef struct {
            uint8_t flags, chan_idx, body_idx, binding_idx;
        } RSelArm;
        RSelArm sel_arms[64];
        for (uint8_t i = 0; i < arm_count && i < 64; i++) {
            RegInstr d1 = READ_INSTR();
            RegInstr d2 = READ_INSTR();
            sel_arms[i].flags = REG_GET_A(d1);
            sel_arms[i].chan_idx = REG_GET_B(d1);
            sel_arms[i].body_idx = REG_GET_C(d1);
            sel_arms[i].binding_idx = REG_GET_A(d2);
        }

#ifdef __EMSCRIPTEN__
        reg_set(&R[dst_reg], value_nil());
        DISPATCH();
#else
                /* Export locals to env for sub-chunk visibility */
                env_push_scope(vm->env);
                for (int fi2 = 0; fi2 < vm->frame_count; fi2++) {
                    RegCallFrame *f2 = &vm->frames[fi2];
                    if (!f2->chunk) continue;
                    for (size_t sl = 0; sl < f2->chunk->local_name_cap; sl++) {
                        if (f2->chunk->local_names[sl])
                            env_define(vm->env, f2->chunk->local_names[sl], rvm_clone(&f2->regs[sl]));
                    }
                }

                /* Find default and timeout arms */
                int default_arm = -1;
                int timeout_arm = -1;
                for (uint8_t i = 0; i < arm_count; i++) {
                    if (sel_arms[i].flags & 0x01) default_arm = (int)i;
                    if (sel_arms[i].flags & 0x02) timeout_arm = (int)i;
                }

                /* Evaluate channel expressions */
                LatChannel **channels = calloc(arm_count, sizeof(LatChannel *));
                if (!channels) return REGVM_RUNTIME_ERROR;
                for (uint8_t i = 0; i < arm_count; i++) {
                    if (sel_arms[i].flags & 0x03) continue; /* skip default/timeout */
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

                /* Evaluate timeout if present */
                long timeout_ms = -1;
                if (timeout_arm >= 0) {
                    RegChunk *to_chunk =
                        (RegChunk *)frame->chunk->constants[sel_arms[timeout_arm].chan_idx].as.closure.native_fn;
                    LatValue to_val;
                    RegVMResult tr = regvm_run_sub(vm, to_chunk, &to_val);
                    frame = &vm->frames[vm->frame_count - 1];
                    R = frame->regs;
                    if (tr != REGVM_OK || to_val.type != VAL_INT) {
                        value_free(&to_val);
                        for (uint8_t i = 0; i < arm_count; i++)
                            if (channels[i]) channel_release(channels[i]);
                        free(channels);
                        env_pop_scope(vm->env);
                        RVM_ERROR("select timeout must be an integer (milliseconds)");
                    }
                    timeout_ms = (long)to_val.as.int_val;
                    value_free(&to_val);
                }

                /* Build shuffled index array for fairness */
                size_t ch_arm_count = 0;
                size_t *sel_indices = malloc(arm_count * sizeof(size_t));
                if (!sel_indices) return REGVM_RUNTIME_ERROR;
                for (uint8_t i = 0; i < arm_count; i++) {
                    if (!(sel_arms[i].flags & 0x03)) sel_indices[ch_arm_count++] = i;
                }
                for (size_t si = ch_arm_count; si > 1; si--) {
                    size_t sj = (size_t)rand() % si;
                    size_t tmp = sel_indices[si - 1];
                    sel_indices[si - 1] = sel_indices[sj];
                    sel_indices[sj] = tmp;
                }

                /* Set up waiter for blocking */
                pthread_mutex_t sel_mutex = PTHREAD_MUTEX_INITIALIZER;
                pthread_cond_t sel_cond = PTHREAD_COND_INITIALIZER;
                LatSelectWaiter sel_waiter = {
                    .mutex = &sel_mutex,
                    .cond = &sel_cond,
                    .next = NULL,
                };

                LatValue select_result = value_unit();
                bool select_found = false;
                bool select_error = false;

                /* Compute deadline for timeout */
                struct timespec sel_deadline;
                if (timeout_ms >= 0) {
                    clock_gettime(CLOCK_REALTIME, &sel_deadline);
                    sel_deadline.tv_sec += timeout_ms / 1000;
                    sel_deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
                    if (sel_deadline.tv_nsec >= 1000000000L) {
                        sel_deadline.tv_sec++;
                        sel_deadline.tv_nsec -= 1000000000L;
                    }
                }

                for (;;) {
                    /* Try non-blocking recv on each channel arm (shuffled order) */
                    bool all_closed = true;
                    for (size_t sk = 0; sk < ch_arm_count; sk++) {
                        size_t si2 = sel_indices[sk];
                        LatChannel *ch = channels[si2];
                        LatValue recv_val;
                        bool closed = false;
                        if (channel_try_recv(ch, &recv_val, &closed)) {
                            /* Got a value — bind in env, run body */
                            env_push_scope(vm->env);
                            if (sel_arms[si2].flags & 0x04) {
                                const char *binding = frame->chunk->constants[sel_arms[si2].binding_idx].as.str_val;
                                if (binding) env_define(vm->env, binding, recv_val);
                                else value_free(&recv_val);
                            } else {
                                value_free(&recv_val);
                            }
                            RegChunk *body_chunk =
                                (RegChunk *)frame->chunk->constants[sel_arms[si2].body_idx].as.closure.native_fn;
                            LatValue arm_result;
                            RegVMResult ar = regvm_run_sub(vm, body_chunk, &arm_result);
                            frame = &vm->frames[vm->frame_count - 1];
                            R = frame->regs;
                            env_pop_scope(vm->env);
                            if (ar == REGVM_OK) {
                                value_free(&select_result);
                                select_result = arm_result;
                            } else {
                                select_error = true;
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
                            RegChunk *def_chunk = (RegChunk *)frame->chunk->constants[sel_arms[default_arm].body_idx]
                                                      .as.closure.native_fn;
                            LatValue def_result;
                            RegVMResult dr = regvm_run_sub(vm, def_chunk, &def_result);
                            if (dr == REGVM_OK) {
                                value_free(&select_result);
                                select_result = def_result;
                            } else {
                                select_error = true;
                            }
                            frame = &vm->frames[vm->frame_count - 1];
                            R = frame->regs;
                            env_pop_scope(vm->env);
                        }
                        break;
                    }

                    /* If there's a default arm, execute it immediately */
                    if (default_arm >= 0) {
                        env_push_scope(vm->env);
                        RegChunk *def_chunk =
                            (RegChunk *)frame->chunk->constants[sel_arms[default_arm].body_idx].as.closure.native_fn;
                        LatValue def_result;
                        RegVMResult dr = regvm_run_sub(vm, def_chunk, &def_result);
                        if (dr == REGVM_OK) {
                            value_free(&select_result);
                            select_result = def_result;
                        } else {
                            select_error = true;
                        }
                        frame = &vm->frames[vm->frame_count - 1];
                        R = frame->regs;
                        env_pop_scope(vm->env);
                        break;
                    }

                    /* Block: register waiter on all channels, then wait */
                    for (size_t sk = 0; sk < ch_arm_count; sk++)
                        channel_add_waiter(channels[sel_indices[sk]], &sel_waiter);

                    pthread_mutex_lock(&sel_mutex);
                    if (timeout_ms >= 0) {
                        int rc = pthread_cond_timedwait(&sel_cond, &sel_mutex, &sel_deadline);
                        if (rc != 0) {
                            /* Timeout expired */
                            pthread_mutex_unlock(&sel_mutex);
                            for (size_t sk = 0; sk < ch_arm_count; sk++)
                                channel_remove_waiter(channels[sel_indices[sk]], &sel_waiter);
                            if (timeout_arm >= 0) {
                                env_push_scope(vm->env);
                                RegChunk *to_body = (RegChunk *)frame->chunk->constants[sel_arms[timeout_arm].body_idx]
                                                        .as.closure.native_fn;
                                LatValue to_result;
                                RegVMResult tor = regvm_run_sub(vm, to_body, &to_result);
                                if (tor == REGVM_OK) {
                                    value_free(&select_result);
                                    select_result = to_result;
                                } else {
                                    select_error = true;
                                }
                                frame = &vm->frames[vm->frame_count - 1];
                                R = frame->regs;
                                env_pop_scope(vm->env);
                            }
                            break;
                        }
                    } else {
                        pthread_cond_wait(&sel_cond, &sel_mutex);
                    }
                    pthread_mutex_unlock(&sel_mutex);

                    /* Remove waiters and retry */
                    for (size_t sk = 0; sk < ch_arm_count; sk++)
                        channel_remove_waiter(channels[sel_indices[sk]], &sel_waiter);
                }

                pthread_mutex_destroy(&sel_mutex);
                pthread_cond_destroy(&sel_cond);
                free(sel_indices);
                for (uint8_t i = 0; i < arm_count; i++)
                    if (channels[i]) channel_release(channels[i]);
                free(channels);
                env_pop_scope(vm->env);

                if (select_error) {
                    value_free(&select_result);
                    char *err_msg = vm->error ? strdup(vm->error) : strdup("select error");
                    free(vm->error);
                    vm->error = NULL;
                    RegVMResult serr = rvm_handle_error(vm, &frame, &R, "%s", err_msg);
                    free(err_msg);
                    if (serr != REGVM_OK) return serr;
                    DISPATCH();
                }

                reg_set(&R[dst_reg], select_result);
#endif /* __EMSCRIPTEN__ */
        DISPATCH();
    }

    /* ── Ephemeral arena ── */

    CASE(RESET_EPHEMERAL) {
        if (vm->ephemeral) bump_arena_reset(vm->ephemeral);
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
            if (blocked) RVM_ERROR("cannot modify a frozen value");
        }
        if (R[a].phase == VTAG_SUBLIMATED) RVM_ERROR("cannot modify a sublimated value");
        if (R[a].type == VAL_MAP && R[b].type == VAL_STR && R[a].as.map.key_phases) {
            PhaseTag *kp = lat_map_get(R[a].as.map.key_phases, R[b].as.str_val);
            if (kp && *kp == VTAG_CRYSTAL) RVM_ERROR("cannot modify frozen key '%s'", R[b].as.str_val);
        }
        if (R[a].type == VAL_ARRAY) {
            if (R[b].type == VAL_INT) {
                int64_t idx = R[b].as.int_val;
                if (idx < 0) idx += (int64_t)R[a].as.array.len;
                if (idx >= 0 && (size_t)idx < R[a].as.array.len) {
                    /* Fast path: primitive-to-primitive array store skips
                     * both value_free and rvm_clone overhead. */
                    if (RVM_IS_PRIMITIVE(R[a].as.array.elems[idx]) && RVM_IS_PRIMITIVE(R[c])) {
                        R[a].as.array.elems[idx] = R[c];
                    } else {
                        value_free(&R[a].as.array.elems[idx]);
                        R[a].as.array.elems[idx] = rvm_clone(&R[c]);
                    }
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
            if (value_is_crystal(&R[a])) RVM_ERROR("cannot mutate a frozen Ref");
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

    CASE(SETSLICE) {
        /* R[A][R[B]..R[B+1]] = R[C] — array slice splice */
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);

        if (R[a].phase == VTAG_CRYSTAL) RVM_ERROR("cannot modify a frozen value");
        if (R[a].phase == VTAG_SUBLIMATED) RVM_ERROR("cannot modify a sublimated value");
        if (R[a].type != VAL_ARRAY) RVM_ERROR("slice assignment target must be an array");
        if (R[c].type != VAL_ARRAY) RVM_ERROR("slice assignment value must be an array");
        if (R[b].type != VAL_INT || R[b + 1].type != VAL_INT) RVM_ERROR("slice bounds must be integers");

        int64_t start = R[b].as.int_val;
        int64_t end = R[b + 1].as.int_val;
        int64_t arr_len = (int64_t)R[a].as.array.len;

        if (start < 0) start = 0;
        if (start > arr_len) start = arr_len;
        if (end < 0) end = 0;
        if (end > arr_len) end = arr_len;
        if (end < start) end = start;

        size_t slice_start = (size_t)start;
        size_t slice_end = (size_t)end;
        size_t old_slice_len = slice_end - slice_start;
        size_t new_slice_len = R[c].as.array.len;
        size_t old_len = R[a].as.array.len;
        size_t new_len = old_len - old_slice_len + new_slice_len;

        for (size_t i = slice_start; i < slice_end; i++) { value_free(&R[a].as.array.elems[i]); }

        if (new_len != old_len) {
            if (new_len > R[a].as.array.cap) {
                size_t new_cap = new_len < 4 ? 4 : new_len * 2;
                R[a].as.array.elems = realloc(R[a].as.array.elems, new_cap * sizeof(LatValue));
                R[a].as.array.cap = new_cap;
            }
            size_t tail_start = slice_end;
            size_t tail_count = old_len - tail_start;
            if (tail_count > 0) {
                memmove(&R[a].as.array.elems[slice_start + new_slice_len], &R[a].as.array.elems[tail_start],
                        tail_count * sizeof(LatValue));
            }
            R[a].as.array.len = new_len;
        }

        for (size_t i = 0; i < new_slice_len; i++) {
            R[a].as.array.elems[slice_start + i] = rvm_clone(&R[c].as.array.elems[i]);
        }
        DISPATCH();
    }

    CASE(SETSLICE_LOCAL) {
        /* R[A][R[B]..R[B+1]] = R[C] — in-place array slice splice */
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        uint8_t c = REG_GET_C(instr);

        if (R[a].phase == VTAG_CRYSTAL) RVM_ERROR("cannot modify a frozen value");
        if (R[a].phase == VTAG_SUBLIMATED) RVM_ERROR("cannot modify a sublimated value");
        if (R[a].type != VAL_ARRAY) RVM_ERROR("slice assignment target must be an array");
        if (R[c].type != VAL_ARRAY) RVM_ERROR("slice assignment value must be an array");
        if (R[b].type != VAL_INT || R[b + 1].type != VAL_INT) RVM_ERROR("slice bounds must be integers");

        int64_t start = R[b].as.int_val;
        int64_t end = R[b + 1].as.int_val;
        int64_t arr_len = (int64_t)R[a].as.array.len;

        if (start < 0) start = 0;
        if (start > arr_len) start = arr_len;
        if (end < 0) end = 0;
        if (end > arr_len) end = arr_len;
        if (end < start) end = start;

        size_t slice_start = (size_t)start;
        size_t slice_end = (size_t)end;
        size_t old_slice_len = slice_end - slice_start;
        size_t new_slice_len = R[c].as.array.len;
        size_t old_len = R[a].as.array.len;
        size_t new_len = old_len - old_slice_len + new_slice_len;

        for (size_t i = slice_start; i < slice_end; i++) { value_free(&R[a].as.array.elems[i]); }

        if (new_len != old_len) {
            if (new_len > R[a].as.array.cap) {
                size_t new_cap = new_len < 4 ? 4 : new_len * 2;
                R[a].as.array.elems = realloc(R[a].as.array.elems, new_cap * sizeof(LatValue));
                R[a].as.array.cap = new_cap;
            }
            size_t tail_start = slice_end;
            size_t tail_count = old_len - tail_start;
            if (tail_count > 0) {
                memmove(&R[a].as.array.elems[slice_start + new_slice_len], &R[a].as.array.elems[tail_start],
                        tail_count * sizeof(LatValue));
            }
            R[a].as.array.len = new_len;
        }

        for (size_t i = 0; i < new_slice_len; i++) {
            R[a].as.array.elems[slice_start + i] = rvm_clone(&R[c].as.array.elems[i]);
        }
        DISPATCH();
    }

    CASE(INVOKE_GLOBAL) {
        /* Two-instruction sequence:
         *   INVOKE_GLOBAL dst, name_ki, argc
         *   data:         method_ki, args_base, 0
         * Mutates the global value in-place (for push/pop/etc). */
        size_t _rgpic_off = (size_t)(frame->ip - frame->chunk->code - 1);
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
            const char *sug = env_find_similar_name(vm->env, global_name);
            if (sug) RVM_ERROR("undefined variable '%s' (did you mean '%s'?)", global_name, sug);
            else RVM_ERROR("undefined variable '%s'", global_name);
            DISPATCH();
        }

        /* ── PIC fast path ── */
        uint8_t _rgobj_type = (uint8_t)obj_ref->type;
        uint32_t _rgmhash = method_hash(method_name);
        PICSlot *_rgpic = pic_slot_for(&frame->chunk->pic, _rgpic_off);
        uint16_t _rgpic_id = _rgpic ? pic_lookup(_rgpic, _rgobj_type, _rgmhash) : 0;
        if (_rgpic_id == PIC_NOT_BUILTIN) goto rvm_invoke_global_not_builtin;

        /* Try builtin — mutates obj_ref in-place */
        {
            LatValue invoke_result;
            LatValue *invoke_args = (argc > 0) ? &R[args_base] : NULL;
            if (rvm_invoke_builtin(vm, obj_ref, method_name, invoke_args, argc, &invoke_result, global_name)) {
                if (vm->error) return REGVM_RUNTIME_ERROR;
                /* Cache builtin hit */
                if (!_rgpic) {
                    pic_table_ensure(&frame->chunk->pic);
                    _rgpic = pic_slot_for(&frame->chunk->pic, _rgpic_off);
                }
                if (_rgpic && _rgpic_id == 0) {
                    uint16_t _rid = rvm_pic_resolve(_rgobj_type, _rgmhash);
                    if (_rid) pic_update(_rgpic, _rgobj_type, _rgmhash, _rid);
                }
                reg_set(&R[dst], invoke_result);
                DISPATCH();
            }
        }
        /* Cache as NOT_BUILTIN */
        if (!_rgpic) {
            pic_table_ensure(&frame->chunk->pic);
            _rgpic = pic_slot_for(&frame->chunk->pic, _rgpic_off);
        }
        if (_rgpic) pic_update(_rgpic, _rgobj_type, _rgmhash, PIC_NOT_BUILTIN);
    rvm_invoke_global_not_builtin:

        /* Check for callable closure field in struct */
        if (obj_ref->type == VAL_STRUCT) {
            for (size_t fi = 0; fi < obj_ref->as.strct.field_count; fi++) {
                if (strcmp(obj_ref->as.strct.field_names[fi], method_name) != 0) continue;
                LatValue *field = &obj_ref->as.strct.field_values[fi];
                if (field->type == VAL_CLOSURE) {
                    /* Copy object into a temp register, then proceed like normal INVOKE */
                    uint8_t tmp = args_base > 0 ? args_base - 1 : dst;
                    reg_set(&R[tmp], rvm_clone(obj_ref));
                    /* Fall through to closure call logic similar to INVOKE */
                    LatValue closure = rvm_clone(field);
                    if (closure.as.closure.body == NULL && closure.as.closure.native_fn != NULL &&
                        closure.as.closure.default_values != VM_NATIVE_MARKER &&
                        closure.as.closure.default_values != VM_EXT_MARKER) {
                        /* Compiled closure */
                        RegChunk *fn_chunk = (RegChunk *)closure.as.closure.native_fn;
                        if (vm->frame_count >= REGVM_FRAMES_MAX) {
                            value_free(&closure);
                            RVM_ERROR("stack overflow");
                            DISPATCH();
                        }
                        /* Save upvalue info BEFORE freeing the closure */
                        ObjUpvalue **upvals = (ObjUpvalue **)closure.as.closure.captured_env;
                        size_t uv_count = closure.region_id != (size_t)-1 ? closure.region_id : 0;

                        LatValue *new_regs = &vm->reg_stack[vm->reg_stack_top];
                        vm->reg_stack_top += REGVM_REG_MAX;
                        int mr = fn_chunk->max_reg ? fn_chunk->max_reg : REGVM_REG_MAX;
                        for (int i = 0; i < mr; i++) new_regs[i] = value_nil();

                        /* Slot 0 = reserved, slot 1 = self, slots 2+ = args */
                        new_regs[0] = value_unit();
                        new_regs[1] = rvm_clone(obj_ref); /* self = first param */
                        value_free(&closure);
                        /* Copy args into param slots */
                        for (int ai = 0; ai < argc && ai + 2 < REGVM_REG_MAX; ai++)
                            new_regs[ai + 2] = rvm_clone(&R[args_base + ai]);

                        RegCallFrame *nf = &vm->frames[vm->frame_count++];
                        nf->chunk = fn_chunk;
                        nf->ip = fn_chunk->code;
                        nf->regs = new_regs;
                        nf->reg_count = mr;
                        nf->caller_result_reg = dst;
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
                    LatValue ret = ext_call_native(field->as.closure.native_fn, call_args, (size_t)argc);
                    if (ret.type == VAL_STR && ret.as.str_val && strncmp(ret.as.str_val, "EVAL_ERROR:", 11) == 0) {
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
                        if (vm->frame_count >= REGVM_FRAMES_MAX) RVM_ERROR("call stack overflow");
                        LatValue *new_regs = &vm->reg_stack[vm->reg_stack_top];
                        vm->reg_stack_top += REGVM_REG_MAX;
                        int mr = fn_chunk->max_reg ? fn_chunk->max_reg : REGVM_REG_MAX;
                        for (int i = 0; i < mr; i++) new_regs[i] = value_nil();
                        new_regs[0] = value_unit();
                        for (int ai = 0; ai < argc; ai++) new_regs[1 + ai] = rvm_clone(&R[args_base + ai]);

                        ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                        size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;

                        RegCallFrame *nf = &vm->frames[vm->frame_count++];
                        nf->chunk = fn_chunk;
                        nf->ip = fn_chunk->code;
                        nf->regs = new_regs;
                        nf->reg_count = mr;
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
            if (rvm_invoke_builtin(vm, &obj_copy, method_name, fb_args, argc, &fb_result, global_name)) {
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

    CASE(INVOKE_LOCAL) {
        /* Two-instruction sequence:
         *   INVOKE_LOCAL dst, local_reg, argc
         *   data:        method_ki, args_base, 0
         * Mutates R[local_reg] in-place (for push/pop/etc). */
        size_t _rpic_off = (size_t)(frame->ip - frame->chunk->code - 1);
        uint8_t dst = REG_GET_A(instr);
        uint8_t loc_reg = REG_GET_B(instr);
        uint8_t argc = REG_GET_C(instr);

        RegInstr data = *frame->ip++;
        uint8_t method_ki = REG_GET_A(data);
        uint8_t args_base = REG_GET_B(data);

        const char *method_name = frame->chunk->constants[method_ki].as.str_val;

        /* ── PIC fast path ── */
        uint8_t _robj_type = (uint8_t)R[loc_reg].type;
        uint32_t _rmhash = method_hash(method_name);
        PICSlot *_rpic = pic_slot_for(&frame->chunk->pic, _rpic_off);
        uint16_t _rpic_id = _rpic ? pic_lookup(_rpic, _robj_type, _rmhash) : 0;
        if (_rpic_id == PIC_NOT_BUILTIN) goto rvm_invoke_local_not_builtin;

        /* Try builtin — mutates R[loc_reg] in-place */
        {
            const char *local_var_name = (frame->chunk->local_names && loc_reg < frame->chunk->local_name_cap)
                                             ? frame->chunk->local_names[loc_reg]
                                             : NULL;
            LatValue invoke_result;
            LatValue *invoke_args = (argc > 0) ? &R[args_base] : NULL;
            if (rvm_invoke_builtin(vm, &R[loc_reg], method_name, invoke_args, argc, &invoke_result, local_var_name)) {
                if (vm->error) return REGVM_RUNTIME_ERROR;
                /* Cache builtin hit */
                if (!_rpic) {
                    pic_table_ensure(&frame->chunk->pic);
                    _rpic = pic_slot_for(&frame->chunk->pic, _rpic_off);
                }
                if (_rpic && _rpic_id == 0) {
                    uint16_t _rid = rvm_pic_resolve(_robj_type, _rmhash);
                    if (_rid) pic_update(_rpic, _robj_type, _rmhash, _rid);
                }
                reg_set(&R[dst], invoke_result);
                DISPATCH();
            }
        }
        /* Cache as NOT_BUILTIN */
        if (!_rpic) {
            pic_table_ensure(&frame->chunk->pic);
            _rpic = pic_slot_for(&frame->chunk->pic, _rpic_off);
        }
        if (_rpic) pic_update(_rpic, _robj_type, _rmhash, PIC_NOT_BUILTIN);
    rvm_invoke_local_not_builtin:

        /* Check for callable closure field in map */
        if (R[loc_reg].type == VAL_MAP) {
            LatValue *field = lat_map_get(R[loc_reg].as.map.map, method_name);
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
                    LatValue ret = ext_call_native(field->as.closure.native_fn, call_args, (size_t)argc);
                    if (ret.type == VAL_STR && ret.as.str_val && strncmp(ret.as.str_val, "EVAL_ERROR:", 11) == 0) {
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
                    if (vm->frame_count >= REGVM_FRAMES_MAX) RVM_ERROR("call stack overflow");

                    size_t new_base = vm->reg_stack_top;
                    LatValue *new_regs = &vm->reg_stack[new_base];
                    vm->reg_stack_top += REGVM_REG_MAX;
                    int mr = fn_chunk->max_reg ? fn_chunk->max_reg : REGVM_REG_MAX;
                    for (int i = 0; i < mr; i++) new_regs[i] = value_nil();

                    /* Slot 0 = reserved, slots 1+ = args (no self for map closures) */
                    new_regs[0] = value_unit();
                    for (int i = 0; i < argc; i++) { new_regs[1 + i] = rvm_clone(&R[args_base + i]); }

                    ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                    size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;

                    RegCallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->chunk = fn_chunk;
                    new_frame->ip = fn_chunk->code;
                    new_frame->regs = new_regs;
                    new_frame->reg_count = mr;
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
        if (R[loc_reg].type == VAL_STRUCT) {
            for (size_t fi = 0; fi < R[loc_reg].as.strct.field_count; fi++) {
                if (strcmp(R[loc_reg].as.strct.field_names[fi], method_name) != 0) continue;
                LatValue *field = &R[loc_reg].as.strct.field_values[fi];
                if (field->type == VAL_CLOSURE && field->as.closure.native_fn) {
                    RegChunk *fn_chunk = (RegChunk *)field->as.closure.native_fn;
                    if (vm->frame_count >= REGVM_FRAMES_MAX) RVM_ERROR("call stack overflow");

                    size_t new_base_s = vm->reg_stack_top;
                    LatValue *new_regs = &vm->reg_stack[new_base_s];
                    vm->reg_stack_top += REGVM_REG_MAX;
                    int mr = fn_chunk->max_reg ? fn_chunk->max_reg : REGVM_REG_MAX;
                    for (int i = 0; i < mr; i++) new_regs[i] = value_nil();

                    /* Slot 0 = reserved, slot 1 = self, slots 2+ = args */
                    new_regs[0] = value_unit();
                    new_regs[1] = rvm_clone(&R[loc_reg]); /* self = first param */
                    for (int i = 0; i < argc; i++) { new_regs[2 + i] = rvm_clone(&R[args_base + i]); }

                    ObjUpvalue **upvals = (ObjUpvalue **)field->as.closure.captured_env;
                    size_t uv_count = field->region_id != (size_t)-1 ? field->region_id : 0;

                    RegCallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->chunk = fn_chunk;
                    new_frame->ip = fn_chunk->code;
                    new_frame->regs = new_regs;
                    new_frame->reg_count = mr;
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
        if (R[loc_reg].type == VAL_STRUCT) {
            char key[256];
            snprintf(key, sizeof(key), "%s::%s", R[loc_reg].as.strct.name, method_name);
            LatValue impl_fn;
            if (env_get(vm->env, key, &impl_fn) && impl_fn.type == VAL_CLOSURE) {
                RegChunk *fn_chunk = (RegChunk *)impl_fn.as.closure.native_fn;
                if (fn_chunk) {
                    if (vm->frame_count >= REGVM_FRAMES_MAX) RVM_ERROR("call stack overflow");

                    size_t new_base_i = vm->reg_stack_top;
                    LatValue *new_regs = &vm->reg_stack[new_base_i];
                    vm->reg_stack_top += REGVM_REG_MAX;
                    int mr = fn_chunk->max_reg ? fn_chunk->max_reg : REGVM_REG_MAX;
                    for (int i = 0; i < mr; i++) new_regs[i] = value_nil();

                    /* ITEM_IMPL compiles self at slot 0, other params at slot 1+ */
                    new_regs[0] = rvm_clone(&R[loc_reg]); /* self */
                    for (int i = 0; i < argc; i++) { new_regs[1 + i] = rvm_clone(&R[args_base + i]); }

                    ObjUpvalue **upvals = (ObjUpvalue **)impl_fn.as.closure.captured_env;
                    size_t uv_count = impl_fn.region_id != (size_t)-1 ? impl_fn.region_id : 0;

                    RegCallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->chunk = fn_chunk;
                    new_frame->ip = fn_chunk->code;
                    new_frame->regs = new_regs;
                    new_frame->reg_count = mr;
                    new_frame->upvalues = upvals;
                    new_frame->upvalue_count = uv_count;
                    new_frame->caller_result_reg = dst;
                    frame = new_frame;
                    R = new_regs;
                    DISPATCH();
                }
            }
        }

        {
            const char *lmsug = builtin_find_similar_method(R[loc_reg].type, method_name);
            if (lmsug)
                RVM_ERROR("no method '%s' on %s (did you mean '%s'?)", method_name, value_type_name(&R[loc_reg]),
                          lmsug);
            else RVM_ERROR("no method '%s' on %s", method_name, value_type_name(&R[loc_reg]));
        }
    }

    CASE(IS_CRYSTAL) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        reg_set(&R[a], value_bool(R[b].phase == VTAG_CRYSTAL));
        DISPATCH();
    }

    CASE(IS_FLUID) {
        uint8_t a = REG_GET_A(instr);
        uint8_t b = REG_GET_B(instr);
        reg_set(&R[a], value_bool(R[b].phase == VTAG_FLUID));
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
        if (!expected || strcmp(expected, "Any") == 0 || strcmp(expected, "any") == 0) {
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
            if (R[a].type == VAL_STRUCT && R[a].as.strct.name) display = R[a].as.strct.name;
            else if (R[a].type == VAL_ENUM && R[a].as.enm.enum_name) display = R[a].as.enm.enum_name;
            else if (R[a].type == VAL_CLOSURE) display = "Fn";
            else display = value_type_name(&R[a]);
            /* Only suggest if the type name is NOT a known built-in */
            const char *tsug = lat_is_known_type(expected) ? NULL : lat_find_similar_type(expected, NULL, NULL);
            if (err_word != 0xFFFFFFFF) {
                const char *fmt = frame->chunk->constants[err_word].as.str_val;
                if (tsug) {
                    char *base = NULL;
                    lat_asprintf(&base, fmt, display);
                    RVM_ERROR("%s (did you mean '%s'?)", base, tsug);
                    free(base);
                } else {
                    RVM_ERROR(fmt, display);
                }
            } else {
                if (tsug) {
                    RVM_ERROR("return type expects %s, got %s (did you mean '%s'?)", expected, display, tsug);
                } else {
                    RVM_ERROR("return type expects %s, got %s", expected, display);
                }
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
                if (strcmp(R[a].as.strct.field_names[i], field_name) == 0) {
                    fi = i;
                    break;
                }
            }
            if (fi == (size_t)-1) RVM_ERROR("struct has no field '%s'", field_name);
            R[a].as.strct.field_values[fi] = value_freeze(R[a].as.strct.field_values[fi]);
            if (!R[a].as.strct.field_phases) {
                R[a].as.strct.field_phases = calloc(R[a].as.strct.field_count, sizeof(PhaseTag));
                if (!R[a].as.strct.field_phases) RVM_ERROR("out of memory");
            }
            R[a].as.strct.field_phases[fi] = VTAG_CRYSTAL;
        } else if (R[a].type == VAL_MAP) {
            LatValue *val_ptr = (LatValue *)lat_map_get(R[a].as.map.map, field_name);
            if (val_ptr) *val_ptr = value_freeze(*val_ptr);
            if (!R[a].as.map.key_phases) {
                R[a].as.map.key_phases = calloc(1, sizeof(LatMap));
                if (!R[a].as.map.key_phases) RVM_ERROR("out of memory");
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
                if (!R[a].as.strct.field_phases) RVM_ERROR("out of memory");
                for (size_t i = 0; i < R[a].as.strct.field_count; i++) R[a].as.strct.field_phases[i] = R[a].phase;
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
                if (!R[a].as.map.key_phases) RVM_ERROR("out of memory");
                *R[a].as.map.key_phases = lat_map_new(sizeof(PhaseTag));
            }
            PhaseTag phase = VTAG_FLUID;
            lat_map_set(R[a].as.map.key_phases, field_name, &phase);
        }
        DISPATCH();
    }

    CASE(FREEZE_EXCEPT) {
        /* Two-instruction sequence:
         *   FREEZE_EXCEPT name_ki, loc_type, slot
         *   data:         except_base, except_count, 0
         * Freezes all fields of a struct/map EXCEPT the named fields. */
        uint8_t name_ki = REG_GET_A(instr);
        uint8_t loc_type = REG_GET_B(instr);
        uint8_t slot = REG_GET_C(instr);

        RegInstr data = *frame->ip++;
        uint8_t except_base = REG_GET_A(data);
        uint8_t except_count = REG_GET_B(data);

        const char *var_name = frame->chunk->constants[name_ki].as.str_val;

        /* Get a working copy of the variable value */
        LatValue val;
        switch (loc_type) {
            case 0: val = value_deep_clone(&R[slot]); break;
            case 1:
                if (frame->upvalues && slot < frame->upvalue_count && frame->upvalues[slot])
                    val = value_deep_clone(frame->upvalues[slot]->location);
                else val = value_nil();
                break;
            default: {
                LatValue tmp;
                if (!env_get(vm->env, var_name, &tmp)) tmp = value_nil();
                val = tmp;
                break;
            }
        }

        /* Collect except field names from registers */
        if (val.type == VAL_STRUCT) {
            if (!val.as.strct.field_phases) {
                val.as.strct.field_phases = calloc(val.as.strct.field_count, sizeof(PhaseTag));
                if (!val.as.strct.field_phases) RVM_ERROR("out of memory");
                for (size_t fi = 0; fi < val.as.strct.field_count; fi++) val.as.strct.field_phases[fi] = val.phase;
            }
            for (size_t fi = 0; fi < val.as.strct.field_count; fi++) {
                bool exempted = false;
                for (uint8_t j = 0; j < except_count; j++) {
                    if (R[except_base + j].type == VAL_STR &&
                        strcmp(val.as.strct.field_names[fi], R[except_base + j].as.str_val) == 0) {
                        exempted = true;
                        break;
                    }
                }
                if (!exempted) {
                    val.as.strct.field_values[fi] = value_freeze(val.as.strct.field_values[fi]);
                    val.as.strct.field_phases[fi] = VTAG_CRYSTAL;
                } else {
                    val.as.strct.field_phases[fi] = VTAG_FLUID;
                }
            }
        } else if (val.type == VAL_MAP) {
            if (!val.as.map.key_phases) {
                val.as.map.key_phases = calloc(1, sizeof(LatMap));
                if (!val.as.map.key_phases) RVM_ERROR("out of memory");
                *val.as.map.key_phases = lat_map_new(sizeof(PhaseTag));
            }
            for (size_t bi = 0; bi < val.as.map.map->cap; bi++) {
                if (val.as.map.map->entries[bi].state != MAP_OCCUPIED) continue;
                const char *key = val.as.map.map->entries[bi].key;
                bool exempted = false;
                for (uint8_t j = 0; j < except_count; j++) {
                    if (R[except_base + j].type == VAL_STR && strcmp(key, R[except_base + j].as.str_val) == 0) {
                        exempted = true;
                        break;
                    }
                }
                PhaseTag phase;
                if (!exempted) {
                    LatValue *vp = (LatValue *)val.as.map.map->entries[bi].value;
                    *vp = value_freeze(*vp);
                    phase = VTAG_CRYSTAL;
                } else {
                    phase = VTAG_FLUID;
                }
                lat_map_set(val.as.map.key_phases, key, &phase);
            }
        }

        /* Write back */
        switch (loc_type) {
            case 0: {
                value_free(&R[slot]);
                R[slot] = val;
                break;
            }
            case 1: {
                if (frame->upvalues && slot < frame->upvalue_count && frame->upvalues[slot]) {
                    value_free(frame->upvalues[slot]->location);
                    *frame->upvalues[slot]->location = val;
                } else {
                    value_free(&val);
                }
                break;
            }
            default: {
                env_set(vm->env, var_name, val);
                break;
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
            default: RVM_ERROR("unknown register opcode %d", REG_GET_OP(instr));
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
        if (offset < f->chunk->lines_len) return f->chunk->lines[offset];
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
                if (!env_set(rvm->env, name, clone)) env_define(rvm->env, name, clone);
                return true;
            }
        }
    }
    /* Fall back to globals */
    if (env_set(rvm->env, name, val)) return true;
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
    frame->reg_count = chunk->max_reg ? chunk->max_reg : REGVM_REG_MAX;
    frame->upvalues = NULL;
    frame->upvalue_count = 0;
    frame->caller_result_reg = 0;
    vm->reg_stack_top += REGVM_REG_MAX;

    /* Initialize registers to nil (not zero — VAL_INT=0, so memset would create VAL_INT(0)) */
    for (int i = 0; i < (int)frame->reg_count; i++) frame->regs[i] = value_nil();

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
