#include "gc.h"
#include "stackvm.h"
#include "env.h"
#include "channel.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Initial GC threshold ──
 * After the first collection, next_gc = object_count * GC_GROWTH_FACTOR */
#define GC_INITIAL_THRESHOLD 256
#define GC_GROWTH_FACTOR     2

/* ── Lifecycle ── */

void gc_init(GC *gc) {
    gc->all_objects = NULL;
    gc->object_count = 0;
    gc->next_gc = GC_INITIAL_THRESHOLD;
    gc->bytes_allocated = 0;
    gc->enabled = false;
    gc->stress = false;
    gc->incremental = false;
    gc->phase = GC_PHASE_IDLE;
    gc->gray_stack = NULL;
    gc->gray_count = 0;
    gc->gray_cap = 0;
    gc->sweep_prev = NULL;
    gc->sweep_cursor = NULL;
    gc->sweep_freed = 0;
    gc->mark_budget = 64;
    gc->sweep_budget = 128;
    gc->roots_rescanned = false;
    gc->total_collected = 0;
    gc->total_cycles = 0;
}

void gc_free(GC *gc) {
    GCObject *obj = gc->all_objects;
    while (obj) {
        GCObject *next = obj->next;
        /* Free the entire block: header + payload */
        free(obj);
        obj = next;
    }
    gc->all_objects = NULL;
    gc->object_count = 0;
    gc->bytes_allocated = 0;
    free(gc->gray_stack);
    gc->gray_stack = NULL;
    gc->gray_count = 0;
    gc->gray_cap = 0;
    gc->phase = GC_PHASE_IDLE;
}

/* ── Allocation ── */

void *gc_alloc(GC *gc, size_t size) {
    GCObject *obj = (GCObject *)malloc(sizeof(GCObject) + size);
    if (!obj) return NULL;

    obj->next = gc->all_objects;
    /* During an active incremental cycle, new objects are born black
     * (marked=true) so the sweep phase won't free them prematurely. */
    obj->marked = (gc->phase != GC_PHASE_IDLE);
    gc->all_objects = obj;
    gc->object_count++;
    gc->bytes_allocated += size;

    /* During incremental sweep: new objects are prepended to all_objects,
     * which inserts them before sweep_cursor.  If sweep_prev still points
     * at &gc->all_objects, freeing sweep_cursor would orphan the new
     * objects.  Fix by advancing sweep_prev past the new object. */
    if (gc->phase == GC_PHASE_SWEEP && gc->sweep_prev == &gc->all_objects) { gc->sweep_prev = &obj->next; }

    /* Return pointer past the header */
    return (void *)(obj + 1);
}

char *gc_strdup(GC *gc, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *ptr = (char *)gc_alloc(gc, len);
    if (!ptr) return NULL;
    memcpy(ptr, s, len);
    return ptr;
}

bool gc_untrack(GC *gc, void *ptr) {
    if (!ptr) return false;
    GCObject *target = (GCObject *)ptr - 1;
    GCObject **prev = &gc->all_objects;
    for (GCObject *obj = gc->all_objects; obj; obj = obj->next) {
        if (obj == target) {
            *prev = obj->next;
            gc->object_count--;
            return true;
        }
        prev = &obj->next;
    }
    return false;
}

/* ── Mark phase helpers ── */

/* Convert a user pointer back to the GCObject header and mark it.
 * Returns true if the pointer was found in the GC list. */
void gc_mark_ptr(GC *gc, void *ptr) {
    if (!ptr) return;
    GCObject *target = (GCObject *)ptr - 1;
    /* Walk the object list to verify this is actually a GC-managed pointer.
     * This is O(n) but correct for an MVP.  A hash set would be faster. */
    for (GCObject *obj = gc->all_objects; obj; obj = obj->next) {
        if (obj == target) {
            obj->marked = true;
            return;
        }
    }
    /* Not a GC-managed pointer — that's fine (could be malloc'd, interned, etc.) */
}

