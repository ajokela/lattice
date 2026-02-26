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
}

/* ── Allocation ── */

void *gc_alloc(GC *gc, size_t size) {
    GCObject *obj = (GCObject *)malloc(sizeof(GCObject) + size);
    if (!obj) return NULL;

    obj->next = gc->all_objects;
    obj->marked = false;
    gc->all_objects = obj;
    gc->object_count++;
    gc->bytes_allocated += size;

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

    if (gc->stress || gc->object_count >= gc->next_gc) { gc_collect(gc, vm_ptr); }
}