/* Recursively mark a LatValue and everything reachable from it. */
void gc_mark_value(GC *gc, LatValue *val) {
    if (!val) return;

    /* Skip values not in the normal heap — arena-backed, ephemeral, interned, const */
    if (val->region_id != REGION_NONE) {
        /* Exception: compiled bytecode closures repurpose region_id as upvalue count */
        bool is_compiled_closure =
            (val->type == VAL_CLOSURE && val->as.closure.body == NULL && val->as.closure.native_fn != NULL);
        if (!is_compiled_closure) return;
    }

    switch (val->type) {
        case VAL_STR: gc_mark_ptr(gc, val->as.str_val); break;

        case VAL_ARRAY:
            gc_mark_ptr(gc, val->as.array.elems);
            for (size_t i = 0; i < val->as.array.len; i++) gc_mark_value(gc, &val->as.array.elems[i]);
            break;

        case VAL_STRUCT:
            gc_mark_ptr(gc, val->as.strct.name);
            gc_mark_ptr(gc, val->as.strct.field_names);
            /* field_names[i] are interned — skip individual marking */
            gc_mark_ptr(gc, val->as.strct.field_values);
            for (size_t i = 0; i < val->as.strct.field_count; i++) gc_mark_value(gc, &val->as.strct.field_values[i]);
            gc_mark_ptr(gc, val->as.strct.field_phases);
            break;

        case VAL_CLOSURE:
            if (val->as.closure.param_names) {
                gc_mark_ptr(gc, val->as.closure.param_names);
                for (size_t i = 0; i < val->as.closure.param_count; i++)
                    gc_mark_ptr(gc, val->as.closure.param_names[i]);
            }
            /* For tree-walk closures with captured_env, mark env values */
            if (val->as.closure.captured_env && !(val->as.closure.body == NULL && val->as.closure.native_fn != NULL)) {
                /* Tree-walk closure: captured_env is an Env* */
                /* We don't traverse Env here for the bytecode VM path;
                 * bytecode closures store ObjUpvalue** in captured_env. */
            }
            break;

        case VAL_MAP:
            if (val->as.map.map) {
                gc_mark_ptr(gc, val->as.map.map);
                /* Mark map entries array — this is allocated by lat_map internally
                 * via malloc, not through GC, so we can't track individual entries.
                 * But we do mark the values stored in the map. */
                for (size_t i = 0; i < val->as.map.map->cap; i++) {
                    if (val->as.map.map->entries[i].state == MAP_OCCUPIED) {
                        gc_mark_value(gc, (LatValue *)val->as.map.map->entries[i].value);
                    }
                }
            }
            if (val->as.map.key_phases) gc_mark_ptr(gc, val->as.map.key_phases);
            break;

        case VAL_ENUM:
            gc_mark_ptr(gc, val->as.enm.enum_name);
            gc_mark_ptr(gc, val->as.enm.variant_name);
            if (val->as.enm.payload) {
                gc_mark_ptr(gc, val->as.enm.payload);
                for (size_t i = 0; i < val->as.enm.payload_count; i++) gc_mark_value(gc, &val->as.enm.payload[i]);
            }
            break;

        case VAL_SET:
            if (val->as.set.map) {
                gc_mark_ptr(gc, val->as.set.map);
                for (size_t i = 0; i < val->as.set.map->cap; i++) {
                    if (val->as.set.map->entries[i].state == MAP_OCCUPIED) {
                        gc_mark_value(gc, (LatValue *)val->as.set.map->entries[i].value);
                    }
                }
            }
            break;

        case VAL_TUPLE:
            gc_mark_ptr(gc, val->as.tuple.elems);
            for (size_t i = 0; i < val->as.tuple.len; i++) gc_mark_value(gc, &val->as.tuple.elems[i]);
            break;

        case VAL_BUFFER: gc_mark_ptr(gc, val->as.buffer.data); break;

        case VAL_CHANNEL:
            /* Channels are refcounted, not GC-managed */
            break;

        case VAL_REF:
            /* LatRef is malloc'd (not GC-managed), refcounted */
            if (val->as.ref.ref) gc_mark_value(gc, &val->as.ref.ref->value);
            break;

        default:
            /* VAL_INT, VAL_FLOAT, VAL_BOOL, VAL_UNIT, VAL_NIL, VAL_RANGE
             * — no heap allocations */
            break;
    }
}

/* Callback for env_iter_values to mark each global value */
static void gc_mark_env_value(LatValue *val, void *ctx) {
    GC *gc = (GC *)ctx;
    gc_mark_value(gc, val);
}

/* ── Mark all roots from a StackVM ── */

static void gc_mark_roots(GC *gc, StackVM *vm) {
    /* 1. Mark all values on the VM stack */
    for (LatValue *slot = vm->stack; slot < vm->stack_top; slot++) { gc_mark_value(gc, slot); }

    /* 2. Mark globals (the global environment) */
    if (vm->env) { env_iter_values(vm->env, gc_mark_env_value, gc); }

    /* 3. Mark struct metadata environment */
    if (vm->struct_meta) { env_iter_values(vm->struct_meta, gc_mark_env_value, gc); }

    /* 4. Mark open upvalues */
    for (ObjUpvalue *uv = vm->open_upvalues; uv; uv = uv->next) {
        if (uv->location) gc_mark_value(gc, uv->location);
        gc_mark_value(gc, &uv->closed);
    }

    /* 5. Mark upvalues in call frames */
    for (size_t i = 0; i < vm->frame_count; i++) {
        StackCallFrame *f = &vm->frames[i];
        if (f->upvalues) {
            for (size_t j = 0; j < f->upvalue_count; j++) {
                if (f->upvalues[j]) {
                    if (f->upvalues[j]->location) gc_mark_value(gc, f->upvalues[j]->location);
                    gc_mark_value(gc, &f->upvalues[j]->closed);
                }
            }
        }
    }

    /* 6. Mark module cache values */
    for (size_t i = 0; i < vm->module_cache.cap; i++) {
        if (vm->module_cache.entries[i].state == MAP_OCCUPIED) {
            gc_mark_value(gc, (LatValue *)vm->module_cache.entries[i].value);
        }
    }

    /* 7. Mark fast_args (pre-allocated native call buffer) */
    for (int i = 0; i < 16; i++) {
        if (vm->fast_args[i].type != VAL_NIL && vm->fast_args[i].type != VAL_UNIT) gc_mark_value(gc, &vm->fast_args[i]);
    }
}

/* ── Sweep phase ── */

static size_t gc_sweep(GC *gc) {
    size_t freed = 0;
    GCObject **prev = &gc->all_objects;
    GCObject *obj = gc->all_objects;

    while (obj) {
        if (!obj->marked) {
            /* Unreachable — free it */
            GCObject *unreached = obj;
            *prev = obj->next;
            obj = obj->next;
            free(unreached);
            gc->object_count--;
            freed++;
        } else {
            /* Reachable — clear mark for next cycle and advance */
            obj->marked = false;
            prev = &obj->next;
            obj = obj->next;
        }
    }

    return freed;
}

/* ── Incremental GC helpers ── */

/* Push a LatValue* onto the gray worklist, skipping non-heap/primitive values.
 * Leaf heap types (strings, buffers) are marked directly without pushing. */
static void gc_gray_push_value(GC *gc, LatValue *val) {
    if (!val) return;

    /* Skip values not in the normal heap (arena, ephemeral, interned, const) */
    if (val->region_id != REGION_NONE) {
        /* Exception: compiled bytecode closures repurpose region_id as upvalue count */
        bool is_compiled_closure =
            (val->type == VAL_CLOSURE && val->as.closure.body == NULL && val->as.closure.native_fn != NULL);
        if (!is_compiled_closure) return;
    }

    switch (val->type) {
        /* Primitives: no heap allocations, skip */
        case VAL_INT:
        case VAL_FLOAT:
        case VAL_BOOL:
        case VAL_UNIT:
        case VAL_NIL:
        case VAL_RANGE: return;
        /* Leaf heap types: mark directly, no children to trace */
        case VAL_STR: gc_mark_ptr(gc, val->as.str_val); return;
        case VAL_BUFFER: gc_mark_ptr(gc, val->as.buffer.data); return;
        case VAL_CHANNEL:
            return; /* refcounted, not GC-managed */
        /* Compound types: push to gray stack for child tracing */
        default: break;
    }

    /* Grow gray stack if needed */
    if (gc->gray_count >= gc->gray_cap) {
        gc->gray_cap = gc->gray_cap ? gc->gray_cap * 2 : 128;
        gc->gray_stack = realloc(gc->gray_stack, gc->gray_cap * sizeof(LatValue *));
    }
    gc->gray_stack[gc->gray_count++] = val;
}

/* Callback for env_iter_values that pushes into the gray stack */
static void gc_gray_env_value(LatValue *val, void *ctx) {
    GC *gc = (GC *)ctx;
    gc_gray_push_value(gc, val);
}

/* Push all VM roots onto the gray worklist (same root sources as gc_mark_roots). */
static void gc_incremental_mark_roots(GC *gc, StackVM *vm) {
    /* 1. Stack values */
    for (LatValue *slot = vm->stack; slot < vm->stack_top; slot++) gc_gray_push_value(gc, slot);

    /* 2. Globals */
    if (vm->env) env_iter_values(vm->env, gc_gray_env_value, gc);

    /* 3. Struct metadata */
    if (vm->struct_meta) env_iter_values(vm->struct_meta, gc_gray_env_value, gc);

    /* 4. Open upvalues */
    for (ObjUpvalue *uv = vm->open_upvalues; uv; uv = uv->next) {
        if (uv->location) gc_gray_push_value(gc, uv->location);
        gc_gray_push_value(gc, &uv->closed);
    }

    /* 5. Frame upvalues */
    for (size_t i = 0; i < vm->frame_count; i++) {
        StackCallFrame *f = &vm->frames[i];
        if (f->upvalues) {
            for (size_t j = 0; j < f->upvalue_count; j++) {
                if (f->upvalues[j]) {
                    if (f->upvalues[j]->location) gc_gray_push_value(gc, f->upvalues[j]->location);
                    gc_gray_push_value(gc, &f->upvalues[j]->closed);
                }
            }
        }
    }

    /* 6. Module cache */
    for (size_t i = 0; i < vm->module_cache.cap; i++) {
        if (vm->module_cache.entries[i].state == MAP_OCCUPIED)
            gc_gray_push_value(gc, (LatValue *)vm->module_cache.entries[i].value);
    }

    /* 7. fast_args */
    for (int i = 0; i < 16; i++) {
        if (vm->fast_args[i].type != VAL_NIL && vm->fast_args[i].type != VAL_UNIT)
            gc_gray_push_value(gc, &vm->fast_args[i]);
    }
}

/* Pop one gray value and trace its children (non-recursive gc_mark_value). */
static void gc_trace_one(GC *gc) {
    if (gc->gray_count == 0) return;
    LatValue *val = gc->gray_stack[--gc->gray_count];

    switch (val->type) {
        case VAL_ARRAY:
            gc_mark_ptr(gc, val->as.array.elems);
            for (size_t i = 0; i < val->as.array.len; i++) gc_gray_push_value(gc, &val->as.array.elems[i]);
            break;

        case VAL_STRUCT:
            gc_mark_ptr(gc, val->as.strct.name);
            gc_mark_ptr(gc, val->as.strct.field_names);
            gc_mark_ptr(gc, val->as.strct.field_values);
            for (size_t i = 0; i < val->as.strct.field_count; i++)
                gc_gray_push_value(gc, &val->as.strct.field_values[i]);
            gc_mark_ptr(gc, val->as.strct.field_phases);
            break;

        case VAL_CLOSURE:
            if (val->as.closure.param_names) {
                gc_mark_ptr(gc, val->as.closure.param_names);
                for (size_t i = 0; i < val->as.closure.param_count; i++)
                    gc_mark_ptr(gc, val->as.closure.param_names[i]);
            }
            break;

        case VAL_MAP:
            if (val->as.map.map) {
                gc_mark_ptr(gc, val->as.map.map);
                for (size_t i = 0; i < val->as.map.map->cap; i++) {
                    if (val->as.map.map->entries[i].state == MAP_OCCUPIED)
                        gc_gray_push_value(gc, (LatValue *)val->as.map.map->entries[i].value);
                }
            }
            if (val->as.map.key_phases) gc_mark_ptr(gc, val->as.map.key_phases);
            break;

        case VAL_ENUM:
            gc_mark_ptr(gc, val->as.enm.enum_name);
            gc_mark_ptr(gc, val->as.enm.variant_name);
            if (val->as.enm.payload) {
                gc_mark_ptr(gc, val->as.enm.payload);
                for (size_t i = 0; i < val->as.enm.payload_count; i++) gc_gray_push_value(gc, &val->as.enm.payload[i]);
            }
            break;

        case VAL_SET:
            if (val->as.set.map) {
                gc_mark_ptr(gc, val->as.set.map);
                for (size_t i = 0; i < val->as.set.map->cap; i++) {
                    if (val->as.set.map->entries[i].state == MAP_OCCUPIED)
                        gc_gray_push_value(gc, (LatValue *)val->as.set.map->entries[i].value);
                }
            }
            break;

        case VAL_TUPLE:
            gc_mark_ptr(gc, val->as.tuple.elems);
            for (size_t i = 0; i < val->as.tuple.len; i++) gc_gray_push_value(gc, &val->as.tuple.elems[i]);
            break;

        case VAL_REF:
            if (val->as.ref.ref) gc_gray_push_value(gc, &val->as.ref.ref->value);
            break;

        default: break;
    }
}

/* ── Incremental collection state machine ── */

void gc_incremental_step(GC *gc, void *vm_ptr) {
    if (!gc->enabled) return;
    StackVM *vm = (StackVM *)vm_ptr;

    switch (gc->phase) {
        case GC_PHASE_IDLE:
            /* Check if a cycle should start */
            if (!gc->stress && gc->object_count < gc->next_gc) return;
            /* Start new cycle: clear all marks */
            for (GCObject *obj = gc->all_objects; obj; obj = obj->next) obj->marked = false;
            gc->gray_count = 0;
            gc->roots_rescanned = false;
            gc->phase = GC_PHASE_MARK_ROOTS;
            /* fall through */

        case GC_PHASE_MARK_ROOTS:
            gc_incremental_mark_roots(gc, vm);
            gc->phase = GC_PHASE_MARK_TRACE;
            break;

        case GC_PHASE_MARK_TRACE: {
            size_t budget = gc->mark_budget;
            while (budget > 0 && gc->gray_count > 0) {
                gc_trace_one(gc);
                budget--;
            }
            if (gc->gray_count == 0) {
                if (!gc->roots_rescanned) {
                    /* Re-scan roots to catch any mutations during marking.
                     * No write barriers — this conservative re-scan ensures
                     * correctness by re-discovering all live roots. */
                    gc->roots_rescanned = true;
                    gc_incremental_mark_roots(gc, vm);
                } else {
                    /* Gray stack drained after re-scan: begin sweep */
                    gc->sweep_prev = &gc->all_objects;
                    gc->sweep_cursor = gc->all_objects;
                    gc->sweep_freed = 0;
                    gc->phase = GC_PHASE_SWEEP;
                }
            }
            break;
        }

        case GC_PHASE_SWEEP: {
            size_t budget = gc->sweep_budget;
            while (gc->sweep_cursor && budget > 0) {
                GCObject *obj = gc->sweep_cursor;
                if (!obj->marked) {
                    /* Unreachable — free it */
                    *gc->sweep_prev = obj->next;
                    gc->sweep_cursor = obj->next;
                    free(obj);
                    gc->object_count--;
                    gc->sweep_freed++;
                } else {
                    /* Reachable — clear mark for next cycle */
                    obj->marked = false;
                    gc->sweep_prev = &obj->next;
                    gc->sweep_cursor = obj->next;
                }
                budget--;
            }
            if (!gc->sweep_cursor) {
                /* Sweep complete — update stats and threshold */
                gc->total_collected += gc->sweep_freed;
                gc->total_cycles++;
                gc->next_gc = gc->object_count * GC_GROWTH_FACTOR;
                if (gc->next_gc < GC_INITIAL_THRESHOLD) gc->next_gc = GC_INITIAL_THRESHOLD;
                gc->phase = GC_PHASE_IDLE;
            }
            break;
        }
    }
}

/* ── Collection ── */

void gc_collect(GC *gc, void *vm_ptr) {
    if (!gc->enabled) return;

    StackVM *vm = (StackVM *)vm_ptr;

    /* 1. Clear all marks from previous cycle */
    for (GCObject *obj = gc->all_objects; obj; obj = obj->next) obj->marked = false;

    /* 2. Mark phase: traverse all roots */
    gc_mark_roots(gc, vm);

    /* Sweep phase: free unmarked objects */
    size_t freed = gc_sweep(gc);

    gc->total_collected += freed;
    gc->total_cycles++;

    /* Adaptive threshold: grow based on surviving objects */
    gc->next_gc = gc->object_count * GC_GROWTH_FACTOR;
    if (gc->next_gc < GC_INITIAL_THRESHOLD) gc->next_gc = GC_INITIAL_THRESHOLD;
}

void gc_maybe_collect(GC *gc, void *vm_ptr) {
    if (!gc->enabled) return;

    if (gc->incremental) {
        gc_incremental_step(gc, vm_ptr);
        return;
    }

    if (gc->stress || gc->object_count >= gc->next_gc) { gc_collect(gc, vm_ptr); }
}
