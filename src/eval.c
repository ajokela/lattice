#include "eval.h"
#include "lattice.h"
#include "intern.h"
#include "string_ops.h"
#include "builtin_methods.h"
#include "format_ops.h"
#include "builtins.h"
#include "array_ops.h"
#include "net.h"
#include "tls.h"
#include "json.h"
#include "math_ops.h"
#include "env_ops.h"
#include "time_ops.h"
#include "datetime_ops.h"
#include "type_ops.h"
#include "fs_ops.h"
#include "path_ops.h"
#include "regex_ops.h"
#include "crypto_ops.h"
#include "process_ops.h"
#include "http.h"
#include "toml_ops.h"
#include "yaml_ops.h"
#include "lexer.h"
#include "parser.h"
#include "channel.h"
#include "ext.h"
#include "runtime.h"
#include "package.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>
#include <libgen.h>
#include <sys/resource.h>
#ifndef __EMSCRIPTEN__
#include <pthread.h>
#endif

/* Monotonic clock in nanoseconds */
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ── Memory stats helpers ── */

static void stats_init(MemoryStats *s) { memset(s, 0, sizeof(*s)); }
static void stats_freeze(MemoryStats *s) { s->freezes++; }
static void stats_thaw(MemoryStats *s) { s->thaws++; }
static void stats_deep_clone(MemoryStats *s) { s->deep_clones++; }
static void stats_array(MemoryStats *s) { s->array_allocs++; }
static void stats_struct(MemoryStats *s) { s->struct_allocs++; }
static void stats_closure(MemoryStats *s) { s->closure_allocs++; }
static void stats_binding(MemoryStats *s) { s->bindings_created++; }
static void stats_fn_call(MemoryStats *s) { s->fn_calls++; }
static void stats_closure_call(MemoryStats *s) { s->closure_calls++; }
static void stats_forge(MemoryStats *s) { s->forge_blocks++; }

static void stats_scope_push(MemoryStats *s) {
    s->scope_pushes++;
    s->current_scope_depth++;
    if (s->current_scope_depth > s->peak_scope_depth)
        s->peak_scope_depth = s->current_scope_depth;
}

static void stats_scope_pop(MemoryStats *s) {
    s->scope_pops++;
    if (s->current_scope_depth > 0) s->current_scope_depth--;
}

/* ── Call stack helpers (stack traces) ── */

static void ev_push_frame(Evaluator *ev, const char *name) {
    if (ev->call_depth >= ev->call_stack_cap) {
        ev->call_stack_cap = ev->call_stack_cap ? ev->call_stack_cap * 2 : 16;
        ev->call_stack = realloc(ev->call_stack, ev->call_stack_cap * sizeof(const char *));
    }
    ev->call_stack[ev->call_depth++] = name;
}

static void ev_pop_frame(Evaluator *ev) {
    if (ev->call_depth > 0) ev->call_depth--;
}

/* Format a stack trace and append it to an error message */
static char *ev_attach_trace(Evaluator *ev, char *msg) {
    if (ev->call_depth == 0) return msg;
    size_t msg_len = strlen(msg);
    /* Estimate space: header + per-frame lines */
    size_t extra = 32;
    for (size_t i = 0; i < ev->call_depth; i++)
        extra += strlen(ev->call_stack[i]) + 16;
    char *out = realloc(msg, msg_len + extra);
    if (!out) return msg;
    size_t pos = msg_len;
    size_t rem = extra;
    int n = snprintf(out + pos, rem, "\nstack trace:");
    pos += (size_t)n; rem -= (size_t)n;
    for (size_t i = ev->call_depth; i > 0; i--) {
        n = snprintf(out + pos, rem, "\n  in %s()", ev->call_stack[i - 1]);
        pos += (size_t)n; rem -= (size_t)n;
    }
    return out;
}

/* ── EvalResult helpers ── */

static EvalResult eval_ok(LatValue v) {
    EvalResult r;
    r.ok = true;
    r.value = v;
    r.error = NULL;
    r.cf.tag = CF_NONE;
    return r;
}

static EvalResult eval_err(char *msg) {
    EvalResult r;
    r.ok = false;
    r.value = value_unit();
    r.error = msg;
    r.cf.tag = CF_NONE;
    return r;
}

static EvalResult eval_signal(ControlFlowTag tag, LatValue v) {
    EvalResult r;
    r.ok = false;
    r.value = value_unit();
    r.error = NULL;
    r.cf.tag = tag;
    r.cf.value = v;
    return r;
}

#define IS_OK(r) ((r).ok)
#define IS_ERR(r) (!(r).ok && (r).error != NULL)
#define IS_SIGNAL(r) (!(r).ok && (r).error == NULL)

/* ── Shadow stack macros ── */
#define GC_PUSH(ev, vptr) lat_vec_push(&(ev)->gc_roots, &(LatValue*){(vptr)})
#define GC_POP(ev)        lat_vec_pop(&(ev)->gc_roots, NULL)
#define GC_POP_N(ev, n)   do { for (size_t _i = 0; _i < (n); _i++) GC_POP(ev); } while(0)

/* Forward declarations */
static EvalResult eval_expr_inner(Evaluator *ev, const Expr *expr);
static EvalResult eval_stmt(Evaluator *ev, const Stmt *stmt);
static EvalResult eval_block_stmts(Evaluator *ev, Stmt **stmts, size_t count);

/* eval_expr now directly calls eval_expr_inner; temporaries are protected
 * by GC_PUSH/GC_POP at individual expression sites instead of the blunt
 * gc_inhibit hammer. */
static inline EvalResult eval_expr(Evaluator *ev, const Expr *expr) {
    return eval_expr_inner(ev, expr);
}

/* ── Garbage Collector ── */

/* Forward declaration for mutual recursion with gc_mark_env_value */
static void gc_mark_value(FluidHeap *fh, LatValue *v, LatVec *reachable_regions);

/* Callback for env_iter_values to mark each value */
typedef struct {
    FluidHeap *fh;
    LatVec    *reachable_regions;
} GcMarkCtx;

static void gc_mark_env_value(LatValue *v, void *ctx) {
    GcMarkCtx *mc = (GcMarkCtx *)ctx;
    gc_mark_value(mc->fh, v, mc->reachable_regions);
}

/*
 * Mark a single LatValue as reachable, recursively marking contained
 * heap pointers in the fluid heap.  Collects reachable crystal region
 * IDs into the supplied vector.
 */
static void gc_mark_value(FluidHeap *fh, LatValue *v, LatVec *reachable_regions) {
    /* Arena-backed values: record the region as reachable and skip traversal.
     * All child pointers reside in the same region, so no fluid marking needed.
     * Note: compiled bytecode closures repurpose region_id as upvalue count,
     * so exclude them from this check. */
    if (v->region_id != REGION_NONE && v->region_id != REGION_EPHEMERAL) {
        bool is_compiled_closure = (v->type == VAL_CLOSURE &&
                                    v->as.closure.body == NULL &&
                                    v->as.closure.native_fn != NULL);
        if (!is_compiled_closure) {
            lat_vec_push(reachable_regions, &v->region_id);
            return;
        }
    }
    switch (v->type) {
        case VAL_STR:
            if (v->as.str_val)
                fluid_mark(fh, v->as.str_val);
            break;
        case VAL_ARRAY:
            if (v->as.array.elems) {
                fluid_mark(fh, v->as.array.elems);
                for (size_t i = 0; i < v->as.array.len; i++)
                    gc_mark_value(fh, &v->as.array.elems[i], reachable_regions);
            }
            break;
        case VAL_STRUCT:
            if (v->as.strct.name) fluid_mark(fh, v->as.strct.name);
            if (v->as.strct.field_names) {
                fluid_mark(fh, v->as.strct.field_names);
                /* field_names[i] are interned — not in fluid heap, skip marking */
            }
            if (v->as.strct.field_values) {
                fluid_mark(fh, v->as.strct.field_values);
                for (size_t i = 0; i < v->as.strct.field_count; i++)
                    gc_mark_value(fh, &v->as.strct.field_values[i], reachable_regions);
            }
            break;
        case VAL_CLOSURE:
            if (v->as.closure.param_names) {
                fluid_mark(fh, v->as.closure.param_names);
                for (size_t i = 0; i < v->as.closure.param_count; i++) {
                    if (v->as.closure.param_names[i])
                        fluid_mark(fh, v->as.closure.param_names[i]);
                }
            }
            /* Mark captured env's values recursively */
            if (v->as.closure.captured_env) {
                Env *cenv = v->as.closure.captured_env;
                GcMarkCtx cctx = { fh, reachable_regions };
                env_iter_values(cenv, gc_mark_env_value, &cctx);
            }
            break;
        case VAL_MAP:
            if (v->as.map.map) {
                fluid_mark(fh, v->as.map.map);
                for (size_t i = 0; i < v->as.map.map->cap; i++) {
                    if (v->as.map.map->entries[i].state == MAP_OCCUPIED) {
                        gc_mark_value(fh, (LatValue *)v->as.map.map->entries[i].value, reachable_regions);
                    }
                }
            }
            break;
        case VAL_ENUM:
            if (v->as.enm.enum_name) fluid_mark(fh, v->as.enm.enum_name);
            if (v->as.enm.variant_name) fluid_mark(fh, v->as.enm.variant_name);
            if (v->as.enm.payload) {
                fluid_mark(fh, v->as.enm.payload);
                for (size_t i = 0; i < v->as.enm.payload_count; i++)
                    gc_mark_value(fh, &v->as.enm.payload[i], reachable_regions);
            }
            break;
        case VAL_SET:
            if (v->as.set.map) {
                fluid_mark(fh, v->as.set.map);
                if (v->as.set.map->entries) {
                    fluid_mark(fh, v->as.set.map->entries);
                    for (size_t i = 0; i < v->as.set.map->cap; i++) {
                        if (v->as.set.map->entries[i].state == MAP_OCCUPIED) {
                            fluid_mark(fh, v->as.set.map->entries[i].key);
                            fluid_mark(fh, v->as.set.map->entries[i].value);
                            gc_mark_value(fh, (LatValue *)v->as.set.map->entries[i].value, reachable_regions);
                        }
                    }
                }
            }
            break;
        default:
            break;
    }
}

#ifndef NDEBUG
#include <assert.h>
/*
 * Debug assertion: verify that no crystal value's heap pointers appear
 * in the fluid alloc list.  Violation would mean freeze didn't properly
 * untrack the pointer, risking a double-free during sweep.
 */
static bool ptr_in_fluid(FluidHeap *fh, void *ptr) {
    if (!ptr) return false;
    for (FluidAlloc *a = fh->allocs; a; a = a->next) {
        if (a->ptr == ptr) return true;
    }
    return false;
}

static void assert_crystal_not_fluid(LatValue *v, void *ctx) {
    FluidHeap *fh = (FluidHeap *)ctx;
    if (v->phase != VTAG_CRYSTAL || v->region_id == (size_t)-1) return;
    switch (v->type) {
        case VAL_STR:
            assert(!ptr_in_fluid(fh, v->as.str_val) &&
                   "crystal string in fluid heap");
            break;
        case VAL_ARRAY:
            assert(!ptr_in_fluid(fh, v->as.array.elems) &&
                   "crystal array elems in fluid heap");
            break;
        case VAL_STRUCT:
            assert(!ptr_in_fluid(fh, v->as.strct.name) &&
                   "crystal struct name in fluid heap");
            assert(!ptr_in_fluid(fh, v->as.strct.field_names) &&
                   "crystal struct field_names in fluid heap");
            assert(!ptr_in_fluid(fh, v->as.strct.field_values) &&
                   "crystal struct field_values in fluid heap");
            for (size_t i = 0; i < v->as.strct.field_count; i++) {
                assert(!ptr_in_fluid(fh, v->as.strct.field_names[i]) &&
                       "crystal struct field_name string in fluid heap");
            }
            break;
        case VAL_CLOSURE:
            assert(!ptr_in_fluid(fh, v->as.closure.param_names) &&
                   "crystal closure param_names in fluid heap");
            for (size_t i = 0; i < v->as.closure.param_count; i++) {
                assert(!ptr_in_fluid(fh, v->as.closure.param_names[i]) &&
                       "crystal closure param_name string in fluid heap");
            }
            break;
        case VAL_MAP:
            if (v->as.map.map) {
                assert(!ptr_in_fluid(fh, v->as.map.map) &&
                       "crystal map struct in fluid heap");
                assert(!ptr_in_fluid(fh, v->as.map.map->entries) &&
                       "crystal map entries in fluid heap");
            }
            break;
        case VAL_SET:
            if (v->as.set.map) {
                assert(!ptr_in_fluid(fh, v->as.set.map) &&
                       "crystal set struct in fluid heap");
                assert(!ptr_in_fluid(fh, v->as.set.map->entries) &&
                       "crystal set entries in fluid heap");
            }
            break;
        default:
            break;
    }
}

static void assert_dual_heap_invariant(Evaluator *ev) {
    env_iter_values(ev->env, assert_crystal_not_fluid, ev->heap->fluid);
    for (size_t i = 0; i < ev->saved_envs.len; i++) {
        Env **ep = lat_vec_get(&ev->saved_envs, i);
        env_iter_values(*ep, assert_crystal_not_fluid, ev->heap->fluid);
    }
}
#endif /* NDEBUG */

/*
 * Run a full GC cycle: mark all roots, sweep unreachable.
 */
static void gc_cycle(Evaluator *ev) {
    FluidHeap *fh = ev->heap->fluid;
    LatVec reachable_regions = lat_vec_new(sizeof(RegionId));

    /* 0. Advance epoch — groups frozen values by GC generation */
    if (!ev->no_regions)
        region_advance_epoch(ev->heap->regions);

    /* 1. Clear all marks */
    fluid_unmark_all(fh);

    /* 2. Mark roots from environment */
    GcMarkCtx ctx = { fh, &reachable_regions };
    env_iter_values(ev->env, gc_mark_env_value, &ctx);

    /* 3. Mark roots from shadow stack */
    for (size_t i = 0; i < ev->gc_roots.len; i++) {
        LatValue **vp = lat_vec_get(&ev->gc_roots, i);
        if (*vp) gc_mark_value(fh, *vp, &reachable_regions);
    }

    /* 4. Mark values from saved caller environments (closure env swap) */
    for (size_t i = 0; i < ev->saved_envs.len; i++) {
        Env **ep = lat_vec_get(&ev->saved_envs, i);
        env_iter_values(*ep, gc_mark_env_value, &ctx);
    }

    /* 5. Sweep unmarked fluid allocations */
    size_t fluid_before = fh->total_bytes;
    size_t swept_fluid = fluid_sweep(fh);
    ev->stats.gc_bytes_swept += fluid_before - fh->total_bytes;

    /* 6. Collect unreachable crystal regions */
    size_t swept_regions = 0;
    if (!ev->no_regions) {
        swept_regions = region_collect(
            ev->heap->regions,
            (RegionId *)reachable_regions.data,
            reachable_regions.len);
    }

    /* 7. Update stats */
    ev->stats.gc_cycles++;
    ev->stats.gc_swept_fluid += swept_fluid;
    ev->stats.gc_swept_regions += swept_regions;

    lat_vec_free(&reachable_regions);

#ifndef NDEBUG
    /* 8. Verify dual-heap invariant: no crystal pointers in fluid heap */
    if (!ev->no_regions)
        assert_dual_heap_invariant(ev);
#endif
}

/*
 * Maybe trigger GC if heap exceeds threshold.
 * Called after allocations.
 */
static void gc_maybe_collect(Evaluator *ev) {
    if (ev->gc_stress ||
        ev->heap->fluid->total_bytes >= ev->heap->fluid->gc_threshold) {
        uint64_t t0 = now_ns();
        gc_cycle(ev);
        ev->stats.gc_total_ns += now_ns() - t0;
    }
}

/*
 * Recursively set region_id on a value and all nested values.
 * Must walk into closure captured environments so that GC knows
 * every arena pointer belongs to this region.
 */
static void set_region_id_env(Env *env, RegionId rid);

static void set_region_id_recursive(LatValue *v, RegionId rid) {
    v->region_id = rid;
    switch (v->type) {
        case VAL_ARRAY:
            for (size_t i = 0; i < v->as.array.len; i++)
                set_region_id_recursive(&v->as.array.elems[i], rid);
            break;
        case VAL_STRUCT:
            for (size_t i = 0; i < v->as.strct.field_count; i++)
                set_region_id_recursive(&v->as.strct.field_values[i], rid);
            break;
        case VAL_CLOSURE:
            if (v->as.closure.captured_env)
                set_region_id_env(v->as.closure.captured_env, rid);
            break;
        case VAL_MAP:
            if (v->as.map.map) {
                for (size_t i = 0; i < v->as.map.map->cap; i++) {
                    if (v->as.map.map->entries[i].state == MAP_OCCUPIED) {
                        LatValue *mv = (LatValue *)v->as.map.map->entries[i].value;
                        set_region_id_recursive(mv, rid);
                    }
                }
            }
            break;
        case VAL_ENUM:
            for (size_t i = 0; i < v->as.enm.payload_count; i++)
                set_region_id_recursive(&v->as.enm.payload[i], rid);
            break;
        case VAL_SET:
            if (v->as.set.map) {
                for (size_t i = 0; i < v->as.set.map->cap; i++) {
                    if (v->as.set.map->entries[i].state == MAP_OCCUPIED) {
                        LatValue *sv = (LatValue *)v->as.set.map->entries[i].value;
                        set_region_id_recursive(sv, rid);
                    }
                }
            }
            break;
        default:
            break;
    }
}

static void set_region_id_env_value(LatValue *v, void *ctx) {
    RegionId rid = *(RegionId *)ctx;
    set_region_id_recursive(v, rid);
}

static void set_region_id_env(Env *env, RegionId rid) {
    env_iter_values(env, set_region_id_env_value, &rid);
}

/*
 * Freeze support: deep-clone value into a new arena-backed region,
 * set region_id recursively, free the original fluid-heap value,
 * and replace it with the arena clone.
 *
 * In no-regions baseline mode, crystal values stay in the fluid heap.
 */
static void freeze_to_region(Evaluator *ev, LatValue *v) {
    if (ev->no_regions) return;

    CrystalRegion *region = region_create(ev->heap->regions);

    value_set_arena(region);
    LatValue clone = value_deep_clone(v);
    value_set_arena(NULL);

    ev->heap->regions->cumulative_data_bytes += region->total_bytes;

    set_region_id_recursive(&clone, region->id);

    value_free(v);
    *v = clone;
}

/* Record a history snapshot for a tracked variable */
static void record_history(Evaluator *ev, const char *name) {
    for (size_t i = 0; i < ev->tracked_count; i++) {
        if (strcmp(ev->tracked_vars[i].name, name) != 0) continue;
        VariableHistory *vh = &ev->tracked_vars[i].history;
        LatValue cur;
        if (!env_get(ev->env, name, &cur)) return;
        if (vh->count >= vh->cap) {
            vh->cap = vh->cap ? vh->cap * 2 : 8;
            vh->snapshots = realloc(vh->snapshots, vh->cap * sizeof(HistorySnapshot));
        }
        const char *phase = builtin_phase_of_str(&cur);
        vh->snapshots[vh->count].phase_name = strdup(phase);
        vh->snapshots[vh->count].value = value_deep_clone(&cur);
        vh->snapshots[vh->count].line = 0;
        vh->snapshots[vh->count].fn_name = NULL;
        vh->count++;
        value_free(&cur);
        return;
    }
}

/* Forward declaration for fire_reactions */
static EvalResult call_closure(Evaluator *ev, char **params, size_t param_count,
                               const Expr *body, Env *closure_env, LatValue *args, size_t arg_count,
                               Expr **default_values, bool has_variadic);

/// @builtin react(var: Ident, callback: Closure) -> Unit
/// @category Phase Reactions
/// Register a callback that fires when a variable's phase changes.
/// @example react(data, |phase, val| { print(phase) })

/// @builtin unreact(var: Ident) -> Unit
/// @category Phase Reactions
/// Remove all phase reaction callbacks from a variable.
/// @example unreact(data)

static EvalResult fire_reactions(Evaluator *ev, const char *var_name, const char *phase_name) {
    for (size_t i = 0; i < ev->reaction_count; i++) {
        if (strcmp(ev->reactions[i].var_name, var_name) != 0) continue;
        LatValue cur;
        if (!env_get(ev->env, var_name, &cur)) return eval_ok(value_unit());
        for (size_t j = 0; j < ev->reactions[i].cb_count; j++) {
            LatValue *cb = &ev->reactions[i].callbacks[j];
            LatValue args[2];
            args[0] = value_string(phase_name);
            args[1] = value_deep_clone(&cur);
            EvalResult r = call_closure(ev, cb->as.closure.param_names,
                cb->as.closure.param_count, cb->as.closure.body,
                cb->as.closure.captured_env, args, 2,
                cb->as.closure.default_values, cb->as.closure.has_variadic);
            if (!IS_OK(r)) {
                value_free(&cur);
                char *err = NULL;
                (void)asprintf(&err, "reaction error: %s", r.error);
                free(r.error);
                return eval_err(err);
            }
            value_free(&r.value);
        }
        value_free(&cur);
        return eval_ok(value_unit());
    }
    return eval_ok(value_unit());
}

/* Cascade freeze through bonded variables */
/* Returns NULL on success, heap-allocated error string on failure */
static char *freeze_cascade(Evaluator *ev, const char *target_name) {
    for (size_t bi = 0; bi < ev->bond_count; bi++) {
        if (strcmp(ev->bonds[bi].target, target_name) != 0) continue;
        /* Found bond entry for target — process all deps by strategy */
        for (size_t di = 0; di < ev->bonds[bi].dep_count; di++) {
            const char *dep = ev->bonds[bi].deps[di];
            const char *strategy = ev->bonds[bi].dep_strategies ? ev->bonds[bi].dep_strategies[di] : "mirror";
            LatValue dval;
            if (!env_get(ev->env, dep, &dval)) continue;  /* variable gone */
            if (dval.type == VAL_CHANNEL) { value_free(&dval); continue; }

            if (strcmp(strategy, "mirror") == 0) {
                if (dval.phase == VTAG_CRYSTAL) { value_free(&dval); continue; }
                dval = value_freeze(dval);
                freeze_to_region(ev, &dval);
                env_set(ev->env, dep, dval);
                EvalResult fr = fire_reactions(ev, dep, "crystal");
                if (!IS_OK(fr)) free(fr.error);
                char *err = freeze_cascade(ev, dep);
                if (err) return err;
            } else if (strcmp(strategy, "inverse") == 0) {
                /* Inverse: thaw the dep when target freezes */
                if (dval.phase != VTAG_CRYSTAL && dval.phase != VTAG_SUBLIMATED) { value_free(&dval); continue; }
                LatValue thawed = value_thaw(&dval);
                value_free(&dval);
                env_set(ev->env, dep, thawed);
                EvalResult fr = fire_reactions(ev, dep, "fluid");
                if (!IS_OK(fr)) free(fr.error);
            } else if (strcmp(strategy, "gate") == 0) {
                /* Gate: dep must already be crystal for target to freeze */
                if (dval.phase != VTAG_CRYSTAL) {
                    value_free(&dval);
                    char *err = NULL;
                    (void)asprintf(&err, "gate bond: '%s' must be crystal before '%s' can freeze", dep, target_name);
                    return err;
                }
                value_free(&dval);
            } else {
                value_free(&dval);
            }
        }
        /* Consume the bond entry */
        for (size_t di = 0; di < ev->bonds[bi].dep_count; di++) {
            free(ev->bonds[bi].deps[di]);
            if (ev->bonds[bi].dep_strategies) free(ev->bonds[bi].dep_strategies[di]);
        }
        free(ev->bonds[bi].deps);
        free(ev->bonds[bi].dep_strategies);
        free(ev->bonds[bi].target);
        ev->bonds[bi] = ev->bonds[--ev->bond_count];
        break;
    }
    return NULL;
}

/*
 * Resolve a mutable pointer to a LatValue from an lvalue expression.
 * Walks chains of field_access and index expressions to find the
 * actual storage location in the environment. Returns NULL + sets *err on failure.
 */
static LatValue *resolve_lvalue(Evaluator *ev, const Expr *expr, char **err) {
    if (expr->tag == EXPR_IDENT) {
        for (size_t s = ev->env->count; s > 0; s--) {
            LatValue *v = lat_map_get(&ev->env->scopes[s-1], expr->as.str_val);
            if (v) return v;
        }
        const char *suggestion = env_find_similar_name(ev->env, expr->as.str_val);
        if (suggestion)
            (void)asprintf(err, "undefined variable '%s' (did you mean '%s'?)", expr->as.str_val, suggestion);
        else
            (void)asprintf(err, "undefined variable '%s'", expr->as.str_val);
        return NULL;
    }
    if (expr->tag == EXPR_FIELD_ACCESS) {
        LatValue *parent = resolve_lvalue(ev, expr->as.field_access.object, err);
        if (!parent) return NULL;
        if (parent->type != VAL_STRUCT) {
            (void)asprintf(err, "cannot access field '%s' on %s",
                           expr->as.field_access.field, value_type_name(parent));
            return NULL;
        }
        for (size_t i = 0; i < parent->as.strct.field_count; i++) {
            if (parent->as.strct.field_names[i] == intern(expr->as.field_access.field)) {
                return &parent->as.strct.field_values[i];
            }
        }
        (void)asprintf(err, "struct has no field '%s'", expr->as.field_access.field);
        return NULL;
    }
    if (expr->tag == EXPR_INDEX) {
        /* Evaluate the index expression BEFORE resolving the parent lvalue.
         * eval_expr may trigger GC or scope-map mutations (lat_map_set →
         * rehash), which would invalidate any raw pointer returned by
         * resolve_lvalue.  By evaluating first we avoid stale pointers. */
        EvalResult idxr = eval_expr(ev, expr->as.index.index);
        if (!IS_OK(idxr)) { *err = idxr.error; return NULL; }

        LatValue *parent = resolve_lvalue(ev, expr->as.index.object, err);
        if (!parent) { value_free(&idxr.value); return NULL; }

        /* Ref unwrap: delegate indexing to the inner value */
        if (parent->type == VAL_REF) parent = &parent->as.ref.ref->value;

        if (parent->type == VAL_MAP) {
            if (idxr.value.type != VAL_STR) {
                value_free(&idxr.value);
                *err = strdup("map key must be a string");
                return NULL;
            }
            const char *key = idxr.value.as.str_val;
            /* Auto-vivify: if key doesn't exist, create it with unit */
            if (!lat_map_contains(parent->as.map.map, key)) {
                LatValue unit = value_unit();
                lat_map_set(parent->as.map.map, key, &unit);
            }
            LatValue *target = (LatValue *)lat_map_get(parent->as.map.map, key);
            value_free(&idxr.value);
            return target;
        }
        if (parent->type == VAL_ARRAY) {
            if (idxr.value.type != VAL_INT) {
                value_free(&idxr.value);
                *err = strdup("array index must be an integer");
                return NULL;
            }
            size_t idx = (size_t)idxr.value.as.int_val;
            value_free(&idxr.value);
            if (idx >= parent->as.array.len) {
                (void)asprintf(err, "index %zu out of bounds (length %zu)",
                               idx, parent->as.array.len);
                return NULL;
            }
            return &parent->as.array.elems[idx];
        }
        value_free(&idxr.value);
        (void)asprintf(err, "cannot index into %s", value_type_name(parent));
        return NULL;
    }
    *err = strdup("invalid lvalue expression");
    return NULL;
}
static EvalResult eval_method_call(Evaluator *ev, LatValue obj, const char *method,
                                   LatValue *args, size_t arg_count);

/* ── FnDecl lookup helpers ── */
/* We store FnDecl* pointers in the map */

static FnDecl *find_fn(Evaluator *ev, const char *name) {
    FnDecl **ptr = lat_map_get(&ev->fn_defs, name);
    return ptr ? *ptr : NULL;
}

/* ── Phase helpers for constraints & dispatch ── */

static const char *phase_tag_name(PhaseTag p) {
    switch (p) {
        case VTAG_FLUID:      return "fluid (flux)";
        case VTAG_CRYSTAL:    return "crystal (fix)";
        case VTAG_UNPHASED:   return "unphased";
        case VTAG_SUBLIMATED: return "sublimated";
    }
    return "unknown";
}

static const char *ast_phase_name(AstPhase p) {
    switch (p) {
        case PHASE_FLUID:       return "flux";
        case PHASE_CRYSTAL:     return "fix";
        case PHASE_UNSPECIFIED: return "unspecified";
    }
    return "unknown";
}

static bool phase_compatible(PhaseTag value_phase, AstPhase param_phase) {
    switch (param_phase) {
        case PHASE_FLUID:       return value_phase != VTAG_CRYSTAL;
        case PHASE_CRYSTAL:     return value_phase != VTAG_FLUID;
        case PHASE_UNSPECIFIED: return true;
    }
    return true;
}

static bool type_matches_value(const LatValue *val, const TypeExpr *te) {
    if (!te || !te->name) return true;  /* no annotation = Any */
    if (strcmp(te->name, "Any") == 0 || strcmp(te->name, "any") == 0) return true;
    if (te->kind == TYPE_ARRAY) {
        if (val->type != VAL_ARRAY) return false;
        if (!te->inner) return true;  /* [Any] or unspecified inner */
        for (size_t i = 0; i < val->as.array.len; i++) {
            if (!type_matches_value(&val->as.array.elems[i], te->inner))
                return false;
        }
        return true;
    }
    /* Named types */
    const char *n = te->name;
    if (strcmp(n, "Int") == 0)    return val->type == VAL_INT;
    if (strcmp(n, "Float") == 0)  return val->type == VAL_FLOAT;
    if (strcmp(n, "String") == 0) return val->type == VAL_STR;
    if (strcmp(n, "Bool") == 0)   return val->type == VAL_BOOL;
    if (strcmp(n, "Nil") == 0)    return val->type == VAL_NIL;
    if (strcmp(n, "Map") == 0)    return val->type == VAL_MAP;
    if (strcmp(n, "Array") == 0)  return val->type == VAL_ARRAY;
    if (strcmp(n, "Fn") == 0 || strcmp(n, "Closure") == 0) return val->type == VAL_CLOSURE;
    if (strcmp(n, "Channel") == 0) return val->type == VAL_CHANNEL;
    if (strcmp(n, "Range") == 0)  return val->type == VAL_RANGE;
    if (strcmp(n, "Set") == 0)    return val->type == VAL_SET;
    if (strcmp(n, "Tuple") == 0)  return val->type == VAL_TUPLE;
    if (strcmp(n, "Buffer") == 0) return val->type == VAL_BUFFER;
    if (strcmp(n, "Ref") == 0) return val->type == VAL_REF;
    if (strcmp(n, "Number") == 0) return val->type == VAL_INT || val->type == VAL_FLOAT;
    /* Struct name check */
    if (val->type == VAL_STRUCT && val->as.strct.name)
        return strcmp(val->as.strct.name, n) == 0;
    /* Enum name check */
    if (val->type == VAL_ENUM && val->as.enm.enum_name)
        return strcmp(val->as.enm.enum_name, n) == 0;
    return false;
}

/* Check if a type name is a known built-in type (wraps lat_is_known_type) */
static bool is_known_type_name(const char *n) {
    if (lat_is_known_type(n)) return true;
    if (strcmp(n, "any") == 0) return true;
    return false;
}

static const char *value_type_display(const LatValue *val) {
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

static bool phase_signatures_match(const FnDecl *a, const FnDecl *b) {
    if (a->param_count != b->param_count) return false;
    for (size_t i = 0; i < a->param_count; i++) {
        if (a->params[i].ty.phase != b->params[i].ty.phase) return false;
    }
    return true;
}

static FnDecl *resolve_overload(FnDecl *head, LatValue *args, size_t argc) {
    FnDecl *best = NULL;
    int best_score = -1;

    for (FnDecl *cand = head; cand; cand = cand->next_overload) {
        /* Check arity */
        size_t required = 0;
        bool has_variadic = false;
        for (size_t i = 0; i < cand->param_count; i++) {
            if (cand->params[i].is_variadic) has_variadic = true;
            else if (!cand->params[i].default_value) required++;
        }
        size_t max_pos = has_variadic ? cand->param_count - 1 : cand->param_count;
        if (argc < required || (!has_variadic && argc > max_pos)) continue;

        /* Check phase compatibility and compute score */
        bool compatible = true;
        int score = 0;
        size_t check_count = argc < cand->param_count ? argc : cand->param_count;
        for (size_t i = 0; i < check_count; i++) {
            if (cand->params[i].is_variadic) break;
            AstPhase pp = cand->params[i].ty.phase;
            PhaseTag vp = args[i].phase;
            if (!phase_compatible(vp, pp)) { compatible = false; break; }
            if (pp == PHASE_FLUID && vp == VTAG_FLUID)
                score += 3;  /* exact: fluid→flux */
            else if (pp == PHASE_CRYSTAL && vp == VTAG_CRYSTAL)
                score += 3;  /* exact: crystal→fix */
            else if (pp == PHASE_UNSPECIFIED && vp == VTAG_UNPHASED)
                score += 2;  /* exact: unphased→unspecified */
            else if (pp == PHASE_UNSPECIFIED)
                score += 1;  /* compatible: specific→unspecified */
            /* else: unphased→specific = 0 (compatible but weakest) */
        }
        if (!compatible) continue;

        if (score >= best_score) {
            best_score = score;
            best = cand;
        }
    }
    return best;
}

static void register_fn_overload(LatMap *fn_defs, FnDecl *new_fn) {
    FnDecl **existing = lat_map_get(fn_defs, new_fn->name);
    if (!existing) {
        /* No existing fn with this name — simple insert */
        lat_map_set(fn_defs, new_fn->name, &new_fn);
        return;
    }
    /* Check if same phase signature — replace */
    FnDecl *head = *existing;
    if (phase_signatures_match(head, new_fn)) {
        /* Replace head: keep the chain */
        new_fn->next_overload = head->next_overload;
        lat_map_set(fn_defs, new_fn->name, &new_fn);
        return;
    }
    /* Check rest of chain */
    for (FnDecl *prev = head; prev->next_overload; prev = prev->next_overload) {
        if (phase_signatures_match(prev->next_overload, new_fn)) {
            /* Replace this link */
            new_fn->next_overload = prev->next_overload->next_overload;
            prev->next_overload = new_fn;
            return;
        }
    }
    /* Different phase signature — chain it */
    new_fn->next_overload = head;
    lat_map_set(fn_defs, new_fn->name, &new_fn);
}

static StructDecl *find_struct(Evaluator *ev, const char *name) {
    StructDecl **ptr = lat_map_get(&ev->struct_defs, name);
    return ptr ? *ptr : NULL;
}

static EnumDecl *find_enum(Evaluator *ev, const char *name) {
    EnumDecl **ptr = lat_map_get(&ev->enum_defs, name);
    return ptr ? *ptr : NULL;
}

static VariantDecl *find_variant(EnumDecl *ed, const char *variant_name) {
    for (size_t i = 0; i < ed->variant_count; i++) {
        if (strcmp(ed->variants[i].name, variant_name) == 0)
            return &ed->variants[i];
    }
    return NULL;
}

/* Find a pressure constraint for a variable (returns mode string or NULL) */
static const char *find_pressure(Evaluator *ev, const char *var_name) {
    for (size_t i = 0; i < ev->pressure_count; i++) {
        if (strcmp(ev->pressures[i].var_name, var_name) == 0)
            return ev->pressures[i].mode;
    }
    return NULL;
}

/* Check if a pressure mode blocks a structural growth operation (push, insert, merge) */
static bool pressure_blocks_grow(const char *mode) {
    return mode && (strcmp(mode, "no_grow") == 0 || strcmp(mode, "no_resize") == 0);
}

/* Check if a pressure mode blocks a structural shrink operation (pop, remove, remove_at) */
static bool pressure_blocks_shrink(const char *mode) {
    return mode && (strcmp(mode, "no_shrink") == 0 || strcmp(mode, "no_resize") == 0);
}

/* Get the root variable name from a method call object expression */
static const char *get_method_obj_varname(const Expr *obj) {
    if (obj->tag == EXPR_IDENT) return obj->as.str_val;
    if (obj->tag == EXPR_FIELD_ACCESS) return get_method_obj_varname(obj->as.field_access.object);
    if (obj->tag == EXPR_INDEX) return get_method_obj_varname(obj->as.index.object);
    return NULL;
}

/* ── Function calling ── */

static EvalResult call_fn(Evaluator *ev, const FnDecl *decl, LatValue *args, size_t arg_count,
                          LatValue **writeback_out) {
    /* Determine required count (params without defaults and not variadic) */
    size_t required = 0;
    bool has_variadic = false;
    for (size_t i = 0; i < decl->param_count; i++) {
        if (decl->params[i].is_variadic) { has_variadic = true; }
        else if (!decl->params[i].default_value) required++;
    }
    size_t max_positional = has_variadic ? decl->param_count - 1 : decl->param_count;
    if (arg_count < required || (!has_variadic && arg_count > max_positional)) {
        char *err = NULL;
        if (has_variadic)
            (void)asprintf(&err, "function '%s' expects at least %zu arguments, got %zu",
                           decl->name, required, arg_count);
        else if (required < max_positional)
            (void)asprintf(&err, "function '%s' expects %zu to %zu arguments, got %zu",
                           decl->name, required, max_positional, arg_count);
        else
            (void)asprintf(&err, "function '%s' expects %zu arguments, got %zu",
                           decl->name, required, arg_count);
        return eval_err(err);
    }
    /* Phase constraint enforcement */
    for (size_t i = 0; i < decl->param_count && i < arg_count; i++) {
        if (decl->params[i].is_variadic) break;
        if (decl->params[i].ty.phase != PHASE_UNSPECIFIED &&
            !phase_compatible(args[i].phase, decl->params[i].ty.phase)) {
            char *err = NULL;
            (void)asprintf(&err, "function '%s' parameter '%s' requires %s argument, got %s",
                           decl->name, decl->params[i].name,
                           ast_phase_name(decl->params[i].ty.phase),
                           phase_tag_name(args[i].phase));
            return eval_err(err);
        }
    }
    /* Runtime type checking */
    for (size_t i = 0; i < decl->param_count && i < arg_count; i++) {
        if (decl->params[i].is_variadic) break;
        if (decl->params[i].ty.name && !type_matches_value(&args[i], &decl->params[i].ty)) {
            char *err = NULL;
            const char *tyname = decl->params[i].ty.name;
            /* If the type name is not a known built-in, suggest a similar type */
            if (!is_known_type_name(tyname)) {
                const char *tsug = lat_find_similar_type(tyname, NULL, NULL);
                if (tsug) {
                    (void)asprintf(&err, "function '%s' parameter '%s' expects type %s, got %s (did you mean '%s'?)",
                                   decl->name, decl->params[i].name,
                                   tyname, value_type_display(&args[i]), tsug);
                    return eval_err(err);
                }
            }
            (void)asprintf(&err, "function '%s' parameter '%s' expects type %s, got %s",
                           decl->name, decl->params[i].name,
                           tyname,
                           value_type_display(&args[i]));
            return eval_err(err);
        }
    }
    stats_fn_call(&ev->stats);
    ev_push_frame(ev, decl->name);
    stats_scope_push(&ev->stats);
    env_push_scope(ev->env);
    for (size_t i = 0; i < decl->param_count; i++) {
        if (decl->params[i].is_variadic) {
            /* Collect remaining args into an array */
            size_t rest_count = (arg_count > i) ? arg_count - i : 0;
            LatValue *rest_elems = malloc(rest_count * sizeof(LatValue));
            for (size_t j = 0; j < rest_count; j++)
                rest_elems[j] = args[i + j];
            LatValue arr = value_array(rest_elems, rest_count);
            free(rest_elems);
            env_define(ev->env, decl->params[i].name, arr);
        } else if (i < arg_count) {
            env_define(ev->env, decl->params[i].name, args[i]);
        } else {
            /* Use default value */
            EvalResult def = eval_expr(ev, decl->params[i].default_value);
            if (!IS_OK(def)) {
                env_pop_scope(ev->env);
                stats_scope_pop(&ev->stats);
                return def;
            }
            env_define(ev->env, decl->params[i].name, def.value);
        }
    }
    /* Evaluate require contracts */
    if (ev->assertions_enabled && decl->contracts) {
        for (size_t i = 0; i < decl->contract_count; i++) {
            if (decl->contracts[i].is_ensure) continue;
            EvalResult cr = eval_expr(ev, decl->contracts[i].condition);
            if (!IS_OK(cr)) { env_pop_scope(ev->env); stats_scope_pop(&ev->stats); return cr; }
            bool truthy = (cr.value.type == VAL_BOOL && cr.value.as.bool_val);
            value_free(&cr.value);
            if (!truthy) {
                char *err = NULL;
                if (decl->contracts[i].message)
                    (void)asprintf(&err, "require failed in '%s': %s", decl->name, decl->contracts[i].message);
                else
                    (void)asprintf(&err, "require contract failed in '%s'", decl->name);
                env_pop_scope(ev->env); stats_scope_pop(&ev->stats);
                return eval_err(err);
            }
        }
    }

    EvalResult result = eval_block_stmts(ev, decl->body, decl->body_count);

    /* Evaluate ensure contracts on the return value */
    if (ev->assertions_enabled && decl->contracts && (IS_OK(result) || (IS_SIGNAL(result) && result.cf.tag == CF_RETURN))) {
        LatValue ret_val = IS_OK(result) ? result.value : result.cf.value;
        for (size_t i = 0; i < decl->contract_count; i++) {
            if (!decl->contracts[i].is_ensure) continue;
            /* Ensure condition is a closure — call it with the return value */
            EvalResult cc = eval_expr(ev, decl->contracts[i].condition);
            if (!IS_OK(cc)) {
                if (IS_OK(result)) value_free(&result.value);
                else value_free(&result.cf.value);
                env_pop_scope(ev->env); stats_scope_pop(&ev->stats);
                return cc;
            }
            if (cc.value.type == VAL_CLOSURE) {
                LatValue arg = value_deep_clone(&ret_val);
                LatValue call_args[1] = { arg };
                EvalResult er = call_closure(ev, cc.value.as.closure.param_names,
                    cc.value.as.closure.param_count, cc.value.as.closure.body,
                    cc.value.as.closure.captured_env, call_args, 1,
                    cc.value.as.closure.default_values, cc.value.as.closure.has_variadic);
                value_free(&arg);
                value_free(&cc.value);
                if (!IS_OK(er)) {
                    if (IS_OK(result)) value_free(&result.value);
                    else value_free(&result.cf.value);
                    env_pop_scope(ev->env); stats_scope_pop(&ev->stats);
                    return er;
                }
                bool truthy = (er.value.type == VAL_BOOL && er.value.as.bool_val);
                value_free(&er.value);
                if (!truthy) {
                    if (IS_OK(result)) value_free(&result.value);
                    else value_free(&result.cf.value);
                    char *err = NULL;
                    if (decl->contracts[i].message)
                        (void)asprintf(&err, "ensure failed in '%s': %s", decl->name, decl->contracts[i].message);
                    else
                        (void)asprintf(&err, "ensure contract failed in '%s'", decl->name);
                    env_pop_scope(ev->env); stats_scope_pop(&ev->stats);
                    return eval_err(err);
                }
            } else {
                /* ensure with a non-closure: evaluate as boolean directly */
                bool truthy = (cc.value.type == VAL_BOOL && cc.value.as.bool_val);
                value_free(&cc.value);
                if (!truthy) {
                    if (IS_OK(result)) value_free(&result.value);
                    else value_free(&result.cf.value);
                    char *err = NULL;
                    if (decl->contracts[i].message)
                        (void)asprintf(&err, "ensure failed in '%s': %s", decl->name, decl->contracts[i].message);
                    else
                        (void)asprintf(&err, "ensure contract failed in '%s'", decl->name);
                    env_pop_scope(ev->env); stats_scope_pop(&ev->stats);
                    return eval_err(err);
                }
            }
        }
    }

    /* Return type checking */
    if (decl->return_type && (IS_OK(result) || (IS_SIGNAL(result) && result.cf.tag == CF_RETURN))) {
        LatValue *ret_val = IS_OK(result) ? &result.value : &result.cf.value;
        if (!type_matches_value(ret_val, decl->return_type)) {
            char *err = NULL;
            const char *rtyname = decl->return_type->name;
            if (rtyname && !is_known_type_name(rtyname)) {
                const char *rtsug = lat_find_similar_type(rtyname, NULL, NULL);
                if (rtsug) {
                    (void)asprintf(&err, "function '%s' return type expects %s, got %s (did you mean '%s'?)",
                                   decl->name, rtyname, value_type_display(ret_val), rtsug);
                    if (IS_OK(result)) value_free(&result.value);
                    else value_free(&result.cf.value);
                    env_pop_scope(ev->env); stats_scope_pop(&ev->stats);
                    return eval_err(err);
                }
            }
            (void)asprintf(&err, "function '%s' return type expects %s, got %s",
                           decl->name, rtyname, value_type_display(ret_val));
            if (IS_OK(result)) value_free(&result.value);
            else value_free(&result.cf.value);
            env_pop_scope(ev->env); stats_scope_pop(&ev->stats);
            return eval_err(err);
        }
    }

    /* Before popping the scope, capture fluid parameter values for write-back */
    if (writeback_out) {
        size_t wb_count = has_variadic ? decl->param_count - 1 : decl->param_count;
        if (wb_count > arg_count) wb_count = arg_count;
        for (size_t i = 0; i < wb_count; i++) {
            if (decl->params[i].ty.phase == PHASE_FLUID) {
                LatValue val;
                if (env_get(ev->env, decl->params[i].name, &val)) {
                    writeback_out[i] = malloc(sizeof(LatValue));
                    *writeback_out[i] = val;
                }
            }
        }
    }

    env_pop_scope(ev->env);
    stats_scope_pop(&ev->stats);

    if (IS_ERR(result)) return result; /* leave frame on stack for trace */
    ev_pop_frame(ev);

    if (IS_SIGNAL(result) && result.cf.tag == CF_RETURN) {
        return eval_ok(result.cf.value);
    }
    return result;
}

static EvalResult call_native_closure(Evaluator *ev, void *native_fn,
                                      LatValue *args, size_t arg_count) {
    (void)ev;
    LatValue result = ext_call_native(native_fn, args, arg_count);
    /* Check if the result is an error string */
    if (result.type == VAL_STR && strncmp(result.as.str_val, "EVAL_ERROR:", 11) == 0) {
        char *msg = strdup(result.as.str_val + 11);
        value_free(&result);
        return eval_err(msg);
    }
    return eval_ok(result);
}

static EvalResult call_closure(Evaluator *ev, char **params, size_t param_count,
                               const Expr *body, Env *closure_env, LatValue *args, size_t arg_count,
                               Expr **default_values, bool has_variadic) {
    /* Determine required count */
    size_t required = 0;
    for (size_t i = 0; i < param_count; i++) {
        if (has_variadic && i == param_count - 1) break;
        if (!default_values || !default_values[i]) required++;
    }
    size_t max_positional = has_variadic ? param_count - 1 : param_count;
    if (arg_count < required || (!has_variadic && arg_count > max_positional)) {
        char *err = NULL;
        if (has_variadic)
            (void)asprintf(&err, "closure expects at least %zu arguments, got %zu", required, arg_count);
        else if (required < max_positional)
            (void)asprintf(&err, "closure expects %zu to %zu arguments, got %zu", required, max_positional, arg_count);
        else
            (void)asprintf(&err, "closure expects %zu arguments, got %zu", param_count, arg_count);
        return eval_err(err);
    }
    stats_closure_call(&ev->stats);

    /* Swap environments — save caller env so GC can still mark it */
    Env *saved = ev->env;
    lat_vec_push(&ev->saved_envs, &saved);
    ev->env = closure_env;
    stats_scope_push(&ev->stats);
    env_push_scope(ev->env);
    for (size_t i = 0; i < param_count; i++) {
        if (has_variadic && i == param_count - 1) {
            /* Collect remaining args into an array */
            size_t rest_count = (arg_count > i) ? arg_count - i : 0;
            LatValue *rest_elems = malloc(rest_count * sizeof(LatValue));
            for (size_t j = 0; j < rest_count; j++)
                rest_elems[j] = args[i + j];
            LatValue arr = value_array(rest_elems, rest_count);
            free(rest_elems);
            env_define(ev->env, params[i], arr);
        } else if (i < arg_count) {
            env_define(ev->env, params[i], args[i]);
        } else if (default_values && default_values[i]) {
            /* Evaluate default in the closure environment */
            EvalResult def = eval_expr(ev, default_values[i]);
            if (!IS_OK(def)) {
                env_pop_scope(ev->env);
                stats_scope_pop(&ev->stats);
                ev->env = saved;
                lat_vec_pop(&ev->saved_envs, NULL);
                return def;
            }
            env_define(ev->env, params[i], def.value);
        }
    }
    EvalResult result = eval_expr(ev, body);
    env_pop_scope(ev->env);
    stats_scope_pop(&ev->stats);
    ev->env = saved;
    lat_vec_pop(&ev->saved_envs, NULL);

    if (IS_SIGNAL(result) && result.cf.tag == CF_RETURN) {
        return eval_ok(result.cf.value);
    }
    return result;
}

/* ── Value equality (for pattern matching) ── */

static bool value_equal(const LatValue *a, const LatValue *b) {
    if (a->type != b->type) return false;
    switch (a->type) {
        case VAL_INT:   return a->as.int_val == b->as.int_val;
        case VAL_FLOAT: return a->as.float_val == b->as.float_val;
        case VAL_BOOL:  return a->as.bool_val == b->as.bool_val;
        case VAL_STR:   return strcmp(a->as.str_val, b->as.str_val) == 0;
        case VAL_UNIT:  return true;
        case VAL_NIL:   return true;
        default:        return false;
    }
}

/* ── Binary operations ── */

static EvalResult eval_binop(BinOpKind op, LatValue *lv, LatValue *rv) {
    /* Integer arithmetic */
    if (lv->type == VAL_INT && rv->type == VAL_INT) {
        int64_t a = lv->as.int_val, b = rv->as.int_val;
        switch (op) {
            case BINOP_ADD: return eval_ok(value_int(a + b));
            case BINOP_SUB: return eval_ok(value_int(a - b));
            case BINOP_MUL: return eval_ok(value_int(a * b));
            case BINOP_DIV:
                if (b == 0) return eval_err(strdup("division by zero"));
                return eval_ok(value_int(a / b));
            case BINOP_MOD:
                if (b == 0) return eval_err(strdup("modulo by zero"));
                return eval_ok(value_int(a % b));
            case BINOP_EQ:   return eval_ok(value_bool(a == b));
            case BINOP_NEQ:  return eval_ok(value_bool(a != b));
            case BINOP_LT:   return eval_ok(value_bool(a < b));
            case BINOP_GT:   return eval_ok(value_bool(a > b));
            case BINOP_LTEQ: return eval_ok(value_bool(a <= b));
            case BINOP_GTEQ: return eval_ok(value_bool(a >= b));
            case BINOP_BIT_AND: return eval_ok(value_int(a & b));
            case BINOP_BIT_OR:  return eval_ok(value_int(a | b));
            case BINOP_BIT_XOR: return eval_ok(value_int(a ^ b));
            case BINOP_LSHIFT:
                if (b < 0 || b > 63) return eval_err(strdup("shift amount out of range (0..63)"));
                return eval_ok(value_int(a << b));
            case BINOP_RSHIFT:
                if (b < 0 || b > 63) return eval_err(strdup("shift amount out of range (0..63)"));
                return eval_ok(value_int(a >> b));
            default: break;
        }
    }
    /* Float arithmetic */
    if (lv->type == VAL_FLOAT && rv->type == VAL_FLOAT) {
        double a = lv->as.float_val, b = rv->as.float_val;
        switch (op) {
            case BINOP_ADD: return eval_ok(value_float(a + b));
            case BINOP_SUB: return eval_ok(value_float(a - b));
            case BINOP_MUL: return eval_ok(value_float(a * b));
            case BINOP_DIV: return eval_ok(value_float(a / b));
            case BINOP_MOD: {
                double r = a - (int64_t)(a / b) * b;
                return eval_ok(value_float(r));
            }
            case BINOP_EQ:   return eval_ok(value_bool(a == b));
            case BINOP_NEQ:  return eval_ok(value_bool(a != b));
            case BINOP_LT:   return eval_ok(value_bool(a < b));
            case BINOP_GT:   return eval_ok(value_bool(a > b));
            case BINOP_LTEQ: return eval_ok(value_bool(a <= b));
            case BINOP_GTEQ: return eval_ok(value_bool(a >= b));
            default: break;
        }
    }
    /* Mixed int/float */
    if ((lv->type == VAL_INT && rv->type == VAL_FLOAT) ||
        (lv->type == VAL_FLOAT && rv->type == VAL_INT)) {
        double a = lv->type == VAL_FLOAT ? lv->as.float_val : (double)lv->as.int_val;
        double b = rv->type == VAL_FLOAT ? rv->as.float_val : (double)rv->as.int_val;
        switch (op) {
            case BINOP_ADD: return eval_ok(value_float(a + b));
            case BINOP_SUB: return eval_ok(value_float(a - b));
            case BINOP_MUL: return eval_ok(value_float(a * b));
            case BINOP_DIV: return eval_ok(value_float(a / b));
            default: break;
        }
    }
    /* String concatenation */
    if (lv->type == VAL_STR && rv->type == VAL_STR && op == BINOP_ADD) {
        size_t al = strlen(lv->as.str_val), bl = strlen(rv->as.str_val);
        char *buf = malloc(al + bl + 1);
        memcpy(buf, lv->as.str_val, al);
        memcpy(buf + al, rv->as.str_val, bl);
        buf[al + bl] = '\0';
        return eval_ok(value_string_owned(buf));
    }
    /* String comparison */
    if (lv->type == VAL_STR && rv->type == VAL_STR) {
        if (op == BINOP_EQ) return eval_ok(value_bool(strcmp(lv->as.str_val, rv->as.str_val) == 0));
        if (op == BINOP_NEQ) return eval_ok(value_bool(strcmp(lv->as.str_val, rv->as.str_val) != 0));
    }
    /* Bool comparison */
    if (lv->type == VAL_BOOL && rv->type == VAL_BOOL) {
        if (op == BINOP_EQ) return eval_ok(value_bool(lv->as.bool_val == rv->as.bool_val));
        if (op == BINOP_NEQ) return eval_ok(value_bool(lv->as.bool_val != rv->as.bool_val));
        if (op == BINOP_AND) return eval_ok(value_bool(lv->as.bool_val && rv->as.bool_val));
        if (op == BINOP_OR) return eval_ok(value_bool(lv->as.bool_val || rv->as.bool_val));
    }
    /* Nil equality: nil == nil is true, nil == anything_else is false */
    if ((op == BINOP_EQ || op == BINOP_NEQ) &&
        (lv->type == VAL_NIL || rv->type == VAL_NIL)) {
        bool eq = (lv->type == VAL_NIL && rv->type == VAL_NIL);
        return eval_ok(value_bool(op == BINOP_EQ ? eq : !eq));
    }
    /* General equality using value_eq (enums, arrays, structs, etc.) */
    if (lv->type == rv->type && (op == BINOP_EQ || op == BINOP_NEQ)) {
        bool eq = value_eq(lv, rv);
        return eval_ok(value_bool(op == BINOP_EQ ? eq : !eq));
    }

    char *err = NULL;
    (void)asprintf(&err, "unsupported binary operation on %s and %s",
                   value_type_name(lv), value_type_name(rv));
    return eval_err(err);
}

static EvalResult eval_unaryop(UnaryOpKind op, LatValue *val) {
    if (op == UNOP_NEG && val->type == VAL_INT) return eval_ok(value_int(-val->as.int_val));
    if (op == UNOP_NEG && val->type == VAL_FLOAT) return eval_ok(value_float(-val->as.float_val));
    if (op == UNOP_NOT && val->type == VAL_BOOL) return eval_ok(value_bool(!val->as.bool_val));
    if (op == UNOP_BIT_NOT && val->type == VAL_INT) return eval_ok(value_int(~val->as.int_val));
    char *err = NULL;
    (void)asprintf(&err, "unsupported unary operation on %s", value_type_name(val));
    return eval_err(err);
}

/* ── Concurrency infrastructure ── */

#ifndef __EMSCRIPTEN__
typedef struct {
    Stmt      **stmts;
    size_t      stmt_count;
    Evaluator  *child_ev;
    char       *error;       /* NULL on success */
    pthread_t   thread;
} SpawnTask;

static Evaluator *create_child_evaluator(Evaluator *parent) {
    Evaluator *child = calloc(1, sizeof(Evaluator));
    child->env = env_clone(parent->env);
    child->mode = parent->mode;
    /* Share AST pointers (borrowed, immutable after parse) */
    child->struct_defs = lat_map_new(sizeof(StructDecl *));
    for (size_t i = 0; i < parent->struct_defs.cap; i++) {
        if (parent->struct_defs.entries[i].state == MAP_OCCUPIED) {
            lat_map_set(&child->struct_defs,
                        parent->struct_defs.entries[i].key,
                        parent->struct_defs.entries[i].value);
        }
    }
    child->fn_defs = lat_map_new(sizeof(FnDecl *));
    for (size_t i = 0; i < parent->fn_defs.cap; i++) {
        if (parent->fn_defs.entries[i].state == MAP_OCCUPIED) {
            lat_map_set(&child->fn_defs,
                        parent->fn_defs.entries[i].key,
                        parent->fn_defs.entries[i].value);
        }
    }
    stats_init(&child->stats);
    child->heap = dual_heap_new();
    child->gc_roots = lat_vec_new(sizeof(LatValue *));
    child->saved_envs = lat_vec_new(sizeof(Env *));
    child->gc_stress = parent->gc_stress;
    child->no_regions = parent->no_regions;
    child->required_files = lat_map_new(sizeof(bool));
    child->script_dir = parent->script_dir ? strdup(parent->script_dir) : NULL;
    return child;
}

static void free_child_evaluator(Evaluator *child) {
    if (!child) return;
    env_free(child->env);
    lat_map_free(&child->struct_defs);
    lat_map_free(&child->fn_defs);
    lat_map_free(&child->required_files);
    free(child->script_dir);
    value_set_heap(NULL);
    dual_heap_free(child->heap);
    lat_vec_free(&child->gc_roots);
    lat_vec_free(&child->saved_envs);
    free(child);
}

static void *spawn_thread_fn(void *arg) {
    SpawnTask *task = (SpawnTask *)arg;
    Evaluator *child = task->child_ev;

    /* Set thread-local heap for this child evaluator */
    value_set_heap(child->heap);
    value_set_arena(NULL);

    EvalResult result = eval_block_stmts(child, task->stmts, task->stmt_count);

    if (IS_ERR(result)) {
        task->error = result.error;  /* transfer ownership */
    } else if (IS_SIGNAL(result)) {
        switch (result.cf.tag) {
            case CF_RETURN:
                task->error = strdup("cannot use 'return' inside spawn");
                value_free(&result.cf.value);
                break;
            case CF_BREAK:
                task->error = strdup("cannot use 'break' inside spawn");
                break;
            case CF_CONTINUE:
                task->error = strdup("cannot use 'continue' inside spawn");
                break;
            default:
                break;
        }
    } else {
        value_free(&result.value);
    }

    return NULL;
}
#endif /* __EMSCRIPTEN__ */

/* ── Expression evaluation ── */

static EvalResult eval_expr_inner(Evaluator *ev, const Expr *expr) {
    switch (expr->tag) {
        case EXPR_INT_LIT:    return eval_ok(value_int(expr->as.int_val));
        case EXPR_FLOAT_LIT:  return eval_ok(value_float(expr->as.float_val));
        case EXPR_STRING_LIT: return eval_ok(value_string(expr->as.str_val));
        case EXPR_BOOL_LIT:   return eval_ok(value_bool(expr->as.bool_val));
        case EXPR_NIL_LIT:    return eval_ok(value_nil());

        case EXPR_IDENT: {
            LatValue val;
            if (!env_get(ev->env, expr->as.str_val, &val)) {
                char *err = NULL;
                const char *suggestion = env_find_similar_name(ev->env, expr->as.str_val);
                if (suggestion)
                    (void)asprintf(&err, "undefined variable '%s' (did you mean '%s'?)", expr->as.str_val, suggestion);
                else
                    (void)asprintf(&err, "undefined variable '%s'", expr->as.str_val);
                return eval_err(err);
            }
            return eval_ok(val);
        }

        case EXPR_BINOP: {
            /* Short-circuit: ?? (nil coalescing) */
            if (expr->as.binop.op == BINOP_NIL_COALESCE) {
                EvalResult lr = eval_expr(ev, expr->as.binop.left);
                if (!IS_OK(lr)) return lr;
                if (lr.value.type != VAL_NIL) return lr;
                value_free(&lr.value);
                return eval_expr(ev, expr->as.binop.right);
            }
            EvalResult lr = eval_expr(ev, expr->as.binop.left);
            if (!IS_OK(lr)) return lr;
            GC_PUSH(ev, &lr.value);
            EvalResult rr = eval_expr(ev, expr->as.binop.right);
            GC_POP(ev);
            if (!IS_OK(rr)) { value_free(&lr.value); return rr; }
            EvalResult res = eval_binop(expr->as.binop.op, &lr.value, &rr.value);
            value_free(&lr.value);
            value_free(&rr.value);
            return res;
        }

        case EXPR_UNARYOP: {
            EvalResult vr = eval_expr(ev, expr->as.unaryop.operand);
            if (!IS_OK(vr)) return vr;
            EvalResult res = eval_unaryop(expr->as.unaryop.op, &vr.value);
            value_free(&vr.value);
            return res;
        }

        case EXPR_CALL: {
            /* ── track(x) / history(x) / phases(x) / rewind(x, n): convert ident to string ── */
            if (expr->as.call.func->tag == EXPR_IDENT) {
                const char *cfn = expr->as.call.func->as.str_val;
                bool is_1arg = (strcmp(cfn, "track") == 0 || strcmp(cfn, "history") == 0 ||
                                strcmp(cfn, "phases") == 0);
                bool is_rewind = strcmp(cfn, "rewind") == 0;
                if ((is_1arg && expr->as.call.arg_count == 1 &&
                     expr->as.call.args[0]->tag == EXPR_IDENT) ||
                    (is_rewind && expr->as.call.arg_count == 2 &&
                     expr->as.call.args[0]->tag == EXPR_IDENT)) {
                    /* Temporarily swap first arg to a string literal with the variable name */
                    Expr *orig = expr->as.call.args[0];
                    Expr tmp_str;
                    tmp_str.tag = EXPR_STRING_LIT;
                    tmp_str.as.str_val = orig->as.str_val;
                    tmp_str.line = orig->line;
                    expr->as.call.args[0] = &tmp_str;
                    EvalResult r = eval_expr(ev, expr);
                    expr->as.call.args[0] = orig; /* restore */
                    return r;
                }
            }
            /* ── react() / unreact(): special handling before arg evaluation ── */
            if (expr->as.call.func->tag == EXPR_IDENT &&
                strcmp(expr->as.call.func->as.str_val, "react") == 0) {
                size_t argc = expr->as.call.arg_count;
                if (argc != 2) {
                    return eval_err(strdup("react() requires exactly 2 arguments (variable, callback)"));
                }
                if (expr->as.call.args[0]->tag != EXPR_IDENT) {
                    return eval_err(strdup("react() first argument must be a variable name"));
                }
                const char *var_name = expr->as.call.args[0]->as.str_val;
                /* Verify variable exists */
                LatValue tmp;
                if (!env_get(ev->env, var_name, &tmp)) {
                    char *err = NULL;
                    (void)asprintf(&err, "cannot react to undefined variable '%s'", var_name);
                    return eval_err(err);
                }
                value_free(&tmp);
                /* Evaluate the callback */
                EvalResult cbr = eval_expr(ev, expr->as.call.args[1]);
                if (!IS_OK(cbr)) return cbr;
                if (cbr.value.type != VAL_CLOSURE) {
                    value_free(&cbr.value);
                    return eval_err(strdup("react() second argument must be a closure"));
                }
                /* Find or create ReactionEntry for var_name */
                ReactionEntry *re = NULL;
                for (size_t i = 0; i < ev->reaction_count; i++) {
                    if (strcmp(ev->reactions[i].var_name, var_name) == 0) {
                        re = &ev->reactions[i];
                        break;
                    }
                }
                if (!re) {
                    if (ev->reaction_count >= ev->reaction_cap) {
                        ev->reaction_cap = ev->reaction_cap ? ev->reaction_cap * 2 : 4;
                        ev->reactions = realloc(ev->reactions, ev->reaction_cap * sizeof(ReactionEntry));
                    }
                    re = &ev->reactions[ev->reaction_count++];
                    re->var_name = strdup(var_name);
                    re->callbacks = NULL;
                    re->cb_count = 0;
                    re->cb_cap = 0;
                }
                /* Add the callback */
                if (re->cb_count >= re->cb_cap) {
                    re->cb_cap = re->cb_cap ? re->cb_cap * 2 : 4;
                    re->callbacks = realloc(re->callbacks, re->cb_cap * sizeof(LatValue));
                }
                re->callbacks[re->cb_count++] = value_deep_clone(&cbr.value);
                value_free(&cbr.value);
                return eval_ok(value_unit());
            }
            if (expr->as.call.func->tag == EXPR_IDENT &&
                strcmp(expr->as.call.func->as.str_val, "unreact") == 0) {
                size_t argc = expr->as.call.arg_count;
                if (argc != 1) {
                    return eval_err(strdup("unreact() requires exactly 1 argument (variable)"));
                }
                if (expr->as.call.args[0]->tag != EXPR_IDENT) {
                    return eval_err(strdup("unreact() argument must be a variable name"));
                }
                const char *var_name = expr->as.call.args[0]->as.str_val;
                /* Find and remove ReactionEntry */
                for (size_t i = 0; i < ev->reaction_count; i++) {
                    if (strcmp(ev->reactions[i].var_name, var_name) == 0) {
                        free(ev->reactions[i].var_name);
                        for (size_t j = 0; j < ev->reactions[i].cb_count; j++)
                            value_free(&ev->reactions[i].callbacks[j]);
                        free(ev->reactions[i].callbacks);
                        ev->reactions[i] = ev->reactions[--ev->reaction_count];
                        break;
                    }
                }
                return eval_ok(value_unit());
            }
            /* ── bond() / unbond(): special handling before arg evaluation ── */
            if (expr->as.call.func->tag == EXPR_IDENT &&
                (strcmp(expr->as.call.func->as.str_val, "bond") == 0 ||
                 strcmp(expr->as.call.func->as.str_val, "unbond") == 0)) {
                bool is_bond = strcmp(expr->as.call.func->as.str_val, "bond") == 0;
                size_t argc = expr->as.call.arg_count;
                if (argc < 2) {
                    return eval_err(strdup(is_bond
                        ? "bond() requires at least 2 arguments (target, dep[, strategy])"
                        : "unbond() requires at least 2 arguments (target, ...deps)"));
                }
                /* First 2 arguments must be identifiers; optional 3rd can be string (strategy) */
                if (expr->as.call.args[0]->tag != EXPR_IDENT ||
                    expr->as.call.args[1]->tag != EXPR_IDENT) {
                    return eval_err(strdup(is_bond
                        ? "bond() requires variable names for first two arguments"
                        : "unbond() requires variable names, not expressions"));
                }
                if (!is_bond) {
                    for (size_t i = 2; i < argc; i++) {
                        if (expr->as.call.args[i]->tag != EXPR_IDENT) {
                            return eval_err(strdup("unbond() requires variable names, not expressions"));
                        }
                    }
                }
                const char *target = expr->as.call.args[0]->as.str_val;
                if (is_bond) {
                    /* Determine strategy: if last arg is a string literal, use as strategy.
                     * Otherwise all args after target are deps with default "mirror" strategy.
                     * bond(target, dep) — mirror
                     * bond(target, dep, "inverse") — explicit strategy
                     * bond(target, dep1, dep2, ...) — multiple deps, all mirror */
                    char *strategy = NULL;
                    size_t dep_end = argc;  /* how many args are deps (excluding target at 0) */
                    if (argc >= 3 && expr->as.call.args[argc - 1]->tag == EXPR_STRING_LIT) {
                        /* Last arg is a string literal — treat as strategy */
                        const char *sval = expr->as.call.args[argc - 1]->as.str_val;
                        if (strcmp(sval, "mirror") == 0 || strcmp(sval, "inverse") == 0 ||
                            strcmp(sval, "gate") == 0) {
                            strategy = strdup(sval);
                            dep_end = argc - 1;
                        }
                    }
                    if (!strategy) strategy = strdup("mirror");
                    /* All args from 1..dep_end-1 must be idents (dep vars) */
                    for (size_t i = 1; i < dep_end; i++) {
                        if (expr->as.call.args[i]->tag != EXPR_IDENT) {
                            free(strategy);
                            return eval_err(strdup("bond() dependency arguments must be variable names"));
                        }
                    }
                    /* Verify all variables exist and are not frozen */
                    for (size_t i = 0; i < dep_end; i++) {
                        const char *vname = expr->as.call.args[i]->as.str_val;
                        LatValue tmp;
                        if (!env_get(ev->env, vname, &tmp)) {
                            char *err = NULL;
                            (void)asprintf(&err, "cannot bond undefined variable '%s'", vname);
                            free(strategy);
                            return eval_err(err);
                        }
                        if (tmp.phase == VTAG_CRYSTAL) {
                            value_free(&tmp);
                            char *err = NULL;
                            (void)asprintf(&err, "cannot bond already-frozen variable '%s'", vname);
                            free(strategy);
                            return eval_err(err);
                        }
                        value_free(&tmp);
                    }
                    /* Register bonds for each dep */
                    BondEntry *be = NULL;
                    for (size_t i = 0; i < ev->bond_count; i++) {
                        if (strcmp(ev->bonds[i].target, target) == 0) { be = &ev->bonds[i]; break; }
                    }
                    if (!be) {
                        if (ev->bond_count >= ev->bond_cap) {
                            ev->bond_cap = ev->bond_cap ? ev->bond_cap * 2 : 4;
                            ev->bonds = realloc(ev->bonds, ev->bond_cap * sizeof(BondEntry));
                        }
                        be = &ev->bonds[ev->bond_count++];
                        be->target = strdup(target);
                        be->deps = NULL;
                        be->dep_strategies = NULL;
                        be->dep_count = 0;
                        be->dep_cap = 0;
                    }
                    for (size_t di = 1; di < dep_end; di++) {
                        const char *dep = expr->as.call.args[di]->as.str_val;
                        if (be->dep_count >= be->dep_cap) {
                            be->dep_cap = be->dep_cap ? be->dep_cap * 2 : 4;
                            be->deps = realloc(be->deps, be->dep_cap * sizeof(char *));
                            be->dep_strategies = realloc(be->dep_strategies, be->dep_cap * sizeof(char *));
                        }
                        be->deps[be->dep_count] = strdup(dep);
                        be->dep_strategies[be->dep_count] = strdup(strategy);
                        be->dep_count++;
                    }
                    free(strategy);
                } else {
                    /* unbond: remove deps from target's bond set */
                    for (size_t i = 0; i < ev->bond_count; i++) {
                        if (strcmp(ev->bonds[i].target, target) == 0) {
                            for (size_t j = 1; j < argc; j++) {
                                const char *dep = expr->as.call.args[j]->as.str_val;
                                for (size_t k = 0; k < ev->bonds[i].dep_count; k++) {
                                    if (strcmp(ev->bonds[i].deps[k], dep) == 0) {
                                        free(ev->bonds[i].deps[k]);
                                        if (ev->bonds[i].dep_strategies) free(ev->bonds[i].dep_strategies[k]);
                                        ev->bonds[i].deps[k] = ev->bonds[i].deps[ev->bonds[i].dep_count - 1];
                                        if (ev->bonds[i].dep_strategies)
                                            ev->bonds[i].dep_strategies[k] = ev->bonds[i].dep_strategies[ev->bonds[i].dep_count - 1];
                                        ev->bonds[i].dep_count--;
                                        break;
                                    }
                                }
                            }
                            /* Remove entry if empty */
                            if (ev->bonds[i].dep_count == 0) {
                                free(ev->bonds[i].target);
                                free(ev->bonds[i].deps);
                                free(ev->bonds[i].dep_strategies);
                                ev->bonds[i] = ev->bonds[--ev->bond_count];
                            }
                            break;
                        }
                    }
                }
                return eval_ok(value_unit());
            }

            /* ── seed() / unseed(): special handling before arg evaluation ── */
            if (expr->as.call.func->tag == EXPR_IDENT &&
                strcmp(expr->as.call.func->as.str_val, "seed") == 0) {
                size_t argc = expr->as.call.arg_count;
                if (argc != 2) return eval_err(strdup("seed() requires exactly 2 arguments (variable, contract)"));
                if (expr->as.call.args[0]->tag != EXPR_IDENT)
                    return eval_err(strdup("seed() first argument must be a variable name"));
                const char *var_name = expr->as.call.args[0]->as.str_val;
                LatValue tmp;
                if (!env_get(ev->env, var_name, &tmp)) {
                    char *err = NULL;
                    (void)asprintf(&err, "seed(): undefined variable '%s'", var_name);
                    return eval_err(err);
                }
                value_free(&tmp);
                EvalResult cbr = eval_expr(ev, expr->as.call.args[1]);
                if (!IS_OK(cbr)) return cbr;
                if (cbr.value.type != VAL_CLOSURE) {
                    value_free(&cbr.value);
                    return eval_err(strdup("seed() second argument must be a closure"));
                }
                /* Store seed entry */
                if (ev->seed_count >= ev->seed_cap) {
                    ev->seed_cap = ev->seed_cap ? ev->seed_cap * 2 : 4;
                    ev->seeds = realloc(ev->seeds, ev->seed_cap * sizeof(SeedEntry));
                }
                ev->seeds[ev->seed_count].var_name = strdup(var_name);
                ev->seeds[ev->seed_count].contract = value_deep_clone(&cbr.value);
                ev->seed_count++;
                value_free(&cbr.value);
                return eval_ok(value_unit());
            }
            if (expr->as.call.func->tag == EXPR_IDENT &&
                strcmp(expr->as.call.func->as.str_val, "unseed") == 0) {
                size_t argc = expr->as.call.arg_count;
                if (argc != 1) return eval_err(strdup("unseed() requires exactly 1 argument (variable)"));
                if (expr->as.call.args[0]->tag != EXPR_IDENT)
                    return eval_err(strdup("unseed() argument must be a variable name"));
                const char *var_name = expr->as.call.args[0]->as.str_val;
                for (size_t i = 0; i < ev->seed_count; i++) {
                    if (strcmp(ev->seeds[i].var_name, var_name) == 0) {
                        free(ev->seeds[i].var_name);
                        value_free(&ev->seeds[i].contract);
                        ev->seeds[i] = ev->seeds[--ev->seed_count];
                        break;
                    }
                }
                return eval_ok(value_unit());
            }
            /* ── pressurize() / depressurize(): special handling before arg evaluation ── */
            if (expr->as.call.func->tag == EXPR_IDENT &&
                strcmp(expr->as.call.func->as.str_val, "pressurize") == 0) {
                size_t argc = expr->as.call.arg_count;
                if (argc != 2) return eval_err(strdup("pressurize() requires 2 arguments (variable, mode)"));
                if (expr->as.call.args[0]->tag != EXPR_IDENT)
                    return eval_err(strdup("pressurize() first argument must be a variable name"));
                const char *var_name = expr->as.call.args[0]->as.str_val;
                LatValue tmp;
                if (!env_get(ev->env, var_name, &tmp)) {
                    char *err = NULL;
                    (void)asprintf(&err, "pressurize(): undefined variable '%s'", var_name);
                    return eval_err(err);
                }
                value_free(&tmp);
                EvalResult mr = eval_expr(ev, expr->as.call.args[1]);
                if (!IS_OK(mr)) return mr;
                if (mr.value.type != VAL_STR) {
                    value_free(&mr.value);
                    return eval_err(strdup("pressurize() mode must be a string"));
                }
                const char *mode = mr.value.as.str_val;
                if (strcmp(mode, "no_grow") != 0 && strcmp(mode, "no_shrink") != 0 &&
                    strcmp(mode, "no_resize") != 0 && strcmp(mode, "read_heavy") != 0) {
                    char *err = NULL;
                    (void)asprintf(&err, "pressurize() unknown mode '%s'", mode);
                    value_free(&mr.value);
                    return eval_err(err);
                }
                /* Find or create PressureEntry */
                PressureEntry *pe = NULL;
                for (size_t i = 0; i < ev->pressure_count; i++) {
                    if (strcmp(ev->pressures[i].var_name, var_name) == 0) { pe = &ev->pressures[i]; break; }
                }
                if (pe) {
                    free(pe->mode);
                    pe->mode = strdup(mode);
                } else {
                    if (ev->pressure_count >= ev->pressure_cap) {
                        ev->pressure_cap = ev->pressure_cap ? ev->pressure_cap * 2 : 4;
                        ev->pressures = realloc(ev->pressures, ev->pressure_cap * sizeof(PressureEntry));
                    }
                    pe = &ev->pressures[ev->pressure_count++];
                    pe->var_name = strdup(var_name);
                    pe->mode = strdup(mode);
                }
                value_free(&mr.value);
                return eval_ok(value_unit());
            }
            if (expr->as.call.func->tag == EXPR_IDENT &&
                strcmp(expr->as.call.func->as.str_val, "depressurize") == 0) {
                size_t argc = expr->as.call.arg_count;
                if (argc != 1) return eval_err(strdup("depressurize() requires 1 argument (variable)"));
                if (expr->as.call.args[0]->tag != EXPR_IDENT)
                    return eval_err(strdup("depressurize() argument must be a variable name"));
                const char *var_name = expr->as.call.args[0]->as.str_val;
                for (size_t i = 0; i < ev->pressure_count; i++) {
                    if (strcmp(ev->pressures[i].var_name, var_name) == 0) {
                        free(ev->pressures[i].var_name);
                        free(ev->pressures[i].mode);
                        ev->pressures[i] = ev->pressures[--ev->pressure_count];
                        break;
                    }
                }
                return eval_ok(value_unit());
            }

            /* Evaluate arguments */
            size_t argc = expr->as.call.arg_count;
            LatValue *args = malloc(argc * sizeof(LatValue));
            for (size_t i = 0; i < argc; i++) {
                EvalResult ar = eval_expr(ev, expr->as.call.args[i]);
                if (!IS_OK(ar)) {
                    GC_POP_N(ev, i);
                    for (size_t j = 0; j < i; j++) value_free(&args[j]);
                    free(args);
                    return ar;
                }
                args[i] = ar.value;
                GC_PUSH(ev, &args[i]);
            }
            GC_POP_N(ev, argc);
            /* Check for named function or built-in */
            if (expr->as.call.func->tag == EXPR_IDENT) {
                const char *fn_name = expr->as.call.func->as.str_val;

                /* ── Built-in functions ── */

                /// @builtin input(prompt?: String) -> String
                /// @category Core
                /// Read a line of input from stdin, optionally displaying a prompt.
                /// @example input("Name: ")  // reads user input
                if (strcmp(fn_name, "input") == 0) {
                    const char *prompt = NULL;
                    if (argc > 0 && args[0].type == VAL_STR) prompt = args[0].as.str_val;
                    char *line = builtin_input(prompt);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!line) return eval_ok(value_unit());
                    return eval_ok(value_string_owned(line));
                }

                /// @builtin is_complete(source: String) -> Bool
                /// @category Metaprogramming
                /// Check if a source string is a complete expression (balanced brackets).
                /// @example is_complete("{ 1 + 2 }")  // true
                if (strcmp(fn_name, "is_complete") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("is_complete() expects 1 string argument")); }
                    const char *source = args[0].as.str_val;
                    Lexer lex = lexer_new(source);
                    char *lex_err = NULL;
                    LatVec toks = lexer_tokenize(&lex, &lex_err);
                    if (lex_err) {
                        /* Lex error (unclosed string etc.) = incomplete */
                        free(lex_err);
                        lat_vec_free(&toks);
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_ok(value_bool(false));
                    }
                    int depth = 0;
                    for (size_t j = 0; j < toks.len; j++) {
                        Token *t = lat_vec_get(&toks, j);
                        switch (t->type) {
                            case TOK_LBRACE: case TOK_LPAREN: case TOK_LBRACKET:
                                depth++;
                                break;
                            case TOK_RBRACE: case TOK_RPAREN: case TOK_RBRACKET:
                                depth--;
                                break;
                            default:
                                break;
                        }
                    }
                    for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
                    lat_vec_free(&toks);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_bool(depth <= 0));
                }

                /// @builtin typeof(val: Any) -> String
                /// @category Core
                /// Returns the type name of a value as a string.
                /// @example typeof(42)  // "Int"
                if (strcmp(fn_name, "typeof") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("typeof() expects 1 argument")); }
                    const char *tn = builtin_typeof_str(&args[0]);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string(tn));
                }

                /// @builtin struct_name(val: Struct) -> String
                /// @category Reflection
                /// Returns the type name of a struct instance.
                /// @example struct_name(user)  // "User"
                if (strcmp(fn_name, "struct_name") == 0) {
                    if (argc != 1 || args[0].type != VAL_STRUCT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("struct_name() expects 1 Struct argument")); }
                    const char *sn = args[0].as.strct.name;
                    LatValue result = value_string(sn);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(result);
                }

                /// @builtin struct_fields(val: Struct) -> Array
                /// @category Reflection
                /// Returns an array of field name strings from a struct instance.
                /// @example struct_fields(user)  // ["name", "age"]
                if (strcmp(fn_name, "struct_fields") == 0) {
                    if (argc != 1 || args[0].type != VAL_STRUCT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("struct_fields() expects 1 Struct argument")); }
                    size_t fc = args[0].as.strct.field_count;
                    LatValue *elems = malloc((fc > 0 ? fc : 1) * sizeof(LatValue));
                    for (size_t j = 0; j < fc; j++) {
                        elems[j] = value_string(args[0].as.strct.field_names[j]);
                    }
                    LatValue result = value_array(elems, fc);
                    free(elems);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(result);
                }

                /// @builtin struct_to_map(val: Struct) -> Map
                /// @category Reflection
                /// Converts a struct instance to a Map of {field_name: value}.
                /// @example struct_to_map(user).get("name")  // "Alice"
                if (strcmp(fn_name, "struct_to_map") == 0) {
                    if (argc != 1 || args[0].type != VAL_STRUCT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("struct_to_map() expects 1 Struct argument")); }
                    LatValue map = value_map_new();
                    size_t fc = args[0].as.strct.field_count;
                    for (size_t j = 0; j < fc; j++) {
                        LatValue v = value_deep_clone(&args[0].as.strct.field_values[j]);
                        lat_map_set(map.as.map.map, args[0].as.strct.field_names[j], &v);
                    }
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(map);
                }

                /// @builtin struct_from_map(name: String, map: Map) -> Struct
                /// @category Reflection
                /// Creates a struct instance from a type name and a Map of field values.
                /// Missing fields default to nil.
                /// @example struct_from_map("User", m)
                if (strcmp(fn_name, "struct_from_map") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_MAP) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("struct_from_map() expects (name: String, map: Map)")); }
                    const char *sname = args[0].as.str_val;
                    StructDecl *sd = find_struct(ev, sname);
                    if (!sd) {
                        char *err = NULL;
                        (void)asprintf(&err, "struct_from_map: undefined struct '%s'", sname);
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(err);
                    }
                    size_t fc = sd->field_count;
                    char **names = malloc(fc * sizeof(char *));
                    LatValue *vals = malloc(fc * sizeof(LatValue));
                    for (size_t j = 0; j < fc; j++) {
                        names[j] = sd->fields[j].name;
                        LatValue *found = (LatValue *)lat_map_get(args[1].as.map.map, sd->fields[j].name);
                        if (found) {
                            vals[j] = value_deep_clone(found);
                        } else {
                            vals[j] = value_nil();
                        }
                    }
                    stats_struct(&ev->stats);
                    LatValue st = value_struct(sname, names, vals, fc);
                    free(names);
                    free(vals);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(st);
                }

                /// @builtin phase_of(val: Any) -> String
                /// @category Core
                /// Returns the phase of a value ("flux", "fix", or "crystal").
                /// @example phase_of(freeze([1, 2]))  // "crystal"
                if (strcmp(fn_name, "phase_of") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("phase_of() expects 1 argument")); }
                    const char *pn = builtin_phase_of_str(&args[0]);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string(pn));
                }

                /// @builtin to_string(val: Any) -> String
                /// @category Core
                /// Convert any value to its string representation.
                /// @example to_string(42)  // "42"
                if (strcmp(fn_name, "to_string") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("to_string() expects 1 argument")); }
                    char *s = builtin_to_string(&args[0]);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string_owned(s));
                }

                /// @builtin repr(val: Any) -> String
                /// @category Core
                /// Return the repr string of a value.  Strings are quoted,
                /// structs with a `repr` closure field use the custom representation.
                /// @example repr(42)        // "42"
                /// @example repr("hello")   // "\"hello\""
                if (strcmp(fn_name, "repr") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("repr() expects 1 argument")); }
                    char *s = eval_repr(ev, &args[0]);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string_owned(s));
                }

                /// @builtin track(name: String) -> Unit
                /// @category Temporal
                /// Enable phase history tracking for a variable.
                /// @example track("counter")
                if (strcmp(fn_name, "track") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("track() expects 1 String argument")); }
                    const char *vname = args[0].as.str_val;
                    LatValue cur;
                    if (!env_get(ev->env, vname, &cur)) {
                        char *err = NULL; (void)asprintf(&err, "track(): undefined variable '%s'", vname);
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(err);
                    }
                    /* Check if already tracked */
                    bool already = false;
                    for (size_t i = 0; i < ev->tracked_count; i++) {
                        if (strcmp(ev->tracked_vars[i].name, vname) == 0) { already = true; break; }
                    }
                    if (!already) {
                        if (ev->tracked_count >= ev->tracked_cap) {
                            ev->tracked_cap = ev->tracked_cap ? ev->tracked_cap * 2 : 4;
                            ev->tracked_vars = realloc(ev->tracked_vars, ev->tracked_cap * sizeof(TrackedVar));
                        }
                        TrackedVar *tv = &ev->tracked_vars[ev->tracked_count++];
                        tv->name = strdup(vname);
                        tv->history.snapshots = NULL;
                        tv->history.count = 0;
                        tv->history.cap = 0;
                        /* Record initial snapshot */
                        const char *phase = builtin_phase_of_str(&cur);
                        if (tv->history.count >= tv->history.cap) {
                            tv->history.cap = tv->history.cap ? tv->history.cap * 2 : 8;
                            tv->history.snapshots = realloc(tv->history.snapshots, tv->history.cap * sizeof(HistorySnapshot));
                        }
                        tv->history.snapshots[tv->history.count].phase_name = strdup(phase);
                        tv->history.snapshots[tv->history.count].value = value_deep_clone(&cur);
                        tv->history.snapshots[tv->history.count].line = 0;
                        tv->history.snapshots[tv->history.count].fn_name = NULL;
                        tv->history.count++;
                    }
                    value_free(&cur);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_unit());
                }

                /// @builtin phases(name: String) -> Array
                /// @category Temporal
                /// Returns the phase history of a tracked variable as an array of Maps.
                /// @example phases("counter")  // [{phase: "fluid", value: 0}, ...]
                if (strcmp(fn_name, "phases") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("phases() expects 1 String argument")); }
                    const char *vname = args[0].as.str_val;
                    VariableHistory *vh = NULL;
                    for (size_t i = 0; i < ev->tracked_count; i++) {
                        if (strcmp(ev->tracked_vars[i].name, vname) == 0) { vh = &ev->tracked_vars[i].history; break; }
                    }
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    if (!vh || vh->count == 0) return eval_ok(value_array(NULL, 0));
                    LatValue *elems = malloc(vh->count * sizeof(LatValue));
                    for (size_t i = 0; i < vh->count; i++) {
                        LatValue m = value_map_new();
                        LatValue pv = value_string(vh->snapshots[i].phase_name);
                        LatValue vv = value_deep_clone(&vh->snapshots[i].value);
                        LatValue lv = value_int(vh->snapshots[i].line);
                        LatValue fv = vh->snapshots[i].fn_name ? value_string(vh->snapshots[i].fn_name) : value_nil();
                        lat_map_set(m.as.map.map, "phase", &pv);
                        lat_map_set(m.as.map.map, "value", &vv);
                        lat_map_set(m.as.map.map, "line", &lv);
                        lat_map_set(m.as.map.map, "fn", &fv);
                        elems[i] = m;
                    }
                    LatValue result = value_array(elems, vh->count);
                    free(elems);
                    return eval_ok(result);
                }

                /// @builtin history(name: String) -> Array
                /// @category Temporal
                /// Returns the full enriched timeline of a tracked variable as an array of Maps
                /// with keys: phase, value, line, fn.
                /// @example history(x)  // [{phase: "fluid", value: 10, line: 3, fn: "main"}, ...]
                if (strcmp(fn_name, "history") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("history() expects 1 String argument")); }
                    const char *vname = args[0].as.str_val;
                    VariableHistory *vh = NULL;
                    for (size_t i = 0; i < ev->tracked_count; i++) {
                        if (strcmp(ev->tracked_vars[i].name, vname) == 0) { vh = &ev->tracked_vars[i].history; break; }
                    }
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    if (!vh || vh->count == 0) return eval_ok(value_array(NULL, 0));
                    LatValue *elems = malloc(vh->count * sizeof(LatValue));
                    for (size_t i = 0; i < vh->count; i++) {
                        LatValue m = value_map_new();
                        LatValue pv = value_string(vh->snapshots[i].phase_name);
                        LatValue vv = value_deep_clone(&vh->snapshots[i].value);
                        LatValue lv = value_int(vh->snapshots[i].line);
                        LatValue fv = vh->snapshots[i].fn_name ? value_string(vh->snapshots[i].fn_name) : value_nil();
                        lat_map_set(m.as.map.map, "phase", &pv);
                        lat_map_set(m.as.map.map, "value", &vv);
                        lat_map_set(m.as.map.map, "line", &lv);
                        lat_map_set(m.as.map.map, "fn", &fv);
                        elems[i] = m;
                    }
                    LatValue result = value_array(elems, vh->count);
                    free(elems);
                    return eval_ok(result);
                }

                /// @builtin rewind(name: String, n: Int) -> Any
                /// @category Temporal
                /// Returns a deep copy of a tracked variable from n steps ago.
                /// @example rewind("counter", 2)  // value from 2 steps back
                if (strcmp(fn_name, "rewind") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("rewind() expects (String, Int)")); }
                    const char *vname = args[0].as.str_val;
                    int64_t steps = args[1].as.int_val;
                    VariableHistory *vh = NULL;
                    for (size_t i = 0; i < ev->tracked_count; i++) {
                        if (strcmp(ev->tracked_vars[i].name, vname) == 0) { vh = &ev->tracked_vars[i].history; break; }
                    }
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    if (!vh || steps < 0 || (size_t)steps >= vh->count)
                        return eval_ok(value_nil());
                    size_t idx = vh->count - 1 - (size_t)steps;
                    return eval_ok(value_deep_clone(&vh->snapshots[idx].value));
                }

                /// @builtin grow(name: String) -> Any
                /// @category Phase Transitions
                /// Freeze a variable and validate any pending seed contracts.
                /// @example grow(config)  // freeze + validate seeds
                if (strcmp(fn_name, "grow") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("grow() expects 1 String argument (variable name)")); }
                    const char *vname = args[0].as.str_val;
                    LatValue val;
                    if (!env_get(ev->env, vname, &val)) {
                        char *err = NULL; (void)asprintf(&err, "grow(): undefined variable '%s'", vname);
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(err);
                    }
                    /* Check and validate all seeds for this variable */
                    for (size_t si = 0; si < ev->seed_count; si++) {
                        if (strcmp(ev->seeds[si].var_name, vname) != 0) continue;
                        LatValue check_val = value_deep_clone(&val);
                        LatValue *cb = &ev->seeds[si].contract;
                        EvalResult vr = call_closure(ev, cb->as.closure.param_names,
                            cb->as.closure.param_count, cb->as.closure.body,
                            cb->as.closure.captured_env, &check_val, 1,
                            cb->as.closure.default_values, cb->as.closure.has_variadic);
                        if (!IS_OK(vr)) {
                            char *msg = NULL;
                            (void)asprintf(&msg, "grow() seed contract failed: %s", vr.error);
                            free(vr.error); value_free(&val);
                            for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                            return eval_err(msg);
                        }
                        if (!value_is_truthy(&vr.value)) {
                            value_free(&vr.value); value_free(&val);
                            for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                            return eval_err(strdup("grow() seed contract returned false"));
                        }
                        value_free(&vr.value);
                        /* Remove this seed */
                        free(ev->seeds[si].var_name);
                        value_free(&ev->seeds[si].contract);
                        ev->seeds[si] = ev->seeds[--ev->seed_count];
                        si--;  /* re-check this index */
                    }
                    /* Freeze the variable */
                    val = value_freeze(val);
                    freeze_to_region(ev, &val);
                    LatValue ret = value_deep_clone(&val);
                    env_set(ev->env, vname, val);
                    record_history(ev, vname);
                    {
                        char *cascade_err = freeze_cascade(ev, vname);
                        if (cascade_err) {
                            for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                            value_free(&ret);
                            return eval_err(cascade_err);
                        }
                    }
                    EvalResult fr = fire_reactions(ev, vname, "crystal");
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    if (!IS_OK(fr)) { value_free(&ret); return fr; }
                    return eval_ok(ret);
                }

                /// @builtin pressure_of(name: String) -> String|Nil
                /// @category Phase Pressure
                /// Returns the current pressure mode of a variable, or nil if none.
                /// @example pressure_of("data")  // "no_grow"
                if (strcmp(fn_name, "pressure_of") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("pressure_of() expects 1 String argument")); }
                    const char *vname = args[0].as.str_val;
                    for (size_t i = 0; i < ev->pressure_count; i++) {
                        if (strcmp(ev->pressures[i].var_name, vname) == 0) {
                            LatValue result = value_string(ev->pressures[i].mode);
                            for (size_t j = 0; j < argc; j++) { value_free(&args[j]); } free(args);
                            return eval_ok(result);
                        }
                    }
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_nil());
                }

                /// @builtin ord(ch: String) -> Int
                /// @category Type Conversion
                /// Return the Unicode code point of the first character.
                /// @example ord("A")  // 65
                if (strcmp(fn_name, "ord") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("ord() expects 1 string argument")); }
                    int64_t code = builtin_ord(args[0].as.str_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_int(code));
                }

                /// @builtin chr(code: Int) -> String
                /// @category Type Conversion
                /// Return the character for a Unicode code point.
                /// @example chr(65)  // "A"
                if (strcmp(fn_name, "chr") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("chr() expects 1 integer argument")); }
                    char *s = builtin_chr(args[0].as.int_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string_owned(s));
                }

                /// @builtin read_file(path: String) -> String
                /// @category File System
                /// Read the entire contents of a file as a string.
                /// @example read_file("data.txt")  // "file contents..."
                if (strcmp(fn_name, "read_file") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("read_file() expects 1 string argument")); }
                    char *contents = builtin_read_file(args[0].as.str_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!contents) return eval_err(strdup("read_file: could not read file"));
                    return eval_ok(value_string_owned(contents));
                }

                /// @builtin write_file(path: String, content: String) -> Bool
                /// @category File System
                /// Write a string to a file, creating or overwriting it.
                /// @example write_file("out.txt", "hello")  // true
                if (strcmp(fn_name, "write_file") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("write_file() expects 2 string arguments")); }
                    bool wf_ok = builtin_write_file(args[0].as.str_val, args[1].as.str_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!wf_ok) return eval_err(strdup("write_file: could not write file"));
                    return eval_ok(value_bool(true));
                }

                /// @builtin file_exists(path: String) -> Bool
                /// @category File System
                /// Check if a file or directory exists at the given path.
                /// @example file_exists("data.txt")  // true
                if (strcmp(fn_name, "file_exists") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("file_exists() expects 1 string argument")); }
                    bool exists = fs_file_exists(args[0].as.str_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_bool(exists));
                }

                /// @builtin delete_file(path: String) -> Bool
                /// @category File System
                /// Delete a file at the given path.
                /// @example delete_file("temp.txt")  // true
                if (strcmp(fn_name, "delete_file") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("delete_file() expects 1 string argument")); }
                    char *df_err = NULL;
                    bool df_ok = fs_delete_file(args[0].as.str_val, &df_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!df_ok) { char *e = df_err; return eval_err(e); }
                    return eval_ok(value_bool(true));
                }

                /// @builtin list_dir(path: String) -> Array
                /// @category File System
                /// List entries in a directory, returning an array of filenames.
                /// @example list_dir(".")  // ["file1.txt", "dir1", ...]
                if (strcmp(fn_name, "list_dir") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("list_dir() expects 1 string argument")); }
                    char *ld_err = NULL;
                    size_t ld_count = 0;
                    char **ld_entries = fs_list_dir(args[0].as.str_val, &ld_count, &ld_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!ld_entries) { char *e = ld_err; return eval_err(e); }
                    LatValue *elems = malloc(ld_count * sizeof(LatValue));
                    for (size_t i = 0; i < ld_count; i++) {
                        elems[i] = value_string(ld_entries[i]);
                        free(ld_entries[i]);
                    }
                    free(ld_entries);
                    LatValue arr = value_array(elems, ld_count);
                    free(elems);
                    return eval_ok(arr);
                }

                /// @builtin read_file_bytes(path: String) -> Buffer
                /// @category File System
                /// Read the entire contents of a file as a Buffer.
                /// @example read_file_bytes("data.bin")  // Buffer<...>
                if (strcmp(fn_name, "read_file_bytes") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("read_file_bytes() expects 1 string argument")); }
                    FILE *bf = fopen(args[0].as.str_val, "rb");
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!bf) return eval_err(strdup("read_file_bytes: could not read file"));
                    fseek(bf, 0, SEEK_END);
                    long bflen = ftell(bf);
                    fseek(bf, 0, SEEK_SET);
                    if (bflen < 0) { fclose(bf); return eval_err(strdup("read_file_bytes: could not read file")); }
                    uint8_t *bfdata = malloc((size_t)bflen);
                    size_t bfnread = fread(bfdata, 1, (size_t)bflen, bf);
                    fclose(bf);
                    LatValue buf = value_buffer(bfdata, bfnread);
                    free(bfdata);
                    return eval_ok(buf);
                }

                /// @builtin write_file_bytes(path: String, buffer: Buffer) -> Bool
                /// @category File System
                /// Write a Buffer to a file.
                /// @example write_file_bytes("out.bin", buf)  // true
                if (strcmp(fn_name, "write_file_bytes") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_BUFFER) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("write_file_bytes() expects (String, Buffer)")); }
                    FILE *wbf = fopen(args[0].as.str_val, "wb");
                    if (!wbf) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("write_file_bytes: could not write file")); }
                    size_t wbwritten = fwrite(args[1].as.buffer.data, 1, args[1].as.buffer.len, wbf);
                    fclose(wbf);
                    bool wbok = (wbwritten == args[1].as.buffer.len);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_bool(wbok));
                }

                /// @builtin append_file(path: String, content: String) -> Bool
                /// @category File System
                /// Append a string to the end of a file.
                /// @example append_file("log.txt", "new line\n")  // true
                if (strcmp(fn_name, "append_file") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("append_file() expects 2 string arguments")); }
                    char *af_err = NULL;
                    bool af_ok = fs_append_file(args[0].as.str_val, args[1].as.str_val, &af_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!af_ok) { char *e = af_err; return eval_err(e); }
                    return eval_ok(value_bool(true));
                }

                /// @builtin mkdir(path: String) -> Bool
                /// @category File System
                /// Create a directory at the given path.
                /// @example mkdir("new_dir")  // true
                if (strcmp(fn_name, "mkdir") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("mkdir() expects 1 string argument")); }
                    char *mk_err = NULL;
                    bool mk_ok = fs_mkdir(args[0].as.str_val, &mk_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!mk_ok) { free(mk_err); return eval_ok(value_bool(false)); }
                    return eval_ok(value_bool(true));
                }

                /// @builtin rename(old_path: String, new_path: String) -> Bool
                /// @category File System
                /// Rename or move a file or directory.
                /// @example rename("old.txt", "new.txt")  // true
                if (strcmp(fn_name, "rename") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("rename() expects 2 string arguments")); }
                    char *rn_err = NULL;
                    bool rn_ok = fs_rename(args[0].as.str_val, args[1].as.str_val, &rn_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!rn_ok) { free(rn_err); return eval_ok(value_bool(false)); }
                    return eval_ok(value_bool(true));
                }

                /// @builtin is_dir(path: String) -> Bool
                /// @category File System
                /// Check if the path points to a directory.
                /// @example is_dir("/tmp")  // true
                if (strcmp(fn_name, "is_dir") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("is_dir() expects 1 string argument")); }
                    bool result = fs_is_dir(args[0].as.str_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_bool(result));
                }

                /// @builtin is_file(path: String) -> Bool
                /// @category File System
                /// Check if the path points to a regular file.
                /// @example is_file("data.txt")  // true
                if (strcmp(fn_name, "is_file") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("is_file() expects 1 string argument")); }
                    bool result = fs_is_file(args[0].as.str_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_bool(result));
                }

                /// @builtin rmdir(path: String) -> Bool
                /// @category File System
                /// Remove a directory (must be empty).
                /// @example rmdir("old_dir")  // true
                if (strcmp(fn_name, "rmdir") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("rmdir() expects 1 string argument")); }
                    char *rm_err = NULL;
                    bool rm_ok = fs_rmdir(args[0].as.str_val, &rm_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!rm_ok) { char *e = rm_err; return eval_err(e); }
                    return eval_ok(value_bool(true));
                }

                /// @builtin glob(pattern: String) -> Array
                /// @category File System
                /// Find files matching a glob pattern, returning an array of paths.
                /// @example glob("*.txt")  // ["a.txt", "b.txt"]
                if (strcmp(fn_name, "glob") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("glob() expects 1 string argument")); }
                    char *gl_err = NULL;
                    size_t gl_count = 0;
                    char **gl_entries = fs_glob(args[0].as.str_val, &gl_count, &gl_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (gl_err) { return eval_err(gl_err); }
                    /* Build array of strings */
                    LatValue *elems = NULL;
                    if (gl_count > 0) {
                        elems = malloc(gl_count * sizeof(LatValue));
                        for (size_t i = 0; i < gl_count; i++) {
                            elems[i] = value_string_owned(gl_entries[i]);
                        }
                        free(gl_entries);
                    }
                    LatValue arr = value_array(elems, gl_count);
                    free(elems);
                    return eval_ok(arr);
                }

                /// @builtin stat(path: String) -> Map
                /// @category File System
                /// Get file metadata (size, mtime, type, permissions) as a map.
                /// @example stat("file.txt")  // {size: 1024, mtime: ..., type: "file", permissions: 644}
                if (strcmp(fn_name, "stat") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("stat() expects 1 string argument")); }
                    int64_t st_size = 0, st_mtime = 0, st_mode = 0;
                    const char *st_type = NULL;
                    char *st_err = NULL;
                    bool st_ok = fs_stat(args[0].as.str_val, &st_size, &st_mtime, &st_mode, &st_type, &st_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!st_ok) { return eval_err(st_err); }
                    /* Build result Map — lat_map_set does shallow memcpy,
                     * so the map takes ownership of internal pointers.
                     * Do NOT value_free the temporaries. */
                    LatValue map = value_map_new();
                    LatValue sz = value_int(st_size);
                    lat_map_set(map.as.map.map, "size", &sz);
                    LatValue mt = value_int(st_mtime);
                    lat_map_set(map.as.map.map, "mtime", &mt);
                    LatValue ty = value_string(st_type);
                    lat_map_set(map.as.map.map, "type", &ty);
                    LatValue pm = value_int(st_mode);
                    lat_map_set(map.as.map.map, "permissions", &pm);
                    return eval_ok(map);
                }

                /// @builtin copy_file(src: String, dest: String) -> Bool
                /// @category File System
                /// Copy a file from source path to destination path.
                /// @example copy_file("a.txt", "b.txt")  // true
                if (strcmp(fn_name, "copy_file") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("copy_file() expects 2 string arguments")); }
                    char *cp_err = NULL;
                    bool cp_ok = fs_copy_file(args[0].as.str_val, args[1].as.str_val, &cp_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!cp_ok) { return eval_err(cp_err); }
                    return eval_ok(value_bool(true));
                }

                /// @builtin realpath(path: String) -> String
                /// @category File System
                /// Resolve a path to its absolute canonical form.
                /// @example realpath("./src/../src")  // "/home/user/src"
                if (strcmp(fn_name, "realpath") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("realpath() expects 1 string argument")); }
                    char *rp_err = NULL;
                    char *rp_result = fs_realpath(args[0].as.str_val, &rp_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!rp_result) { return eval_err(rp_err); }
                    return eval_ok(value_string_owned(rp_result));
                }

                /// @builtin tempdir() -> String
                /// @category File System
                /// Create a temporary directory and return its path.
                /// @example tempdir()  // "/tmp/lat_XXXXXX"
                if (strcmp(fn_name, "tempdir") == 0) {
                    if (argc != 0) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tempdir() expects no arguments")); }
                    free(args);
                    char *td_err = NULL;
                    char *td_result = fs_tempdir(&td_err);
                    if (!td_result) { return eval_err(td_err); }
                    return eval_ok(value_string_owned(td_result));
                }

                /// @builtin tempfile() -> String
                /// @category File System
                /// Create a temporary file and return its path.
                /// @example tempfile()  // "/tmp/lat_XXXXXX"
                if (strcmp(fn_name, "tempfile") == 0) {
                    if (argc != 0) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tempfile() expects no arguments")); }
                    free(args);
                    char *tf_err = NULL;
                    char *tf_result = fs_tempfile(&tf_err);
                    if (!tf_result) { return eval_err(tf_err); }
                    return eval_ok(value_string_owned(tf_result));
                }

                /// @builtin chmod(path: String, mode: Int) -> Bool
                /// @category File System
                /// Change file permissions using a numeric mode.
                /// @example chmod("script.sh", 755)  // true
                if (strcmp(fn_name, "chmod") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("chmod() expects 2 arguments (string path, integer mode)")); }
                    char *ch_err = NULL;
                    bool ch_ok = fs_chmod(args[0].as.str_val, (int)args[1].as.int_val, &ch_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!ch_ok) { return eval_err(ch_err); }
                    return eval_ok(value_bool(true));
                }

                /// @builtin file_size(path: String) -> Int
                /// @category File System
                /// Return the size of a file in bytes.
                /// @example file_size("data.bin")  // 4096
                if (strcmp(fn_name, "file_size") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("file_size() expects 1 string argument")); }
                    char *fs_err = NULL;
                    int64_t sz = fs_file_size(args[0].as.str_val, &fs_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (sz < 0) { return eval_err(fs_err); }
                    return eval_ok(value_int(sz));
                }

                /* ── Path builtins ── */

                /// @builtin path_join(parts: String...) -> String
                /// @category Path
                /// Join path components into a single path string.
                /// @example path_join("/home", "user", "file.txt")  // "/home/user/file.txt"
                if (strcmp(fn_name, "path_join") == 0) {
                    if (argc < 1) { free(args); return eval_err(strdup("path_join() expects at least 1 argument")); }
                    for (size_t i = 0; i < argc; i++) {
                        if (args[i].type != VAL_STR) { for (size_t j = 0; j < argc; j++) { value_free(&args[j]); } free(args); return eval_err(strdup("path_join() expects String arguments")); }
                    }
                    const char **parts = malloc(argc * sizeof(char*));
                    for (size_t i = 0; i < argc; i++) parts[i] = args[i].as.str_val;
                    char *result = path_join(parts, argc);
                    free(parts);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string_owned(result));
                }

                /// @builtin path_dir(path: String) -> String
                /// @category Path
                /// Return the directory component of a path.
                /// @example path_dir("/home/user/file.txt")  // "/home/user"
                if (strcmp(fn_name, "path_dir") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("path_dir() expects 1 String argument")); }
                    char *result = path_dir(args[0].as.str_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string_owned(result));
                }

                /// @builtin path_base(path: String) -> String
                /// @category Path
                /// Return the filename component of a path.
                /// @example path_base("/home/user/file.txt")  // "file.txt"
                if (strcmp(fn_name, "path_base") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("path_base() expects 1 String argument")); }
                    char *result = path_base(args[0].as.str_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string_owned(result));
                }

                /// @builtin path_ext(path: String) -> String
                /// @category Path
                /// Return the file extension of a path (including the dot).
                /// @example path_ext("file.txt")  // ".txt"
                if (strcmp(fn_name, "path_ext") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("path_ext() expects 1 String argument")); }
                    char *result = path_ext(args[0].as.str_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string_owned(result));
                }

                /// @builtin require(path: String) -> Bool
                /// @category Metaprogramming
                /// Load and execute a Lattice source file, importing its definitions.
                /// @example require("stdlib.lat")  // true
                if (strcmp(fn_name, "require") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("require() expects 1 string argument")); }
                    const char *raw_path = args[0].as.str_val;
                    /* Build the file path: append .lat if not already present */
                    size_t plen = strlen(raw_path);
                    char *file_path;
                    if (plen >= 4 && strcmp(raw_path + plen - 4, ".lat") == 0) {
                        file_path = strdup(raw_path);
                    } else {
                        file_path = malloc(plen + 5);
                        memcpy(file_path, raw_path, plen);
                        memcpy(file_path + plen, ".lat", 5);
                    }
                    /* Resolve to an absolute path for dedup.
                     * Try cwd-relative first, then script_dir-relative. */
                    char resolved[PATH_MAX];
                    bool found = (realpath(file_path, resolved) != NULL);
                    if (!found && ev->script_dir && file_path[0] != '/') {
                        /* Try relative to the script's directory */
                        char script_rel[PATH_MAX];
                        snprintf(script_rel, sizeof(script_rel), "%s/%s",
                                 ev->script_dir, file_path);
                        found = (realpath(script_rel, resolved) != NULL);
                    }
                    if (!found) {
                        char *err = NULL;
                        (void)asprintf(&err, "require: cannot find '%s'", file_path);
                        free(file_path);
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(err);
                    }
                    free(file_path);
                    /* Skip if already required */
                    if (lat_map_get(&ev->required_files, resolved)) {
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_ok(value_bool(true));
                    }
                    /* Mark as required before evaluating (guards against circular requires) */
                    bool marker = true;
                    lat_map_set(&ev->required_files, resolved, &marker);
                    /* Read the file */
                    char *source = builtin_read_file(resolved);
                    if (!source) {
                        char *err = NULL;
                        (void)asprintf(&err, "require: cannot read '%s'", resolved);
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(err);
                    }
                    /* Lex */
                    Lexer req_lex = lexer_new(source);
                    char *req_lex_err = NULL;
                    LatVec req_toks = lexer_tokenize(&req_lex, &req_lex_err);
                    free(source);
                    if (req_lex_err) {
                        char *err = NULL;
                        (void)asprintf(&err, "require '%s': %s", resolved, req_lex_err);
                        free(req_lex_err);
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(err);
                    }
                    /* Parse */
                    Parser req_parser = parser_new(&req_toks);
                    char *req_parse_err = NULL;
                    Program req_prog = parser_parse(&req_parser, &req_parse_err);
                    if (req_parse_err) {
                        char *err = NULL;
                        (void)asprintf(&err, "require '%s': %s", resolved, req_parse_err);
                        free(req_parse_err);
                        program_free(&req_prog);
                        for (size_t j = 0; j < req_toks.len; j++) token_free(lat_vec_get(&req_toks, j));
                        lat_vec_free(&req_toks);
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(err);
                    }
                    /* Register functions, structs, traits, impls */
                    for (size_t j = 0; j < req_prog.item_count; j++) {
                        if (req_prog.items[j].tag == ITEM_STRUCT) {
                            StructDecl *ptr = &req_prog.items[j].as.struct_decl;
                            lat_map_set(&ev->struct_defs, ptr->name, &ptr);
                        } else if (req_prog.items[j].tag == ITEM_FUNCTION) {
                            FnDecl *ptr = &req_prog.items[j].as.fn_decl;
                            register_fn_overload(&ev->fn_defs, ptr);
                        } else if (req_prog.items[j].tag == ITEM_TRAIT) {
                            TraitDecl *ptr = &req_prog.items[j].as.trait_decl;
                            lat_map_set(&ev->trait_defs, ptr->name, &ptr);
                        } else if (req_prog.items[j].tag == ITEM_IMPL) {
                            ImplBlock *ptr = &req_prog.items[j].as.impl_block;
                            char key[512];
                            snprintf(key, sizeof(key), "%s::%s", ptr->type_name, ptr->trait_name);
                            lat_map_set(&ev->impl_registry, key, &ptr);
                        }
                    }
                    /* Set script_dir to the required file's directory for nested requires */
                    char *prev_script_dir = ev->script_dir;
                    char *resolved_copy = strdup(resolved);
                    ev->script_dir = strdup(dirname(resolved_copy));
                    free(resolved_copy);
                    /* Execute top-level statements */
                    size_t saved_scope = ev->lat_eval_scope;
                    ev->lat_eval_scope = ev->env->count;
                    EvalResult req_r = eval_ok(value_unit());
                    for (size_t j = 0; j < req_prog.item_count; j++) {
                        if (req_prog.items[j].tag == ITEM_STMT) {
                            value_free(&req_r.value);
                            req_r = eval_stmt(ev, req_prog.items[j].as.stmt);
                            if (!IS_OK(req_r)) break;
                        }
                    }
                    ev->lat_eval_scope = saved_scope;
                    /* Restore previous script_dir */
                    free(ev->script_dir);
                    ev->script_dir = prev_script_dir;
                    /* Cleanup: free statements, keep decl items alive */
                    bool req_has_decls = false;
                    for (size_t j = 0; j < req_prog.item_count; j++) {
                        if (req_prog.items[j].tag == ITEM_STMT)
                            stmt_free(req_prog.items[j].as.stmt);
                        else
                            req_has_decls = true;
                    }
                    if (!req_has_decls) free(req_prog.items);
                    for (size_t j = 0; j < req_toks.len; j++) token_free(lat_vec_get(&req_toks, j));
                    lat_vec_free(&req_toks);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!IS_OK(req_r)) return req_r;
                    value_free(&req_r.value);
                    return eval_ok(value_bool(true));
                }

                /// @builtin require_ext(name: String) -> Map
                /// @category Metaprogramming
                /// Load a native extension (.dylib/.so) and return a Map of its functions.
                /// @example let pg = require_ext("pg")
                if (strcmp(fn_name, "require_ext") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("require_ext() expects 1 string argument")); }
                    const char *ext_name = args[0].as.str_val;
                    /* Check cache */
                    LatValue *cached = (LatValue *)lat_map_get(&ev->loaded_extensions, ext_name);
                    if (cached) {
                        LatValue result = value_deep_clone(cached);
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_ok(result);
                    }
                    char *ext_err = NULL;
                    LatValue ext_map = ext_load(ev, ext_name, &ext_err);
                    if (ext_err) {
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(ext_err);
                    }
                    /* Cache the extension */
                    LatValue cached_copy = value_deep_clone(&ext_map);
                    lat_map_set(&ev->loaded_extensions, ext_name, &cached_copy);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(ext_map);
                }

                /// @builtin lat_eval(source: String) -> Any
                /// @category Metaprogramming
                /// Parse and execute a string as Lattice source code, returning the result.
                /// @example lat_eval("1 + 2")  // 3
                if (strcmp(fn_name, "lat_eval") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("lat_eval() expects 1 string argument")); }
                    const char *source = args[0].as.str_val;
                    Lexer lex = lexer_new(source);
                    char *lex_err = NULL;
                    LatVec toks = lexer_tokenize(&lex, &lex_err);
                    if (lex_err) {
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(lex_err);
                    }
                    Parser parser = parser_new(&toks);
                    char *parse_err = NULL;
                    Program prog = parser_parse(&parser, &parse_err);
                    if (parse_err) {
                        program_free(&prog);
                        for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
                        lat_vec_free(&toks);
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(parse_err);
                    }
                    /* Register functions, structs, traits, impls (same as evaluator_run) */
                    for (size_t j = 0; j < prog.item_count; j++) {
                        if (prog.items[j].tag == ITEM_STRUCT) {
                            StructDecl *ptr = &prog.items[j].as.struct_decl;
                            lat_map_set(&ev->struct_defs, ptr->name, &ptr);
                        } else if (prog.items[j].tag == ITEM_FUNCTION) {
                            FnDecl *ptr = &prog.items[j].as.fn_decl;
                            register_fn_overload(&ev->fn_defs, ptr);
                        } else if (prog.items[j].tag == ITEM_TRAIT) {
                            TraitDecl *ptr = &prog.items[j].as.trait_decl;
                            lat_map_set(&ev->trait_defs, ptr->name, &ptr);
                        } else if (prog.items[j].tag == ITEM_IMPL) {
                            ImplBlock *ptr = &prog.items[j].as.impl_block;
                            char key[512];
                            snprintf(key, sizeof(key), "%s::%s", ptr->type_name, ptr->trait_name);
                            lat_map_set(&ev->impl_registry, key, &ptr);
                        }
                    }
                    /* Execute statements — set lat_eval_scope so top-level
                     * bindings persist in the caller's scope */
                    size_t saved_scope = ev->lat_eval_scope;
                    ev->lat_eval_scope = ev->env->count;
                    EvalResult eval_r = eval_ok(value_unit());
                    for (size_t j = 0; j < prog.item_count; j++) {
                        if (prog.items[j].tag == ITEM_STMT) {
                            value_free(&eval_r.value);
                            eval_r = eval_stmt(ev, prog.items[j].as.stmt);
                            if (!IS_OK(eval_r)) break;
                        }
                    }
                    ev->lat_eval_scope = saved_scope;
                    /* Free statement items. If fn/struct decls were registered,
                     * keep the items array alive (decls live inline in it).
                     * Otherwise free the whole program. */
                    bool has_decls = false;
                    for (size_t j = 0; j < prog.item_count; j++) {
                        if (prog.items[j].tag == ITEM_STMT)
                            stmt_free(prog.items[j].as.stmt);
                        else
                            has_decls = true;
                    }
                    if (!has_decls) free(prog.items);
                    for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
                    lat_vec_free(&toks);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_r;
                }

                /// @builtin tokenize(source: String) -> Array
                /// @category Metaprogramming
                /// Tokenize a source string, returning an array of Token structs.
                /// @example tokenize("1 + 2")  // [{type: "INT_LIT", text: "1"}, ...]
                if (strcmp(fn_name, "tokenize") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tokenize() expects 1 string argument")); }
                    const char *source = args[0].as.str_val;
                    Lexer lex = lexer_new(source);
                    char *lex_err = NULL;
                    LatVec toks = lexer_tokenize(&lex, &lex_err);
                    if (lex_err) {
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(lex_err);
                    }
                    size_t tok_count = toks.len > 0 ? toks.len - 1 : 0;
                    LatValue *elems = malloc((tok_count > 0 ? tok_count : 1) * sizeof(LatValue));
                    for (size_t j = 0; j < tok_count; j++) {
                        Token *t = lat_vec_get(&toks, j);
                        const char *type_str = token_type_name(t->type);
                        char *text;
                        if (t->type == TOK_IDENT || t->type == TOK_STRING_LIT || t->type == TOK_MODE_DIRECTIVE) {
                            text = strdup(t->as.str_val);
                        } else if (t->type == TOK_INT_LIT) {
                            (void)asprintf(&text, "%lld", (long long)t->as.int_val);
                        } else if (t->type == TOK_FLOAT_LIT) {
                            (void)asprintf(&text, "%g", t->as.float_val);
                        } else {
                            text = strdup(token_type_name(t->type));
                        }
                        char *fnames[2] = { "type", "text" };
                        LatValue fvals[2];
                        fvals[0] = value_string(type_str);
                        fvals[1] = value_string_owned(text);
                        elems[j] = value_struct("Token", fnames, fvals, 2);
                    }
                    for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
                    lat_vec_free(&toks);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    LatValue arr = value_array(elems, tok_count);
                    free(elems);
                    return eval_ok(arr);
                }

                /// @builtin Map::new() -> Map
                /// @category Type Constructors
                /// Create a new empty map.
                /// @example Map::new()  // {}
                if (strcmp(fn_name, "Map::new") == 0) {
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_map_new());
                }

                /// @builtin Channel::new() -> Channel
                /// @category Type Constructors
                /// Create a new channel for concurrent communication.
                /// @example Channel::new()  // <Channel>
                if (strcmp(fn_name, "Channel::new") == 0) {
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    LatChannel *ch = channel_new();
                    LatValue val = value_channel(ch);
                    channel_release(ch);  /* value_channel retained; drop our creation ref */
                    return eval_ok(val);
                }

                /// @builtin Set::new() -> Set
                /// @category Type Constructors
                /// Create a new empty set.
                /// @example Set::new()  // Set{}
                if (strcmp(fn_name, "Set::new") == 0) {
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_set_new());
                }

                /// @builtin Set::from(array: Array) -> Set
                /// @category Type Constructors
                /// Create a set from an array (duplicates removed).
                /// @example Set::from([1, 2, 2, 3])  // Set{1, 2, 3}
                if (strcmp(fn_name, "Set::from") == 0) {
                    if (argc != 1 || args[0].type != VAL_ARRAY) {
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(strdup("Set::from() expects 1 array argument"));
                    }
                    LatValue set = value_set_new();
                    for (size_t i = 0; i < args[0].as.array.len; i++) {
                        LatValue *elem = &args[0].as.array.elems[i];
                        char *key = value_display(elem);
                        LatValue cloned = value_deep_clone(elem);
                        lat_map_set(set.as.set.map, key, &cloned);
                        free(key);
                    }
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(set);
                }

                /// @builtin Buffer::new(size: Int) -> Buffer
                /// @category Type Constructors
                /// Create a new zero-filled buffer of the given size.
                /// @example Buffer::new(16)  // Buffer<16 bytes>
                if (strcmp(fn_name, "Buffer::new") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) {
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(strdup("Buffer::new() expects 1 Int argument"));
                    }
                    int64_t size = args[0].as.int_val;
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_buffer_alloc(size < 0 ? 0 : (size_t)size));
                }

                /// @builtin Buffer::from(arr: Array) -> Buffer
                /// @category Type Constructors
                /// Create a buffer from an array of byte integers (0-255).
                /// @example Buffer::from([0xFF, 0x00, 0x42])
                if (strcmp(fn_name, "Buffer::from") == 0) {
                    if (argc != 1 || args[0].type != VAL_ARRAY) {
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(strdup("Buffer::from() expects 1 Array argument"));
                    }
                    size_t blen = args[0].as.array.len;
                    uint8_t *data = malloc(blen > 0 ? blen : 1);
                    for (size_t bi = 0; bi < blen; bi++) {
                        if (args[0].as.array.elems[bi].type == VAL_INT)
                            data[bi] = (uint8_t)(args[0].as.array.elems[bi].as.int_val & 0xFF);
                        else
                            data[bi] = 0;
                    }
                    LatValue buf = value_buffer(data, blen);
                    free(data);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(buf);
                }

                /// @builtin Buffer::from_string(s: String) -> Buffer
                /// @category Type Constructors
                /// Create a buffer from a UTF-8 string.
                /// @example Buffer::from_string("hello")
                if (strcmp(fn_name, "Buffer::from_string") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) {
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(strdup("Buffer::from_string() expects 1 String argument"));
                    }
                    const char *s = args[0].as.str_val;
                    size_t slen = strlen(s);
                    LatValue buf = value_buffer((const uint8_t *)s, slen);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(buf);
                }

                /// @builtin Ref::new(value: Any) -> Ref
                /// @category Type Constructors
                /// Create a new reference-counted shared wrapper around a value.
                /// @example Ref::new({})
                if (strcmp(fn_name, "Ref::new") == 0) {
                    if (argc != 1) {
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(strdup("Ref::new() expects 1 argument"));
                    }
                    LatValue ref = value_ref(args[0]);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(ref);
                }

                /// @builtin parse_int(s: String) -> Int
                /// @category Type Conversion
                /// Parse a string as an integer.
                /// @example parse_int("42")  // 42
                if (strcmp(fn_name, "parse_int") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("parse_int() expects 1 string argument")); }
                    bool ok;
                    int64_t val = builtin_parse_int(args[0].as.str_val, &ok);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!ok) return eval_err(strdup("parse_int: invalid integer"));
                    return eval_ok(value_int(val));
                }

                /// @builtin parse_float(s: String) -> Float
                /// @category Type Conversion
                /// Parse a string as a floating-point number.
                /// @example parse_float("3.14")  // 3.14
                if (strcmp(fn_name, "parse_float") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("parse_float() expects 1 string argument")); }
                    bool ok;
                    double val = builtin_parse_float(args[0].as.str_val, &ok);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!ok) return eval_err(strdup("parse_float: invalid float"));
                    return eval_ok(value_float(val));
                }

                /// @builtin error(msg: String) -> String
                /// @category Error Handling
                /// Create an error value with the given message.
                /// @example error("something went wrong")  // "EVAL_ERROR:something went wrong"
                if (strcmp(fn_name, "error") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("error() expects 1 string argument")); }
                    char *msg = NULL;
                    (void)asprintf(&msg, "EVAL_ERROR:%s", args[0].as.str_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string_owned(msg));
                }

                /// @builtin panic(msg: String) -> Unit
                /// @category Error Handling
                /// Trigger an immediate fatal error that cannot be caught by try/catch.
                /// @example panic("unrecoverable state")
                if (strcmp(fn_name, "panic") == 0) {
                    const char *msg = (argc >= 1 && args[0].type == VAL_STR) ? args[0].as.str_val : "panic";
                    char *err = strdup(msg);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_err(err);
                }

                /// @builtin is_error(val: Any) -> Bool
                /// @category Error Handling
                /// Check if a value is an error value.
                /// @example is_error(error("oops"))  // true
                if (strcmp(fn_name, "is_error") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("is_error() expects 1 argument")); }
                    bool is_err = args[0].type == VAL_STR && strncmp(args[0].as.str_val, "EVAL_ERROR:", 11) == 0;
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_bool(is_err));
                }

                /// @builtin len(val: String|Array|Map) -> Int
                /// @category Core
                /// Returns the length of a string, array, or map.
                /// @example len("hello")  // 5
                /// @example len([1, 2, 3])  // 3
                if (strcmp(fn_name, "len") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("len() expects 1 argument")); }
                    int64_t l = -1;
                    if (args[0].type == VAL_STR) l = (int64_t)strlen(args[0].as.str_val);
                    else if (args[0].type == VAL_ARRAY) l = (int64_t)args[0].as.array.len;
                    else if (args[0].type == VAL_MAP) l = (int64_t)lat_map_len(args[0].as.map.map);
                    else if (args[0].type == VAL_SET) l = (int64_t)lat_map_len(args[0].as.set.map);
                    else if (args[0].type == VAL_BUFFER) l = (int64_t)args[0].as.buffer.len;
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (l < 0) return eval_err(strdup("len() not supported on this type"));
                    return eval_ok(value_int(l));
                }

                /// @builtin exit(code?: Int) -> Unit
                /// @category Core
                /// Exit the program with an optional exit code (default 0).
                /// @example exit(1)  // exits with code 1
                if (strcmp(fn_name, "exit") == 0) {
                    int code = 0;
                    if (argc > 0 && args[0].type == VAL_INT) code = (int)args[0].as.int_val;
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    exit(code);
                }

                /// @builtin version() -> String
                /// @category Core
                /// Return the Lattice interpreter version string.
                /// @example version()  // "0.1.0"
                if (strcmp(fn_name, "version") == 0) {
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string(LATTICE_VERSION));
                }

                /// @builtin print_raw(args: Any...) -> Unit
                /// @category Core
                /// Print values separated by spaces without a trailing newline.
                /// @example print_raw("hello", "world")  // prints: hello world
                if (strcmp(fn_name, "print_raw") == 0) {
                    for (size_t i = 0; i < argc; i++) {
                        if (i > 0) printf(" ");
                        char *s = value_display(&args[i]);
                        printf("%s", s);
                        free(s);
                    }
                    fflush(stdout);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_unit());
                }

                /// @builtin eprint(args: Any...) -> Unit
                /// @category Core
                /// Print values to stderr with a trailing newline.
                /// @example eprint("warning:", msg)  // prints to stderr
                if (strcmp(fn_name, "eprint") == 0) {
                    for (size_t i = 0; i < argc; i++) {
                        if (i > 0) fprintf(stderr, " ");
                        char *s = value_display(&args[i]);
                        fprintf(stderr, "%s", s);
                        free(s);
                    }
                    fprintf(stderr, "\n");
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_unit());
                }

                /* ── TCP networking builtins ── */

                /// @builtin tcp_listen(host: String, port: Int) -> Int
                /// @category Networking
                /// Create a TCP server socket listening on host:port, returning a file descriptor.
                /// @example tcp_listen("0.0.0.0", 8080)  // 3
                if (strcmp(fn_name, "tcp_listen") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tcp_listen() expects (String host, Int port)")); }
                    char *net_err = NULL;
                    int fd = net_tcp_listen(args[0].as.str_val, (int)args[1].as.int_val, &net_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (fd < 0) return eval_err(net_err);
                    return eval_ok(value_int(fd));
                }

                /// @builtin tcp_accept(server_fd: Int) -> Int
                /// @category Networking
                /// Accept an incoming TCP connection, returning a new client file descriptor.
                /// @example tcp_accept(server_fd)  // 4
                if (strcmp(fn_name, "tcp_accept") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tcp_accept() expects (Int server_fd)")); }
                    char *net_err = NULL;
                    int fd = net_tcp_accept((int)args[0].as.int_val, &net_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (fd < 0) return eval_err(net_err);
                    return eval_ok(value_int(fd));
                }

                /// @builtin tcp_connect(host: String, port: Int) -> Int
                /// @category Networking
                /// Connect to a TCP server, returning a file descriptor.
                /// @example tcp_connect("localhost", 8080)  // 3
                if (strcmp(fn_name, "tcp_connect") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tcp_connect() expects (String host, Int port)")); }
                    char *net_err = NULL;
                    int fd = net_tcp_connect(args[0].as.str_val, (int)args[1].as.int_val, &net_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (fd < 0) return eval_err(net_err);
                    return eval_ok(value_int(fd));
                }

                /// @builtin tcp_read(fd: Int) -> String
                /// @category Networking
                /// Read data from a TCP socket as a string.
                /// @example tcp_read(client_fd)  // "HTTP/1.1 200 OK..."
                if (strcmp(fn_name, "tcp_read") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tcp_read() expects (Int fd)")); }
                    char *net_err = NULL;
                    char *data = net_tcp_read((int)args[0].as.int_val, &net_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!data) return eval_err(net_err);
                    return eval_ok(value_string_owned(data));
                }

                /// @builtin tcp_read_bytes(fd: Int, n: Int) -> String
                /// @category Networking
                /// Read exactly n bytes from a TCP socket.
                /// @example tcp_read_bytes(fd, 1024)  // "..."
                if (strcmp(fn_name, "tcp_read_bytes") == 0) {
                    if (argc != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tcp_read_bytes() expects (Int fd, Int n)")); }
                    char *net_err = NULL;
                    char *data = net_tcp_read_bytes((int)args[0].as.int_val, (size_t)args[1].as.int_val, &net_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!data) return eval_err(net_err);
                    return eval_ok(value_string_owned(data));
                }

                /// @builtin tcp_write(fd: Int, data: String) -> Bool
                /// @category Networking
                /// Write a string to a TCP socket.
                /// @example tcp_write(fd, "GET / HTTP/1.1\r\n\r\n")  // true
                if (strcmp(fn_name, "tcp_write") == 0) {
                    if (argc != 2 || args[0].type != VAL_INT || args[1].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tcp_write() expects (Int fd, String data)")); }
                    char *net_err = NULL;
                    bool ok = net_tcp_write((int)args[0].as.int_val, args[1].as.str_val, strlen(args[1].as.str_val), &net_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!ok) return eval_err(net_err);
                    return eval_ok(value_bool(true));
                }

                /// @builtin tcp_close(fd: Int) -> Unit
                /// @category Networking
                /// Close a TCP socket.
                /// @example tcp_close(fd)
                if (strcmp(fn_name, "tcp_close") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tcp_close() expects (Int fd)")); }
                    net_tcp_close((int)args[0].as.int_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_unit());
                }

                /// @builtin tcp_peer_addr(fd: Int) -> String
                /// @category Networking
                /// Get the remote address of a connected TCP socket.
                /// @example tcp_peer_addr(client_fd)  // "192.168.1.1:54321"
                if (strcmp(fn_name, "tcp_peer_addr") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tcp_peer_addr() expects (Int fd)")); }
                    char *net_err = NULL;
                    char *addr = net_tcp_peer_addr((int)args[0].as.int_val, &net_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!addr) return eval_err(net_err);
                    return eval_ok(value_string_owned(addr));
                }

                /// @builtin tcp_set_timeout(fd: Int, secs: Int) -> Bool
                /// @category Networking
                /// Set read/write timeout on a TCP socket in seconds.
                /// @example tcp_set_timeout(fd, 30)  // true
                if (strcmp(fn_name, "tcp_set_timeout") == 0) {
                    if (argc != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tcp_set_timeout() expects (Int fd, Int secs)")); }
                    char *net_err = NULL;
                    bool ok = net_tcp_set_timeout((int)args[0].as.int_val, (int)args[1].as.int_val, &net_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!ok) return eval_err(net_err);
                    return eval_ok(value_bool(true));
                }

                /* ── TLS networking builtins ── */

                /// @builtin tls_connect(host: String, port: Int) -> Int
                /// @category Networking
                /// Establish a TLS connection to a server, returning a handle.
                /// @example tls_connect("example.com", 443)  // 1
                if (strcmp(fn_name, "tls_connect") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tls_connect() expects (String host, Int port)")); }
                    char *net_err = NULL;
                    int fd = net_tls_connect(args[0].as.str_val, (int)args[1].as.int_val, &net_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (fd < 0) return eval_err(net_err);
                    return eval_ok(value_int(fd));
                }

                /// @builtin tls_read(handle: Int) -> String
                /// @category Networking
                /// Read data from a TLS connection as a string.
                /// @example tls_read(handle)  // "HTTP/1.1 200 OK..."
                if (strcmp(fn_name, "tls_read") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tls_read() expects (Int fd)")); }
                    char *net_err = NULL;
                    char *data = net_tls_read((int)args[0].as.int_val, &net_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!data) return eval_err(net_err);
                    return eval_ok(value_string_owned(data));
                }

                /// @builtin tls_read_bytes(handle: Int, n: Int) -> String
                /// @category Networking
                /// Read exactly n bytes from a TLS connection.
                /// @example tls_read_bytes(handle, 512)  // "..."
                if (strcmp(fn_name, "tls_read_bytes") == 0) {
                    if (argc != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tls_read_bytes() expects (Int fd, Int n)")); }
                    char *net_err = NULL;
                    char *data = net_tls_read_bytes((int)args[0].as.int_val, (size_t)args[1].as.int_val, &net_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!data) return eval_err(net_err);
                    return eval_ok(value_string_owned(data));
                }

                /// @builtin tls_write(handle: Int, data: String) -> Bool
                /// @category Networking
                /// Write a string to a TLS connection.
                /// @example tls_write(handle, "GET / HTTP/1.1\r\n\r\n")  // true
                if (strcmp(fn_name, "tls_write") == 0) {
                    if (argc != 2 || args[0].type != VAL_INT || args[1].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tls_write() expects (Int fd, String data)")); }
                    char *net_err = NULL;
                    bool ok = net_tls_write((int)args[0].as.int_val, args[1].as.str_val, strlen(args[1].as.str_val), &net_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!ok) return eval_err(net_err);
                    return eval_ok(value_bool(true));
                }

                /// @builtin tls_close(handle: Int) -> Unit
                /// @category Networking
                /// Close a TLS connection.
                /// @example tls_close(handle)
                if (strcmp(fn_name, "tls_close") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tls_close() expects (Int fd)")); }
                    net_tls_close((int)args[0].as.int_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_unit());
                }

                /// @builtin tls_available() -> Bool
                /// @category Networking
                /// Check if TLS support is available (OpenSSL linked).
                /// @example tls_available()  // true
                if (strcmp(fn_name, "tls_available") == 0) {
                    if (argc != 0) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tls_available() expects no arguments")); }
                    free(args);
                    return eval_ok(value_bool(net_tls_available()));
                }

                /* ── JSON builtins ── */

                /// @builtin json_parse(s: String) -> Any
                /// @category JSON
                /// Parse a JSON string into a Lattice value.
                /// @example json_parse("{\"a\": 1}")  // {a: 1}
                if (strcmp(fn_name, "json_parse") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("json_parse() expects (String)")); }
                    char *jerr = NULL;
                    LatValue result = json_parse(args[0].as.str_val, &jerr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (jerr) return eval_err(jerr);
                    return eval_ok(result);
                }

                /// @builtin json_stringify(val: Any) -> String
                /// @category JSON
                /// Serialize a Lattice value to a JSON string.
                /// @example json_stringify([1, 2, 3])  // "[1,2,3]"
                if (strcmp(fn_name, "json_stringify") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("json_stringify() expects (value)")); }
                    char *jerr = NULL;
                    char *json = json_stringify(&args[0], &jerr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!json) return eval_err(jerr);
                    return eval_ok(value_string_owned(json));
                }

                /* ── HTTP builtins ── */

                /// @builtin http_get(url: String) -> Map
                /// @category HTTP
                /// Perform an HTTP GET request. Returns a map with "status", "headers", and "body".
                /// @example http_get("https://httpbin.org/get")
                if (strcmp(fn_name, "http_get") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) {
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(strdup("http_get() expects (url: String)"));
                    }
                    HttpRequest hreq = {
                        .method = "GET", .url = args[0].as.str_val,
                        .header_keys = NULL, .header_values = NULL, .header_count = 0,
                        .body = NULL, .body_len = 0, .timeout_ms = 0
                    };
                    char *herr = NULL;
                    HttpResponse *hresp = http_execute(&hreq, &herr);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    if (!hresp) return eval_err(herr ? herr : strdup("http_get failed"));
                    /* Build result map */
                    LatValue result = value_map_new();
                    LatValue st = value_int(hresp->status_code);
                    lat_map_set(result.as.map.map, "status", &st);
                    LatValue bd = value_string(hresp->body ? hresp->body : "");
                    lat_map_set(result.as.map.map, "body", &bd);
                    LatValue hdrs = value_map_new();
                    for (size_t i = 0; i < hresp->header_count; i++) {
                        LatValue hv = value_string(hresp->header_values[i]);
                        lat_map_set(hdrs.as.map.map, hresp->header_keys[i], &hv);
                    }
                    lat_map_set(result.as.map.map, "headers", &hdrs);
                    http_response_free(hresp);
                    return eval_ok(result);
                }

                /// @builtin http_post(url: String, options: Map) -> Map
                /// @category HTTP
                /// Perform an HTTP POST request. Options map may contain "headers" (Map), "body" (String), and "timeout" (Int ms).
                /// @example http_post("https://httpbin.org/post", {"body": "hello"})
                if (strcmp(fn_name, "http_post") == 0) {
                    if (argc < 1 || argc > 2 || args[0].type != VAL_STR) {
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(strdup("http_post() expects (url: String, options?: Map)"));
                    }
                    /* Extract options */
                    const char *body_str = NULL;
                    size_t body_len = 0;
                    int timeout_ms = 0;
                    char **hdr_keys = NULL, **hdr_vals = NULL;
                    size_t hdr_count = 0;
                    if (argc == 2 && args[1].type == VAL_MAP) {
                        LatValue *bv = (LatValue *)lat_map_get(args[1].as.map.map, "body");
                        if (bv && bv->type == VAL_STR) { body_str = bv->as.str_val; body_len = strlen(body_str); }
                        LatValue *tv = (LatValue *)lat_map_get(args[1].as.map.map, "timeout");
                        if (tv && tv->type == VAL_INT) timeout_ms = (int)tv->as.int_val;
                        LatValue *hm = (LatValue *)lat_map_get(args[1].as.map.map, "headers");
                        if (hm && hm->type == VAL_MAP) {
                            hdr_count = lat_map_len(hm->as.map.map);
                            hdr_keys = malloc(hdr_count * sizeof(char *));
                            hdr_vals = malloc(hdr_count * sizeof(char *));
                            size_t hi = 0;
                            for (size_t i = 0; i < hm->as.map.map->cap && hi < hdr_count; i++) {
                                if (hm->as.map.map->entries[i].state == MAP_OCCUPIED) {
                                    hdr_keys[hi] = (char *)hm->as.map.map->entries[i].key;
                                    LatValue *v = (LatValue *)hm->as.map.map->entries[i].value;
                                    hdr_vals[hi] = v->type == VAL_STR ? v->as.str_val : "";
                                    hi++;
                                }
                            }
                        }
                    }
                    HttpRequest hreq = {
                        .method = "POST", .url = args[0].as.str_val,
                        .header_keys = hdr_keys, .header_values = hdr_vals, .header_count = hdr_count,
                        .body = body_str, .body_len = body_len, .timeout_ms = timeout_ms
                    };
                    char *herr = NULL;
                    HttpResponse *hresp = http_execute(&hreq, &herr);
                    free(hdr_keys); free(hdr_vals);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    if (!hresp) return eval_err(herr ? herr : strdup("http_post failed"));
                    LatValue result = value_map_new();
                    LatValue st = value_int(hresp->status_code);
                    lat_map_set(result.as.map.map, "status", &st);
                    LatValue bd = value_string(hresp->body ? hresp->body : "");
                    lat_map_set(result.as.map.map, "body", &bd);
                    LatValue hdrs = value_map_new();
                    for (size_t i = 0; i < hresp->header_count; i++) {
                        LatValue hv = value_string(hresp->header_values[i]);
                        lat_map_set(hdrs.as.map.map, hresp->header_keys[i], &hv);
                    }
                    lat_map_set(result.as.map.map, "headers", &hdrs);
                    http_response_free(hresp);
                    return eval_ok(result);
                }

                /// @builtin http_request(method: String, url: String, options?: Map) -> Map
                /// @category HTTP
                /// Perform an HTTP request with a custom method. Options may contain "headers", "body", and "timeout".
                /// @example http_request("PUT", "https://api.example.com/data", {"body": "{}"})
                if (strcmp(fn_name, "http_request") == 0) {
                    if (argc < 2 || argc > 3 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(strdup("http_request() expects (method: String, url: String, options?: Map)"));
                    }
                    const char *body_str = NULL;
                    size_t body_len = 0;
                    int timeout_ms = 0;
                    char **hdr_keys = NULL, **hdr_vals = NULL;
                    size_t hdr_count = 0;
                    if (argc == 3 && args[2].type == VAL_MAP) {
                        LatValue *bv = (LatValue *)lat_map_get(args[2].as.map.map, "body");
                        if (bv && bv->type == VAL_STR) { body_str = bv->as.str_val; body_len = strlen(body_str); }
                        LatValue *tv = (LatValue *)lat_map_get(args[2].as.map.map, "timeout");
                        if (tv && tv->type == VAL_INT) timeout_ms = (int)tv->as.int_val;
                        LatValue *hm = (LatValue *)lat_map_get(args[2].as.map.map, "headers");
                        if (hm && hm->type == VAL_MAP) {
                            hdr_count = lat_map_len(hm->as.map.map);
                            hdr_keys = malloc(hdr_count * sizeof(char *));
                            hdr_vals = malloc(hdr_count * sizeof(char *));
                            size_t hi = 0;
                            for (size_t i = 0; i < hm->as.map.map->cap && hi < hdr_count; i++) {
                                if (hm->as.map.map->entries[i].state == MAP_OCCUPIED) {
                                    hdr_keys[hi] = (char *)hm->as.map.map->entries[i].key;
                                    LatValue *v = (LatValue *)hm->as.map.map->entries[i].value;
                                    hdr_vals[hi] = v->type == VAL_STR ? v->as.str_val : "";
                                    hi++;
                                }
                            }
                        }
                    }
                    HttpRequest hreq = {
                        .method = args[0].as.str_val, .url = args[1].as.str_val,
                        .header_keys = hdr_keys, .header_values = hdr_vals, .header_count = hdr_count,
                        .body = body_str, .body_len = body_len, .timeout_ms = timeout_ms
                    };
                    char *herr = NULL;
                    HttpResponse *hresp = http_execute(&hreq, &herr);
                    free(hdr_keys); free(hdr_vals);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    if (!hresp) return eval_err(herr ? herr : strdup("http_request failed"));
                    LatValue result = value_map_new();
                    LatValue st = value_int(hresp->status_code);
                    lat_map_set(result.as.map.map, "status", &st);
                    LatValue bd = value_string(hresp->body ? hresp->body : "");
                    lat_map_set(result.as.map.map, "body", &bd);
                    LatValue hdrs = value_map_new();
                    for (size_t i = 0; i < hresp->header_count; i++) {
                        LatValue hv = value_string(hresp->header_values[i]);
                        lat_map_set(hdrs.as.map.map, hresp->header_keys[i], &hv);
                    }
                    lat_map_set(result.as.map.map, "headers", &hdrs);
                    http_response_free(hresp);
                    return eval_ok(result);
                }

                /* ── Math builtins ── */

                /// @builtin abs(x: Int|Float) -> Int|Float
                /// @category Math
                /// Return the absolute value of a number.
                /// @example abs(-5)  // 5
                if (strcmp(fn_name, "abs") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("abs() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_abs(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin floor(x: Int|Float) -> Int
                /// @category Math
                /// Round down to the nearest integer.
                /// @example floor(3.7)  // 3
                if (strcmp(fn_name, "floor") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("floor() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_floor(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin ceil(x: Int|Float) -> Int
                /// @category Math
                /// Round up to the nearest integer.
                /// @example ceil(3.2)  // 4
                if (strcmp(fn_name, "ceil") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("ceil() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_ceil(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin round(x: Int|Float) -> Int
                /// @category Math
                /// Round to the nearest integer.
                /// @example round(3.5)  // 4
                if (strcmp(fn_name, "round") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("round() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_round(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin sqrt(x: Int|Float) -> Float
                /// @category Math
                /// Return the square root of a number.
                /// @example sqrt(16)  // 4.0
                if (strcmp(fn_name, "sqrt") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("sqrt() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_sqrt(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin pow(base: Int|Float, exp: Int|Float) -> Float
                /// @category Math
                /// Raise base to the power of exp.
                /// @example pow(2, 10)  // 1024.0
                if (strcmp(fn_name, "pow") == 0) {
                    if (argc != 2) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("pow() expects (Int|Float, Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_pow(&args[0], &args[1], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin min(a: Int|Float, b: Int|Float) -> Int|Float
                /// @category Math
                /// Return the smaller of two numbers.
                /// @example min(3, 7)  // 3
                if (strcmp(fn_name, "min") == 0) {
                    if (argc != 2) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("min() expects (Int|Float, Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_min(&args[0], &args[1], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin max(a: Int|Float, b: Int|Float) -> Int|Float
                /// @category Math
                /// Return the larger of two numbers.
                /// @example max(3, 7)  // 7
                if (strcmp(fn_name, "max") == 0) {
                    if (argc != 2) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("max() expects (Int|Float, Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_max(&args[0], &args[1], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin random() -> Float
                /// @category Math
                /// Return a random float between 0.0 (inclusive) and 1.0 (exclusive).
                /// @example random()  // 0.7231...
                if (strcmp(fn_name, "random") == 0) {
                    if (argc != 0) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("random() expects no arguments")); }
                    free(args);
                    return eval_ok(math_random());
                }

                /// @builtin random_int(min: Int, max: Int) -> Int
                /// @category Math
                /// Return a random integer in the range [min, max).
                /// @example random_int(1, 100)  // 42
                if (strcmp(fn_name, "random_int") == 0) {
                    if (argc != 2) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("random_int() expects (Int, Int)")); }
                    char *merr = NULL;
                    LatValue result = math_random_int(&args[0], &args[1], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin log(x: Int|Float) -> Float
                /// @category Math
                /// Return the natural logarithm (base e) of a number.
                /// @example log(math_e())  // 1.0
                if (strcmp(fn_name, "log") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("log() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_log(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin log2(x: Int|Float) -> Float
                /// @category Math
                /// Return the base-2 logarithm of a number.
                /// @example log2(8)  // 3.0
                if (strcmp(fn_name, "log2") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("log2() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_log2(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin log10(x: Int|Float) -> Float
                /// @category Math
                /// Return the base-10 logarithm of a number.
                /// @example log10(1000)  // 3.0
                if (strcmp(fn_name, "log10") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("log10() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_log10(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin sin(x: Int|Float) -> Float
                /// @category Math
                /// Return the sine of an angle in radians.
                /// @example sin(0)  // 0.0
                if (strcmp(fn_name, "sin") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("sin() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_sin(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin cos(x: Int|Float) -> Float
                /// @category Math
                /// Return the cosine of an angle in radians.
                /// @example cos(0)  // 1.0
                if (strcmp(fn_name, "cos") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("cos() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_cos(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin tan(x: Int|Float) -> Float
                /// @category Math
                /// Return the tangent of an angle in radians.
                /// @example tan(0)  // 0.0
                if (strcmp(fn_name, "tan") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tan() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_tan(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin atan2(y: Int|Float, x: Int|Float) -> Float
                /// @category Math
                /// Return the two-argument arctangent in radians.
                /// @example atan2(1, 1)  // 0.7853...
                if (strcmp(fn_name, "atan2") == 0) {
                    if (argc != 2) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("atan2() expects (Int|Float, Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_atan2(&args[0], &args[1], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin clamp(x: Int|Float, lo: Int|Float, hi: Int|Float) -> Int|Float
                /// @category Math
                /// Clamp a value between a minimum and maximum.
                /// @example clamp(15, 0, 10)  // 10
                if (strcmp(fn_name, "clamp") == 0) {
                    if (argc != 3) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("clamp() expects (Int|Float, Int|Float, Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_clamp(&args[0], &args[1], &args[2], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin math_pi() -> Float
                /// @category Math
                /// Return the mathematical constant pi.
                /// @example math_pi()  // 3.14159265358979...
                if (strcmp(fn_name, "math_pi") == 0) {
                    if (argc != 0) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("math_pi() expects no arguments")); }
                    free(args);
                    return eval_ok(math_pi());
                }

                /// @builtin math_e() -> Float
                /// @category Math
                /// Return Euler's number (e).
                /// @example math_e()  // 2.71828182845904...
                if (strcmp(fn_name, "math_e") == 0) {
                    if (argc != 0) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("math_e() expects no arguments")); }
                    free(args);
                    return eval_ok(math_e());
                }

                /// @builtin asin(x: Int|Float) -> Float
                /// @category Math
                /// Return the arcsine in radians.
                /// @example asin(1)  // 1.5707...
                if (strcmp(fn_name, "asin") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("asin() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_asin(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin acos(x: Int|Float) -> Float
                /// @category Math
                /// Return the arccosine in radians.
                /// @example acos(1)  // 0.0
                if (strcmp(fn_name, "acos") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("acos() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_acos(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin atan(x: Int|Float) -> Float
                /// @category Math
                /// Return the arctangent in radians.
                /// @example atan(1)  // 0.7853...
                if (strcmp(fn_name, "atan") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("atan() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_atan(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin exp(x: Int|Float) -> Float
                /// @category Math
                /// Return e raised to the power of x.
                /// @example exp(1)  // 2.71828...
                if (strcmp(fn_name, "exp") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("exp() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_exp(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin sign(x: Int|Float) -> Int
                /// @category Math
                /// Return -1, 0, or 1 indicating the sign of a number.
                /// @example sign(-42)  // -1
                if (strcmp(fn_name, "sign") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("sign() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_sign(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin gcd(a: Int, b: Int) -> Int
                /// @category Math
                /// Return the greatest common divisor of two integers.
                /// @example gcd(12, 8)  // 4
                if (strcmp(fn_name, "gcd") == 0) {
                    if (argc != 2) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("gcd() expects (Int, Int)")); }
                    char *merr = NULL;
                    LatValue result = math_gcd(&args[0], &args[1], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin lcm(a: Int, b: Int) -> Int
                /// @category Math
                /// Return the least common multiple of two integers.
                /// @example lcm(4, 6)  // 12
                if (strcmp(fn_name, "lcm") == 0) {
                    if (argc != 2) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("lcm() expects (Int, Int)")); }
                    char *merr = NULL;
                    LatValue result = math_lcm(&args[0], &args[1], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin float_to_bits(x: Float) -> Int
                /// @category Math
                /// Reinterpret a float as its IEEE 754 bit pattern (64-bit integer).
                /// @example float_to_bits(1.0)  // 4607182418800017408
                if (strcmp(fn_name, "float_to_bits") == 0) {
                    if (argc != 1 || args[0].type != VAL_FLOAT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("float_to_bits() expects 1 Float argument")); }
                    double d = args[0].as.float_val;
                    uint64_t bits; memcpy(&bits, &d, 8);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_int((int64_t)bits));
                }

                /// @builtin bits_to_float(x: Int) -> Float
                /// @category Math
                /// Reinterpret a 64-bit integer as an IEEE 754 float.
                /// @example bits_to_float(4607182418800017408)  // 1.0
                if (strcmp(fn_name, "bits_to_float") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("bits_to_float() expects 1 Int argument")); }
                    uint64_t bits = (uint64_t)args[0].as.int_val;
                    double d; memcpy(&d, &bits, 8);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_float(d));
                }

                /// @builtin is_nan(x: Int|Float) -> Bool
                /// @category Math
                /// Check if a value is NaN (not a number).
                /// @example is_nan(0.0 / 0.0)  // true
                if (strcmp(fn_name, "is_nan") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("is_nan() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_is_nan(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin is_inf(x: Int|Float) -> Bool
                /// @category Math
                /// Check if a value is positive or negative infinity.
                /// @example is_inf(1.0 / 0.0)  // true
                if (strcmp(fn_name, "is_inf") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("is_inf() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_is_inf(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin sinh(x: Int|Float) -> Float
                /// @category Math
                /// Return the hyperbolic sine.
                /// @example sinh(1)  // 1.1752...
                if (strcmp(fn_name, "sinh") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("sinh() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_sinh(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin cosh(x: Int|Float) -> Float
                /// @category Math
                /// Return the hyperbolic cosine.
                /// @example cosh(0)  // 1.0
                if (strcmp(fn_name, "cosh") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("cosh() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_cosh(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin tanh(x: Int|Float) -> Float
                /// @category Math
                /// Return the hyperbolic tangent.
                /// @example tanh(0)  // 0.0
                if (strcmp(fn_name, "tanh") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("tanh() expects (Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_tanh(&args[0], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /// @builtin lerp(a: Int|Float, b: Int|Float, t: Int|Float) -> Float
                /// @category Math
                /// Linear interpolation between a and b by factor t.
                /// @example lerp(0, 10, 0.5)  // 5.0
                if (strcmp(fn_name, "lerp") == 0) {
                    if (argc != 3) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("lerp() expects (Int|Float, Int|Float, Int|Float)")); }
                    char *merr = NULL;
                    LatValue result = math_lerp(&args[0], &args[1], &args[2], &merr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (merr) return eval_err(merr);
                    return eval_ok(result);
                }

                /* ── range() builtin ── */

                /// @builtin range(start: Int, end: Int, step?: Int) -> Array
                /// @category Type Constructors
                /// Generate an array of integers from start (inclusive) to end (exclusive).
                /// @example range(0, 5)  // [0, 1, 2, 3, 4]
                /// @example range(0, 10, 2)  // [0, 2, 4, 6, 8]
                if (strcmp(fn_name, "range") == 0) {
                    if (argc < 2 || argc > 3) {
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(strdup("range() expects 2 or 3 integer arguments (start, end, step?)"));
                    }
                    if (args[0].type != VAL_INT || args[1].type != VAL_INT) {
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(strdup("range() start and end must be integers"));
                    }
                    int64_t rstart = args[0].as.int_val;
                    int64_t rend = args[1].as.int_val;
                    int64_t rstep = (rstart <= rend) ? 1 : -1;
                    if (argc == 3) {
                        if (args[2].type != VAL_INT) {
                            for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                            free(args);
                            return eval_err(strdup("range() step must be an integer"));
                        }
                        rstep = args[2].as.int_val;
                    }
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (rstep == 0) {
                        return eval_err(strdup("range() step cannot be 0"));
                    }
                    /* Calculate count and build array */
                    size_t rcount = 0;
                    if (rstep > 0 && rstart < rend) {
                        rcount = (size_t)((rend - rstart + rstep - 1) / rstep);
                    } else if (rstep < 0 && rstart > rend) {
                        rcount = (size_t)((rstart - rend + (-rstep) - 1) / (-rstep));
                    }
                    LatValue *relems = malloc((rcount > 0 ? rcount : 1) * sizeof(LatValue));
                    int64_t rcur = rstart;
                    for (size_t ri = 0; ri < rcount; ri++) {
                        relems[ri] = value_int(rcur);
                        rcur += rstep;
                    }
                    LatValue range_arr = value_array(relems, rcount);
                    free(relems);
                    return eval_ok(range_arr);
                }

                /* ── Type coercion builtins ── */

                /// @builtin to_int(val: Any) -> Int
                /// @category Type Conversion
                /// Convert a value to an integer (truncates floats, parses strings).
                /// @example to_int(3.9)  // 3
                if (strcmp(fn_name, "to_int") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("to_int() expects (value)")); }
                    char *terr = NULL;
                    LatValue result = type_to_int(&args[0], &terr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (terr) return eval_err(terr);
                    return eval_ok(result);
                }

                /// @builtin to_float(val: Any) -> Float
                /// @category Type Conversion
                /// Convert a value to a floating-point number.
                /// @example to_float(42)  // 42.0
                if (strcmp(fn_name, "to_float") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("to_float() expects (value)")); }
                    char *terr = NULL;
                    LatValue result = type_to_float(&args[0], &terr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (terr) return eval_err(terr);
                    return eval_ok(result);
                }

                /* ── Environment variable builtins ── */

                /// @builtin env(name: String) -> String|Unit
                /// @category Environment
                /// Get an environment variable's value, or unit if not set.
                /// @example env("HOME")  // "/home/user"
                if (strcmp(fn_name, "env") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("env() expects (String)")); }
                    char *val = envvar_get(args[0].as.str_val);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!val) return eval_ok(value_unit());
                    return eval_ok(value_string_owned(val));
                }

                /// @builtin env_set(name: String, value: String) -> Unit
                /// @category Environment
                /// Set an environment variable.
                /// @example env_set("MY_VAR", "hello")
                if (strcmp(fn_name, "env_set") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("env_set() expects (String, String)")); }
                    char *eerr = NULL;
                    bool ok = envvar_set(args[0].as.str_val, args[1].as.str_val, &eerr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!ok) return eval_err(eerr);
                    return eval_ok(value_unit());
                }

                /// @builtin env_keys() -> Array
                /// @category Environment
                /// Return an array of all environment variable names.
                /// @example env_keys()  // ["HOME", "PATH", ...]
                if (strcmp(fn_name, "env_keys") == 0) {
                    if (argc != 0) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("env_keys() expects no arguments")); }
                    free(args);
                    char **keys = NULL;
                    size_t key_count = 0;
                    envvar_keys(&keys, &key_count);
                    LatValue *elems = NULL;
                    if (key_count > 0) {
                        elems = malloc(key_count * sizeof(LatValue));
                        for (size_t i = 0; i < key_count; i++) {
                            elems[i] = value_string_owned(keys[i]);
                        }
                    }
                    free(keys);
                    LatValue arr = value_array(elems, key_count);
                    free(elems);
                    return eval_ok(arr);
                }

                /* ── Time builtins ── */

                /// @builtin time() -> Int
                /// @category Date & Time
                /// Return the current Unix timestamp in milliseconds.
                /// @example time()  // 1700000000000
                if (strcmp(fn_name, "time") == 0) {
                    if (argc != 0) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("time() expects no arguments")); }
                    free(args);
                    return eval_ok(value_int(time_now_ms()));
                }

                /// @builtin sleep(ms: Int) -> Unit
                /// @category Date & Time
                /// Pause execution for the given number of milliseconds.
                /// @example sleep(1000)  // sleeps for 1 second
                if (strcmp(fn_name, "sleep") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("sleep() expects (Int milliseconds)")); }
                    char *terr = NULL;
                    bool ok = time_sleep_ms(args[0].as.int_val, &terr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (!ok) return eval_err(terr);
                    return eval_ok(value_unit());
                }

                /* ── Process/system builtins ── */

                /// @builtin cwd() -> String
                /// @category Process
                /// Return the current working directory.
                /// @example cwd()  // "/home/user/project"
                if (strcmp(fn_name, "cwd") == 0) {
                    if (argc != 0) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("cwd() expects no arguments")); }
                    free(args);
                    char *cwd_err = NULL;
                    char *dir = process_cwd(&cwd_err);
                    if (!dir) return eval_err(cwd_err);
                    return eval_ok(value_string_owned(dir));
                }

                /// @builtin exec(cmd: String) -> Map
                /// @category Process
                /// Execute a command directly (no shell), returning {stdout, stderr, status}.
                /// @example exec("ls -la")  // {stdout: "...", stderr: "", status: 0}
                if (strcmp(fn_name, "exec") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("exec() expects 1 argument")); }
                    if (args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("exec() expects a string command")); }
                    char *exec_err = NULL;
                    LatValue result = process_exec(args[0].as.str_val, &exec_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (exec_err) return eval_err(exec_err);
                    return eval_ok(result);
                }

                /// @builtin shell(cmd: String) -> Map
                /// @category Process
                /// Execute a command via the system shell, returning {stdout, stderr, status}.
                /// @example shell("echo hello")  // {stdout: "hello\n", stderr: "", status: 0}
                if (strcmp(fn_name, "shell") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("shell() expects 1 argument")); }
                    if (args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("shell() expects a string command")); }
                    char *shell_err = NULL;
                    LatValue result = process_shell(args[0].as.str_val, &shell_err);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (shell_err) return eval_err(shell_err);
                    return eval_ok(result);
                }

                /// @builtin args() -> Array
                /// @category Process
                /// Return command-line arguments as an array of strings.
                /// @example args()  // ["script.lat", "--flag"]
                if (strcmp(fn_name, "args") == 0) {
                    if (argc != 0) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("args() expects no arguments")); }
                    free(args);
#ifdef __EMSCRIPTEN__
                    LatValue arr = value_array(NULL, 0);
                    return eval_ok(arr);
#else
                    int ac = ev->prog_argc;
                    char **av = ev->prog_argv;
                    LatValue *elems = NULL;
                    if (ac > 0) {
                        elems = malloc((size_t)ac * sizeof(LatValue));
                        for (int i = 0; i < ac; i++)
                            elems[i] = value_string(av[i]);
                    }
                    LatValue arr = value_array(elems, (size_t)ac);
                    free(elems);
                    return eval_ok(arr);
#endif
                }

                /// @builtin platform() -> String
                /// @category Process
                /// Return the operating system name ("darwin", "linux", etc.).
                /// @example platform()  // "darwin"
                if (strcmp(fn_name, "platform") == 0) {
                    if (argc != 0) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("platform() expects no arguments")); }
                    free(args);
                    return eval_ok(value_string(process_platform()));
                }

                /// @builtin hostname() -> String
                /// @category Process
                /// Return the system hostname.
                /// @example hostname()  // "my-machine"
                if (strcmp(fn_name, "hostname") == 0) {
                    if (argc != 0) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("hostname() expects no arguments")); }
                    free(args);
                    char *h_err = NULL;
                    char *name = process_hostname(&h_err);
                    if (!name) return eval_err(h_err);
                    LatValue v = value_string_owned(name);
                    return eval_ok(v);
                }

                /// @builtin pid() -> Int
                /// @category Process
                /// Return the current process ID.
                /// @example pid()  // 12345
                if (strcmp(fn_name, "pid") == 0) {
                    if (argc != 0) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("pid() expects no arguments")); }
                    free(args);
                    return eval_ok(value_int((int64_t)process_pid()));
                }

                /* ── URL encoding builtins ── */

                /// @builtin url_encode(s: String) -> String
                /// @category URL
                /// Percent-encode a string for use in URLs.
                /// @example url_encode("hello world")  // "hello%20world"
                if (strcmp(fn_name, "url_encode") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("url_encode() expects (String)")); }
                    const char *src = args[0].as.str_val;
                    size_t slen = strlen(src);
                    /* Worst case: every byte becomes %XX (3x expansion) */
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
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string_owned(out));
                }

                /// @builtin url_decode(s: String) -> String
                /// @category URL
                /// Decode a percent-encoded URL string.
                /// @example url_decode("hello%20world")  // "hello world"
                if (strcmp(fn_name, "url_decode") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("url_decode() expects (String)")); }
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
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string_owned(out));
                }

                /* ── CSV builtins ── */

                /// @builtin csv_parse(s: String) -> Array
                /// @category CSV
                /// Parse a CSV string into an array of arrays (rows of fields).
                /// @example csv_parse("a,b\n1,2")  // [["a", "b"], ["1", "2"]]
                if (strcmp(fn_name, "csv_parse") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("csv_parse() expects (String)")); }
                    const char *input = args[0].as.str_val;
                    size_t pos = 0;
                    size_t input_len = strlen(input);

                    /* Collect rows into a temporary array */
                    size_t rows_cap = 8;
                    size_t rows_len = 0;
                    LatValue *rows = malloc(rows_cap * sizeof(LatValue));

                    while (pos < input_len) {
                        /* Parse one row: collect fields */
                        size_t fields_cap = 8;
                        size_t fields_len = 0;
                        LatValue *fields = malloc(fields_cap * sizeof(LatValue));

                        for (;;) {
                            /* Parse one field */
                            size_t field_cap = 64;
                            size_t field_len = 0;
                            char *field = malloc(field_cap);

                            if (pos < input_len && input[pos] == '"') {
                                /* Quoted field */
                                pos++; /* skip opening quote */
                                for (;;) {
                                    if (pos >= input_len) break;
                                    if (input[pos] == '"') {
                                        if (pos + 1 < input_len && input[pos + 1] == '"') {
                                            /* Escaped quote */
                                            if (field_len + 1 >= field_cap) { field_cap *= 2; field = realloc(field, field_cap); }
                                            field[field_len++] = '"';
                                            pos += 2;
                                        } else {
                                            /* End of quoted field */
                                            pos++; /* skip closing quote */
                                            break;
                                        }
                                    } else {
                                        if (field_len + 1 >= field_cap) { field_cap *= 2; field = realloc(field, field_cap); }
                                        field[field_len++] = input[pos++];
                                    }
                                }
                            } else {
                                /* Unquoted field */
                                while (pos < input_len && input[pos] != ',' && input[pos] != '\n' && input[pos] != '\r') {
                                    if (field_len + 1 >= field_cap) { field_cap *= 2; field = realloc(field, field_cap); }
                                    field[field_len++] = input[pos++];
                                }
                            }

                            field[field_len] = '\0';

                            /* Add field to fields array */
                            if (fields_len >= fields_cap) { fields_cap *= 2; fields = realloc(fields, fields_cap * sizeof(LatValue)); }
                            fields[fields_len++] = value_string_owned(field);

                            /* Check what follows */
                            if (pos < input_len && input[pos] == ',') {
                                pos++; /* skip comma, continue to next field */
                            } else {
                                break; /* end of row */
                            }
                        }

                        /* Skip line ending */
                        if (pos < input_len && input[pos] == '\r') pos++;
                        if (pos < input_len && input[pos] == '\n') pos++;

                        /* Build row array and add to rows (value_array does shallow copy, so don't free elements) */
                        LatValue row = value_array(fields, fields_len);
                        free(fields);

                        if (rows_len >= rows_cap) { rows_cap *= 2; rows = realloc(rows, rows_cap * sizeof(LatValue)); }
                        rows[rows_len++] = row;
                    }

                    LatValue result = value_array(rows, rows_len);
                    free(rows);

                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(result);
                }

                /// @builtin csv_stringify(rows: Array) -> String
                /// @category CSV
                /// Convert an array of arrays into a CSV string.
                /// @example csv_stringify([["a", "b"], ["1", "2"]])  // "a,b\n1,2\n"
                if (strcmp(fn_name, "csv_stringify") == 0) {
                    if (argc != 1 || args[0].type != VAL_ARRAY) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("csv_stringify() expects (Array)")); }

                    LatValue *data = &args[0];
                    size_t out_cap = 256;
                    size_t out_len = 0;
                    char *out = malloc(out_cap);

                    for (size_t r = 0; r < data->as.array.len; r++) {
                        LatValue *row = &data->as.array.elems[r];
                        if (row->type != VAL_ARRAY) {
                            free(out);
                            for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                            free(args);
                            return eval_err(strdup("csv_stringify(): each row must be an Array"));
                        }
                        for (size_t c = 0; c < row->as.array.len; c++) {
                            if (c > 0) {
                                if (out_len + 1 >= out_cap) { out_cap *= 2; out = realloc(out, out_cap); }
                                out[out_len++] = ',';
                            }

                            char *field_str = value_display(&row->as.array.elems[c]);
                            size_t flen = strlen(field_str);

                            /* Check if quoting is needed */
                            bool needs_quote = false;
                            for (size_t k = 0; k < flen; k++) {
                                if (field_str[k] == ',' || field_str[k] == '"' || field_str[k] == '\n' || field_str[k] == '\r') {
                                    needs_quote = true;
                                    break;
                                }
                            }

                            if (needs_quote) {
                                /* Count quotes for escaped size */
                                size_t extra = 0;
                                for (size_t k = 0; k < flen; k++) {
                                    if (field_str[k] == '"') extra++;
                                }
                                size_t needed = flen + extra + 2; /* +2 for surrounding quotes */
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
                        /* Append newline */
                        if (out_len + 1 >= out_cap) { out_cap *= 2; out = realloc(out, out_cap); }
                        out[out_len++] = '\n';
                    }
                    out[out_len] = '\0';

                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string_owned(out));
                }

                /* ── TOML builtins ── */

                /// @builtin toml_parse(s: String) -> Map
                /// @category Data Formats
                /// Parse a TOML string into a Lattice Map.
                /// @example toml_parse("[server]\nhost = \"localhost\"\nport = 8080")
                if (strcmp(fn_name, "toml_parse") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) {
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(strdup("toml_parse() expects (String)"));
                    }
                    char *terr = NULL;
                    LatValue result = toml_ops_parse(args[0].as.str_val, &terr);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    if (terr) return eval_err(terr);
                    return eval_ok(result);
                }

                /// @builtin toml_stringify(val: Map) -> String
                /// @category Data Formats
                /// Serialize a Lattice Map to a TOML string.
                /// @example toml_stringify({"host": "localhost", "port": 8080})
                if (strcmp(fn_name, "toml_stringify") == 0) {
                    if (argc != 1) {
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(strdup("toml_stringify() expects (Map)"));
                    }
                    char *terr = NULL;
                    char *toml = toml_ops_stringify(&args[0], &terr);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    if (terr) { free(toml); return eval_err(terr); }
                    return eval_ok(value_string_owned(toml));
                }

                /* ── YAML builtins ── */

                /// @builtin yaml_parse(s: String) -> Map|Array
                /// @category Data Formats
                /// Parse a YAML string into a Lattice value.
                /// @example yaml_parse("name: Alice\nage: 30")
                if (strcmp(fn_name, "yaml_parse") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) {
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(strdup("yaml_parse() expects (String)"));
                    }
                    char *yerr = NULL;
                    LatValue result = yaml_ops_parse(args[0].as.str_val, &yerr);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    if (yerr) return eval_err(yerr);
                    return eval_ok(result);
                }

                /// @builtin yaml_stringify(val: Map|Array) -> String
                /// @category Data Formats
                /// Serialize a Lattice value to a YAML string.
                /// @example yaml_stringify({"name": "Alice", "age": 30})
                if (strcmp(fn_name, "yaml_stringify") == 0) {
                    if (argc != 1) {
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(strdup("yaml_stringify() expects (value)"));
                    }
                    if (args[0].type != VAL_MAP && args[0].type != VAL_ARRAY) {
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(strdup("yaml_stringify: value must be a Map or Array"));
                    }
                    char *yerr = NULL;
                    char *yaml = yaml_ops_stringify(&args[0], &yerr);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    if (yerr) { free(yaml); return eval_err(yerr); }
                    return eval_ok(value_string_owned(yaml));
                }

                /* ── Regex builtins ── */

                /// @builtin regex_match(pattern: String, str: String) -> Bool
                /// @category Regex
                /// Test if a string matches a regular expression pattern.
                /// @example regex_match("^[0-9]+$", "123")  // true
                if (strcmp(fn_name, "regex_match") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("regex_match() expects (String pattern, String str)")); }
                    char *rerr = NULL;
                    LatValue result = regex_match(args[0].as.str_val, args[1].as.str_val, &rerr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (rerr) return eval_err(rerr);
                    return eval_ok(result);
                }

                /// @builtin regex_find_all(pattern: String, str: String) -> Array
                /// @category Regex
                /// Find all matches of a pattern in a string, returning an array.
                /// @example regex_find_all("[0-9]+", "a1b2c3")  // ["1", "2", "3"]
                if (strcmp(fn_name, "regex_find_all") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("regex_find_all() expects (String pattern, String str)")); }
                    char *rerr = NULL;
                    LatValue result = regex_find_all(args[0].as.str_val, args[1].as.str_val, &rerr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (rerr) return eval_err(rerr);
                    return eval_ok(result);
                }

                /// @builtin regex_replace(pattern: String, str: String, replacement: String) -> String
                /// @category Regex
                /// Replace all matches of a pattern in a string.
                /// @example regex_replace("[0-9]", "a1b2", "X")  // "aXbX"
                if (strcmp(fn_name, "regex_replace") == 0) {
                    if (argc != 3 || args[0].type != VAL_STR || args[1].type != VAL_STR || args[2].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("regex_replace() expects (String pattern, String str, String replacement)")); }
                    char *rerr = NULL;
                    char *result = regex_replace(args[0].as.str_val, args[1].as.str_val, args[2].as.str_val, &rerr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (rerr) return eval_err(rerr);
                    return eval_ok(value_string_owned(result));
                }

                /// @builtin format(fmt: String, args: Any...) -> String
                /// @category String Formatting
                /// Format a string with placeholders replaced by arguments.
                /// @example format("{} is {}", "sky", "blue")  // "sky is blue"
                if (strcmp(fn_name, "format") == 0) {
                    if (argc < 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("format() expects (String fmt, ...)")); }
                    char *ferr = NULL;
                    char *result = format_string(args[0].as.str_val, args + 1, argc - 1, &ferr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (ferr) return eval_err(ferr);
                    return eval_ok(value_string_owned(result));
                }

                /* ── Crypto builtins ── */

                /// @builtin sha256(s: String) -> String
                /// @category Crypto
                /// Compute the SHA-256 hash of a string, returned as hex.
                /// @example sha256("hello")  // "2cf24dba..."
                if (strcmp(fn_name, "sha256") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("sha256() expects (String)")); }
                    char *cerr = NULL;
                    char *result = crypto_sha256(args[0].as.str_val, strlen(args[0].as.str_val), &cerr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (cerr) return eval_err(cerr);
                    return eval_ok(value_string_owned(result));
                }

                /// @builtin md5(s: String) -> String
                /// @category Crypto
                /// Compute the MD5 hash of a string, returned as hex.
                /// @example md5("hello")  // "5d41402a..."
                if (strcmp(fn_name, "md5") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("md5() expects (String)")); }
                    char *cerr = NULL;
                    char *result = crypto_md5(args[0].as.str_val, strlen(args[0].as.str_val), &cerr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (cerr) return eval_err(cerr);
                    return eval_ok(value_string_owned(result));
                }

                /// @builtin base64_encode(s: String) -> String
                /// @category Crypto
                /// Encode a string to Base64.
                /// @example base64_encode("hello")  // "aGVsbG8="
                if (strcmp(fn_name, "base64_encode") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("base64_encode() expects (String)")); }
                    char *result = crypto_base64_encode(args[0].as.str_val, strlen(args[0].as.str_val));
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_string_owned(result));
                }

                /// @builtin base64_decode(s: String) -> String
                /// @category Crypto
                /// Decode a Base64 string.
                /// @example base64_decode("aGVsbG8=")  // "hello"
                if (strcmp(fn_name, "base64_decode") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("base64_decode() expects (String)")); }
                    char *cerr = NULL;
                    size_t decoded_len = 0;
                    char *result = crypto_base64_decode(args[0].as.str_val, strlen(args[0].as.str_val), &decoded_len, &cerr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (cerr) return eval_err(cerr);
                    return eval_ok(value_string_owned(result));
                }

                /// @builtin sha512(s: String) -> String
                /// @category Crypto
                /// Compute the SHA-512 hash of a string, returned as hex.
                /// @example sha512("hello")  // "9b71d224..."
                if (strcmp(fn_name, "sha512") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("sha512() expects (String)")); }
                    char *cerr = NULL;
                    char *result = crypto_sha512(args[0].as.str_val, strlen(args[0].as.str_val), &cerr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (cerr) return eval_err(cerr);
                    return eval_ok(value_string_owned(result));
                }

                /// @builtin hmac_sha256(key: String, data: String) -> String
                /// @category Crypto
                /// Compute the HMAC-SHA256 of data with key, returned as hex.
                /// @example hmac_sha256("secret", "hello")  // "88aab3ed..."
                if (strcmp(fn_name, "hmac_sha256") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("hmac_sha256() expects (String key, String data)")); }
                    char *cerr = NULL;
                    char *result = crypto_hmac_sha256(args[0].as.str_val, strlen(args[0].as.str_val),
                                                      args[1].as.str_val, strlen(args[1].as.str_val), &cerr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (cerr) return eval_err(cerr);
                    return eval_ok(value_string_owned(result));
                }

                /// @builtin random_bytes(n: Int) -> Buffer
                /// @category Crypto
                /// Generate n cryptographically secure random bytes.
                /// @example random_bytes(16).length()  // 16
                if (strcmp(fn_name, "random_bytes") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("random_bytes() expects (Int n)")); }
                    int64_t n = args[0].as.int_val;
                    if (n < 0 || n > 1048576) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("random_bytes(): n must be 0..1048576")); }
                    char *cerr = NULL;
                    uint8_t *buf = crypto_random_bytes((size_t)n, &cerr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (cerr) return eval_err(cerr);
                    LatValue result = value_buffer(buf, (size_t)n);
                    free(buf);
                    return eval_ok(result);
                }

                /* ── Date/time formatting builtins ── */

                /// @builtin time_format(epoch_ms: Int, fmt: String) -> String
                /// @category Date & Time
                /// Format a Unix timestamp (ms) using a strftime format string.
                /// @example time_format(0, "%Y-%m-%d")  // "1970-01-01"
                if (strcmp(fn_name, "time_format") == 0) {
                    if (argc != 2 || args[0].type != VAL_INT || args[1].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("time_format() expects (Int epoch_ms, String fmt)")); }
                    char *terr = NULL;
                    char *result = datetime_format(args[0].as.int_val, args[1].as.str_val, &terr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (terr) return eval_err(terr);
                    return eval_ok(value_string_owned(result));
                }

                /// @builtin time_parse(datetime: String, fmt: String) -> Int
                /// @category Date & Time
                /// Parse a datetime string into a Unix timestamp (ms).
                /// @example time_parse("2024-01-01", "%Y-%m-%d")  // 1704067200000
                if (strcmp(fn_name, "time_parse") == 0) {
                    if (argc != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("time_parse() expects (String datetime, String fmt)")); }
                    char *terr = NULL;
                    int64_t result = datetime_parse(args[0].as.str_val, args[1].as.str_val, &terr);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    if (terr) return eval_err(terr);
                    return eval_ok(value_int(result));
                }

                /// @builtin time_year(epoch_ms: Int) -> Int
                /// @category Date & Time
                /// Extract the year from a timestamp.
                /// @example time_year(0)  // 1970
                if (strcmp(fn_name, "time_year") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("time_year() expects (Int epoch_ms)")); }
                    int r = datetime_year(args[0].as.int_val);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_int(r));
                }
                /// @builtin time_month(epoch_ms: Int) -> Int
                /// @category Date & Time
                /// Extract the month (1-12) from a timestamp.
                /// @example time_month(0)  // 1
                if (strcmp(fn_name, "time_month") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("time_month() expects (Int epoch_ms)")); }
                    int r = datetime_month(args[0].as.int_val);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_int(r));
                }
                /// @builtin time_day(epoch_ms: Int) -> Int
                /// @category Date & Time
                /// Extract the day of month (1-31) from a timestamp.
                /// @example time_day(0)  // 1
                if (strcmp(fn_name, "time_day") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("time_day() expects (Int epoch_ms)")); }
                    int r = datetime_day(args[0].as.int_val);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_int(r));
                }
                /// @builtin time_hour(epoch_ms: Int) -> Int
                /// @category Date & Time
                /// Extract the hour (0-23) from a timestamp.
                /// @example time_hour(0)  // 0
                if (strcmp(fn_name, "time_hour") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("time_hour() expects (Int epoch_ms)")); }
                    int r = datetime_hour(args[0].as.int_val);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_int(r));
                }
                /// @builtin time_minute(epoch_ms: Int) -> Int
                /// @category Date & Time
                /// Extract the minute (0-59) from a timestamp.
                /// @example time_minute(0)  // 0
                if (strcmp(fn_name, "time_minute") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("time_minute() expects (Int epoch_ms)")); }
                    int r = datetime_minute(args[0].as.int_val);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_int(r));
                }
                /// @builtin time_second(epoch_ms: Int) -> Int
                /// @category Date & Time
                /// Extract the second (0-59) from a timestamp.
                /// @example time_second(0)  // 0
                if (strcmp(fn_name, "time_second") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("time_second() expects (Int epoch_ms)")); }
                    int r = datetime_second(args[0].as.int_val);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_int(r));
                }
                /// @builtin time_weekday(epoch_ms: Int) -> Int
                /// @category Date & Time
                /// Extract the day of week (0=Sunday, 6=Saturday) from a timestamp.
                /// @example time_weekday(0)  // 4
                if (strcmp(fn_name, "time_weekday") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("time_weekday() expects (Int epoch_ms)")); }
                    int r = datetime_weekday(args[0].as.int_val);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_int(r));
                }
                /// @builtin time_add(epoch_ms: Int, delta_ms: Int) -> Int
                /// @category Date & Time
                /// Add milliseconds to a timestamp.
                /// @example time_add(0, 86400000)  // 86400000
                if (strcmp(fn_name, "time_add") == 0) {
                    if (argc != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("time_add() expects (Int epoch_ms, Int delta_ms)")); }
                    int64_t r = datetime_add(args[0].as.int_val, args[1].as.int_val);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_int(r));
                }
                /// @builtin is_leap_year(year: Int) -> Bool
                /// @category Date & Time
                /// Check if a year is a leap year.
                /// @example is_leap_year(2024)  // true
                if (strcmp(fn_name, "is_leap_year") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("is_leap_year() expects (Int year)")); }
                    bool r = datetime_is_leap_year((int)args[0].as.int_val);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_bool(r));
                }

                /// @builtin days_in_month(year: Int, month: Int) -> Int
                /// @category Date & Time
                /// Number of days in the given month of the given year.
                /// @example days_in_month(2024, 2)  // 29
                if (strcmp(fn_name, "days_in_month") == 0) {
                    if (argc != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("days_in_month() expects (Int year, Int month)")); }
                    int r = datetime_days_in_month((int)args[0].as.int_val, (int)args[1].as.int_val);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    if (r < 0) return eval_err(strdup("days_in_month: month must be 1-12"));
                    return eval_ok(value_int(r));
                }

                /// @builtin day_of_week(year: Int, month: Int, day: Int) -> Int
                /// @category Date & Time
                /// Day of week (0=Sunday, 6=Saturday).
                /// @example day_of_week(2026, 2, 24)  // 2
                if (strcmp(fn_name, "day_of_week") == 0) {
                    if (argc != 3 || args[0].type != VAL_INT || args[1].type != VAL_INT || args[2].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("day_of_week() expects (Int year, Int month, Int day)")); }
                    int r = datetime_day_of_week((int)args[0].as.int_val, (int)args[1].as.int_val, (int)args[2].as.int_val);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_int(r));
                }

                /// @builtin day_of_year(year: Int, month: Int, day: Int) -> Int
                /// @category Date & Time
                /// Day of year (1-366).
                /// @example day_of_year(2026, 2, 24)  // 55
                if (strcmp(fn_name, "day_of_year") == 0) {
                    if (argc != 3 || args[0].type != VAL_INT || args[1].type != VAL_INT || args[2].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("day_of_year() expects (Int year, Int month, Int day)")); }
                    int r = datetime_day_of_year((int)args[0].as.int_val, (int)args[1].as.int_val, (int)args[2].as.int_val);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    if (r < 0) return eval_err(strdup("day_of_year: month must be 1-12"));
                    return eval_ok(value_int(r));
                }

                /// @builtin timezone_offset() -> Int
                /// @category Date & Time
                /// Current local timezone offset from UTC in seconds.
                if (strcmp(fn_name, "timezone_offset") == 0) {
                    if (argc != 0) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("timezone_offset() expects no arguments")); }
                    free(args);
                    return eval_ok(value_int(datetime_tz_offset_seconds()));
                }

                /// @builtin duration(hours: Int, minutes: Int, seconds: Int, millis: Int) -> Map
                /// @category Date & Time
                /// Create a Duration map.
                if (strcmp(fn_name, "duration") == 0) {
                    if (argc != 4 || args[0].type != VAL_INT || args[1].type != VAL_INT ||
                        args[2].type != VAL_INT || args[3].type != VAL_INT) {
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(strdup("duration() expects (Int hours, Int minutes, Int seconds, Int millis)"));
                    }
                    int64_t total = args[0].as.int_val * 3600000 + args[1].as.int_val * 60000 +
                                    args[2].as.int_val * 1000    + args[3].as.int_val;
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    /* Build duration map */
                    int64_t ms = total % 1000; if (ms < 0) ms = -ms;
                    int64_t rem = total / 1000;
                    int64_t s = rem % 60; if (s < 0) s = -s;
                    rem /= 60;
                    int64_t m = rem % 60; if (m < 0) m = -m;
                    int64_t h = rem / 60;
                    LatValue map = value_map_new();
                    LatValue vh = value_int(h); lat_map_set(map.as.map.map, "hours", &vh);
                    LatValue vm = value_int(m); lat_map_set(map.as.map.map, "minutes", &vm);
                    LatValue vs = value_int(s); lat_map_set(map.as.map.map, "seconds", &vs);
                    LatValue vms = value_int(ms); lat_map_set(map.as.map.map, "millis", &vms);
                    LatValue vtot = value_int(total); lat_map_set(map.as.map.map, "total_ms", &vtot);
                    return eval_ok(map);
                }

                /// @builtin duration_from_seconds(s: Int) -> Map
                /// @category Date & Time
                /// Create a Duration from total seconds.
                if (strcmp(fn_name, "duration_from_seconds") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("duration_from_seconds() expects (Int seconds)")); }
                    int64_t total = args[0].as.int_val * 1000;
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    int64_t ms = total % 1000; if (ms < 0) ms = -ms;
                    int64_t rem = total / 1000;
                    int64_t s = rem % 60; if (s < 0) s = -s;
                    rem /= 60;
                    int64_t m = rem % 60; if (m < 0) m = -m;
                    int64_t h = rem / 60;
                    LatValue map = value_map_new();
                    LatValue vh = value_int(h); lat_map_set(map.as.map.map, "hours", &vh);
                    LatValue vm = value_int(m); lat_map_set(map.as.map.map, "minutes", &vm);
                    LatValue vs = value_int(s); lat_map_set(map.as.map.map, "seconds", &vs);
                    LatValue vms = value_int(ms); lat_map_set(map.as.map.map, "millis", &vms);
                    LatValue vtot = value_int(total); lat_map_set(map.as.map.map, "total_ms", &vtot);
                    return eval_ok(map);
                }

                /// @builtin duration_from_millis(ms: Int) -> Map
                /// @category Date & Time
                /// Create a Duration from total milliseconds.
                if (strcmp(fn_name, "duration_from_millis") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("duration_from_millis() expects (Int millis)")); }
                    int64_t total = args[0].as.int_val;
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    int64_t ms = total % 1000; if (ms < 0) ms = -ms;
                    int64_t rem = total / 1000;
                    int64_t s = rem % 60; if (s < 0) s = -s;
                    rem /= 60;
                    int64_t m = rem % 60; if (m < 0) m = -m;
                    int64_t h = rem / 60;
                    LatValue map = value_map_new();
                    LatValue vh = value_int(h); lat_map_set(map.as.map.map, "hours", &vh);
                    LatValue vm = value_int(m); lat_map_set(map.as.map.map, "minutes", &vm);
                    LatValue vs = value_int(s); lat_map_set(map.as.map.map, "seconds", &vs);
                    LatValue vms = value_int(ms); lat_map_set(map.as.map.map, "millis", &vms);
                    LatValue vtot = value_int(total); lat_map_set(map.as.map.map, "total_ms", &vtot);
                    return eval_ok(map);
                }

                /// @builtin duration_add(d1: Map, d2: Map) -> Map
                /// @category Date & Time
                /// Add two Duration maps.
                if (strcmp(fn_name, "duration_add") == 0) {
                    if (argc != 2 || args[0].type != VAL_MAP || args[1].type != VAL_MAP) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("duration_add() expects (Map d1, Map d2)")); }
                    LatValue *t1 = lat_map_get(args[0].as.map.map, "total_ms");
                    LatValue *t2 = lat_map_get(args[1].as.map.map, "total_ms");
                    int64_t total = ((t1 && t1->type == VAL_INT) ? t1->as.int_val : 0) +
                                    ((t2 && t2->type == VAL_INT) ? t2->as.int_val : 0);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    int64_t ms = total % 1000; if (ms < 0) ms = -ms;
                    int64_t rem = total / 1000;
                    int64_t s = rem % 60; if (s < 0) s = -s;
                    rem /= 60;
                    int64_t m = rem % 60; if (m < 0) m = -m;
                    int64_t h = rem / 60;
                    LatValue map = value_map_new();
                    LatValue vh = value_int(h); lat_map_set(map.as.map.map, "hours", &vh);
                    LatValue vm = value_int(m); lat_map_set(map.as.map.map, "minutes", &vm);
                    LatValue vs = value_int(s); lat_map_set(map.as.map.map, "seconds", &vs);
                    LatValue vms = value_int(ms); lat_map_set(map.as.map.map, "millis", &vms);
                    LatValue vtot = value_int(total); lat_map_set(map.as.map.map, "total_ms", &vtot);
                    return eval_ok(map);
                }

                /// @builtin duration_sub(d1: Map, d2: Map) -> Map
                /// @category Date & Time
                /// Subtract Duration d2 from d1.
                if (strcmp(fn_name, "duration_sub") == 0) {
                    if (argc != 2 || args[0].type != VAL_MAP || args[1].type != VAL_MAP) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("duration_sub() expects (Map d1, Map d2)")); }
                    LatValue *t1 = lat_map_get(args[0].as.map.map, "total_ms");
                    LatValue *t2 = lat_map_get(args[1].as.map.map, "total_ms");
                    int64_t total = ((t1 && t1->type == VAL_INT) ? t1->as.int_val : 0) -
                                    ((t2 && t2->type == VAL_INT) ? t2->as.int_val : 0);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    int64_t ms = total % 1000; if (ms < 0) ms = -ms;
                    int64_t rem = total / 1000;
                    int64_t s = rem % 60; if (s < 0) s = -s;
                    rem /= 60;
                    int64_t m = rem % 60; if (m < 0) m = -m;
                    int64_t h = rem / 60;
                    LatValue map = value_map_new();
                    LatValue vh = value_int(h); lat_map_set(map.as.map.map, "hours", &vh);
                    LatValue vm = value_int(m); lat_map_set(map.as.map.map, "minutes", &vm);
                    LatValue vs = value_int(s); lat_map_set(map.as.map.map, "seconds", &vs);
                    LatValue vms = value_int(ms); lat_map_set(map.as.map.map, "millis", &vms);
                    LatValue vtot = value_int(total); lat_map_set(map.as.map.map, "total_ms", &vtot);
                    return eval_ok(map);
                }

                /// @builtin duration_to_string(d: Map) -> String
                /// @category Date & Time
                /// Format a Duration as "2h 30m 15s".
                if (strcmp(fn_name, "duration_to_string") == 0) {
                    if (argc != 1 || args[0].type != VAL_MAP) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("duration_to_string() expects (Map duration)")); }
                    LatValue *tot = lat_map_get(args[0].as.map.map, "total_ms");
                    int64_t total = (tot && tot->type == VAL_INT) ? tot->as.int_val : 0;
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    int64_t ms = total % 1000; if (ms < 0) ms = -ms;
                    int64_t rem = total / 1000;
                    int64_t s = rem % 60; if (s < 0) s = -s;
                    rem /= 60;
                    int64_t m = rem % 60; if (m < 0) m = -m;
                    int64_t h = rem / 60;
                    char buf[128];
                    if (ms > 0) {
                        snprintf(buf, sizeof(buf), "%lldh %lldm %llds %lldms",
                                 (long long)h, (long long)m, (long long)s, (long long)ms);
                    } else {
                        snprintf(buf, sizeof(buf), "%lldh %lldm %llds",
                                 (long long)h, (long long)m, (long long)s);
                    }
                    return eval_ok(value_string(buf));
                }

                /// @builtin duration_hours(d: Map) -> Int
                /// @category Date & Time
                /// Extract hours from a Duration.
                if (strcmp(fn_name, "duration_hours") == 0) {
                    if (argc != 1 || args[0].type != VAL_MAP) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("duration_hours() expects (Map duration)")); }
                    LatValue *v = lat_map_get(args[0].as.map.map, "hours");
                    int64_t r = (v && v->type == VAL_INT) ? v->as.int_val : 0;
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_int(r));
                }
                /// @builtin duration_minutes(d: Map) -> Int
                /// @category Date & Time
                /// Extract minutes from a Duration.
                if (strcmp(fn_name, "duration_minutes") == 0) {
                    if (argc != 1 || args[0].type != VAL_MAP) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("duration_minutes() expects (Map duration)")); }
                    LatValue *v = lat_map_get(args[0].as.map.map, "minutes");
                    int64_t r = (v && v->type == VAL_INT) ? v->as.int_val : 0;
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_int(r));
                }
                /// @builtin duration_seconds(d: Map) -> Int
                /// @category Date & Time
                /// Extract seconds from a Duration.
                if (strcmp(fn_name, "duration_seconds") == 0) {
                    if (argc != 1 || args[0].type != VAL_MAP) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("duration_seconds() expects (Map duration)")); }
                    LatValue *v = lat_map_get(args[0].as.map.map, "seconds");
                    int64_t r = (v && v->type == VAL_INT) ? v->as.int_val : 0;
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_int(r));
                }
                /// @builtin duration_millis(d: Map) -> Int
                /// @category Date & Time
                /// Extract millis from a Duration.
                if (strcmp(fn_name, "duration_millis") == 0) {
                    if (argc != 1 || args[0].type != VAL_MAP) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("duration_millis() expects (Map duration)")); }
                    LatValue *v = lat_map_get(args[0].as.map.map, "millis");
                    int64_t r = (v && v->type == VAL_INT) ? v->as.int_val : 0;
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_int(r));
                }

                /// @builtin datetime_now() -> Map
                /// @category Date & Time
                /// Returns DateTime map with current local time.
                if (strcmp(fn_name, "datetime_now") == 0) {
                    if (argc != 0) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("datetime_now() expects no arguments")); }
                    free(args);
                    time_t now = time(NULL);
                    struct tm local;
                    localtime_r(&now, &local);
                    int tz_off = datetime_tz_offset_seconds();
                    LatValue map = value_map_new();
                    LatValue vy = value_int(local.tm_year + 1900); lat_map_set(map.as.map.map, "year", &vy);
                    LatValue vmo = value_int(local.tm_mon + 1); lat_map_set(map.as.map.map, "month", &vmo);
                    LatValue vd = value_int(local.tm_mday); lat_map_set(map.as.map.map, "day", &vd);
                    LatValue vh = value_int(local.tm_hour); lat_map_set(map.as.map.map, "hour", &vh);
                    LatValue vmi = value_int(local.tm_min); lat_map_set(map.as.map.map, "minute", &vmi);
                    LatValue vs = value_int(local.tm_sec); lat_map_set(map.as.map.map, "second", &vs);
                    LatValue vtz = value_int(tz_off); lat_map_set(map.as.map.map, "tz_offset", &vtz);
                    return eval_ok(map);
                }

                /// @builtin datetime_from_epoch(epoch_seconds: Int) -> Map
                /// @category Date & Time
                /// Create DateTime from epoch seconds (UTC).
                if (strcmp(fn_name, "datetime_from_epoch") == 0) {
                    if (argc != 1 || args[0].type != VAL_INT) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("datetime_from_epoch() expects (Int epoch_seconds)")); }
                    int64_t epoch = args[0].as.int_val;
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    int y, mo, d, h, mi, s;
                    datetime_to_utc_components(epoch, &y, &mo, &d, &h, &mi, &s);
                    LatValue map = value_map_new();
                    LatValue vy = value_int(y); lat_map_set(map.as.map.map, "year", &vy);
                    LatValue vmo2 = value_int(mo); lat_map_set(map.as.map.map, "month", &vmo2);
                    LatValue vd2 = value_int(d); lat_map_set(map.as.map.map, "day", &vd2);
                    LatValue vh2 = value_int(h); lat_map_set(map.as.map.map, "hour", &vh2);
                    LatValue vmi2 = value_int(mi); lat_map_set(map.as.map.map, "minute", &vmi2);
                    LatValue vs2 = value_int(s); lat_map_set(map.as.map.map, "second", &vs2);
                    LatValue vtz = value_int(0); lat_map_set(map.as.map.map, "tz_offset", &vtz);
                    return eval_ok(map);
                }

                /// @builtin datetime_to_epoch(dt: Map) -> Int
                /// @category Date & Time
                /// Convert DateTime map to epoch seconds.
                if (strcmp(fn_name, "datetime_to_epoch") == 0) {
                    if (argc != 1 || args[0].type != VAL_MAP) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("datetime_to_epoch() expects (Map dt)")); }
                    LatValue *vy = lat_map_get(args[0].as.map.map, "year");
                    LatValue *vmo = lat_map_get(args[0].as.map.map, "month");
                    LatValue *vd = lat_map_get(args[0].as.map.map, "day");
                    LatValue *vh = lat_map_get(args[0].as.map.map, "hour");
                    LatValue *vmi = lat_map_get(args[0].as.map.map, "minute");
                    LatValue *vs = lat_map_get(args[0].as.map.map, "second");
                    LatValue *vtz = lat_map_get(args[0].as.map.map, "tz_offset");
                    if (!vy || vy->type != VAL_INT || !vmo || vmo->type != VAL_INT || !vd || vd->type != VAL_INT) {
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(strdup("datetime_to_epoch: invalid DateTime map"));
                    }
                    int64_t epoch = datetime_from_components(
                        (int)vy->as.int_val, (int)vmo->as.int_val, (int)vd->as.int_val,
                        (vh && vh->type == VAL_INT) ? (int)vh->as.int_val : 0,
                        (vmi && vmi->type == VAL_INT) ? (int)vmi->as.int_val : 0,
                        (vs && vs->type == VAL_INT) ? (int)vs->as.int_val : 0,
                        (vtz && vtz->type == VAL_INT) ? (int)vtz->as.int_val : 0);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    return eval_ok(value_int(epoch));
                }

                /// @builtin datetime_from_iso(str: String) -> Map
                /// @category Date & Time
                /// Parse ISO 8601 string into DateTime map.
                if (strcmp(fn_name, "datetime_from_iso") == 0) {
                    if (argc != 1 || args[0].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("datetime_from_iso() expects (String iso)")); }
                    char *err = NULL;
                    int64_t epoch = datetime_parse_iso(args[0].as.str_val, &err);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    if (err) return eval_err(err);
                    int y, mo, d, h, mi, s;
                    datetime_to_utc_components(epoch, &y, &mo, &d, &h, &mi, &s);
                    LatValue map = value_map_new();
                    LatValue vy2 = value_int(y); lat_map_set(map.as.map.map, "year", &vy2);
                    LatValue vmo2 = value_int(mo); lat_map_set(map.as.map.map, "month", &vmo2);
                    LatValue vd2 = value_int(d); lat_map_set(map.as.map.map, "day", &vd2);
                    LatValue vh2 = value_int(h); lat_map_set(map.as.map.map, "hour", &vh2);
                    LatValue vmi2 = value_int(mi); lat_map_set(map.as.map.map, "minute", &vmi2);
                    LatValue vs2 = value_int(s); lat_map_set(map.as.map.map, "second", &vs2);
                    LatValue vtz = value_int(0); lat_map_set(map.as.map.map, "tz_offset", &vtz);
                    return eval_ok(map);
                }

                /// @builtin datetime_to_iso(dt: Map) -> String
                /// @category Date & Time
                /// Format DateTime map as ISO 8601 string.
                if (strcmp(fn_name, "datetime_to_iso") == 0) {
                    if (argc != 1 || args[0].type != VAL_MAP) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("datetime_to_iso() expects (Map dt)")); }
                    LatValue *vy = lat_map_get(args[0].as.map.map, "year");
                    LatValue *vmo = lat_map_get(args[0].as.map.map, "month");
                    LatValue *vd = lat_map_get(args[0].as.map.map, "day");
                    LatValue *vh = lat_map_get(args[0].as.map.map, "hour");
                    LatValue *vmi = lat_map_get(args[0].as.map.map, "minute");
                    LatValue *vs = lat_map_get(args[0].as.map.map, "second");
                    LatValue *vtz = lat_map_get(args[0].as.map.map, "tz_offset");
                    if (!vy || vy->type != VAL_INT || !vmo || vmo->type != VAL_INT || !vd || vd->type != VAL_INT) {
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(strdup("datetime_to_iso: invalid DateTime map"));
                    }
                    int64_t epoch = datetime_from_components(
                        (int)vy->as.int_val, (int)vmo->as.int_val, (int)vd->as.int_val,
                        (vh && vh->type == VAL_INT) ? (int)vh->as.int_val : 0,
                        (vmi && vmi->type == VAL_INT) ? (int)vmi->as.int_val : 0,
                        (vs && vs->type == VAL_INT) ? (int)vs->as.int_val : 0,
                        (vtz && vtz->type == VAL_INT) ? (int)vtz->as.int_val : 0);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    char *iso = datetime_to_iso(epoch);
                    return eval_ok(value_string_owned(iso));
                }

                /// @builtin datetime_add_duration(dt: Map, dur: Map) -> Map
                /// @category Date & Time
                /// Add Duration to DateTime.
                if (strcmp(fn_name, "datetime_add_duration") == 0) {
                    if (argc != 2 || args[0].type != VAL_MAP || args[1].type != VAL_MAP) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("datetime_add_duration() expects (Map dt, Map dur)")); }
                    LatValue *vy = lat_map_get(args[0].as.map.map, "year");
                    LatValue *vmo = lat_map_get(args[0].as.map.map, "month");
                    LatValue *vd = lat_map_get(args[0].as.map.map, "day");
                    LatValue *vh = lat_map_get(args[0].as.map.map, "hour");
                    LatValue *vmi = lat_map_get(args[0].as.map.map, "minute");
                    LatValue *vs = lat_map_get(args[0].as.map.map, "second");
                    LatValue *vtz = lat_map_get(args[0].as.map.map, "tz_offset");
                    if (!vy || vy->type != VAL_INT || !vmo || vmo->type != VAL_INT || !vd || vd->type != VAL_INT) {
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(strdup("datetime_add_duration: invalid DateTime map"));
                    }
                    int tz = (vtz && vtz->type == VAL_INT) ? (int)vtz->as.int_val : 0;
                    int64_t epoch = datetime_from_components(
                        (int)vy->as.int_val, (int)vmo->as.int_val, (int)vd->as.int_val,
                        (vh && vh->type == VAL_INT) ? (int)vh->as.int_val : 0,
                        (vmi && vmi->type == VAL_INT) ? (int)vmi->as.int_val : 0,
                        (vs && vs->type == VAL_INT) ? (int)vs->as.int_val : 0, tz);
                    LatValue *dur_tot = lat_map_get(args[1].as.map.map, "total_ms");
                    int64_t dur_ms = (dur_tot && dur_tot->type == VAL_INT) ? dur_tot->as.int_val : 0;
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    epoch += dur_ms / 1000;
                    int64_t utc_epoch = epoch + (int64_t)tz;
                    int ny, nmo, nd, nh, nmi, ns;
                    datetime_to_utc_components(utc_epoch, &ny, &nmo, &nd, &nh, &nmi, &ns);
                    LatValue map = value_map_new();
                    LatValue vy2 = value_int(ny); lat_map_set(map.as.map.map, "year", &vy2);
                    LatValue vmo2 = value_int(nmo); lat_map_set(map.as.map.map, "month", &vmo2);
                    LatValue vd2 = value_int(nd); lat_map_set(map.as.map.map, "day", &vd2);
                    LatValue vh2 = value_int(nh); lat_map_set(map.as.map.map, "hour", &vh2);
                    LatValue vmi2 = value_int(nmi); lat_map_set(map.as.map.map, "minute", &vmi2);
                    LatValue vs2 = value_int(ns); lat_map_set(map.as.map.map, "second", &vs2);
                    LatValue vtz2 = value_int(tz); lat_map_set(map.as.map.map, "tz_offset", &vtz2);
                    return eval_ok(map);
                }

                /// @builtin datetime_sub(dt1: Map, dt2: Map) -> Map
                /// @category Date & Time
                /// Subtract two DateTimes, returning a Duration.
                if (strcmp(fn_name, "datetime_sub") == 0) {
                    if (argc != 2 || args[0].type != VAL_MAP || args[1].type != VAL_MAP) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("datetime_sub() expects (Map dt1, Map dt2)")); }
                    LatValue *vy1 = lat_map_get(args[0].as.map.map, "year");
                    LatValue *vmo1 = lat_map_get(args[0].as.map.map, "month");
                    LatValue *vd1 = lat_map_get(args[0].as.map.map, "day");
                    LatValue *vh1 = lat_map_get(args[0].as.map.map, "hour");
                    LatValue *vmi1 = lat_map_get(args[0].as.map.map, "minute");
                    LatValue *vs1 = lat_map_get(args[0].as.map.map, "second");
                    LatValue *vtz1 = lat_map_get(args[0].as.map.map, "tz_offset");
                    LatValue *vy2 = lat_map_get(args[1].as.map.map, "year");
                    LatValue *vmo2 = lat_map_get(args[1].as.map.map, "month");
                    LatValue *vd2 = lat_map_get(args[1].as.map.map, "day");
                    LatValue *vh2 = lat_map_get(args[1].as.map.map, "hour");
                    LatValue *vmi2 = lat_map_get(args[1].as.map.map, "minute");
                    LatValue *vs2 = lat_map_get(args[1].as.map.map, "second");
                    LatValue *vtz2 = lat_map_get(args[1].as.map.map, "tz_offset");
                    if (!vy1 || vy1->type != VAL_INT || !vmo1 || vmo1->type != VAL_INT || !vd1 || vd1->type != VAL_INT ||
                        !vy2 || vy2->type != VAL_INT || !vmo2 || vmo2->type != VAL_INT || !vd2 || vd2->type != VAL_INT) {
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(strdup("datetime_sub: invalid DateTime map"));
                    }
                    int64_t e1 = datetime_from_components(
                        (int)vy1->as.int_val, (int)vmo1->as.int_val, (int)vd1->as.int_val,
                        (vh1 && vh1->type == VAL_INT) ? (int)vh1->as.int_val : 0,
                        (vmi1 && vmi1->type == VAL_INT) ? (int)vmi1->as.int_val : 0,
                        (vs1 && vs1->type == VAL_INT) ? (int)vs1->as.int_val : 0,
                        (vtz1 && vtz1->type == VAL_INT) ? (int)vtz1->as.int_val : 0);
                    int64_t e2 = datetime_from_components(
                        (int)vy2->as.int_val, (int)vmo2->as.int_val, (int)vd2->as.int_val,
                        (vh2 && vh2->type == VAL_INT) ? (int)vh2->as.int_val : 0,
                        (vmi2 && vmi2->type == VAL_INT) ? (int)vmi2->as.int_val : 0,
                        (vs2 && vs2->type == VAL_INT) ? (int)vs2->as.int_val : 0,
                        (vtz2 && vtz2->type == VAL_INT) ? (int)vtz2->as.int_val : 0);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    int64_t total = (e1 - e2) * 1000;
                    int64_t ms = total % 1000; if (ms < 0) ms = -ms;
                    int64_t rem = total / 1000;
                    int64_t s = rem % 60; if (s < 0) s = -s;
                    rem /= 60;
                    int64_t m = rem % 60; if (m < 0) m = -m;
                    int64_t h = rem / 60;
                    LatValue map = value_map_new();
                    LatValue vh_r = value_int(h); lat_map_set(map.as.map.map, "hours", &vh_r);
                    LatValue vm_r = value_int(m); lat_map_set(map.as.map.map, "minutes", &vm_r);
                    LatValue vs_r = value_int(s); lat_map_set(map.as.map.map, "seconds", &vs_r);
                    LatValue vms_r = value_int(ms); lat_map_set(map.as.map.map, "millis", &vms_r);
                    LatValue vtot = value_int(total); lat_map_set(map.as.map.map, "total_ms", &vtot);
                    return eval_ok(map);
                }

                /// @builtin datetime_format(dt: Map, fmt: String) -> String
                /// @category Date & Time
                /// Format DateTime using strftime-style format.
                if (strcmp(fn_name, "datetime_format") == 0 && argc == 2 && args[0].type == VAL_MAP) {
                    if (args[1].type != VAL_STR) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("datetime_format() expects (Map dt, String fmt)")); }
                    LatValue *vy = lat_map_get(args[0].as.map.map, "year");
                    LatValue *vmo = lat_map_get(args[0].as.map.map, "month");
                    LatValue *vd = lat_map_get(args[0].as.map.map, "day");
                    LatValue *vh = lat_map_get(args[0].as.map.map, "hour");
                    LatValue *vmi = lat_map_get(args[0].as.map.map, "minute");
                    LatValue *vs = lat_map_get(args[0].as.map.map, "second");
                    LatValue *vtz = lat_map_get(args[0].as.map.map, "tz_offset");
                    if (!vy || vy->type != VAL_INT || !vmo || vmo->type != VAL_INT || !vd || vd->type != VAL_INT) {
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(strdup("datetime_format: invalid DateTime map"));
                    }
                    int64_t epoch = datetime_from_components(
                        (int)vy->as.int_val, (int)vmo->as.int_val, (int)vd->as.int_val,
                        (vh && vh->type == VAL_INT) ? (int)vh->as.int_val : 0,
                        (vmi && vmi->type == VAL_INT) ? (int)vmi->as.int_val : 0,
                        (vs && vs->type == VAL_INT) ? (int)vs->as.int_val : 0,
                        (vtz && vtz->type == VAL_INT) ? (int)vtz->as.int_val : 0);
                    struct tm tm;
                    time_t t = (time_t)epoch;
                    gmtime_r(&t, &tm);
                    char buf[512];
                    size_t n = strftime(buf, sizeof(buf), args[1].as.str_val, &tm);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    if (n == 0) return eval_err(strdup("datetime_format: format produced empty string"));
                    return eval_ok(value_string(buf));
                }

                /// @builtin datetime_to_utc(dt: Map) -> Map
                /// @category Date & Time
                /// Convert DateTime to UTC.
                if (strcmp(fn_name, "datetime_to_utc") == 0) {
                    if (argc != 1 || args[0].type != VAL_MAP) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("datetime_to_utc() expects (Map dt)")); }
                    LatValue *vy = lat_map_get(args[0].as.map.map, "year");
                    LatValue *vmo = lat_map_get(args[0].as.map.map, "month");
                    LatValue *vd = lat_map_get(args[0].as.map.map, "day");
                    LatValue *vh = lat_map_get(args[0].as.map.map, "hour");
                    LatValue *vmi = lat_map_get(args[0].as.map.map, "minute");
                    LatValue *vs = lat_map_get(args[0].as.map.map, "second");
                    LatValue *vtz = lat_map_get(args[0].as.map.map, "tz_offset");
                    if (!vy || vy->type != VAL_INT || !vmo || vmo->type != VAL_INT || !vd || vd->type != VAL_INT) {
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(strdup("datetime_to_utc: invalid DateTime map"));
                    }
                    int64_t epoch = datetime_from_components(
                        (int)vy->as.int_val, (int)vmo->as.int_val, (int)vd->as.int_val,
                        (vh && vh->type == VAL_INT) ? (int)vh->as.int_val : 0,
                        (vmi && vmi->type == VAL_INT) ? (int)vmi->as.int_val : 0,
                        (vs && vs->type == VAL_INT) ? (int)vs->as.int_val : 0,
                        (vtz && vtz->type == VAL_INT) ? (int)vtz->as.int_val : 0);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    int ny, nmo, nd, nh, nmi, ns;
                    datetime_to_utc_components(epoch, &ny, &nmo, &nd, &nh, &nmi, &ns);
                    LatValue map = value_map_new();
                    LatValue vy2 = value_int(ny); lat_map_set(map.as.map.map, "year", &vy2);
                    LatValue vmo2 = value_int(nmo); lat_map_set(map.as.map.map, "month", &vmo2);
                    LatValue vd2 = value_int(nd); lat_map_set(map.as.map.map, "day", &vd2);
                    LatValue vh2 = value_int(nh); lat_map_set(map.as.map.map, "hour", &vh2);
                    LatValue vmi2 = value_int(nmi); lat_map_set(map.as.map.map, "minute", &vmi2);
                    LatValue vs2 = value_int(ns); lat_map_set(map.as.map.map, "second", &vs2);
                    LatValue vtz2 = value_int(0); lat_map_set(map.as.map.map, "tz_offset", &vtz2);
                    return eval_ok(map);
                }

                /// @builtin datetime_to_local(dt: Map) -> Map
                /// @category Date & Time
                /// Convert DateTime to local timezone.
                if (strcmp(fn_name, "datetime_to_local") == 0) {
                    if (argc != 1 || args[0].type != VAL_MAP) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("datetime_to_local() expects (Map dt)")); }
                    LatValue *vy = lat_map_get(args[0].as.map.map, "year");
                    LatValue *vmo = lat_map_get(args[0].as.map.map, "month");
                    LatValue *vd = lat_map_get(args[0].as.map.map, "day");
                    LatValue *vh = lat_map_get(args[0].as.map.map, "hour");
                    LatValue *vmi = lat_map_get(args[0].as.map.map, "minute");
                    LatValue *vs = lat_map_get(args[0].as.map.map, "second");
                    LatValue *vtz = lat_map_get(args[0].as.map.map, "tz_offset");
                    if (!vy || vy->type != VAL_INT || !vmo || vmo->type != VAL_INT || !vd || vd->type != VAL_INT) {
                        for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                        return eval_err(strdup("datetime_to_local: invalid DateTime map"));
                    }
                    int64_t epoch = datetime_from_components(
                        (int)vy->as.int_val, (int)vmo->as.int_val, (int)vd->as.int_val,
                        (vh && vh->type == VAL_INT) ? (int)vh->as.int_val : 0,
                        (vmi && vmi->type == VAL_INT) ? (int)vmi->as.int_val : 0,
                        (vs && vs->type == VAL_INT) ? (int)vs->as.int_val : 0,
                        (vtz && vtz->type == VAL_INT) ? (int)vtz->as.int_val : 0);
                    for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args);
                    time_t t = (time_t)epoch;
                    struct tm local;
                    localtime_r(&t, &local);
                    int local_tz = datetime_tz_offset_seconds();
                    LatValue map = value_map_new();
                    LatValue vy2 = value_int(local.tm_year + 1900); lat_map_set(map.as.map.map, "year", &vy2);
                    LatValue vmo2 = value_int(local.tm_mon + 1); lat_map_set(map.as.map.map, "month", &vmo2);
                    LatValue vd2 = value_int(local.tm_mday); lat_map_set(map.as.map.map, "day", &vd2);
                    LatValue vh2 = value_int(local.tm_hour); lat_map_set(map.as.map.map, "hour", &vh2);
                    LatValue vmi2 = value_int(local.tm_min); lat_map_set(map.as.map.map, "minute", &vmi2);
                    LatValue vs2 = value_int(local.tm_sec); lat_map_set(map.as.map.map, "second", &vs2);
                    LatValue vtz2 = value_int(local_tz); lat_map_set(map.as.map.map, "tz_offset", &vtz2);
                    return eval_ok(map);
                }

                /* ── Assertion builtin ── */

                /// @builtin assert(cond: Any, msg?: String) -> Unit
                /// @category Core
                /// Assert that a condition is truthy, or raise an error with an optional message.
                /// @example assert(1 == 1, "math works")
                if (strcmp(fn_name, "assert") == 0) {
                    if (argc < 1 || argc > 2) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("assert() expects 1 or 2 arguments")); }
                    bool truthy = value_is_truthy(&args[0]);
                    if (!truthy) {
                        char *msg = NULL;
                        if (argc == 2 && args[1].type == VAL_STR)
                            msg = strdup(args[1].as.str_val);
                        else
                            msg = strdup("assertion failed");
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(msg);
                    }
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_unit());
                }

                /// @builtin debug_assert(cond: Any, msg?: String) -> Unit
                /// @category Core
                /// Assert that a condition is truthy (no-op when assertions are disabled via --no-assertions).
                /// @example debug_assert(x > 0, "x must be positive")
                if (strcmp(fn_name, "debug_assert") == 0) {
                    if (argc < 1 || argc > 2) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("debug_assert() expects 1 or 2 arguments")); }
                    if (ev->assertions_enabled) {
                        bool truthy = value_is_truthy(&args[0]);
                        if (!truthy) {
                            char *msg = NULL;
                            if (argc == 2 && args[1].type == VAL_STR)
                                msg = strdup(args[1].as.str_val);
                            else
                                msg = strdup("debug assertion failed");
                            for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                            free(args);
                            return eval_err(msg);
                        }
                    }
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(value_unit());
                }

                /* ── Functional programming builtins ── */

                /// @builtin identity(val: Any) -> Any
                /// @category Functional
                /// Return the argument unchanged.
                /// @example identity(42)  // 42
                if (strcmp(fn_name, "identity") == 0) {
                    if (argc != 1) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("identity() expects 1 argument")); }
                    LatValue result = value_deep_clone(&args[0]);
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(result);
                }

                /// @builtin pipe(val: Any, fns: Closure...) -> Any
                /// @category Functional
                /// Thread a value through a series of functions left to right.
                /// @example pipe(5, |x| { x * 2 }, |x| { x + 1 })  // 11
                if (strcmp(fn_name, "pipe") == 0) {
                    if (argc < 2) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("pipe() expects a value and at least one function")); }
                    for (size_t i = 1; i < argc; i++) {
                        if (args[i].type != VAL_CLOSURE) {
                            char *err = NULL;
                            (void)asprintf(&err, "pipe() argument %zu is not a function", i + 1);
                            for (size_t j = 0; j < argc; j++) value_free(&args[j]);
                            free(args);
                            return eval_err(err);
                        }
                    }
                    LatValue current = value_deep_clone(&args[0]);
                    for (size_t i = 1; i < argc; i++) {
                        LatValue call_arg = current;
                        EvalResult r = call_closure(ev,
                            args[i].as.closure.param_names,
                            args[i].as.closure.param_count,
                            args[i].as.closure.body,
                            args[i].as.closure.captured_env,
                            &call_arg, 1,
                            args[i].as.closure.default_values, args[i].as.closure.has_variadic);
                        if (!IS_OK(r)) {
                            for (size_t j = 0; j < argc; j++) value_free(&args[j]);
                            free(args);
                            return r;
                        }
                        current = r.value;
                    }
                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(current);
                }

                /// @builtin compose(f: Closure, g: Closure) -> Closure
                /// @category Functional
                /// Compose two functions: compose(f, g)(x) calls f(g(x)).
                /// @example compose(|x| { x + 1 }, |x| { x * 2 })(3)  // 7
                if (strcmp(fn_name, "compose") == 0) {
                    if (argc != 2) { for (size_t i = 0; i < argc; i++) { value_free(&args[i]); } free(args); return eval_err(strdup("compose() expects 2 arguments (both closures)")); }
                    if (args[0].type != VAL_CLOSURE || args[1].type != VAL_CLOSURE) {
                        for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                        free(args);
                        return eval_err(strdup("compose() arguments must be closures"));
                    }
                    /* Build closure |x| { __compose_f(__compose_g(x)) }
                     * where f = args[0], g = args[1] */
                    Env *cenv = env_clone(ev->env);
                    env_push_scope(cenv);
                    env_define(cenv, "__compose_f", value_deep_clone(&args[0]));
                    env_define(cenv, "__compose_g", value_deep_clone(&args[1]));

                    /* AST: __compose_f(__compose_g(x))  (intentionally leaked — borrowed by closure) */
                    Expr *x_var = expr_ident(strdup("x"));
                    Expr **g_cargs = malloc(sizeof(Expr *));
                    g_cargs[0] = x_var;
                    Expr *g_call = expr_call(expr_ident(strdup("__compose_g")), g_cargs, 1);
                    Expr **f_cargs = malloc(sizeof(Expr *));
                    f_cargs[0] = g_call;
                    Expr *body = expr_call(expr_ident(strdup("__compose_f")), f_cargs, 1);

                    char **params = malloc(sizeof(char *));
                    params[0] = strdup("x");
                    LatValue closure = value_closure(params, 1, body, cenv, NULL, false);

                    for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                    free(args);
                    return eval_ok(closure);
                }

                /* ── Named function lookup ── */
                FnDecl *fd_head = find_fn(ev, fn_name);
                if (fd_head) {
                    FnDecl *fd = fd_head;
                    if (fd_head->next_overload) {
                        fd = resolve_overload(fd_head, args, argc);
                        if (!fd) {
                            char *err = NULL;
                            (void)asprintf(&err, "no matching overload for '%s' with given argument phases", fn_name);
                            for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                            free(args);
                            return eval_err(err);
                        }
                    }
                    /* Allocate write-back slots for fluid parameters */
                    LatValue **writeback = calloc(argc, sizeof(LatValue *));
                    EvalResult res = call_fn(ev, fd, args, argc, writeback);
                    /* Write back fluid parameters to caller's env */
                    if (IS_OK(res)) {
                        for (size_t i = 0; i < argc; i++) {
                            if (writeback[i] && expr->as.call.args[i]->tag == EXPR_IDENT) {
                                env_set(ev->env, expr->as.call.args[i]->as.str_val, *writeback[i]);
                                free(writeback[i]);
                                writeback[i] = NULL;
                            }
                        }
                    }
                    /* Clean up any unused writebacks */
                    for (size_t i = 0; i < argc; i++) {
                        if (writeback[i]) {
                            value_free(writeback[i]);
                            free(writeback[i]);
                        }
                    }
                    free(writeback);
                    free(args);
                    return res;
                }
            }
            /* Otherwise evaluate callee — re-protect args since eval may trigger GC */
            for (size_t i = 0; i < argc; i++) GC_PUSH(ev, &args[i]);
            EvalResult callee_r = eval_expr(ev, expr->as.call.func);
            GC_POP_N(ev, argc);
            if (!IS_OK(callee_r)) {
                for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                free(args);
                return callee_r;
            }
            if (callee_r.value.type != VAL_CLOSURE) {
                char *err = NULL;
                (void)asprintf(&err, "'%s' is not callable", value_type_name(&callee_r.value));
                value_free(&callee_r.value);
                for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                free(args);
                return eval_err(err);
            }
            /* Derive a name for stack traces */
            const char *closure_name = (expr->as.call.func->tag == EXPR_IDENT)
                ? expr->as.call.func->as.str_val : "<closure>";
            /* Native closure dispatch */
            if (callee_r.value.as.closure.native_fn && !callee_r.value.as.closure.body) {
                ev_push_frame(ev, closure_name);
                EvalResult res;
                if (callee_r.value.as.closure.default_values == (struct Expr **)(uintptr_t)0x1) {
                    /* VM-style native (VMNativeFn signature) — used by builtin modules */
                    typedef LatValue (*VMNativeFn)(LatValue *, int);
                    VMNativeFn fn = (VMNativeFn)callee_r.value.as.closure.native_fn;
                    /* Ensure current_rt exists for native error reporting */
                    LatRuntime *prev_rt = lat_runtime_current();
                    LatRuntime tmp_rt;
                    if (!prev_rt) {
                        memset(&tmp_rt, 0, sizeof(tmp_rt));
                        lat_runtime_set_current(&tmp_rt);
                    }
                    LatRuntime *rt = lat_runtime_current();
                    LatValue result = fn(args, (int)argc);
                    if (rt->error) {
                        char *msg = rt->error;
                        rt->error = NULL;
                        value_free(&result);
                        res = eval_err(msg);
                    } else {
                        res = eval_ok(result);
                    }
                    if (!prev_rt) lat_runtime_set_current(NULL);
                } else {
                    /* Extension native (LatExtFn signature) */
                    res = call_native_closure(ev,
                        callee_r.value.as.closure.native_fn, args, argc);
                }
                if (!IS_ERR(res)) ev_pop_frame(ev);
                for (size_t i = 0; i < argc; i++) value_free(&args[i]);
                value_free(&callee_r.value);
                free(args);
                return res;
            }
            /* Root callee and args so GC inside block-body closures won't sweep them */
            GC_PUSH(ev, &callee_r.value);
            for (size_t i = 0; i < argc; i++) GC_PUSH(ev, &args[i]);
            ev_push_frame(ev, closure_name);
            EvalResult res = call_closure(ev,
                callee_r.value.as.closure.param_names,
                callee_r.value.as.closure.param_count,
                callee_r.value.as.closure.body,
                callee_r.value.as.closure.captured_env,
                args, argc,
                callee_r.value.as.closure.default_values, callee_r.value.as.closure.has_variadic);
            if (!IS_ERR(res)) ev_pop_frame(ev);
            GC_POP_N(ev, argc);
            GC_POP(ev);
            value_free(&callee_r.value);
            free(args);
            return res;
        }

        case EXPR_METHOD_CALL: {
            /* Handle .push() specially - needs to mutate the binding in env */
            if (strcmp(expr->as.method_call.method, "push") == 0 &&
                expr->as.method_call.object->tag == EXPR_IDENT &&
                expr->as.method_call.arg_count == 1) {
                const char *var_name = expr->as.method_call.object->as.str_val;
                LatValue existing;
                if (!env_get(ev->env, var_name, &existing)) {
                    char *err = NULL;
                    (void)asprintf(&err, "undefined variable '%s'", var_name);
                    return eval_err(err);
                }
                if (existing.type != VAL_ARRAY && existing.type != VAL_BUFFER && existing.type != VAL_REF) {
                    value_free(&existing);
                    return eval_err(strdup(".push() is not defined on non-array"));
                }
                /* Ref/Buffer push: handle via resolve_lvalue path below */
                if (existing.type == VAL_BUFFER || existing.type == VAL_REF) {
                    value_free(&existing);
                    goto buffer_mutating_methods;
                }
                if (value_is_crystal(&existing)) {
                    value_free(&existing);
                    return eval_err(strdup("cannot push to a crystal array"));
                }
                if (existing.phase == VTAG_SUBLIMATED) {
                    value_free(&existing);
                    return eval_err(strdup("cannot push to a sublimated array"));
                }
                {
                    const char *pmode = find_pressure(ev, var_name);
                    if (pressure_blocks_grow(pmode)) {
                        value_free(&existing);
                        char *err = NULL;
                        (void)asprintf(&err, "pressurized (%s): cannot push to '%s'", pmode, var_name);
                        return eval_err(err);
                    }
                }
                GC_PUSH(ev, &existing);
                EvalResult ar = eval_expr(ev, expr->as.method_call.args[0]);
                GC_POP(ev);
                if (!IS_OK(ar)) { value_free(&existing); return ar; }
                /* Grow the array */
                if (existing.as.array.len >= existing.as.array.cap) {
                    size_t old_cap = existing.as.array.cap;
                    existing.as.array.cap = old_cap < 4 ? 4 : old_cap * 2;
                    LatValue *new_buf = fluid_alloc(ev->heap->fluid,
                        existing.as.array.cap * sizeof(LatValue));
                    memcpy(new_buf, existing.as.array.elems, old_cap * sizeof(LatValue));
                    if (!fluid_dealloc(ev->heap->fluid, existing.as.array.elems))
                        free(existing.as.array.elems);
                    existing.as.array.elems = new_buf;
                }
                existing.as.array.elems[existing.as.array.len++] = ar.value;
                env_set(ev->env, var_name, existing);
                return eval_ok(value_unit());
            }
            /// @method Map.set(key: String, value: Any) -> Unit
            /// @category Map Methods
            /// Set a key-value pair in the map (mutates in place).
            /// @example m.set("name", "Alice")
            /* Handle .set() on maps - needs mutation like .push() */
            if (strcmp(expr->as.method_call.method, "set") == 0 &&
                expr->as.method_call.arg_count == 2) {
                char *lv_err = NULL;
                LatValue *map_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (map_lv && map_lv->type == VAL_REF) map_lv = &map_lv->as.ref.ref->value;
                if (map_lv && map_lv->type == VAL_MAP) {
                    if (map_lv->phase == VTAG_SUBLIMATED)
                        return eval_err(strdup("cannot set on a sublimated map"));
                    EvalResult kr = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(kr)) return kr;
                    if (kr.value.type != VAL_STR) {
                        value_free(&kr.value);
                        return eval_err(strdup(".set() key must be a string"));
                    }
                    GC_PUSH(ev, &kr.value);
                    EvalResult vr = eval_expr(ev, expr->as.method_call.args[1]);
                    GC_POP(ev);
                    if (!IS_OK(vr)) { value_free(&kr.value); return vr; }
                    /* Free old value if key exists */
                    LatValue *old = (LatValue *)lat_map_get(map_lv->as.map.map, kr.value.as.str_val);
                    if (old) value_free(old);
                    lat_map_set(map_lv->as.map.map, kr.value.as.str_val, &vr.value);
                    value_free(&kr.value);
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }
            /* Handle .pop() on arrays - needs mutation */
            if (strcmp(expr->as.method_call.method, "pop") == 0 &&
                expr->as.method_call.arg_count == 0) {
                char *lv_err = NULL;
                LatValue *arr_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (arr_lv && arr_lv->type == VAL_REF) arr_lv = &arr_lv->as.ref.ref->value;
                if (arr_lv && arr_lv->type == VAL_ARRAY) {
                    if (value_is_crystal(arr_lv))
                        return eval_err(strdup("cannot pop from a crystal array"));
                    if (arr_lv->phase == VTAG_SUBLIMATED)
                        return eval_err(strdup("cannot pop from a sublimated array"));
                    {
                        const char *vn = get_method_obj_varname(expr->as.method_call.object);
                        if (vn) {
                            const char *pmode = find_pressure(ev, vn);
                            if (pressure_blocks_shrink(pmode)) {
                                char *err = NULL;
                                (void)asprintf(&err, "pressurized (%s): cannot pop from '%s'", pmode, vn);
                                return eval_err(err);
                            }
                        }
                    }
                    if (arr_lv->as.array.len == 0)
                        return eval_err(strdup("pop on empty array"));
                    LatValue popped = arr_lv->as.array.elems[--arr_lv->as.array.len];
                    return eval_ok(popped);
                }
                if (lv_err) free(lv_err);
            }
            /* Handle .insert() on arrays - needs mutation */
            if (strcmp(expr->as.method_call.method, "insert") == 0 &&
                expr->as.method_call.arg_count == 2) {
                char *lv_err = NULL;
                LatValue *arr_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (arr_lv && arr_lv->type == VAL_REF) arr_lv = &arr_lv->as.ref.ref->value;
                if (arr_lv && arr_lv->type == VAL_ARRAY) {
                    if (value_is_crystal(arr_lv))
                        return eval_err(strdup("cannot insert into a crystal array"));
                    if (arr_lv->phase == VTAG_SUBLIMATED)
                        return eval_err(strdup("cannot insert into a sublimated array"));
                    {
                        const char *vn = get_method_obj_varname(expr->as.method_call.object);
                        if (vn) {
                            const char *pmode = find_pressure(ev, vn);
                            if (pressure_blocks_grow(pmode)) {
                                char *err = NULL;
                                (void)asprintf(&err, "pressurized (%s): cannot insert into '%s'", pmode, vn);
                                return eval_err(err);
                            }
                        }
                    }
                    EvalResult ir = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(ir)) return ir;
                    if (ir.value.type != VAL_INT) {
                        value_free(&ir.value);
                        return eval_err(strdup(".insert() index must be an integer"));
                    }
                    int64_t idx = ir.value.as.int_val;
                    value_free(&ir.value);
                    if (idx < 0 || (size_t)idx > arr_lv->as.array.len)
                        return eval_err(strdup(".insert() index out of bounds"));
                    EvalResult vr = eval_expr(ev, expr->as.method_call.args[1]);
                    if (!IS_OK(vr)) return vr;
                    /* Grow if needed */
                    if (arr_lv->as.array.len >= arr_lv->as.array.cap) {
                        size_t old_cap = arr_lv->as.array.cap;
                        arr_lv->as.array.cap = old_cap < 4 ? 4 : old_cap * 2;
                        LatValue *new_buf = fluid_alloc(ev->heap->fluid,
                            arr_lv->as.array.cap * sizeof(LatValue));
                        memcpy(new_buf, arr_lv->as.array.elems, old_cap * sizeof(LatValue));
                        if (!fluid_dealloc(ev->heap->fluid, arr_lv->as.array.elems))
                            free(arr_lv->as.array.elems);
                        arr_lv->as.array.elems = new_buf;
                    }
                    /* Shift elements right */
                    memmove(&arr_lv->as.array.elems[(size_t)idx + 1],
                            &arr_lv->as.array.elems[(size_t)idx],
                            (arr_lv->as.array.len - (size_t)idx) * sizeof(LatValue));
                    arr_lv->as.array.elems[(size_t)idx] = vr.value;
                    arr_lv->as.array.len++;
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }
            /* Handle .remove_at() on arrays - needs mutation */
            if (strcmp(expr->as.method_call.method, "remove_at") == 0 &&
                expr->as.method_call.arg_count == 1) {
                char *lv_err = NULL;
                LatValue *arr_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (arr_lv && arr_lv->type == VAL_REF) arr_lv = &arr_lv->as.ref.ref->value;
                if (arr_lv && arr_lv->type == VAL_ARRAY) {
                    if (value_is_crystal(arr_lv))
                        return eval_err(strdup("cannot remove from a crystal array"));
                    if (arr_lv->phase == VTAG_SUBLIMATED)
                        return eval_err(strdup("cannot remove from a sublimated array"));
                    {
                        const char *vn = get_method_obj_varname(expr->as.method_call.object);
                        if (vn) {
                            const char *pmode = find_pressure(ev, vn);
                            if (pressure_blocks_shrink(pmode)) {
                                char *err = NULL;
                                (void)asprintf(&err, "pressurized (%s): cannot remove from '%s'", pmode, vn);
                                return eval_err(err);
                            }
                        }
                    }
                    EvalResult ir = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(ir)) return ir;
                    if (ir.value.type != VAL_INT) {
                        value_free(&ir.value);
                        return eval_err(strdup(".remove_at() index must be an integer"));
                    }
                    int64_t idx = ir.value.as.int_val;
                    value_free(&ir.value);
                    if (idx < 0 || (size_t)idx >= arr_lv->as.array.len)
                        return eval_err(strdup(".remove_at() index out of bounds"));
                    LatValue removed = arr_lv->as.array.elems[(size_t)idx];
                    /* Shift elements left */
                    memmove(&arr_lv->as.array.elems[(size_t)idx],
                            &arr_lv->as.array.elems[(size_t)idx + 1],
                            (arr_lv->as.array.len - (size_t)idx - 1) * sizeof(LatValue));
                    arr_lv->as.array.len--;
                    return eval_ok(removed);
                }
                if (lv_err) free(lv_err);
            }
            /* Handle .merge() on maps - needs mutation */
            if (strcmp(expr->as.method_call.method, "merge") == 0 &&
                expr->as.method_call.arg_count == 1) {
                char *lv_err = NULL;
                LatValue *map_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (map_lv && map_lv->type == VAL_REF) map_lv = &map_lv->as.ref.ref->value;
                if (map_lv && map_lv->type == VAL_MAP) {
                    if (map_lv->phase == VTAG_SUBLIMATED)
                        return eval_err(strdup("cannot merge into a sublimated map"));
                    {
                        const char *vn = get_method_obj_varname(expr->as.method_call.object);
                        if (vn) {
                            const char *pmode = find_pressure(ev, vn);
                            if (pressure_blocks_grow(pmode)) {
                                char *err = NULL;
                                (void)asprintf(&err, "pressurized (%s): cannot merge into '%s'", pmode, vn);
                                return eval_err(err);
                            }
                        }
                    }
                    EvalResult mr = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(mr)) return mr;
                    if (mr.value.type != VAL_MAP) {
                        value_free(&mr.value);
                        return eval_err(strdup(".merge() argument must be a Map"));
                    }
                    LatMap *other = mr.value.as.map.map;
                    for (size_t i = 0; i < other->cap; i++) {
                        if (other->entries[i].state == MAP_OCCUPIED) {
                            LatValue cloned = value_deep_clone((LatValue *)other->entries[i].value);
                            LatValue *old = (LatValue *)lat_map_get(map_lv->as.map.map, other->entries[i].key);
                            if (old) value_free(old);
                            lat_map_set(map_lv->as.map.map, other->entries[i].key, &cloned);
                        }
                    }
                    value_free(&mr.value);
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }
            /// @method Map.remove(key: String) -> Unit
            /// @category Map Methods
            /// Remove a key from the map (mutates in place).
            /// @example m.remove("name")
            /* Handle .remove() on maps - needs mutation */
            if (strcmp(expr->as.method_call.method, "remove") == 0 &&
                expr->as.method_call.arg_count == 1) {
                char *lv_err = NULL;
                LatValue *map_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (map_lv && map_lv->type == VAL_REF) map_lv = &map_lv->as.ref.ref->value;
                if (map_lv && map_lv->type == VAL_MAP) {
                    if (map_lv->phase == VTAG_SUBLIMATED)
                        return eval_err(strdup("cannot remove from a sublimated map"));
                    {
                        const char *vn = get_method_obj_varname(expr->as.method_call.object);
                        if (vn) {
                            const char *pmode = find_pressure(ev, vn);
                            if (pressure_blocks_shrink(pmode)) {
                                char *err = NULL;
                                (void)asprintf(&err, "pressurized (%s): cannot remove from '%s'", pmode, vn);
                                return eval_err(err);
                            }
                        }
                    }
                    EvalResult kr = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(kr)) return kr;
                    if (kr.value.type != VAL_STR) {
                        value_free(&kr.value);
                        return eval_err(strdup(".remove() key must be a string"));
                    }
                    /* Free old value if key exists */
                    LatValue *old = (LatValue *)lat_map_get(map_lv->as.map.map, kr.value.as.str_val);
                    if (old) value_free(old);
                    lat_map_remove(map_lv->as.map.map, kr.value.as.str_val);
                    value_free(&kr.value);
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }

            /// @method Set.add(value: Any) -> Unit
            /// @category Set Methods
            /// Add an element to the set (mutates in place).
            /// @example s.add(42)
            if (strcmp(expr->as.method_call.method, "add") == 0 &&
                expr->as.method_call.arg_count == 1) {
                char *lv_err = NULL;
                LatValue *set_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (set_lv && set_lv->type == VAL_SET) {
                    EvalResult vr = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(vr)) return vr;
                    char *key = value_display(&vr.value);
                    /* If key already exists, free old value */
                    LatValue *old = (LatValue *)lat_map_get(set_lv->as.set.map, key);
                    if (old) value_free(old);
                    lat_map_set(set_lv->as.set.map, key, &vr.value);
                    free(key);
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }

            /// @method Set.remove(value: Any) -> Unit
            /// @category Set Methods
            /// Remove an element from the set (mutates in place).
            /// @example s.remove(42)
            if (strcmp(expr->as.method_call.method, "remove") == 0 &&
                expr->as.method_call.arg_count == 1) {
                char *lv_err = NULL;
                LatValue *set_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (set_lv && set_lv->type == VAL_SET) {
                    EvalResult vr = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(vr)) return vr;
                    char *key = value_display(&vr.value);
                    LatValue *old = (LatValue *)lat_map_get(set_lv->as.set.map, key);
                    if (old) value_free(old);
                    lat_map_remove(set_lv->as.set.map, key);
                    free(key);
                    value_free(&vr.value);
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }

            /* ── Buffer mutating methods ── */
            buffer_mutating_methods:
            if (strcmp(expr->as.method_call.method, "push") == 0 &&
                expr->as.method_call.arg_count == 1) {
                char *lv_err = NULL;
                LatValue *buf_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (buf_lv && buf_lv->type == VAL_REF) buf_lv = &buf_lv->as.ref.ref->value;
                if (buf_lv && buf_lv->type == VAL_ARRAY) {
                    /* Ref-wrapped array push via resolve_lvalue */
                    if (value_is_crystal(buf_lv))
                        return eval_err(strdup("cannot push to a crystal array"));
                    EvalResult ar = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(ar)) return ar;
                    if (buf_lv->as.array.len >= buf_lv->as.array.cap) {
                        size_t old_cap = buf_lv->as.array.cap;
                        buf_lv->as.array.cap = old_cap < 4 ? 4 : old_cap * 2;
                        buf_lv->as.array.elems = realloc(buf_lv->as.array.elems, buf_lv->as.array.cap * sizeof(LatValue));
                    }
                    buf_lv->as.array.elems[buf_lv->as.array.len++] = ar.value;
                    return eval_ok(value_unit());
                }
                if (buf_lv && buf_lv->type == VAL_BUFFER) {
                    EvalResult vr = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(vr)) return vr;
                    uint8_t byte = (vr.value.type == VAL_INT) ? (uint8_t)(vr.value.as.int_val & 0xFF) : 0;
                    value_free(&vr.value);
                    if (buf_lv->as.buffer.len >= buf_lv->as.buffer.cap) {
                        buf_lv->as.buffer.cap = buf_lv->as.buffer.cap ? buf_lv->as.buffer.cap * 2 : 8;
                        buf_lv->as.buffer.data = realloc(buf_lv->as.buffer.data, buf_lv->as.buffer.cap);
                    }
                    buf_lv->as.buffer.data[buf_lv->as.buffer.len++] = byte;
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }
            if (strcmp(expr->as.method_call.method, "push_u16") == 0 &&
                expr->as.method_call.arg_count == 1) {
                char *lv_err = NULL;
                LatValue *buf_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (buf_lv && buf_lv->type == VAL_BUFFER) {
                    EvalResult vr = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(vr)) return vr;
                    uint16_t v = (vr.value.type == VAL_INT) ? (uint16_t)(vr.value.as.int_val & 0xFFFF) : 0;
                    value_free(&vr.value);
                    while (buf_lv->as.buffer.len + 2 > buf_lv->as.buffer.cap) {
                        buf_lv->as.buffer.cap = buf_lv->as.buffer.cap ? buf_lv->as.buffer.cap * 2 : 8;
                        buf_lv->as.buffer.data = realloc(buf_lv->as.buffer.data, buf_lv->as.buffer.cap);
                    }
                    buf_lv->as.buffer.data[buf_lv->as.buffer.len++] = (uint8_t)(v & 0xFF);
                    buf_lv->as.buffer.data[buf_lv->as.buffer.len++] = (uint8_t)((v >> 8) & 0xFF);
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }
            if (strcmp(expr->as.method_call.method, "push_u32") == 0 &&
                expr->as.method_call.arg_count == 1) {
                char *lv_err = NULL;
                LatValue *buf_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (buf_lv && buf_lv->type == VAL_BUFFER) {
                    EvalResult vr = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(vr)) return vr;
                    uint32_t v = (vr.value.type == VAL_INT) ? (uint32_t)(vr.value.as.int_val & 0xFFFFFFFF) : 0;
                    value_free(&vr.value);
                    while (buf_lv->as.buffer.len + 4 > buf_lv->as.buffer.cap) {
                        buf_lv->as.buffer.cap = buf_lv->as.buffer.cap ? buf_lv->as.buffer.cap * 2 : 8;
                        buf_lv->as.buffer.data = realloc(buf_lv->as.buffer.data, buf_lv->as.buffer.cap);
                    }
                    buf_lv->as.buffer.data[buf_lv->as.buffer.len++] = (uint8_t)(v & 0xFF);
                    buf_lv->as.buffer.data[buf_lv->as.buffer.len++] = (uint8_t)((v >> 8) & 0xFF);
                    buf_lv->as.buffer.data[buf_lv->as.buffer.len++] = (uint8_t)((v >> 16) & 0xFF);
                    buf_lv->as.buffer.data[buf_lv->as.buffer.len++] = (uint8_t)((v >> 24) & 0xFF);
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }
            if (strcmp(expr->as.method_call.method, "write_u8") == 0 &&
                expr->as.method_call.arg_count == 2) {
                char *lv_err = NULL;
                LatValue *buf_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (buf_lv && buf_lv->type == VAL_BUFFER) {
                    EvalResult ir = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(ir)) return ir;
                    EvalResult vr = eval_expr(ev, expr->as.method_call.args[1]);
                    if (!IS_OK(vr)) { value_free(&ir.value); return vr; }
                    if (ir.value.type != VAL_INT || ir.value.as.int_val < 0 || (size_t)ir.value.as.int_val >= buf_lv->as.buffer.len) {
                        value_free(&ir.value); value_free(&vr.value);
                        return eval_err(strdup("Buffer.write_u8: index out of bounds"));
                    }
                    buf_lv->as.buffer.data[ir.value.as.int_val] = (uint8_t)(vr.value.as.int_val & 0xFF);
                    value_free(&ir.value); value_free(&vr.value);
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }
            if (strcmp(expr->as.method_call.method, "write_u16") == 0 &&
                expr->as.method_call.arg_count == 2) {
                char *lv_err = NULL;
                LatValue *buf_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (buf_lv && buf_lv->type == VAL_BUFFER) {
                    EvalResult ir = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(ir)) return ir;
                    EvalResult vr = eval_expr(ev, expr->as.method_call.args[1]);
                    if (!IS_OK(vr)) { value_free(&ir.value); return vr; }
                    size_t idx = (size_t)ir.value.as.int_val;
                    if (ir.value.type != VAL_INT || idx + 2 > buf_lv->as.buffer.len) {
                        value_free(&ir.value); value_free(&vr.value);
                        return eval_err(strdup("Buffer.write_u16: index out of bounds"));
                    }
                    uint16_t v = (uint16_t)(vr.value.as.int_val & 0xFFFF);
                    buf_lv->as.buffer.data[idx] = (uint8_t)(v & 0xFF);
                    buf_lv->as.buffer.data[idx+1] = (uint8_t)((v >> 8) & 0xFF);
                    value_free(&ir.value); value_free(&vr.value);
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }
            if (strcmp(expr->as.method_call.method, "write_u32") == 0 &&
                expr->as.method_call.arg_count == 2) {
                char *lv_err = NULL;
                LatValue *buf_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (buf_lv && buf_lv->type == VAL_BUFFER) {
                    EvalResult ir = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(ir)) return ir;
                    EvalResult vr = eval_expr(ev, expr->as.method_call.args[1]);
                    if (!IS_OK(vr)) { value_free(&ir.value); return vr; }
                    size_t idx = (size_t)ir.value.as.int_val;
                    if (ir.value.type != VAL_INT || idx + 4 > buf_lv->as.buffer.len) {
                        value_free(&ir.value); value_free(&vr.value);
                        return eval_err(strdup("Buffer.write_u32: index out of bounds"));
                    }
                    uint32_t v = (uint32_t)(vr.value.as.int_val & 0xFFFFFFFF);
                    buf_lv->as.buffer.data[idx]   = (uint8_t)(v & 0xFF);
                    buf_lv->as.buffer.data[idx+1] = (uint8_t)((v >> 8) & 0xFF);
                    buf_lv->as.buffer.data[idx+2] = (uint8_t)((v >> 16) & 0xFF);
                    buf_lv->as.buffer.data[idx+3] = (uint8_t)((v >> 24) & 0xFF);
                    value_free(&ir.value); value_free(&vr.value);
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }
            if (strcmp(expr->as.method_call.method, "clear") == 0 &&
                expr->as.method_call.arg_count == 0) {
                char *lv_err = NULL;
                LatValue *buf_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (buf_lv && buf_lv->type == VAL_BUFFER) {
                    buf_lv->as.buffer.len = 0;
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }
            if (strcmp(expr->as.method_call.method, "fill") == 0 &&
                expr->as.method_call.arg_count == 1) {
                char *lv_err = NULL;
                LatValue *buf_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (buf_lv && buf_lv->type == VAL_BUFFER) {
                    EvalResult vr = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(vr)) return vr;
                    uint8_t byte = (vr.value.type == VAL_INT) ? (uint8_t)(vr.value.as.int_val & 0xFF) : 0;
                    value_free(&vr.value);
                    memset(buf_lv->as.buffer.data, byte, buf_lv->as.buffer.len);
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }
            if (strcmp(expr->as.method_call.method, "resize") == 0 &&
                expr->as.method_call.arg_count == 1) {
                char *lv_err = NULL;
                LatValue *buf_lv = resolve_lvalue(ev, expr->as.method_call.object, &lv_err);
                if (buf_lv && buf_lv->type == VAL_BUFFER) {
                    EvalResult vr = eval_expr(ev, expr->as.method_call.args[0]);
                    if (!IS_OK(vr)) return vr;
                    if (vr.value.type != VAL_INT || vr.value.as.int_val < 0) {
                        value_free(&vr.value);
                        return eval_ok(value_unit());
                    }
                    size_t new_len = (size_t)vr.value.as.int_val;
                    value_free(&vr.value);
                    if (new_len > buf_lv->as.buffer.cap) {
                        buf_lv->as.buffer.cap = new_len;
                        buf_lv->as.buffer.data = realloc(buf_lv->as.buffer.data, buf_lv->as.buffer.cap);
                    }
                    if (new_len > buf_lv->as.buffer.len)
                        memset(buf_lv->as.buffer.data + buf_lv->as.buffer.len, 0, new_len - buf_lv->as.buffer.len);
                    buf_lv->as.buffer.len = new_len;
                    return eval_ok(value_unit());
                }
                if (lv_err) free(lv_err);
            }

            EvalResult objr = eval_expr(ev, expr->as.method_call.object);
            if (!IS_OK(objr)) return objr;
            /* Optional chaining: if receiver is nil, return nil */
            if (expr->as.method_call.optional && objr.value.type == VAL_NIL) {
                value_free(&objr.value);
                return eval_ok(value_nil());
            }
            GC_PUSH(ev, &objr.value);
            size_t argc = expr->as.method_call.arg_count;
            LatValue *args = malloc((argc < 1 ? 1 : argc) * sizeof(LatValue));
            for (size_t i = 0; i < argc; i++) {
                EvalResult ar = eval_expr(ev, expr->as.method_call.args[i]);
                if (!IS_OK(ar)) {
                    GC_POP_N(ev, i);  /* args */
                    GC_POP(ev);       /* objr */
                    for (size_t j = 0; j < i; j++) value_free(&args[j]);
                    free(args);
                    value_free(&objr.value);
                    return ar;
                }
                args[i] = ar.value;
                GC_PUSH(ev, &args[i]);
            }
            EvalResult res = eval_method_call(ev, objr.value, expr->as.method_call.method, args, argc);
            GC_POP_N(ev, argc);  /* args */
            GC_POP(ev);          /* objr */
            value_free(&objr.value);
            for (size_t i = 0; i < argc; i++) value_free(&args[i]);
            free(args);
            return res;
        }

        case EXPR_FIELD_ACCESS: {
            EvalResult objr = eval_expr(ev, expr->as.field_access.object);
            if (!IS_OK(objr)) return objr;
            /* Optional chaining: if receiver is nil, return nil */
            if (expr->as.field_access.optional && objr.value.type == VAL_NIL) {
                value_free(&objr.value);
                return eval_ok(value_nil());
            }
            /* Tuple numeric field access: tuple.0, tuple.1, etc. */
            if (objr.value.type == VAL_TUPLE) {
                const char *field = expr->as.field_access.field;
                char *endptr;
                long idx = strtol(field, &endptr, 10);
                if (*endptr != '\0' || idx < 0) {
                    char *err = NULL;
                    (void)asprintf(&err, "tuple field must be a non-negative integer, got '%s'", field);
                    value_free(&objr.value);
                    return eval_err(err);
                }
                if ((size_t)idx >= objr.value.as.tuple.len) {
                    char *err = NULL;
                    (void)asprintf(&err, "tuple index %ld out of bounds (len=%zu)",
                                   idx, objr.value.as.tuple.len);
                    value_free(&objr.value);
                    return eval_err(err);
                }
                LatValue result = value_deep_clone(&objr.value.as.tuple.elems[idx]);
                value_free(&objr.value);
                return eval_ok(result);
            }
            if (objr.value.type == VAL_MAP) {
                const char *field = expr->as.field_access.field;
                LatValue *val = (LatValue *)lat_map_get(objr.value.as.map.map, field);
                if (val) {
                    LatValue result = value_deep_clone(val);
                    value_free(&objr.value);
                    return eval_ok(result);
                }
                char *err = NULL;
                (void)asprintf(&err, "map has no key '%s'", field);
                value_free(&objr.value);
                return eval_err(err);
            }
            if (objr.value.type != VAL_STRUCT) {
                char *err = NULL;
                (void)asprintf(&err, "cannot access field '%s' on %s",
                               expr->as.field_access.field, value_type_name(&objr.value));
                value_free(&objr.value);
                return eval_err(err);
            }
            for (size_t i = 0; i < objr.value.as.strct.field_count; i++) {
                if (objr.value.as.strct.field_names[i] == intern(expr->as.field_access.field)) {
                    LatValue result = value_deep_clone(&objr.value.as.strct.field_values[i]);
                    value_free(&objr.value);
                    return eval_ok(result);
                }
            }
            char *err = NULL;
            (void)asprintf(&err, "struct has no field '%s'", expr->as.field_access.field);
            value_free(&objr.value);
            return eval_err(err);
        }

        case EXPR_INDEX: {
            EvalResult objr = eval_expr(ev, expr->as.index.object);
            if (!IS_OK(objr)) return objr;
            /* Optional chaining: if receiver is nil, return nil */
            if (expr->as.index.optional && objr.value.type == VAL_NIL) {
                value_free(&objr.value);
                return eval_ok(value_nil());
            }
            GC_PUSH(ev, &objr.value);
            EvalResult idxr = eval_expr(ev, expr->as.index.index);
            GC_POP(ev);
            if (!IS_OK(idxr)) { value_free(&objr.value); return idxr; }
            if (objr.value.type == VAL_ARRAY && idxr.value.type == VAL_INT) {
                size_t idx = (size_t)idxr.value.as.int_val;
                value_free(&idxr.value);
                if (idx >= objr.value.as.array.len) {
                    char *err = NULL;
                    (void)asprintf(&err, "index %zu out of bounds (length %zu)",
                                   idx, objr.value.as.array.len);
                    value_free(&objr.value);
                    return eval_err(err);
                }
                LatValue result = value_deep_clone(&objr.value.as.array.elems[idx]);
                value_free(&objr.value);
                return eval_ok(result);
            }
            if (objr.value.type == VAL_STR && idxr.value.type == VAL_INT) {
                size_t idx = (size_t)idxr.value.as.int_val;
                value_free(&idxr.value);
                size_t slen = strlen(objr.value.as.str_val);
                if (idx >= slen) {
                    char *err = NULL;
                    (void)asprintf(&err, "string index %zu out of bounds (length %zu)", idx, slen);
                    value_free(&objr.value);
                    return eval_err(err);
                }
                char buf[2] = { objr.value.as.str_val[idx], '\0' };
                value_free(&objr.value);
                return eval_ok(value_string(buf));
            }
            /* String slicing with range */
            if (objr.value.type == VAL_STR && idxr.value.type == VAL_RANGE) {
                char *sliced = lat_str_substring(objr.value.as.str_val,
                                                  idxr.value.as.range.start,
                                                  idxr.value.as.range.end);
                value_free(&objr.value);
                value_free(&idxr.value);
                return eval_ok(value_string_owned(sliced));
            }
            /* Map indexing: map["key"] */
            if (objr.value.type == VAL_MAP && idxr.value.type == VAL_STR) {
                LatValue *found = (LatValue *)lat_map_get(objr.value.as.map.map, idxr.value.as.str_val);
                LatValue result = found ? value_deep_clone(found) : value_unit();
                value_free(&objr.value);
                value_free(&idxr.value);
                return eval_ok(result);
            }
            /* Buffer indexing: buf[i] -> Int (0-255) */
            if (objr.value.type == VAL_BUFFER && idxr.value.type == VAL_INT) {
                size_t bidx = (size_t)idxr.value.as.int_val;
                value_free(&idxr.value);
                if (bidx >= objr.value.as.buffer.len) {
                    char *berr = NULL;
                    (void)asprintf(&berr, "buffer index %zu out of bounds (length %zu)",
                                   bidx, objr.value.as.buffer.len);
                    value_free(&objr.value);
                    return eval_err(berr);
                }
                LatValue result = value_int(objr.value.as.buffer.data[bidx]);
                value_free(&objr.value);
                return eval_ok(result);
            }
            /* Ref proxy: delegate indexing to inner value */
            if (objr.value.type == VAL_REF) {
                LatValue *inner = &objr.value.as.ref.ref->value;
                if (inner->type == VAL_MAP && idxr.value.type == VAL_STR) {
                    LatValue *found = (LatValue *)lat_map_get(inner->as.map.map, idxr.value.as.str_val);
                    LatValue result = found ? value_deep_clone(found) : value_unit();
                    value_free(&objr.value);
                    value_free(&idxr.value);
                    return eval_ok(result);
                }
                if (inner->type == VAL_ARRAY && idxr.value.type == VAL_INT) {
                    size_t idx = (size_t)idxr.value.as.int_val;
                    value_free(&idxr.value);
                    if (idx >= inner->as.array.len) {
                        char *berr = NULL;
                        (void)asprintf(&berr, "index %zu out of bounds (length %zu)",
                                       idx, inner->as.array.len);
                        value_free(&objr.value);
                        return eval_err(berr);
                    }
                    LatValue result = value_deep_clone(&inner->as.array.elems[idx]);
                    value_free(&objr.value);
                    return eval_ok(result);
                }
            }
            char *err = NULL;
            (void)asprintf(&err, "cannot index %s with %s",
                           value_type_name(&objr.value), value_type_name(&idxr.value));
            value_free(&objr.value);
            value_free(&idxr.value);
            return eval_err(err);
        }

        case EXPR_ARRAY: {
            size_t n = expr->as.array.count;
            size_t cap = n > 0 ? n : 4;
            size_t out = 0;
            LatValue *elems = malloc(cap * sizeof(LatValue));
            size_t gc_count = 0;
            for (size_t i = 0; i < n; i++) {
                if (expr->as.array.elems[i]->tag == EXPR_SPREAD) {
                    EvalResult er = eval_expr(ev, expr->as.array.elems[i]->as.spread_expr);
                    if (!IS_OK(er)) {
                        GC_POP_N(ev, gc_count);
                        for (size_t j = 0; j < out; j++) value_free(&elems[j]);
                        free(elems);
                        return er;
                    }
                    if (er.value.type != VAL_ARRAY) {
                        char *msg = NULL;
                        (void)asprintf(&msg, "cannot spread non-array value of type %s",
                                       value_type_name(&er.value));
                        GC_POP_N(ev, gc_count);
                        for (size_t j = 0; j < out; j++) value_free(&elems[j]);
                        free(elems);
                        value_free(&er.value);
                        return eval_err(msg);
                    }
                    size_t slen = er.value.as.array.len;
                    while (out + slen > cap) { cap *= 2; elems = realloc(elems, cap * sizeof(LatValue)); }
                    for (size_t j = 0; j < slen; j++) {
                        elems[out] = value_deep_clone(&er.value.as.array.elems[j]);
                        GC_PUSH(ev, &elems[out]);
                        gc_count++;
                        out++;
                    }
                    value_free(&er.value);
                } else {
                    EvalResult er = eval_expr(ev, expr->as.array.elems[i]);
                    if (!IS_OK(er)) {
                        GC_POP_N(ev, gc_count);
                        for (size_t j = 0; j < out; j++) value_free(&elems[j]);
                        free(elems);
                        return er;
                    }
                    if (out >= cap) { cap *= 2; elems = realloc(elems, cap * sizeof(LatValue)); }
                    elems[out] = er.value;
                    GC_PUSH(ev, &elems[out]);
                    gc_count++;
                    out++;
                }
            }
            GC_POP_N(ev, gc_count);
            stats_array(&ev->stats);
            LatValue arr = value_array(elems, out);
            free(elems);
            return eval_ok(arr);
        }

        case EXPR_TUPLE: {
            size_t n = expr->as.tuple.count;
            LatValue *elems = malloc(n * sizeof(LatValue));
            for (size_t i = 0; i < n; i++) {
                EvalResult er = eval_expr(ev, expr->as.tuple.elems[i]);
                if (!IS_OK(er)) {
                    GC_POP_N(ev, i);
                    for (size_t j = 0; j < i; j++) value_free(&elems[j]);
                    free(elems);
                    return er;
                }
                elems[i] = er.value;
                GC_PUSH(ev, &elems[i]);
            }
            GC_POP_N(ev, n);
            LatValue tup = value_tuple(elems, n);
            free(elems);
            return eval_ok(tup);
        }

        case EXPR_STRUCT_LIT: {
            const char *sname = expr->as.struct_lit.name;
            size_t fc = expr->as.struct_lit.field_count;
            /* Validate fields if struct def is registered */
            StructDecl *sd = find_struct(ev, sname);
            if (sd) {
                for (size_t i = 0; i < fc; i++) {
                    bool found = false;
                    for (size_t j = 0; j < sd->field_count; j++) {
                        if (strcmp(expr->as.struct_lit.fields[i].name, sd->fields[j].name) == 0) {
                            found = true; break;
                        }
                    }
                    if (!found) {
                        char *err = NULL;
                        (void)asprintf(&err, "struct '%s' has no field '%s'",
                                       sname, expr->as.struct_lit.fields[i].name);
                        return eval_err(err);
                    }
                }
            }
            char **names = malloc(fc * sizeof(char *));
            LatValue *vals = malloc(fc * sizeof(LatValue));
            for (size_t i = 0; i < fc; i++) {
                names[i] = expr->as.struct_lit.fields[i].name;
                EvalResult er = eval_expr(ev, expr->as.struct_lit.fields[i].value);
                if (!IS_OK(er)) {
                    GC_POP_N(ev, i);
                    for (size_t j = 0; j < i; j++) value_free(&vals[j]);
                    free(names); free(vals);
                    return er;
                }
                vals[i] = er.value;
                GC_PUSH(ev, &vals[i]);
            }
            GC_POP_N(ev, fc);
            stats_struct(&ev->stats);
            LatValue st = value_struct(sname, names, vals, fc);
            free(names); free(vals);
            /* Alloy enforcement: apply per-field phase from struct declaration */
            if (sd) {
                bool has_phase_decl = false;
                for (size_t i = 0; i < sd->field_count; i++) {
                    if (sd->fields[i].ty.phase != PHASE_UNSPECIFIED) { has_phase_decl = true; break; }
                }
                if (has_phase_decl) {
                    st.as.strct.field_phases = calloc(st.as.strct.field_count, sizeof(PhaseTag));
                    for (size_t i = 0; i < st.as.strct.field_count; i++) {
                        /* Find matching decl field */
                        for (size_t j = 0; j < sd->field_count; j++) {
                            if (st.as.strct.field_names[i] == intern(sd->fields[j].name)) {
                                if (sd->fields[j].ty.phase == PHASE_CRYSTAL) {
                                    st.as.strct.field_values[i] = value_freeze(st.as.strct.field_values[i]);
                                    st.as.strct.field_phases[i] = VTAG_CRYSTAL;
                                } else if (sd->fields[j].ty.phase == PHASE_FLUID) {
                                    st.as.strct.field_phases[i] = VTAG_FLUID;
                                } else {
                                    st.as.strct.field_phases[i] = st.phase;
                                }
                                break;
                            }
                        }
                    }
                }
            }
            return eval_ok(st);
        }

        /// @builtin freeze(val: Any) -> Any
        /// @category Phase Transitions
        /// Transition a value to the crystal (immutable) phase.
        /// @example freeze([1, 2, 3])  // crystal [1, 2, 3]
        case EXPR_FREEZE: {
            stats_freeze(&ev->stats);

            /* Partial crystallization: freeze(s.field) or freeze(m["key"]) */
            if (expr->as.freeze.expr->tag == EXPR_FIELD_ACCESS) {
                char *lv_err = NULL;
                /* Resolve parent struct */
                LatValue *parent = resolve_lvalue(ev, expr->as.freeze.expr->as.field_access.object, &lv_err);
                if (!parent) return eval_err(lv_err);
                if (parent->type != VAL_STRUCT) {
                    return eval_err(strdup("partial freeze requires a struct"));
                }
                if (parent->phase == VTAG_CRYSTAL) {
                    return eval_ok(value_deep_clone(parent));  /* already fully frozen */
                }
                const char *fname = expr->as.freeze.expr->as.field_access.field;
                size_t fi = (size_t)-1;
                for (size_t i = 0; i < parent->as.strct.field_count; i++) {
                    if (parent->as.strct.field_names[i] == intern(fname)) { fi = i; break; }
                }
                if (fi == (size_t)-1) {
                    char *err = NULL;
                    (void)asprintf(&err, "struct has no field '%s'", fname);
                    return eval_err(err);
                }
                /* Run contract if present */
                if (expr->as.freeze.contract) {
                    EvalResult cr = eval_expr(ev, expr->as.freeze.contract);
                    if (!IS_OK(cr)) return cr;
                    LatValue check_val = value_deep_clone(&parent->as.strct.field_values[fi]);
                    EvalResult vr = call_closure(ev, cr.value.as.closure.param_names,
                        cr.value.as.closure.param_count, cr.value.as.closure.body,
                        cr.value.as.closure.captured_env, &check_val, 1,
                        cr.value.as.closure.default_values, cr.value.as.closure.has_variadic);
                    value_free(&cr.value);
                    if (!IS_OK(vr)) {
                        char *msg = NULL;
                        (void)asprintf(&msg, "freeze contract failed: %s", vr.error);
                        free(vr.error);
                        return eval_err(msg);
                    }
                    value_free(&vr.value);
                }
                /* Freeze the field value */
                parent->as.strct.field_values[fi] = value_freeze(parent->as.strct.field_values[fi]);
                /* Lazy-allocate field_phases */
                if (!parent->as.strct.field_phases) {
                    parent->as.strct.field_phases = calloc(parent->as.strct.field_count, sizeof(PhaseTag));
                }
                parent->as.strct.field_phases[fi] = VTAG_CRYSTAL;
                return eval_ok(value_deep_clone(&parent->as.strct.field_values[fi]));
            }
            if (expr->as.freeze.expr->tag == EXPR_INDEX) {
                const Expr *idx_expr = expr->as.freeze.expr;
                /* Evaluate key first */
                EvalResult kr = eval_expr(ev, idx_expr->as.index.index);
                if (!IS_OK(kr)) return kr;
                if (kr.value.type != VAL_STR) {
                    value_free(&kr.value);
                    return eval_err(strdup("partial freeze: map key must be a string"));
                }
                char *key = strdup(kr.value.as.str_val);
                value_free(&kr.value);
                /* Resolve parent map */
                char *lv_err = NULL;
                LatValue *parent = resolve_lvalue(ev, idx_expr->as.index.object, &lv_err);
                if (!parent) { free(key); return eval_err(lv_err); }
                if (parent->type != VAL_MAP) {
                    free(key);
                    return eval_err(strdup("partial freeze requires a map"));
                }
                if (parent->phase == VTAG_CRYSTAL) {
                    free(key);
                    return eval_ok(value_deep_clone(parent));
                }
                LatValue *val_ptr = (LatValue *)lat_map_get(parent->as.map.map, key);
                if (!val_ptr) {
                    char *err = NULL;
                    (void)asprintf(&err, "map has no key '%s'", key);
                    free(key);
                    return eval_err(err);
                }
                /* Run contract if present */
                if (expr->as.freeze.contract) {
                    EvalResult cr = eval_expr(ev, expr->as.freeze.contract);
                    if (!IS_OK(cr)) { free(key); return cr; }
                    LatValue check_val = value_deep_clone(val_ptr);
                    EvalResult vr = call_closure(ev, cr.value.as.closure.param_names,
                        cr.value.as.closure.param_count, cr.value.as.closure.body,
                        cr.value.as.closure.captured_env, &check_val, 1,
                        cr.value.as.closure.default_values, cr.value.as.closure.has_variadic);
                    value_free(&cr.value);
                    if (!IS_OK(vr)) {
                        char *msg = NULL;
                        (void)asprintf(&msg, "freeze contract failed: %s", vr.error);
                        free(vr.error);
                        free(key);
                        return eval_err(msg);
                    }
                    value_free(&vr.value);
                }
                /* Freeze the value at this key */
                *val_ptr = value_freeze(*val_ptr);
                /* Lazy-allocate key_phases */
                if (!parent->as.map.key_phases) {
                    parent->as.map.key_phases = calloc(1, sizeof(LatMap));
                    *parent->as.map.key_phases = lat_map_new(sizeof(PhaseTag));
                }
                PhaseTag crystal = VTAG_CRYSTAL;
                lat_map_set(parent->as.map.key_phases, key, &crystal);
                LatValue ret = value_deep_clone(val_ptr);
                free(key);
                return eval_ok(ret);
            }

            if (expr->as.freeze.expr->tag == EXPR_IDENT) {
                const char *name = expr->as.freeze.expr->as.str_val;
                if (ev->mode == MODE_STRICT) {
                    /* Strict mode: consuming freeze removes the binding */
                    LatValue val;
                    if (!env_remove(ev->env, name, &val)) {
                        char *err = NULL;
                        (void)asprintf(&err, "undefined variable '%s'", name);
                        return eval_err(err);
                    }
                    if (val.type == VAL_CHANNEL) {
                        value_free(&val);
                        return eval_err(strdup("cannot freeze a Channel"));
                    }
                    /* Run crystallization contract if present */
                    if (expr->as.freeze.contract) {
                        EvalResult cr = eval_expr(ev, expr->as.freeze.contract);
                        if (!IS_OK(cr)) { value_free(&val); return cr; }
                        LatValue check_val = value_deep_clone(&val);
                        EvalResult vr = call_closure(ev, cr.value.as.closure.param_names,
                            cr.value.as.closure.param_count, cr.value.as.closure.body,
                            cr.value.as.closure.captured_env, &check_val, 1,
                            cr.value.as.closure.default_values, cr.value.as.closure.has_variadic);
                        value_free(&cr.value);
                        if (!IS_OK(vr)) {
                            char *msg = NULL;
                            (void)asprintf(&msg, "freeze contract failed: %s", vr.error);
                            free(vr.error);
                            value_free(&val);
                            return eval_err(msg);
                        }
                        value_free(&vr.value);
                    }
                    uint64_t ft0 = now_ns();
                    val = value_freeze(val);
                    freeze_to_region(ev, &val);
                    ev->stats.freeze_total_ns += now_ns() - ft0;
                    record_history(ev, name);
                    {
                        char *cascade_err = freeze_cascade(ev, name);
                        if (cascade_err) { value_free(&val); return eval_err(cascade_err); }
                    }
                    EvalResult fr = fire_reactions(ev, name, "crystal");
                    if (!IS_OK(fr)) { value_free(&val); return fr; }
                    return eval_ok(val);
                }
                /* Casual mode: freeze the binding in-place */
                LatValue val;
                if (!env_get(ev->env, name, &val)) {
                    char *err = NULL;
                    (void)asprintf(&err, "undefined variable '%s'", name);
                    return eval_err(err);
                }
                if (val.type == VAL_CHANNEL) {
                    value_free(&val);
                    return eval_err(strdup("cannot freeze a Channel"));
                }
                /* Freeze-except: selectively freeze struct fields/map keys */
                if (expr->as.freeze.except_count > 0) {
                    /* Evaluate except field names */
                    char **except_names = malloc(expr->as.freeze.except_count * sizeof(char *));
                    for (size_t i = 0; i < expr->as.freeze.except_count; i++) {
                        EvalResult er = eval_expr(ev, expr->as.freeze.except_fields[i]);
                        if (!IS_OK(er)) {
                            for (size_t j = 0; j < i; j++) free(except_names[j]);
                            free(except_names);
                            value_free(&val);
                            return er;
                        }
                        if (er.value.type != VAL_STR) {
                            for (size_t j = 0; j < i; j++) free(except_names[j]);
                            free(except_names);
                            value_free(&val);
                            value_free(&er.value);
                            return eval_err(strdup("freeze except: field names must be strings"));
                        }
                        except_names[i] = strdup(er.value.as.str_val);
                        value_free(&er.value);
                    }
                    if (val.type == VAL_STRUCT) {
                        /* Lazy-allocate field_phases */
                        if (!val.as.strct.field_phases) {
                            val.as.strct.field_phases = calloc(val.as.strct.field_count, sizeof(PhaseTag));
                            for (size_t i = 0; i < val.as.strct.field_count; i++)
                                val.as.strct.field_phases[i] = val.phase;
                        }
                        for (size_t i = 0; i < val.as.strct.field_count; i++) {
                            bool exempted = false;
                            for (size_t j = 0; j < expr->as.freeze.except_count; j++) {
                                if (val.as.strct.field_names[i] == intern(except_names[j])) {
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
                        /* Lazy-allocate key_phases */
                        if (!val.as.map.key_phases) {
                            val.as.map.key_phases = calloc(1, sizeof(LatMap));
                            *val.as.map.key_phases = lat_map_new(sizeof(PhaseTag));
                        }
                        for (size_t i = 0; i < val.as.map.map->cap; i++) {
                            if (val.as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                            const char *key = val.as.map.map->entries[i].key;
                            bool exempted = false;
                            for (size_t j = 0; j < expr->as.freeze.except_count; j++) {
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
                    } else {
                        for (size_t j = 0; j < expr->as.freeze.except_count; j++) free(except_names[j]);
                        free(except_names);
                        value_free(&val);
                        return eval_err(strdup("freeze except requires a struct or map"));
                    }
                    for (size_t j = 0; j < expr->as.freeze.except_count; j++) free(except_names[j]);
                    free(except_names);
                    LatValue ret = value_deep_clone(&val);
                    env_set(ev->env, name, val);
                    record_history(ev, name);
                    return eval_ok(ret);
                }
                /* Validate pending seed contracts */
                for (size_t si = 0; si < ev->seed_count; si++) {
                    if (strcmp(ev->seeds[si].var_name, name) == 0) {
                        LatValue check_val = value_deep_clone(&val);
                        EvalResult vr = call_closure(ev,
                            ev->seeds[si].contract.as.closure.param_names,
                            ev->seeds[si].contract.as.closure.param_count,
                            ev->seeds[si].contract.as.closure.body,
                            ev->seeds[si].contract.as.closure.captured_env,
                            &check_val, 1,
                            ev->seeds[si].contract.as.closure.default_values,
                            ev->seeds[si].contract.as.closure.has_variadic);
                        if (!IS_OK(vr)) {
                            char *msg = NULL;
                            (void)asprintf(&msg, "seed contract failed on freeze: %s", vr.error);
                            free(vr.error);
                            value_free(&val);
                            return eval_err(msg);
                        }
                        if (!value_is_truthy(&vr.value)) {
                            value_free(&vr.value);
                            value_free(&val);
                            return eval_err(strdup("seed contract failed on freeze: contract returned false"));
                        }
                        value_free(&vr.value);
                    }
                }
                /* Run crystallization contract if present */
                if (expr->as.freeze.contract) {
                    EvalResult cr = eval_expr(ev, expr->as.freeze.contract);
                    if (!IS_OK(cr)) { value_free(&val); return cr; }
                    LatValue check_val = value_deep_clone(&val);
                    EvalResult vr = call_closure(ev, cr.value.as.closure.param_names,
                        cr.value.as.closure.param_count, cr.value.as.closure.body,
                        cr.value.as.closure.captured_env, &check_val, 1,
                        cr.value.as.closure.default_values, cr.value.as.closure.has_variadic);
                    value_free(&cr.value);
                    if (!IS_OK(vr)) {
                        char *msg = NULL;
                        (void)asprintf(&msg, "freeze contract failed: %s", vr.error);
                        free(vr.error);
                        value_free(&val);
                        return eval_err(msg);
                    }
                    value_free(&vr.value);
                }
                uint64_t ft0 = now_ns();
                val = value_freeze(val);
                freeze_to_region(ev, &val);
                ev->stats.freeze_total_ns += now_ns() - ft0;
                LatValue ret = value_deep_clone(&val);
                env_set(ev->env, name, val);
                record_history(ev, name);
                {
                    char *cascade_err = freeze_cascade(ev, name);
                    if (cascade_err) { value_free(&ret); return eval_err(cascade_err); }
                }
                EvalResult fr = fire_reactions(ev, name, "crystal");
                if (!IS_OK(fr)) { value_free(&ret); return fr; }
                return eval_ok(ret);
            }
            EvalResult er = eval_expr(ev, expr->as.freeze.expr);
            if (!IS_OK(er)) return er;
            if (er.value.type == VAL_CHANNEL) {
                value_free(&er.value);
                return eval_err(strdup("cannot freeze a Channel"));
            }
            /* Run crystallization contract if present */
            if (expr->as.freeze.contract) {
                EvalResult cr = eval_expr(ev, expr->as.freeze.contract);
                if (!IS_OK(cr)) { value_free(&er.value); return cr; }
                LatValue check_val = value_deep_clone(&er.value);
                EvalResult vr = call_closure(ev, cr.value.as.closure.param_names,
                    cr.value.as.closure.param_count, cr.value.as.closure.body,
                    cr.value.as.closure.captured_env, &check_val, 1,
                    cr.value.as.closure.default_values, cr.value.as.closure.has_variadic);
                value_free(&cr.value);
                if (!IS_OK(vr)) {
                    char *msg = NULL;
                    (void)asprintf(&msg, "freeze contract failed: %s", vr.error);
                    free(vr.error);
                    value_free(&er.value);
                    return eval_err(msg);
                }
                value_free(&vr.value);
            }
            { uint64_t ft0 = now_ns();
            er.value = value_freeze(er.value);
            freeze_to_region(ev, &er.value);
            ev->stats.freeze_total_ns += now_ns() - ft0; }
            return eval_ok(er.value);
        }

        /// @builtin thaw(val: Any) -> Any
        /// @category Phase Transitions
        /// Transition a crystal value back to the flux (mutable) phase.
        /// @example thaw(freeze([1, 2]))  // flux [1, 2]
        case EXPR_THAW: {
            stats_thaw(&ev->stats);
            if (expr->as.freeze_expr->tag == EXPR_IDENT) {
                const char *name = expr->as.freeze_expr->as.str_val;
                /* Thaw the binding in-place */
                LatValue val;
                if (!env_get(ev->env, name, &val)) {
                    char *err = NULL;
                    (void)asprintf(&err, "undefined variable '%s'", name);
                    return eval_err(err);
                }
                uint64_t tt0 = now_ns();
                LatValue thawed = value_thaw(&val);
                ev->stats.thaw_total_ns += now_ns() - tt0;
                value_free(&val);
                LatValue ret = value_deep_clone(&thawed);
                env_set(ev->env, name, thawed);
                record_history(ev, name);
                EvalResult fr = fire_reactions(ev, name, "fluid");
                if (!IS_OK(fr)) { value_free(&ret); return fr; }
                return eval_ok(ret);
            }
            EvalResult er = eval_expr(ev, expr->as.freeze_expr);
            if (!IS_OK(er)) return er;
            { uint64_t tt0 = now_ns();
            LatValue thawed = value_thaw(&er.value);
            ev->stats.thaw_total_ns += now_ns() - tt0;
            value_free(&er.value);
            return eval_ok(thawed); }
        }

        /// @builtin clone(val: Any) -> Any
        /// @category Phase Transitions
        /// Create a deep copy of a value.
        /// @example clone(my_array)  // independent copy
        case EXPR_CLONE: {
            stats_deep_clone(&ev->stats);
            EvalResult er = eval_expr(ev, expr->as.freeze_expr);
            if (!IS_OK(er)) return er;
            LatValue cloned = value_deep_clone(&er.value);
            value_free(&er.value);
            return eval_ok(cloned);
        }

        /// @builtin anneal(val) |transform| { ... } -> Any
        /// @category Phase Transitions
        /// Atomically thaw a crystal value, apply a transformation, and refreeze.
        /// @example anneal(frozen_map) |m| { m["key"] = "value"; m }
        case EXPR_ANNEAL: {
            stats_thaw(&ev->stats);
            stats_freeze(&ev->stats);

            /* Evaluate the closure expression */
            EvalResult clr = eval_expr(ev, expr->as.anneal.closure);
            if (!IS_OK(clr)) return clr;

            /* Special handling for identifier targets (in-place update) */
            if (expr->as.anneal.expr->tag == EXPR_IDENT) {
                const char *name = expr->as.anneal.expr->as.str_val;
                LatValue val;
                if (!env_get(ev->env, name, &val)) {
                    value_free(&clr.value);
                    char *err = NULL;
                    (void)asprintf(&err, "undefined variable '%s'", name);
                    return eval_err(err);
                }
                if (val.phase != VTAG_CRYSTAL) {
                    value_free(&val);
                    value_free(&clr.value);
                    return eval_err(strdup("anneal requires a crystal value"));
                }
                /* Thaw */
                uint64_t tt0 = now_ns();
                LatValue thawed = value_thaw(&val);
                ev->stats.thaw_total_ns += now_ns() - tt0;
                value_free(&val);

                /* Call the transformation closure */
                EvalResult tr = call_closure(ev,
                    clr.value.as.closure.param_names,
                    clr.value.as.closure.param_count,
                    clr.value.as.closure.body,
                    clr.value.as.closure.captured_env,
                    &thawed, 1,
                    clr.value.as.closure.default_values,
                    clr.value.as.closure.has_variadic);
                value_free(&clr.value);

                if (!IS_OK(tr)) {
                    char *msg = NULL;
                    (void)asprintf(&msg, "anneal failed: %s", tr.error);
                    free(tr.error);
                    return eval_err(msg);
                }

                /* Refreeze */
                uint64_t ft0 = now_ns();
                tr.value = value_freeze(tr.value);
                freeze_to_region(ev, &tr.value);
                ev->stats.freeze_total_ns += now_ns() - ft0;

                /* Update binding in-place */
                LatValue ret = value_deep_clone(&tr.value);
                env_set(ev->env, name, tr.value);
                record_history(ev, name);
                {
                    char *cascade_err = freeze_cascade(ev, name);
                    if (cascade_err) { value_free(&ret); return eval_err(cascade_err); }
                }
                EvalResult fr = fire_reactions(ev, name, "crystal");
                if (!IS_OK(fr)) { value_free(&ret); return fr; }
                return eval_ok(ret);
            }

            /* General expression path */
            EvalResult er = eval_expr(ev, expr->as.anneal.expr);
            if (!IS_OK(er)) { value_free(&clr.value); return er; }

            if (er.value.phase != VTAG_CRYSTAL) {
                value_free(&er.value);
                value_free(&clr.value);
                return eval_err(strdup("anneal requires a crystal value"));
            }

            /* Thaw */
            uint64_t tt0 = now_ns();
            LatValue thawed = value_thaw(&er.value);
            ev->stats.thaw_total_ns += now_ns() - tt0;
            value_free(&er.value);

            /* Call transformation */
            EvalResult tr = call_closure(ev,
                clr.value.as.closure.param_names,
                clr.value.as.closure.param_count,
                clr.value.as.closure.body,
                clr.value.as.closure.captured_env,
                &thawed, 1,
                clr.value.as.closure.default_values,
                clr.value.as.closure.has_variadic);
            value_free(&clr.value);

            if (!IS_OK(tr)) {
                char *msg = NULL;
                (void)asprintf(&msg, "anneal failed: %s", tr.error);
                free(tr.error);
                return eval_err(msg);
            }

            /* Refreeze */
            uint64_t ft0 = now_ns();
            tr.value = value_freeze(tr.value);
            freeze_to_region(ev, &tr.value);
            ev->stats.freeze_total_ns += now_ns() - ft0;
            return eval_ok(tr.value);
        }

        case EXPR_CRYSTALLIZE: {
            /* crystallize(expr) { body } — temporarily freeze, execute body, restore */
            if (expr->as.crystallize.expr->tag != EXPR_IDENT) {
                return eval_err(strdup("crystallize() target must be a variable name"));
            }
            const char *name = expr->as.crystallize.expr->as.str_val;
            LatValue val;
            if (!env_get(ev->env, name, &val)) {
                char *err = NULL;
                (void)asprintf(&err, "crystallize(): undefined variable '%s'", name);
                return eval_err(err);
            }
            PhaseTag saved_phase = val.phase;
            /* If already crystal, just run the body */
            if (saved_phase != VTAG_CRYSTAL) {
                val = value_freeze(val);
                env_set(ev->env, name, val);
            } else {
                value_free(&val);
            }
            /* Execute body */
            stats_scope_push(&ev->stats);
            env_push_scope(ev->env);
            EvalResult result = eval_block_stmts(ev, expr->as.crystallize.body, expr->as.crystallize.body_count);
            env_pop_scope(ev->env);
            stats_scope_pop(&ev->stats);
            /* Restore original phase */
            if (saved_phase != VTAG_CRYSTAL) {
                LatValue cur;
                if (env_get(ev->env, name, &cur)) {
                    LatValue thawed = value_thaw(&cur);
                    value_free(&cur);
                    thawed.phase = saved_phase;
                    env_set(ev->env, name, thawed);
                }
            }
            if (!IS_OK(result)) return result;
            return eval_ok(result.value);
        }

        case EXPR_BORROW: {
            /* borrow(expr) { body } — temporarily thaw, execute body, restore phase */
            if (expr->as.borrow.expr->tag != EXPR_IDENT) {
                return eval_err(strdup("borrow() target must be a variable name"));
            }
            const char *name = expr->as.borrow.expr->as.str_val;
            LatValue val;
            if (!env_get(ev->env, name, &val)) {
                char *err = NULL;
                (void)asprintf(&err, "borrow(): undefined variable '%s'", name);
                return eval_err(err);
            }
            PhaseTag saved_phase = val.phase;
            /* If already fluid, just run the body */
            if (saved_phase != VTAG_FLUID) {
                LatValue thawed = value_thaw(&val);
                value_free(&val);
                env_set(ev->env, name, thawed);
            } else {
                value_free(&val);
            }
            /* Execute body */
            stats_scope_push(&ev->stats);
            env_push_scope(ev->env);
            EvalResult result = eval_block_stmts(ev, expr->as.borrow.body, expr->as.borrow.body_count);
            env_pop_scope(ev->env);
            stats_scope_pop(&ev->stats);
            /* Restore original phase */
            if (saved_phase != VTAG_FLUID) {
                LatValue cur;
                if (env_get(ev->env, name, &cur)) {
                    cur = value_freeze(cur);
                    cur.phase = saved_phase;
                    env_set(ev->env, name, cur);
                }
            }
            if (!IS_OK(result)) return result;
            return eval_ok(result.value);
        }

        case EXPR_SUBLIMATE: {
            /* sublimate(expr) — shallow freeze: top-level locked, children mutable */
            if (expr->as.freeze_expr->tag == EXPR_IDENT) {
                const char *name = expr->as.freeze_expr->as.str_val;
                LatValue val;
                if (!env_get(ev->env, name, &val)) {
                    char *err = NULL;
                    (void)asprintf(&err, "sublimate(): undefined variable '%s'", name);
                    return eval_err(err);
                }
                val.phase = VTAG_SUBLIMATED;  /* Only set top-level phase, don't recurse */
                LatValue ret = value_deep_clone(&val);
                env_set(ev->env, name, val);
                record_history(ev, name);
                EvalResult fr = fire_reactions(ev, name, "sublimated");
                if (!IS_OK(fr)) { value_free(&ret); return fr; }
                return eval_ok(ret);
            }
            EvalResult er = eval_expr(ev, expr->as.freeze_expr);
            if (!IS_OK(er)) return er;
            er.value.phase = VTAG_SUBLIMATED;
            return eval_ok(er.value);
        }

        case EXPR_FORGE: {
            stats_forge(&ev->stats);
            stats_scope_push(&ev->stats);
            env_push_scope(ev->env);
            EvalResult result = eval_block_stmts(ev, expr->as.block.stmts, expr->as.block.count);
            env_pop_scope(ev->env);
            stats_scope_pop(&ev->stats);
            stats_freeze(&ev->stats);
            if (IS_OK(result)) {
                uint64_t ft0 = now_ns();
                result.value = value_freeze(result.value);
                freeze_to_region(ev, &result.value);
                ev->stats.freeze_total_ns += now_ns() - ft0;
                return eval_ok(result.value);
            }
            if (IS_SIGNAL(result) && result.cf.tag == CF_RETURN) {
                uint64_t ft0 = now_ns();
                result.cf.value = value_freeze(result.cf.value);
                freeze_to_region(ev, &result.cf.value);
                ev->stats.freeze_total_ns += now_ns() - ft0;
                return eval_ok(result.cf.value);
            }
            return result;
        }

        case EXPR_IF: {
            EvalResult condr = eval_expr(ev, expr->as.if_expr.cond);
            if (!IS_OK(condr)) return condr;
            bool truthy = value_is_truthy(&condr.value);
            value_free(&condr.value);
            if (truthy) {
                stats_scope_push(&ev->stats);
                env_push_scope(ev->env);
                EvalResult r = eval_block_stmts(ev, expr->as.if_expr.then_stmts,
                                                 expr->as.if_expr.then_count);
                env_pop_scope(ev->env);
                stats_scope_pop(&ev->stats);
                return r;
            } else if (expr->as.if_expr.else_stmts) {
                stats_scope_push(&ev->stats);
                env_push_scope(ev->env);
                EvalResult r = eval_block_stmts(ev, expr->as.if_expr.else_stmts,
                                                 expr->as.if_expr.else_count);
                env_pop_scope(ev->env);
                stats_scope_pop(&ev->stats);
                return r;
            }
            return eval_ok(value_unit());
        }

        case EXPR_BLOCK: {
            stats_scope_push(&ev->stats);
            env_push_scope(ev->env);
            EvalResult r = eval_block_stmts(ev, expr->as.block.stmts, expr->as.block.count);
            env_pop_scope(ev->env);
            stats_scope_pop(&ev->stats);
            return r;
        }

        case EXPR_CLOSURE: {
            stats_closure(&ev->stats);
            gc_maybe_collect(ev);
            Env *captured = env_clone(ev->env);
            return eval_ok(value_closure(
                expr->as.closure.params,
                expr->as.closure.param_count,
                expr->as.closure.body,
                captured,
                expr->as.closure.default_values,
                expr->as.closure.has_variadic));
        }

        case EXPR_RANGE: {
            EvalResult sr = eval_expr(ev, expr->as.range.start);
            if (!IS_OK(sr)) return sr;
            GC_PUSH(ev, &sr.value);
            EvalResult er = eval_expr(ev, expr->as.range.end);
            GC_POP(ev);
            if (!IS_OK(er)) { value_free(&sr.value); return er; }
            if (sr.value.type != VAL_INT || er.value.type != VAL_INT) {
                value_free(&sr.value); value_free(&er.value);
                return eval_err(strdup("range bounds must be integers"));
            }
            int64_t s = sr.value.as.int_val, e = er.value.as.int_val;
            return eval_ok(value_range(s, e));
        }

        /// @builtin print(args: Any...) -> Unit
        /// @category Core
        /// Print values separated by spaces with a trailing newline.
        /// @example print("hello", "world")  // prints: hello world
        case EXPR_PRINT: {
            for (size_t i = 0; i < expr->as.print.arg_count; i++) {
                if (i > 0) printf(" ");
                EvalResult er = eval_expr(ev, expr->as.print.args[i]);
                if (!IS_OK(er)) return er;
                char *s = value_display(&er.value);
                printf("%s", s);
                free(s);
                value_free(&er.value);
            }
            printf("\n");
            return eval_ok(value_unit());
        }

        case EXPR_TRY_CATCH: {
            stats_scope_push(&ev->stats);
            env_push_scope(ev->env);
            EvalResult tr = eval_block_stmts(ev, expr->as.try_catch.try_stmts,
                                              expr->as.try_catch.try_count);
            env_pop_scope(ev->env);
            stats_scope_pop(&ev->stats);
            if (IS_ERR(tr)) {
                /* Error: bind to catch variable and execute catch block */
                stats_scope_push(&ev->stats);
                env_push_scope(ev->env);
                env_define(ev->env, expr->as.try_catch.catch_var, value_string(tr.error));
                free(tr.error);
                EvalResult cr = eval_block_stmts(ev, expr->as.try_catch.catch_stmts,
                                                  expr->as.try_catch.catch_count);
                env_pop_scope(ev->env);
                stats_scope_pop(&ev->stats);
                return cr;
            }
            return tr;
        }

        case EXPR_SPAWN: {
            stats_scope_push(&ev->stats);
            env_push_scope(ev->env);
            EvalResult result = eval_block_stmts(ev, expr->as.block.stmts, expr->as.block.count);
            env_pop_scope(ev->env);
            stats_scope_pop(&ev->stats);
            if (IS_SIGNAL(result) && result.cf.tag == CF_RETURN) {
                return eval_ok(result.cf.value);
            }
            return result;
        }

        case EXPR_SCOPE: {
#ifdef __EMSCRIPTEN__
            /* WASM fallback: run as a regular block */
            stats_scope_push(&ev->stats);
            env_push_scope(ev->env);
            EvalResult result = eval_block_stmts(ev, expr->as.block.stmts, expr->as.block.count);
            env_pop_scope(ev->env);
            stats_scope_pop(&ev->stats);
            return result;
#else
            /* Count spawn statements */
            size_t total = expr->as.block.count;
            size_t spawn_count = 0;
            for (size_t i = 0; i < total; i++) {
                Stmt *s = expr->as.block.stmts[i];
                if (s->tag == STMT_EXPR && s->as.expr->tag == EXPR_SPAWN)
                    spawn_count++;
            }

            if (spawn_count == 0) {
                /* No spawns — run as regular block */
                stats_scope_push(&ev->stats);
                env_push_scope(ev->env);
                EvalResult result = eval_block_stmts(ev, expr->as.block.stmts, total);
                env_pop_scope(ev->env);
                stats_scope_pop(&ev->stats);
                return result;
            }

            /* Run non-spawn statements synchronously, spawn tasks in parallel */
            SpawnTask *tasks = calloc(spawn_count, sizeof(SpawnTask));
            size_t task_idx = 0;
            char *first_error = NULL;

            stats_scope_push(&ev->stats);
            env_push_scope(ev->env);

            /* Execute non-spawn statements first, create tasks for spawns */
            for (size_t i = 0; i < total; i++) {
                Stmt *s = expr->as.block.stmts[i];
                if (s->tag == STMT_EXPR && s->as.expr->tag == EXPR_SPAWN) {
                    Expr *spawn_expr = s->as.expr;
                    tasks[task_idx].stmts = spawn_expr->as.block.stmts;
                    tasks[task_idx].stmt_count = spawn_expr->as.block.count;
                    tasks[task_idx].child_ev = create_child_evaluator(ev);
                    tasks[task_idx].error = NULL;
                    task_idx++;
                } else {
                    if (!first_error) {
                        EvalResult r = eval_stmt(ev, s);
                        if (IS_ERR(r)) {
                            first_error = r.error;
                        } else if (IS_SIGNAL(r)) {
                            first_error = strdup("unexpected control flow in scope");
                            value_free(&r.cf.value);
                        } else {
                            value_free(&r.value);
                        }
                    }
                }
            }

            /* Launch all spawn threads */
            for (size_t i = 0; i < task_idx; i++) {
                pthread_create(&tasks[i].thread, NULL, spawn_thread_fn, &tasks[i]);
            }

            /* Join all threads */
            for (size_t i = 0; i < task_idx; i++) {
                pthread_join(tasks[i].thread, NULL);
            }

            /* Restore parent TLS heap */
            value_set_heap(ev->heap);
            value_set_arena(NULL);

            /* Collect first error from child threads */
            for (size_t i = 0; i < task_idx; i++) {
                if (tasks[i].error && !first_error) {
                    first_error = tasks[i].error;
                } else if (tasks[i].error) {
                    free(tasks[i].error);
                }
                free_child_evaluator(tasks[i].child_ev);
            }
            free(tasks);

            env_pop_scope(ev->env);
            stats_scope_pop(&ev->stats);

            if (first_error) return eval_err(first_error);
            return eval_ok(value_unit());
#endif
        }

        case EXPR_INTERP_STRING: {
            size_t count = expr->as.interp.count;
            /* Estimate buffer size */
            size_t buf_cap = 64;
            size_t buf_len = 0;
            char *buf = malloc(buf_cap);
            for (size_t i = 0; i < count; i++) {
                /* Append string segment */
                const char *part = expr->as.interp.parts[i];
                size_t plen = strlen(part);
                while (buf_len + plen + 1 >= buf_cap) { buf_cap *= 2; buf = realloc(buf, buf_cap); }
                memcpy(buf + buf_len, part, plen);
                buf_len += plen;
                /* Evaluate expression and convert to string */
                EvalResult er = eval_expr(ev, expr->as.interp.exprs[i]);
                if (!IS_OK(er)) { free(buf); return er; }
                char *s = value_display(&er.value);
                value_free(&er.value);
                size_t slen = strlen(s);
                while (buf_len + slen + 1 >= buf_cap) { buf_cap *= 2; buf = realloc(buf, buf_cap); }
                memcpy(buf + buf_len, s, slen);
                buf_len += slen;
                free(s);
            }
            /* Append trailing segment */
            const char *last_part = expr->as.interp.parts[count];
            size_t lplen = strlen(last_part);
            while (buf_len + lplen + 1 >= buf_cap) { buf_cap *= 2; buf = realloc(buf, buf_cap); }
            memcpy(buf + buf_len, last_part, lplen);
            buf_len += lplen;
            buf[buf_len] = '\0';
            return eval_ok(value_string_owned(buf));
        }
        case EXPR_MATCH: {
            EvalResult scr = eval_expr(ev, expr->as.match_expr.scrutinee);
            if (!IS_OK(scr)) return scr;
            GC_PUSH(ev, &scr.value);

            for (size_t i = 0; i < expr->as.match_expr.arm_count; i++) {
                MatchArm *arm = &expr->as.match_expr.arms[i];
                bool matched = false;
                char *bind_name = NULL;
                LatValue bind_val = value_unit();

                switch (arm->pattern->tag) {
                    case PAT_WILDCARD:
                        matched = true;
                        break;
                    case PAT_BINDING:
                        matched = true;
                        bind_name = arm->pattern->as.binding_name;
                        bind_val = value_deep_clone(&scr.value);
                        break;
                    case PAT_LITERAL: {
                        EvalResult pr = eval_expr(ev, arm->pattern->as.literal);
                        if (!IS_OK(pr)) { GC_POP(ev); value_free(&scr.value); return pr; }
                        matched = value_equal(&scr.value, &pr.value);
                        value_free(&pr.value);
                        break;
                    }
                    case PAT_RANGE: {
                        EvalResult sr = eval_expr(ev, arm->pattern->as.range.start);
                        if (!IS_OK(sr)) { GC_POP(ev); value_free(&scr.value); return sr; }
                        EvalResult er = eval_expr(ev, arm->pattern->as.range.end);
                        if (!IS_OK(er)) { GC_POP(ev); value_free(&scr.value); value_free(&sr.value); return er; }
                        if (scr.value.type == VAL_INT && sr.value.type == VAL_INT && er.value.type == VAL_INT) {
                            matched = scr.value.as.int_val >= sr.value.as.int_val &&
                                      scr.value.as.int_val <= er.value.as.int_val;
                        }
                        value_free(&sr.value);
                        value_free(&er.value);
                        break;
                    }
                }

                /* Check phase qualifier */
                if (matched && arm->pattern->phase_qualifier != PHASE_UNSPECIFIED) {
                    bool phase_ok = false;
                    if (arm->pattern->phase_qualifier == PHASE_FLUID)
                        phase_ok = (scr.value.phase == VTAG_FLUID || scr.value.phase == VTAG_UNPHASED);
                    else if (arm->pattern->phase_qualifier == PHASE_CRYSTAL)
                        phase_ok = (scr.value.phase == VTAG_CRYSTAL);
                    if (!phase_ok) {
                        if (bind_name) { value_free(&bind_val); bind_name = NULL; }
                        continue;
                    }
                }

                if (!matched) continue;

                /* Check guard */
                if (arm->guard) {
                    env_push_scope(ev->env);
                    if (bind_name) env_define(ev->env, bind_name, value_deep_clone(&bind_val));
                    EvalResult gr = eval_expr(ev, arm->guard);
                    env_pop_scope(ev->env);
                    if (!IS_OK(gr)) {
                        if (bind_name) value_free(&bind_val);
                        GC_POP(ev); value_free(&scr.value);
                        return gr;
                    }
                    bool guard_pass = gr.value.type == VAL_BOOL && gr.value.as.bool_val;
                    value_free(&gr.value);
                    if (!guard_pass) {
                        if (bind_name) value_free(&bind_val);
                        continue;
                    }
                }

                /* Execute arm body */
                env_push_scope(ev->env);
                if (bind_name) env_define(ev->env, bind_name, bind_val);
                EvalResult result = eval_ok(value_unit());
                for (size_t j = 0; j < arm->body_count; j++) {
                    value_free(&result.value);
                    result = eval_stmt(ev, arm->body[j]);
                    if (!IS_OK(result)) break;
                }
                env_pop_scope(ev->env);
                GC_POP(ev); value_free(&scr.value);
                return result;
            }

            GC_POP(ev); value_free(&scr.value);
            return eval_ok(value_nil());
        }

        case EXPR_ENUM_VARIANT: {
            const char *enum_name = expr->as.enum_variant.enum_name;
            const char *variant_name = expr->as.enum_variant.variant_name;

            EnumDecl *ed = find_enum(ev, enum_name);
            if (!ed) {
                /* Fall back to static call: Name::method(args)
                 * Build a temporary EXPR_CALL node and evaluate it so that
                 * builtins like Map::new(), Channel::new() etc. work. */
                size_t ac = expr->as.enum_variant.arg_count;
                size_t nlen = strlen(enum_name) + 2 + strlen(variant_name) + 1;
                char *full = malloc(nlen);
                snprintf(full, nlen, "%s::%s", enum_name, variant_name);

                /* Borrow the arg expressions for the temp node */
                Expr **arg_refs = NULL;
                if (ac > 0) {
                    arg_refs = malloc(ac * sizeof(Expr *));
                    for (size_t i = 0; i < ac; i++)
                        arg_refs[i] = expr->as.enum_variant.args[i];
                }

                /* Build a temporary EXPR_CALL:  full(args...)
                 * Use a stack-allocated EXPR_IDENT for the func. */
                Expr tmp_ident;
                tmp_ident.tag = EXPR_IDENT;
                tmp_ident.as.str_val = full;

                Expr tmp_call;
                tmp_call.tag = EXPR_CALL;
                tmp_call.as.call.func = &tmp_ident;
                tmp_call.as.call.args = arg_refs;
                tmp_call.as.call.arg_count = ac;

                EvalResult cr = eval_expr(ev, &tmp_call);

                /* Don't free arg expressions — they belong to the original node */
                free(arg_refs);
                free(full);
                return cr;
            }

            VariantDecl *vd = find_variant(ed, variant_name);
            if (!vd) {
                char *err2 = NULL;
                /* Build NULL-terminated variant name array for suggestion */
                const char **vcands = malloc((ed->variant_count + 1) * sizeof(const char *));
                for (size_t vi = 0; vi < ed->variant_count; vi++)
                    vcands[vi] = ed->variants[vi].name;
                vcands[ed->variant_count] = NULL;
                const char *vsug = lat_find_similar(variant_name, vcands, 2);
                if (vsug)
                    (void)asprintf(&err2, "enum '%s' has no variant '%s' (did you mean '%s'?)", enum_name, variant_name, vsug);
                else
                    (void)asprintf(&err2, "enum '%s' has no variant '%s'", enum_name, variant_name);
                free(vcands);
                return eval_err(err2);
            }

            size_t provided = expr->as.enum_variant.arg_count;
            if (provided != vd->param_count) {
                char *err2 = NULL;
                (void)asprintf(&err2, "variant '%s::%s' expects %zu argument%s, got %zu",
                               enum_name, variant_name, vd->param_count,
                               vd->param_count == 1 ? "" : "s", provided);
                return eval_err(err2);
            }

            LatValue *payload = NULL;
            if (provided > 0) {
                payload = malloc(provided * sizeof(LatValue));
                for (size_t i = 0; i < provided; i++) {
                    EvalResult er = eval_expr(ev, expr->as.enum_variant.args[i]);
                    if (!IS_OK(er)) {
                        for (size_t j = 0; j < i; j++) value_free(&payload[j]);
                        free(payload);
                        return er;
                    }
                    payload[i] = er.value;
                }
            }
            LatValue enm = value_enum(enum_name, variant_name, payload, provided);
            if (payload) {
                for (size_t i = 0; i < provided; i++) value_free(&payload[i]);
                free(payload);
            }
            return eval_ok(enm);
        }

        case EXPR_TRY_PROPAGATE: {
            EvalResult inner = eval_expr(ev, expr->as.try_propagate_expr);
            if (!inner.ok) return inner;
            if (inner.value.type != VAL_MAP) {
                value_free(&inner.value);
                return eval_err(strdup("? operator requires a Result map (got non-Map value)"));
            }
            LatValue *tag = lat_map_get(inner.value.as.map.map, "tag");
            if (!tag || tag->type != VAL_STR) {
                value_free(&inner.value);
                return eval_err(strdup("? operator requires a Map with a string \"tag\" field"));
            }
            if (strcmp(tag->as.str_val, "ok") == 0) {
                LatValue *val = lat_map_get(inner.value.as.map.map, "value");
                LatValue result = val ? value_deep_clone(val) : value_nil();
                value_free(&inner.value);
                return eval_ok(result);
            }
            if (strcmp(tag->as.str_val, "err") == 0) {
                /* Propagate: return from enclosing function with the err Map */
                return eval_signal(CF_RETURN, inner.value);
            }
            value_free(&inner.value);
            return eval_err(strdup("? operator: tag must be \"ok\" or \"err\""));
        }

        case EXPR_SELECT: {
#ifdef __EMSCRIPTEN__
            return eval_err(strdup("select is not supported in WASM builds"));
#else
            size_t arm_count = expr->as.select_expr.arm_count;
            SelectArm *arms = expr->as.select_expr.arms;

            /* Find default and timeout arms */
            int default_idx = -1;
            int timeout_idx = -1;
            for (size_t i = 0; i < arm_count; i++) {
                if (arms[i].is_default) default_idx = (int)i;
                if (arms[i].is_timeout) timeout_idx = (int)i;
            }

            /* Evaluate all channel expressions upfront */
            LatChannel **channels = calloc(arm_count, sizeof(LatChannel *));
            for (size_t i = 0; i < arm_count; i++) {
                if (arms[i].is_default || arms[i].is_timeout) continue;
                EvalResult cer = eval_expr(ev, arms[i].channel_expr);
                if (!IS_OK(cer)) { free(channels); return cer; }
                if (cer.value.type != VAL_CHANNEL) {
                    value_free(&cer.value);
                    free(channels);
                    return eval_err(strdup("select arm: expression is not a Channel"));
                }
                channels[i] = cer.value.as.channel.ch;
                channel_retain(channels[i]);
                value_free(&cer.value);
            }

            /* Evaluate timeout if present */
            long timeout_ms = -1;
            if (timeout_idx >= 0) {
                EvalResult ter = eval_expr(ev, arms[timeout_idx].timeout_expr);
                if (!IS_OK(ter)) {
                    for (size_t i = 0; i < arm_count; i++)
                        if (channels[i]) channel_release(channels[i]);
                    free(channels);
                    return ter;
                }
                if (ter.value.type != VAL_INT) {
                    value_free(&ter.value);
                    for (size_t i = 0; i < arm_count; i++)
                        if (channels[i]) channel_release(channels[i]);
                    free(channels);
                    return eval_err(strdup("select timeout must be an integer (milliseconds)"));
                }
                timeout_ms = (long)ter.value.as.int_val;
                value_free(&ter.value);
            }

            /* Build shuffled index array for fairness */
            size_t ch_arm_count = 0;
            size_t *indices = malloc(arm_count * sizeof(size_t));
            for (size_t i = 0; i < arm_count; i++) {
                if (!arms[i].is_default && !arms[i].is_timeout)
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

            EvalResult select_result = eval_ok(value_unit());
            bool found = false;

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
                        /* Got a value — bind and execute body */
                        env_push_scope(ev->env);
                        if (arms[i].binding_name)
                            env_define(ev->env, arms[i].binding_name, recv_val);
                        else
                            value_free(&recv_val);
                        select_result = eval_block_stmts(ev, arms[i].body, arms[i].body_count);
                        env_pop_scope(ev->env);
                        found = true;
                        break;
                    }
                    if (!closed) all_closed = false;
                }
                if (found) break;

                if (all_closed && ch_arm_count > 0) {
                    /* All channels closed — execute default if present, otherwise return unit */
                    if (default_idx >= 0) {
                        env_push_scope(ev->env);
                        select_result = eval_block_stmts(ev, arms[default_idx].body, arms[default_idx].body_count);
                        env_pop_scope(ev->env);
                    }
                    break;
                }

                /* If there's a default arm, execute it immediately (non-blocking select) */
                if (default_idx >= 0) {
                    env_push_scope(ev->env);
                    select_result = eval_block_stmts(ev, arms[default_idx].body, arms[default_idx].body_count);
                    env_pop_scope(ev->env);
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
                        if (timeout_idx >= 0) {
                            env_push_scope(ev->env);
                            select_result = eval_block_stmts(ev, arms[timeout_idx].body, arms[timeout_idx].body_count);
                            env_pop_scope(ev->env);
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
            for (size_t i = 0; i < arm_count; i++)
                if (channels[i]) channel_release(channels[i]);
            free(channels);
            return select_result;
#endif
        }

        case EXPR_SPREAD:
            return eval_err(strdup("spread operator ... can only be used inside array literals"));
    }
    return eval_err(strdup("unknown expression type"));
}

/* ── Module loading ── */

static EvalResult load_module(Evaluator *ev, const char *raw_path) {
    /* Check for built-in stdlib module */
    LatValue builtin_mod;
    if (rt_try_builtin_import(raw_path, &builtin_mod)) {
        return eval_ok(builtin_mod);
    }

    /* Try lat_modules/ resolution for bare module names */
    char *pkg_resolved = pkg_resolve_module(raw_path, ev->script_dir);

    /* Resolve file path: append .lat if not present */
    size_t plen = strlen(raw_path);
    char *file_path;
    if (pkg_resolved) {
        file_path = pkg_resolved; /* already absolute */
    } else if (plen >= 4 && strcmp(raw_path + plen - 4, ".lat") == 0) {
        file_path = strdup(raw_path);
    } else {
        file_path = malloc(plen + 5);
        memcpy(file_path, raw_path, plen);
        memcpy(file_path + plen, ".lat", 5);
    }

    /* Resolve to an absolute path */
    char resolved[PATH_MAX];
    bool found;
    if (pkg_resolved) {
        strncpy(resolved, file_path, PATH_MAX - 1);
        resolved[PATH_MAX - 1] = '\0';
        free(file_path);
        found = true;
    } else {
        found = (realpath(file_path, resolved) != NULL);
        if (!found && ev->script_dir && file_path[0] != '/') {
            char script_rel[PATH_MAX];
            snprintf(script_rel, sizeof(script_rel), "%s/%s", ev->script_dir, file_path);
            found = (realpath(script_rel, resolved) != NULL);
        }
        if (!found) {
            char *err = NULL;
            (void)asprintf(&err, "import: cannot find '%s'", file_path);
            free(file_path);
            return eval_err(err);
        }
        free(file_path);
    }

    /* Check module cache */
    LatValue *cached = (LatValue *)lat_map_get(&ev->module_cache, resolved);
    if (cached) {
        return eval_ok(value_deep_clone(cached));
    }

    /* Check for circular imports */
    if (lat_map_get(&ev->required_files, resolved)) {
        char *err = NULL;
        (void)asprintf(&err, "import: circular dependency on '%s'", resolved);
        return eval_err(err);
    }

    /* Mark as loading */
    bool marker = true;
    lat_map_set(&ev->required_files, resolved, &marker);

    /* Read the file */
    char *source = builtin_read_file(resolved);
    if (!source) {
        char *err = NULL;
        (void)asprintf(&err, "import: cannot read '%s'", resolved);
        return eval_err(err);
    }

    /* Lex */
    Lexer mod_lex = lexer_new(source);
    char *mod_lex_err = NULL;
    LatVec mod_toks = lexer_tokenize(&mod_lex, &mod_lex_err);
    free(source);
    if (mod_lex_err) {
        char *err = NULL;
        (void)asprintf(&err, "import '%s': %s", resolved, mod_lex_err);
        free(mod_lex_err);
        return eval_err(err);
    }

    /* Parse */
    Parser mod_parser = parser_new(&mod_toks);
    char *mod_parse_err = NULL;
    Program mod_prog = parser_parse(&mod_parser, &mod_parse_err);
    if (mod_parse_err) {
        char *err = NULL;
        (void)asprintf(&err, "import '%s': %s", resolved, mod_parse_err);
        free(mod_parse_err);
        program_free(&mod_prog);
        for (size_t j = 0; j < mod_toks.len; j++) token_free(lat_vec_get(&mod_toks, j));
        lat_vec_free(&mod_toks);
        return eval_err(err);
    }

    /* Register functions, structs, enums, traits, impls globally */
    for (size_t j = 0; j < mod_prog.item_count; j++) {
        if (mod_prog.items[j].tag == ITEM_STRUCT) {
            StructDecl *ptr = &mod_prog.items[j].as.struct_decl;
            lat_map_set(&ev->struct_defs, ptr->name, &ptr);
        } else if (mod_prog.items[j].tag == ITEM_FUNCTION) {
            FnDecl *ptr = &mod_prog.items[j].as.fn_decl;
            register_fn_overload(&ev->fn_defs, ptr);
        } else if (mod_prog.items[j].tag == ITEM_ENUM) {
            EnumDecl *ptr = &mod_prog.items[j].as.enum_decl;
            lat_map_set(&ev->enum_defs, ptr->name, &ptr);
        } else if (mod_prog.items[j].tag == ITEM_TRAIT) {
            TraitDecl *ptr = &mod_prog.items[j].as.trait_decl;
            lat_map_set(&ev->trait_defs, ptr->name, &ptr);
        } else if (mod_prog.items[j].tag == ITEM_IMPL) {
            ImplBlock *ptr = &mod_prog.items[j].as.impl_block;
            char key[512];
            snprintf(key, sizeof(key), "%s::%s", ptr->type_name, ptr->trait_name);
            lat_map_set(&ev->impl_registry, key, &ptr);
        }
    }

    /* Push a module scope and execute statements */
    env_push_scope(ev->env);

    char *prev_script_dir = ev->script_dir;
    char *resolved_copy = strdup(resolved);
    ev->script_dir = strdup(dirname(resolved_copy));
    free(resolved_copy);

    EvalResult exec_r = eval_ok(value_unit());
    for (size_t j = 0; j < mod_prog.item_count; j++) {
        if (mod_prog.items[j].tag == ITEM_STMT) {
            value_free(&exec_r.value);
            exec_r = eval_stmt(ev, mod_prog.items[j].as.stmt);
            if (!IS_OK(exec_r)) break;
        }
    }

    free(ev->script_dir);
    ev->script_dir = prev_script_dir;

    if (!IS_OK(exec_r)) {
        env_pop_scope(ev->env);
        for (size_t j = 0; j < mod_toks.len; j++) token_free(lat_vec_get(&mod_toks, j));
        lat_vec_free(&mod_toks);
        /* Don't free prog items since decls may be registered */
        return exec_r;
    }
    value_free(&exec_r.value);

    /* Build module Map from scope bindings and functions */
    LatValue module_map = value_map_new();

    /* Export top-level variable bindings from module scope */
    Scope *mod_scope = &ev->env->scopes[ev->env->count - 1];
    for (size_t i = 0; i < mod_scope->cap; i++) {
        if (mod_scope->entries[i].state == MAP_OCCUPIED) {
            const char *name = mod_scope->entries[i].key;
            if (!module_should_export(name,
                    (const char **)mod_prog.export_names,
                    mod_prog.export_count, mod_prog.has_exports))
                continue;
            LatValue *val_ptr = (LatValue *)mod_scope->entries[i].value;
            LatValue exported = value_deep_clone(val_ptr);
            lat_map_set(module_map.as.map.map, name, &exported);
        }
    }

    /* Export functions as closures */
    for (size_t j = 0; j < mod_prog.item_count; j++) {
        if (mod_prog.items[j].tag == ITEM_FUNCTION) {
            FnDecl *fn = &mod_prog.items[j].as.fn_decl;

            /* Filter based on export declarations */
            if (!module_should_export(fn->name,
                    (const char **)mod_prog.export_names,
                    mod_prog.export_count, mod_prog.has_exports))
                continue;

            /* Create an expr_block wrapping the function body.
             * This borrows fn->body (kept alive via program items). */
            Expr *body = calloc(1, sizeof(Expr));
            body->tag = EXPR_BLOCK;
            body->as.block.stmts = fn->body;
            body->as.block.count = fn->body_count;
            /* Track this Expr so it stays alive */
            lat_vec_push(&ev->module_exprs, &body);

            Env *captured = env_clone(ev->env);
            Expr **defaults = NULL;
            bool has_variadic = false;
            if (fn->param_count > 0) {
                defaults = malloc(fn->param_count * sizeof(Expr *));
                for (size_t k = 0; k < fn->param_count; k++) {
                    defaults[k] = fn->params[k].default_value;
                    if (fn->params[k].is_variadic) has_variadic = true;
                }
            }

            char **param_names = malloc(fn->param_count * sizeof(char *));
            for (size_t k = 0; k < fn->param_count; k++) {
                param_names[k] = fn->params[k].name;
            }
            LatValue closure = value_closure(param_names, fn->param_count, body,
                                             captured, defaults, has_variadic);
            free(param_names);
            /* Don't free defaults — value_closure borrows it. Track for cleanup. */
            if (defaults) lat_vec_push(&ev->module_exprs, &defaults);
            lat_map_set(module_map.as.map.map, fn->name, &closure);
        }
    }

    env_pop_scope(ev->env);

    /* Cache the module */
    LatValue cached_copy = value_deep_clone(&module_map);
    lat_map_set(&ev->module_cache, resolved, &cached_copy);

    /* Cleanup: free statements, keep decl items alive */
    for (size_t j = 0; j < mod_prog.item_count; j++) {
        if (mod_prog.items[j].tag == ITEM_STMT)
            stmt_free(mod_prog.items[j].as.stmt);
    }
    /* Don't free prog.items since fn/struct/enum decls are still referenced */

    for (size_t j = 0; j < mod_toks.len; j++) token_free(lat_vec_get(&mod_toks, j));
    lat_vec_free(&mod_toks);

    return eval_ok(module_map);
}

/* ── Statement evaluation ── */

static EvalResult eval_stmt(Evaluator *ev, const Stmt *stmt) {
    switch (stmt->tag) {
        case STMT_BINDING: {
            EvalResult vr = eval_expr(ev, stmt->as.binding.value);
            if (!IS_OK(vr)) return vr;

            if (ev->mode == MODE_CASUAL) {
                switch (stmt->as.binding.phase) {
                    case PHASE_FLUID: vr.value.phase = VTAG_FLUID; break;
                    case PHASE_CRYSTAL:
                        stats_freeze(&ev->stats);
                        { uint64_t ft0 = now_ns();
                        vr.value = value_freeze(vr.value);
                        freeze_to_region(ev, &vr.value);
                        ev->stats.freeze_total_ns += now_ns() - ft0; }
                        break;
                    case PHASE_UNSPECIFIED: break;
                }
            } else { /* MODE_STRICT */
                switch (stmt->as.binding.phase) {
                    case PHASE_FLUID:
                        if (value_is_crystal(&vr.value)) {
                            char *err = NULL;
                            (void)asprintf(&err, "strict mode: 'flux' binding '%s' produced a crystal value",
                                           stmt->as.binding.name);
                            value_free(&vr.value);
                            return eval_err(err);
                        }
                        vr.value.phase = VTAG_FLUID;
                        break;
                    case PHASE_CRYSTAL:
                        stats_freeze(&ev->stats);
                        { uint64_t ft0 = now_ns();
                        vr.value = value_freeze(vr.value);
                        freeze_to_region(ev, &vr.value);
                        ev->stats.freeze_total_ns += now_ns() - ft0; }
                        break;
                    case PHASE_UNSPECIFIED: {
                        char *err = NULL;
                        (void)asprintf(&err, "strict mode: binding '%s' requires an explicit phase (flux/fix)",
                                       stmt->as.binding.name);
                        value_free(&vr.value);
                        return eval_err(err);
                    }
                }
            }
            stats_binding(&ev->stats);
            /* In lat_eval context, top-level bindings go to the root scope
             * so they persist across calls (needed for REPL) */
            if (ev->lat_eval_scope > 0 && ev->env->count == ev->lat_eval_scope)
                env_define_at(ev->env, 0, stmt->as.binding.name, vr.value);
            else
                env_define(ev->env, stmt->as.binding.name, vr.value);
            return eval_ok(value_unit());
        }

        case STMT_ASSIGN: {
            EvalResult valr = eval_expr(ev, stmt->as.assign.value);
            if (!IS_OK(valr)) return valr;

            /* Simple ident assignment uses env_set for proper scoping */
            if (stmt->as.assign.target->tag == EXPR_IDENT) {
                const char *name = stmt->as.assign.target->as.str_val;
                if (ev->mode == MODE_STRICT) {
                    LatValue existing;
                    if (env_get(ev->env, name, &existing)) {
                        bool is_crys = value_is_crystal(&existing);
                        value_free(&existing);
                        if (is_crys) {
                            char *err = NULL;
                            (void)asprintf(&err, "strict mode: cannot assign to crystal binding '%s'", name);
                            value_free(&valr.value);
                            return eval_err(err);
                        }
                    }
                }
                if (!env_set(ev->env, name, valr.value)) {
                    char *err = NULL;
                    (void)asprintf(&err, "undefined variable '%s'", name);
                    return eval_err(err);
                }
                record_history(ev, name);
                return eval_ok(value_unit());
            }

            /* Buffer index assignment: buf[i] = byte (must be handled before resolve_lvalue
             * since buffers store raw bytes, not LatValues) */
            if (stmt->as.assign.target->tag == EXPR_INDEX) {
                char *buf_chk_err = NULL;
                LatValue *buf_chk = resolve_lvalue(ev, stmt->as.assign.target->as.index.object, &buf_chk_err);
                if (buf_chk_err) free(buf_chk_err);
                if (buf_chk && buf_chk->type == VAL_BUFFER) {
                    EvalResult buf_idxr = eval_expr(ev, stmt->as.assign.target->as.index.index);
                    if (!IS_OK(buf_idxr)) { value_free(&valr.value); return buf_idxr; }
                    if (buf_idxr.value.type != VAL_INT) {
                        value_free(&buf_idxr.value); value_free(&valr.value);
                        return eval_err(strdup("buffer index must be an integer"));
                    }
                    size_t bidx = (size_t)buf_idxr.value.as.int_val;
                    value_free(&buf_idxr.value);
                    if (bidx >= buf_chk->as.buffer.len) {
                        value_free(&valr.value);
                        char *berr = NULL;
                        (void)asprintf(&berr, "buffer index %zu out of bounds (length %zu)",
                                       bidx, buf_chk->as.buffer.len);
                        return eval_err(berr);
                    }
                    if (valr.value.type != VAL_INT) {
                        value_free(&valr.value);
                        return eval_err(strdup("buffer element must be an integer"));
                    }
                    buf_chk->as.buffer.data[bidx] = (uint8_t)(valr.value.as.int_val & 0xFF);
                    value_free(&valr.value);
                    return eval_ok(value_unit());
                }
            }

            /* For field access, index, and nested chains: use resolve_lvalue */
            char *lv_err = NULL;
            LatValue *target = resolve_lvalue(ev, stmt->as.assign.target, &lv_err);
            if (!target) {
                value_free(&valr.value);
                return eval_err(lv_err);
            }
            if (ev->mode == MODE_STRICT && value_is_crystal(target)) {
                value_free(&valr.value);
                return eval_err(strdup("strict mode: cannot assign to crystal value"));
            }
            /* Check sublimated phase for direct parent */
            if (stmt->as.assign.target->tag == EXPR_FIELD_ACCESS) {
                LatValue *parent = resolve_lvalue(ev, stmt->as.assign.target->as.field_access.object, &lv_err);
                if (parent && parent->phase == VTAG_SUBLIMATED) {
                    const char *fname = stmt->as.assign.target->as.field_access.field;
                    char *err = NULL;
                    (void)asprintf(&err, "cannot assign to field '%s' of sublimated value", fname);
                    value_free(&valr.value);
                    return eval_err(err);
                }
            }
            if (stmt->as.assign.target->tag == EXPR_INDEX) {
                LatValue *parent = resolve_lvalue(ev, stmt->as.assign.target->as.index.object, &lv_err);
                if (parent && parent->phase == VTAG_SUBLIMATED) {
                    value_free(&valr.value);
                    return eval_err(strdup("cannot assign to index of sublimated value"));
                }
            }
            /* Check per-field phase for struct field assignments */
            if (stmt->as.assign.target->tag == EXPR_FIELD_ACCESS) {
                LatValue *parent = resolve_lvalue(ev, stmt->as.assign.target->as.field_access.object, &lv_err);
                if (parent && parent->type == VAL_STRUCT && parent->phase == VTAG_CRYSTAL) {
                    const char *fname = stmt->as.assign.target->as.field_access.field;
                    char *err = NULL;
                    (void)asprintf(&err, "cannot assign to field '%s' of frozen struct", fname);
                    value_free(&valr.value);
                    return eval_err(err);
                }
                if (parent && parent->type == VAL_STRUCT && parent->as.strct.field_phases) {
                    const char *fname = stmt->as.assign.target->as.field_access.field;
                    for (size_t fi = 0; fi < parent->as.strct.field_count; fi++) {
                        if (parent->as.strct.field_names[fi] == intern(fname)) {
                            if (parent->as.strct.field_phases[fi] == VTAG_CRYSTAL) {
                                char *err = NULL;
                                (void)asprintf(&err, "cannot assign to frozen field '%s'", fname);
                                value_free(&valr.value);
                                return eval_err(err);
                            }
                            break;
                        }
                    }
                }
            }
            /* Check per-key phase for map key assignments */
            if (stmt->as.assign.target->tag == EXPR_INDEX) {
                LatValue *parent = resolve_lvalue(ev, stmt->as.assign.target->as.index.object, &lv_err);
                if (parent && parent->type == VAL_REF && parent->phase == VTAG_CRYSTAL) {
                    value_free(&valr.value);
                    return eval_err(strdup("cannot assign index on a frozen Ref"));
                }
                if (parent && parent->type == VAL_MAP && parent->phase == VTAG_CRYSTAL) {
                    value_free(&valr.value);
                    return eval_err(strdup("cannot assign to key of frozen map"));
                }
                if (parent && parent->type == VAL_MAP && parent->as.map.key_phases) {
                    EvalResult kidxr = eval_expr(ev, stmt->as.assign.target->as.index.index);
                    if (IS_OK(kidxr) && kidxr.value.type == VAL_STR) {
                        PhaseTag *kp = (PhaseTag *)lat_map_get(parent->as.map.key_phases, kidxr.value.as.str_val);
                        if (kp && *kp == VTAG_CRYSTAL) {
                            char *err = NULL;
                            (void)asprintf(&err, "cannot assign to frozen key '%s'", kidxr.value.as.str_val);
                            value_free(&kidxr.value);
                            value_free(&valr.value);
                            return eval_err(err);
                        }
                    }
                    value_free(&kidxr.value);
                }
            }
            value_free(target);
            *target = valr.value;
            /* Record history for root variable of field/index chain */
            if (ev->tracked_count > 0) {
                const Expr *root = stmt->as.assign.target;
                while (root->tag == EXPR_FIELD_ACCESS) root = root->as.field_access.object;
                while (root->tag == EXPR_INDEX) root = root->as.index.object;
                if (root->tag == EXPR_IDENT)
                    record_history(ev, root->as.str_val);
            }
            return eval_ok(value_unit());
        }

        case STMT_EXPR:
            return eval_expr(ev, stmt->as.expr);

        case STMT_RETURN: {
            if (stmt->as.return_expr) {
                EvalResult er = eval_expr(ev, stmt->as.return_expr);
                if (!IS_OK(er)) return er;
                return eval_signal(CF_RETURN, er.value);
            }
            return eval_signal(CF_RETURN, value_unit());
        }

        case STMT_FOR: {
            EvalResult iter_r = eval_expr(ev, stmt->as.for_loop.iter);
            if (!IS_OK(iter_r)) return iter_r;

            if (iter_r.value.type == VAL_RANGE) {
                int64_t s = iter_r.value.as.range.start;
                int64_t e = iter_r.value.as.range.end;
                value_free(&iter_r.value);
                for (int64_t i = s; i < e; i++) {
                    stats_scope_push(&ev->stats);
                    env_push_scope(ev->env);
                    env_define(ev->env, stmt->as.for_loop.var, value_int(i));
                    EvalResult r = eval_block_stmts(ev, stmt->as.for_loop.body,
                                                     stmt->as.for_loop.body_count);
                    env_pop_scope(ev->env);
                    stats_scope_pop(&ev->stats);
                    if (IS_SIGNAL(r) && r.cf.tag == CF_BREAK) break;
                    if (IS_SIGNAL(r) && r.cf.tag == CF_CONTINUE) continue;
                    if (!IS_OK(r)) return r;
                    value_free(&r.value);
                }
            } else if (iter_r.value.type == VAL_ARRAY) {
                GC_PUSH(ev, &iter_r.value);
                size_t len = iter_r.value.as.array.len;
                for (size_t i = 0; i < len; i++) {
                    stats_scope_push(&ev->stats);
                    env_push_scope(ev->env);
                    LatValue elem = value_deep_clone(&iter_r.value.as.array.elems[i]);
                    env_define(ev->env, stmt->as.for_loop.var, elem);
                    EvalResult r = eval_block_stmts(ev, stmt->as.for_loop.body,
                                                     stmt->as.for_loop.body_count);
                    env_pop_scope(ev->env);
                    stats_scope_pop(&ev->stats);
                    if (IS_SIGNAL(r) && r.cf.tag == CF_BREAK) break;
                    if (IS_SIGNAL(r) && r.cf.tag == CF_CONTINUE) continue;
                    if (!IS_OK(r)) { GC_POP(ev); value_free(&iter_r.value); return r; }
                    value_free(&r.value);
                }
                GC_POP(ev);
                value_free(&iter_r.value);
            } else if (iter_r.value.type == VAL_MAP) {
                /* Iterate over map keys */
                GC_PUSH(ev, &iter_r.value);
                for (size_t i = 0; i < iter_r.value.as.map.map->cap; i++) {
                    if (iter_r.value.as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                    stats_scope_push(&ev->stats);
                    env_push_scope(ev->env);
                    env_define(ev->env, stmt->as.for_loop.var,
                               value_string(iter_r.value.as.map.map->entries[i].key));
                    EvalResult r = eval_block_stmts(ev, stmt->as.for_loop.body,
                                                     stmt->as.for_loop.body_count);
                    env_pop_scope(ev->env);
                    stats_scope_pop(&ev->stats);
                    if (IS_SIGNAL(r) && r.cf.tag == CF_BREAK) break;
                    if (IS_SIGNAL(r) && r.cf.tag == CF_CONTINUE) continue;
                    if (!IS_OK(r)) { GC_POP(ev); value_free(&iter_r.value); return r; }
                    value_free(&r.value);
                }
                GC_POP(ev);
                value_free(&iter_r.value);
            } else if (iter_r.value.type == VAL_SET) {
                /* Iterate over set elements */
                GC_PUSH(ev, &iter_r.value);
                for (size_t i = 0; i < iter_r.value.as.set.map->cap; i++) {
                    if (iter_r.value.as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                    stats_scope_push(&ev->stats);
                    env_push_scope(ev->env);
                    LatValue *sv = (LatValue *)iter_r.value.as.set.map->entries[i].value;
                    env_define(ev->env, stmt->as.for_loop.var, value_deep_clone(sv));
                    EvalResult r = eval_block_stmts(ev, stmt->as.for_loop.body,
                                                     stmt->as.for_loop.body_count);
                    env_pop_scope(ev->env);
                    stats_scope_pop(&ev->stats);
                    if (IS_SIGNAL(r) && r.cf.tag == CF_BREAK) break;
                    if (IS_SIGNAL(r) && r.cf.tag == CF_CONTINUE) continue;
                    if (!IS_OK(r)) { GC_POP(ev); value_free(&iter_r.value); return r; }
                    value_free(&r.value);
                }
                GC_POP(ev);
                value_free(&iter_r.value);
            } else {
                char *err = NULL;
                (void)asprintf(&err, "cannot iterate over %s", value_type_name(&iter_r.value));
                value_free(&iter_r.value);
                return eval_err(err);
            }
            return eval_ok(value_unit());
        }

        case STMT_WHILE: {
            for (;;) {
                EvalResult condr = eval_expr(ev, stmt->as.while_loop.cond);
                if (!IS_OK(condr)) return condr;
                bool truthy = value_is_truthy(&condr.value);
                value_free(&condr.value);
                if (!truthy) break;
                stats_scope_push(&ev->stats);
                env_push_scope(ev->env);
                EvalResult r = eval_block_stmts(ev, stmt->as.while_loop.body,
                                                 stmt->as.while_loop.body_count);
                env_pop_scope(ev->env);
                stats_scope_pop(&ev->stats);
                if (IS_SIGNAL(r) && r.cf.tag == CF_BREAK) break;
                if (IS_SIGNAL(r) && r.cf.tag == CF_CONTINUE) continue;
                if (!IS_OK(r)) return r;
                value_free(&r.value);
            }
            return eval_ok(value_unit());
        }

        case STMT_LOOP: {
            for (;;) {
                stats_scope_push(&ev->stats);
                env_push_scope(ev->env);
                EvalResult r = eval_block_stmts(ev, stmt->as.loop.body, stmt->as.loop.body_count);
                env_pop_scope(ev->env);
                stats_scope_pop(&ev->stats);
                if (IS_SIGNAL(r) && r.cf.tag == CF_BREAK) break;
                if (IS_SIGNAL(r) && r.cf.tag == CF_CONTINUE) continue;
                if (!IS_OK(r)) return r;
                value_free(&r.value);
            }
            return eval_ok(value_unit());
        }

        case STMT_BREAK:    return eval_signal(CF_BREAK, value_unit());
        case STMT_CONTINUE: return eval_signal(CF_CONTINUE, value_unit());

        case STMT_DEFER: {
            /* Push to defer stack — don't execute now */
            if (ev->defer_count >= ev->defer_cap) {
                ev->defer_cap = ev->defer_cap < 8 ? 8 : ev->defer_cap * 2;
                ev->defer_stack = realloc(ev->defer_stack, ev->defer_cap * sizeof(DeferEntry));
            }
            ev->defer_stack[ev->defer_count].body = stmt->as.defer.body;
            ev->defer_stack[ev->defer_count].body_count = stmt->as.defer.body_count;
            ev->defer_stack[ev->defer_count].scope_depth = ev->stats.current_scope_depth;
            ev->defer_count++;
            return eval_ok(value_unit());
        }

        case STMT_DESTRUCTURE: {
            EvalResult vr = eval_expr(ev, stmt->as.destructure.value);
            if (!IS_OK(vr)) return vr;

            if (stmt->as.destructure.kind == DESTRUCT_ARRAY) {
                if (vr.value.type != VAL_ARRAY) {
                    char *err = NULL;
                    (void)asprintf(&err, "cannot destructure %s as array",
                                   value_type_name(&vr.value));
                    value_free(&vr.value);
                    return eval_err(err);
                }
                size_t arr_len = vr.value.as.array.len;
                size_t name_count = stmt->as.destructure.name_count;
                bool has_rest = (stmt->as.destructure.rest_name != NULL);

                if (!has_rest && arr_len != name_count) {
                    char *err = NULL;
                    (void)asprintf(&err, "array destructure: expected %zu elements, got %zu",
                                   name_count, arr_len);
                    value_free(&vr.value);
                    return eval_err(err);
                }
                if (has_rest && arr_len < name_count) {
                    char *err = NULL;
                    (void)asprintf(&err, "array destructure: expected at least %zu elements, got %zu",
                                   name_count, arr_len);
                    value_free(&vr.value);
                    return eval_err(err);
                }

                /* Bind named elements */
                for (size_t i = 0; i < name_count; i++) {
                    LatValue elem = value_deep_clone(&vr.value.as.array.elems[i]);
                    /* Apply phase */
                    if (stmt->as.destructure.phase == PHASE_FLUID)
                        elem.phase = VTAG_FLUID;
                    else if (stmt->as.destructure.phase == PHASE_CRYSTAL) {
                        stats_freeze(&ev->stats);
                        elem = value_freeze(elem);
                        freeze_to_region(ev, &elem);
                    }
                    stats_binding(&ev->stats);
                    env_define(ev->env, stmt->as.destructure.names[i], elem);
                }

                /* Bind rest */
                if (has_rest) {
                    size_t rest_count = arr_len - name_count;
                    LatValue *rest_elems = malloc(rest_count * sizeof(LatValue));
                    for (size_t i = 0; i < rest_count; i++)
                        rest_elems[i] = value_deep_clone(&vr.value.as.array.elems[name_count + i]);
                    LatValue rest_arr = value_array(rest_elems, rest_count);
                    free(rest_elems);
                    if (stmt->as.destructure.phase == PHASE_FLUID)
                        rest_arr.phase = VTAG_FLUID;
                    else if (stmt->as.destructure.phase == PHASE_CRYSTAL) {
                        stats_freeze(&ev->stats);
                        rest_arr = value_freeze(rest_arr);
                        freeze_to_region(ev, &rest_arr);
                    }
                    stats_binding(&ev->stats);
                    env_define(ev->env, stmt->as.destructure.rest_name, rest_arr);
                }
                value_free(&vr.value);

            } else {
                /* DESTRUCT_STRUCT */
                if (vr.value.type != VAL_STRUCT && vr.value.type != VAL_MAP) {
                    char *err = NULL;
                    (void)asprintf(&err, "cannot destructure %s as struct",
                                   value_type_name(&vr.value));
                    value_free(&vr.value);
                    return eval_err(err);
                }

                for (size_t i = 0; i < stmt->as.destructure.name_count; i++) {
                    const char *fname = stmt->as.destructure.names[i];
                    LatValue elem = value_unit();
                    bool found = false;

                    if (vr.value.type == VAL_STRUCT) {
                        for (size_t j = 0; j < vr.value.as.strct.field_count; j++) {
                            if (vr.value.as.strct.field_names[j] == intern(fname)) {
                                elem = value_deep_clone(&vr.value.as.strct.field_values[j]);
                                found = true;
                                break;
                            }
                        }
                    } else {
                        /* VAL_MAP */
                        LatValue *mval = lat_map_get(vr.value.as.map.map, fname);
                        if (mval) {
                            elem = value_deep_clone(mval);
                            found = true;
                        }
                    }

                    if (!found) {
                        char *err = NULL;
                        (void)asprintf(&err, "destructure: field '%s' not found", fname);
                        value_free(&vr.value);
                        return eval_err(err);
                    }

                    if (stmt->as.destructure.phase == PHASE_FLUID)
                        elem.phase = VTAG_FLUID;
                    else if (stmt->as.destructure.phase == PHASE_CRYSTAL) {
                        stats_freeze(&ev->stats);
                        elem = value_freeze(elem);
                        freeze_to_region(ev, &elem);
                    }
                    stats_binding(&ev->stats);
                    env_define(ev->env, fname, elem);
                }
                value_free(&vr.value);
            }
            return eval_ok(value_unit());
        }

        case STMT_IMPORT: {
            const char *path = stmt->as.import.module_path;
            const char *alias = stmt->as.import.alias;
            char **selective = stmt->as.import.selective_names;
            size_t sel_count = stmt->as.import.selective_count;

            EvalResult mod_r = load_module(ev, path);
            if (!IS_OK(mod_r)) return mod_r;

            LatValue module_map = mod_r.value;

            /* Selective import: import { x, y } from "path" */
            if (selective) {
                for (size_t i = 0; i < sel_count; i++) {
                    const char *name = selective[i];
                    LatValue *exported = (LatValue *)lat_map_get(module_map.as.map.map, name);
                    if (!exported) {
                        char *err = NULL;
                        (void)asprintf(&err, "module '%s' does not export '%s'", path, name);
                        value_free(&module_map);
                        return eval_err(err);
                    }
                    env_define(ev->env, name, value_deep_clone(exported));
                }
                value_free(&module_map);
                return eval_ok(value_unit());
            }

            /* Full import: import "path" as name */
            if (!alias) {
                value_free(&module_map);
                return eval_err(strdup("import requires 'as <name>' or selective '{ ... } from'"));
            }

            env_define(ev->env, alias, module_map);
            return eval_ok(value_unit());
        }
    }
    return eval_err(strdup("unknown statement type"));
}

/* Run deferred blocks for the current scope depth (LIFO) */
static EvalResult run_defers_for_scope(Evaluator *ev, size_t scope_depth) {
    EvalResult first_err = { .ok = true, .error = NULL };
    first_err.value = value_unit();
    first_err.cf.tag = CF_NONE;
    while (ev->defer_count > 0 && ev->defer_stack[ev->defer_count - 1].scope_depth >= scope_depth) {
        DeferEntry de = ev->defer_stack[--ev->defer_count];
        EvalResult dr = eval_block_stmts(ev, de.body, de.body_count);
        if (!IS_OK(dr) && first_err.ok) {
            first_err = dr;
        } else if (!IS_OK(dr)) {
            if (IS_ERR(dr)) free(dr.error);
        }
    }
    return first_err;
}

static EvalResult eval_block_stmts(Evaluator *ev, Stmt **stmts, size_t count) {
    size_t defer_base = ev->defer_count;
    size_t scope_depth = ev->stats.current_scope_depth;
    LatValue last = value_unit();
    GC_PUSH(ev, &last);
    for (size_t i = 0; i < count; i++) {
        gc_maybe_collect(ev);
        value_free(&last);
        EvalResult r = eval_stmt(ev, stmts[i]);
        if (!IS_OK(r)) {
            /* Run defers before propagating error/signal */
            EvalResult dr = run_defers_for_scope(ev, scope_depth);
            GC_POP(ev);
            if (!dr.ok && r.ok) return dr;
            if (!dr.ok && IS_ERR(dr)) free(dr.error);
            return r;
        }
        last = r.value;
    }
    /* Run defers on normal exit */
    EvalResult dr = run_defers_for_scope(ev, scope_depth);
    (void)defer_base;
    GC_POP(ev);
    if (!dr.ok) { value_free(&last); return dr; }
    return eval_ok(last);
}

/* ── Method calls ── */

static EvalResult eval_method_call(Evaluator *ev, LatValue obj, const char *method,
                                   LatValue *args, size_t arg_count) {
    /* ── Enum methods ── */
    if (obj.type == VAL_ENUM) {
        if (strcmp(method, "variant_name") == 0 || strcmp(method, "tag") == 0) {
            if (arg_count != 0) return eval_err(strdup("variant_name() takes no arguments"));
            return eval_ok(value_string(obj.as.enm.variant_name));
        }
        if (strcmp(method, "enum_name") == 0) {
            if (arg_count != 0) return eval_err(strdup("enum_name() takes no arguments"));
            return eval_ok(value_string(obj.as.enm.enum_name));
        }
        if (strcmp(method, "is_variant") == 0) {
            if (arg_count != 1)
                return eval_err(strdup("is_variant() expects 1 argument"));
            if (args[0].type != VAL_STR)
                return eval_err(strdup("is_variant() expects a String argument"));
            bool match = (strcmp(obj.as.enm.variant_name, args[0].as.str_val) == 0);
            return eval_ok(value_bool(match));
        }
        if (strcmp(method, "payload") == 0) {
            if (arg_count != 0) return eval_err(strdup("payload() takes no arguments"));
            LatValue r;
            if (obj.as.enm.payload_count > 0) {
                LatValue *elems = malloc(obj.as.enm.payload_count * sizeof(LatValue));
                for (size_t i = 0; i < obj.as.enm.payload_count; i++)
                    elems[i] = value_deep_clone(&obj.as.enm.payload[i]);
                r = value_array(elems, obj.as.enm.payload_count);
                free(elems);
            } else {
                r = value_array(NULL, 0);
            }
            return eval_ok(r);
        }
        char *err2 = NULL;
        const char *esug = builtin_find_similar_method(VAL_ENUM, method);
        if (esug)
            (void)asprintf(&err2, "Enum has no method '%s' (did you mean '%s'?)", method, esug);
        else
            (void)asprintf(&err2, "Enum has no method '%s'", method);
        return eval_err(err2);
    }

    /* ── Set methods ── */
    if (obj.type == VAL_SET) {
        /// @method Set.has(value: Any) -> Bool
        /// @category Set Methods
        /// Check if the set contains the value.
        /// @example s.has(42)
        if (strcmp(method, "has") == 0) {
            if (arg_count != 1) return eval_err(strdup(".has() expects 1 argument"));
            char *key = value_display(&args[0]);
            bool result = lat_map_contains(obj.as.set.map, key);
            free(key);
            return eval_ok(value_bool(result));
        }
        /// @method Set.len() -> Int
        /// @category Set Methods
        /// Return the number of elements in the set. Also available as .length().
        /// @example s.len()  // 3
        if (strcmp(method, "len") == 0 || strcmp(method, "length") == 0) {
            if (arg_count != 0) return eval_err(strdup(".len() takes no arguments"));
            return eval_ok(value_int((int64_t)lat_map_len(obj.as.set.map)));
        }
        /// @method Set.to_array() -> Array
        /// @category Set Methods
        /// Convert the set to an array of its elements.
        /// @example s.to_array()
        if (strcmp(method, "to_array") == 0) {
            if (arg_count != 0) return eval_err(strdup(".to_array() takes no arguments"));
            size_t n = lat_map_len(obj.as.set.map);
            LatValue *elems = malloc((n > 0 ? n : 1) * sizeof(LatValue));
            size_t ei = 0;
            for (size_t i = 0; i < obj.as.set.map->cap; i++) {
                if (obj.as.set.map->entries[i].state != MAP_OCCUPIED) continue;
                LatValue *sv = (LatValue *)obj.as.set.map->entries[i].value;
                elems[ei++] = value_deep_clone(sv);
            }
            LatValue arr = value_array(elems, ei);
            free(elems);
            return eval_ok(arr);
        }
        /// @method Set.union(other: Set) -> Set
        /// @category Set Methods
        /// Return a new set containing all elements from both sets.
        /// @example s1.union(s2)
        if (strcmp(method, "union") == 0) {
            if (arg_count != 1 || args[0].type != VAL_SET)
                return eval_err(strdup(".union() expects 1 Set argument"));
            LatValue result = value_set_new();
            for (size_t i = 0; i < obj.as.set.map->cap; i++) {
                if (obj.as.set.map->entries[i].state == MAP_OCCUPIED) {
                    LatValue *sv = (LatValue *)obj.as.set.map->entries[i].value;
                    LatValue cloned = value_deep_clone(sv);
                    lat_map_set(result.as.set.map, obj.as.set.map->entries[i].key, &cloned);
                }
            }
            for (size_t i = 0; i < args[0].as.set.map->cap; i++) {
                if (args[0].as.set.map->entries[i].state == MAP_OCCUPIED) {
                    if (!lat_map_contains(result.as.set.map, args[0].as.set.map->entries[i].key)) {
                        LatValue *sv = (LatValue *)args[0].as.set.map->entries[i].value;
                        LatValue cloned = value_deep_clone(sv);
                        lat_map_set(result.as.set.map, args[0].as.set.map->entries[i].key, &cloned);
                    }
                }
            }
            return eval_ok(result);
        }
        /// @method Set.intersection(other: Set) -> Set
        /// @category Set Methods
        /// Return a new set containing only elements in both sets.
        /// @example s1.intersection(s2)
        if (strcmp(method, "intersection") == 0) {
            if (arg_count != 1 || args[0].type != VAL_SET)
                return eval_err(strdup(".intersection() expects 1 Set argument"));
            LatValue result = value_set_new();
            for (size_t i = 0; i < obj.as.set.map->cap; i++) {
                if (obj.as.set.map->entries[i].state == MAP_OCCUPIED) {
                    const char *key = obj.as.set.map->entries[i].key;
                    if (lat_map_contains(args[0].as.set.map, key)) {
                        LatValue *sv = (LatValue *)obj.as.set.map->entries[i].value;
                        LatValue cloned = value_deep_clone(sv);
                        lat_map_set(result.as.set.map, key, &cloned);
                    }
                }
            }
            return eval_ok(result);
        }
        /// @method Set.difference(other: Set) -> Set
        /// @category Set Methods
        /// Return a new set with elements in this set but not in other.
        /// @example s1.difference(s2)
        if (strcmp(method, "difference") == 0) {
            if (arg_count != 1 || args[0].type != VAL_SET)
                return eval_err(strdup(".difference() expects 1 Set argument"));
            LatValue result = value_set_new();
            for (size_t i = 0; i < obj.as.set.map->cap; i++) {
                if (obj.as.set.map->entries[i].state == MAP_OCCUPIED) {
                    const char *key = obj.as.set.map->entries[i].key;
                    if (!lat_map_contains(args[0].as.set.map, key)) {
                        LatValue *sv = (LatValue *)obj.as.set.map->entries[i].value;
                        LatValue cloned = value_deep_clone(sv);
                        lat_map_set(result.as.set.map, key, &cloned);
                    }
                }
            }
            return eval_ok(result);
        }
        /// @method Set.is_subset(other: Set) -> Bool
        /// @category Set Methods
        /// Check if this set is a subset of other.
        /// @example s1.is_subset(s2)
        if (strcmp(method, "is_subset") == 0) {
            if (arg_count != 1 || args[0].type != VAL_SET)
                return eval_err(strdup(".is_subset() expects 1 Set argument"));
            for (size_t i = 0; i < obj.as.set.map->cap; i++) {
                if (obj.as.set.map->entries[i].state == MAP_OCCUPIED) {
                    if (!lat_map_contains(args[0].as.set.map, obj.as.set.map->entries[i].key))
                        return eval_ok(value_bool(false));
                }
            }
            return eval_ok(value_bool(true));
        }
        /// @method Set.is_superset(other: Set) -> Bool
        /// @category Set Methods
        /// Check if this set is a superset of other.
        /// @example s1.is_superset(s2)
        if (strcmp(method, "is_superset") == 0) {
            if (arg_count != 1 || args[0].type != VAL_SET)
                return eval_err(strdup(".is_superset() expects 1 Set argument"));
            for (size_t i = 0; i < args[0].as.set.map->cap; i++) {
                if (args[0].as.set.map->entries[i].state == MAP_OCCUPIED) {
                    if (!lat_map_contains(obj.as.set.map, args[0].as.set.map->entries[i].key))
                        return eval_ok(value_bool(false));
                }
            }
            return eval_ok(value_bool(true));
        }
        char *err2 = NULL;
        const char *ssug = builtin_find_similar_method(VAL_SET, method);
        if (ssug)
            (void)asprintf(&err2, "Set has no method '%s' (did you mean '%s'?)", method, ssug);
        else
            (void)asprintf(&err2, "Set has no method '%s'", method);
        return eval_err(err2);
    }

    /* ── Buffer methods ── */
    if (obj.type == VAL_BUFFER) {
        /// @method Buffer.len() -> Int
        /// @category Buffer Methods
        /// Return the number of bytes in the buffer. Also available as .length().
        /// @example buf.len()  // 16
        if (strcmp(method, "len") == 0 || strcmp(method, "length") == 0) {
            if (arg_count != 0) return eval_err(strdup(".len() takes no arguments"));
            return eval_ok(value_int((int64_t)obj.as.buffer.len));
        }
        /// @method Buffer.capacity() -> Int
        /// @category Buffer Methods
        /// Return the current capacity of the buffer.
        /// @example buf.capacity()
        if (strcmp(method, "capacity") == 0) {
            if (arg_count != 0) return eval_err(strdup(".capacity() takes no arguments"));
            return eval_ok(value_int((int64_t)obj.as.buffer.cap));
        }
        /// @method Buffer.push(byte: Int) -> Unit
        /// @category Buffer Methods
        /// Append a single byte (0-255) to the buffer.
        /// @example buf.push(0x42)
        if (strcmp(method, "push") == 0) {
            if (arg_count != 1) return eval_err(strdup("Buffer.push() expects 1 argument"));
            /* Note: in tree-walker, push on copy doesn't mutate original */
            return eval_ok(value_unit());
        }
        /// @method Buffer.push_u16(val: Int) -> Unit
        /// @category Buffer Methods
        /// Append a 16-bit value as 2 bytes (little-endian).
        /// @example buf.push_u16(0x1234)
        if (strcmp(method, "push_u16") == 0) {
            return eval_ok(value_unit());
        }
        /// @method Buffer.push_u32(val: Int) -> Unit
        /// @category Buffer Methods
        /// Append a 32-bit value as 4 bytes (little-endian).
        /// @example buf.push_u32(0x12345678)
        if (strcmp(method, "push_u32") == 0) {
            return eval_ok(value_unit());
        }
        /// @method Buffer.read_u8(idx: Int) -> Int
        /// @category Buffer Methods
        /// Read a single byte at the given index.
        /// @example buf.read_u8(0)
        if (strcmp(method, "read_u8") == 0) {
            if (arg_count != 1 || args[0].type != VAL_INT) return eval_err(strdup("Buffer.read_u8() expects 1 Int argument"));
            size_t i = (size_t)args[0].as.int_val;
            if (i >= obj.as.buffer.len) return eval_err(strdup("Buffer.read_u8: index out of bounds"));
            return eval_ok(value_int(obj.as.buffer.data[i]));
        }
        /// @method Buffer.write_u8(idx: Int, val: Int) -> Unit
        /// @category Buffer Methods
        /// Write a single byte at the given index.
        /// @example buf.write_u8(0, 42)
        if (strcmp(method, "write_u8") == 0) {
            return eval_ok(value_unit());
        }
        /// @method Buffer.read_u16(idx: Int) -> Int
        /// @category Buffer Methods
        /// Read a 16-bit value (little-endian) at the given index.
        /// @example buf.read_u16(0)
        if (strcmp(method, "read_u16") == 0) {
            if (arg_count != 1 || args[0].type != VAL_INT) return eval_err(strdup("Buffer.read_u16() expects 1 Int argument"));
            size_t i = (size_t)args[0].as.int_val;
            if (i + 2 > obj.as.buffer.len) return eval_err(strdup("Buffer.read_u16: index out of bounds"));
            uint16_t v = (uint16_t)(obj.as.buffer.data[i] | (obj.as.buffer.data[i+1] << 8));
            return eval_ok(value_int(v));
        }
        /// @method Buffer.write_u16(idx: Int, val: Int) -> Unit
        /// @category Buffer Methods
        /// Write a 16-bit value (little-endian) at the given index.
        /// @example buf.write_u16(0, 0x1234)
        if (strcmp(method, "write_u16") == 0) {
            return eval_ok(value_unit());
        }
        /// @method Buffer.read_u32(idx: Int) -> Int
        /// @category Buffer Methods
        /// Read a 32-bit value (little-endian) at the given index.
        /// @example buf.read_u32(0)
        if (strcmp(method, "read_u32") == 0) {
            if (arg_count != 1 || args[0].type != VAL_INT) return eval_err(strdup("Buffer.read_u32() expects 1 Int argument"));
            size_t i = (size_t)args[0].as.int_val;
            if (i + 4 > obj.as.buffer.len) return eval_err(strdup("Buffer.read_u32: index out of bounds"));
            uint32_t v = (uint32_t)obj.as.buffer.data[i]
                       | ((uint32_t)obj.as.buffer.data[i+1] << 8)
                       | ((uint32_t)obj.as.buffer.data[i+2] << 16)
                       | ((uint32_t)obj.as.buffer.data[i+3] << 24);
            return eval_ok(value_int((int64_t)v));
        }
        /// @method Buffer.write_u32(idx: Int, val: Int) -> Unit
        /// @category Buffer Methods
        /// Write a 32-bit value (little-endian) at the given index.
        /// @example buf.write_u32(0, 0x12345678)
        if (strcmp(method, "write_u32") == 0) {
            return eval_ok(value_unit());
        }
        /// @method Buffer.read_i8(idx: Int) -> Int
        /// @category Buffer Methods
        /// Read a signed 8-bit integer at the given index.
        if (strcmp(method, "read_i8") == 0) {
            if (arg_count != 1 || args[0].type != VAL_INT) return eval_err(strdup("Buffer.read_i8() expects 1 Int argument"));
            size_t i = (size_t)args[0].as.int_val;
            if (i >= obj.as.buffer.len) return eval_err(strdup("Buffer.read_i8: index out of bounds"));
            return eval_ok(value_int((int8_t)obj.as.buffer.data[i]));
        }
        /// @method Buffer.read_i16(idx: Int) -> Int
        /// @category Buffer Methods
        /// Read a signed 16-bit integer (little-endian) at the given index.
        if (strcmp(method, "read_i16") == 0) {
            if (arg_count != 1 || args[0].type != VAL_INT) return eval_err(strdup("Buffer.read_i16() expects 1 Int argument"));
            size_t i = (size_t)args[0].as.int_val;
            if (i + 2 > obj.as.buffer.len) return eval_err(strdup("Buffer.read_i16: index out of bounds"));
            int16_t v;
            memcpy(&v, obj.as.buffer.data + i, 2);
            return eval_ok(value_int(v));
        }
        /// @method Buffer.read_i32(idx: Int) -> Int
        /// @category Buffer Methods
        /// Read a signed 32-bit integer (little-endian) at the given index.
        if (strcmp(method, "read_i32") == 0) {
            if (arg_count != 1 || args[0].type != VAL_INT) return eval_err(strdup("Buffer.read_i32() expects 1 Int argument"));
            size_t i = (size_t)args[0].as.int_val;
            if (i + 4 > obj.as.buffer.len) return eval_err(strdup("Buffer.read_i32: index out of bounds"));
            int32_t v;
            memcpy(&v, obj.as.buffer.data + i, 4);
            return eval_ok(value_int(v));
        }
        /// @method Buffer.read_f32(idx: Int) -> Float
        /// @category Buffer Methods
        /// Read a 32-bit float (little-endian) at the given index.
        if (strcmp(method, "read_f32") == 0) {
            if (arg_count != 1 || args[0].type != VAL_INT) return eval_err(strdup("Buffer.read_f32() expects 1 Int argument"));
            size_t i = (size_t)args[0].as.int_val;
            if (i + 4 > obj.as.buffer.len) return eval_err(strdup("Buffer.read_f32: index out of bounds"));
            float v;
            memcpy(&v, obj.as.buffer.data + i, 4);
            return eval_ok(value_float((double)v));
        }
        /// @method Buffer.read_f64(idx: Int) -> Float
        /// @category Buffer Methods
        /// Read a 64-bit double (little-endian) at the given index.
        if (strcmp(method, "read_f64") == 0) {
            if (arg_count != 1 || args[0].type != VAL_INT) return eval_err(strdup("Buffer.read_f64() expects 1 Int argument"));
            size_t i = (size_t)args[0].as.int_val;
            if (i + 8 > obj.as.buffer.len) return eval_err(strdup("Buffer.read_f64: index out of bounds"));
            double v;
            memcpy(&v, obj.as.buffer.data + i, 8);
            return eval_ok(value_float(v));
        }
        /// @method Buffer.slice(start: Int, end: Int) -> Buffer
        /// @category Buffer Methods
        /// Return a new buffer containing bytes from start (inclusive) to end (exclusive).
        /// @example buf.slice(0, 4)
        if (strcmp(method, "slice") == 0) {
            if (arg_count != 2) return eval_err(strdup("Buffer.slice() expects 2 arguments"));
            if (args[0].type != VAL_INT || args[1].type != VAL_INT) return eval_err(strdup("Buffer.slice() expects Int arguments"));
            int64_t s = args[0].as.int_val, e = args[1].as.int_val;
            if (s < 0) s = 0;
            if (e > (int64_t)obj.as.buffer.len) e = (int64_t)obj.as.buffer.len;
            if (s >= e) return eval_ok(value_buffer(NULL, 0));
            return eval_ok(value_buffer(obj.as.buffer.data + s, (size_t)(e - s)));
        }
        /// @method Buffer.clear() -> Unit
        /// @category Buffer Methods
        /// Set the buffer length to 0 (capacity unchanged).
        /// @example buf.clear()
        if (strcmp(method, "clear") == 0) {
            return eval_ok(value_unit());
        }
        /// @method Buffer.fill(byte: Int) -> Unit
        /// @category Buffer Methods
        /// Fill all bytes in the buffer with the given value.
        /// @example buf.fill(0)
        if (strcmp(method, "fill") == 0) {
            return eval_ok(value_unit());
        }
        /// @method Buffer.resize(new_len: Int) -> Unit
        /// @category Buffer Methods
        /// Change the buffer length. New bytes are zero-filled.
        /// @example buf.resize(32)
        if (strcmp(method, "resize") == 0) {
            return eval_ok(value_unit());
        }
        /// @method Buffer.to_string() -> String
        /// @category Buffer Methods
        /// Interpret the buffer contents as a UTF-8 string.
        /// @example Buffer::from_string("hi").to_string()  // "hi"
        if (strcmp(method, "to_string") == 0) {
            if (arg_count != 0) return eval_err(strdup(".to_string() takes no arguments"));
            char *s = malloc(obj.as.buffer.len + 1);
            memcpy(s, obj.as.buffer.data, obj.as.buffer.len);
            s[obj.as.buffer.len] = '\0';
            return eval_ok(value_string_owned(s));
        }
        /// @method Buffer.to_array() -> Array
        /// @category Buffer Methods
        /// Convert the buffer to an array of integers (0-255).
        /// @example buf.to_array()
        if (strcmp(method, "to_array") == 0) {
            if (arg_count != 0) return eval_err(strdup(".to_array() takes no arguments"));
            size_t blen = obj.as.buffer.len;
            LatValue *elems = malloc((blen > 0 ? blen : 1) * sizeof(LatValue));
            for (size_t i = 0; i < blen; i++)
                elems[i] = value_int(obj.as.buffer.data[i]);
            LatValue arr = value_array(elems, blen);
            free(elems);
            return eval_ok(arr);
        }
        /// @method Buffer.to_hex() -> String
        /// @category Buffer Methods
        /// Convert the buffer contents to a hexadecimal string.
        /// @example Buffer::from([0x48, 0x69]).to_hex()  // "4869"
        if (strcmp(method, "to_hex") == 0) {
            if (arg_count != 0) return eval_err(strdup(".to_hex() takes no arguments"));
            size_t blen = obj.as.buffer.len;
            char *hex = malloc(blen * 2 + 1);
            for (size_t i = 0; i < blen; i++)
                snprintf(hex + i * 2, 3, "%02x", obj.as.buffer.data[i]);
            hex[blen * 2] = '\0';
            return eval_ok(value_string_owned(hex));
        }
        char *berr2 = NULL;
        const char *bsug = builtin_find_similar_method(VAL_BUFFER, method);
        if (bsug)
            (void)asprintf(&berr2, "Buffer has no method '%s' (did you mean '%s'?)", method, bsug);
        else
            (void)asprintf(&berr2, "Buffer has no method '%s'", method);
        return eval_err(berr2);
    }

    /// @method Array.push(val: Any) -> Unit
    /// @category Array Methods
    /// Append a value to the end of the array (mutates in place).
    /// @example arr.push(42)
    if (strcmp(method, "push") == 0) {
        if (obj.type != VAL_ARRAY)
            return eval_err(strdup(".push() is not defined on non-array"));
        if (value_is_crystal(&obj))
            return eval_err(strdup("cannot push to a crystal array"));
        if (arg_count != 1)
            return eval_err(strdup(".push() expects exactly 1 argument"));
        /* We need to push to the actual array in the env. The obj here is already freed copy.
           For method calls that mutate, we'd need the original. Instead, let's not support
           standalone push -- in the Rust version it uses Rc<RefCell> for shared mutation.
           For C, we'll handle array.push by finding the original binding. */
        /* This is a simplification - we handle push on the env directly */
        return eval_ok(value_unit());
    }
    /// @method Array.len() -> Int
    /// @method String.len() -> Int
    /// @method Map.len() -> Int
    /// @category Array Methods
    /// Return the number of elements or characters. Also available as .length().
    /// @example [1, 2, 3].len()  // 3
    /// @example "hello".length()  // 5
    if (strcmp(method, "len") == 0 || strcmp(method, "length") == 0) {
        if (obj.type == VAL_ARRAY) return eval_ok(value_int((int64_t)obj.as.array.len));
        if (obj.type == VAL_STR) return eval_ok(value_int((int64_t)strlen(obj.as.str_val)));
        if (obj.type == VAL_MAP) return eval_ok(value_int((int64_t)lat_map_len(obj.as.map.map)));
        if (obj.type == VAL_TUPLE) return eval_ok(value_int((int64_t)obj.as.tuple.len));
        if (obj.type == VAL_BUFFER) return eval_ok(value_int((int64_t)obj.as.buffer.len));
        if (obj.type == VAL_REF) {
            LatValue *inner = &obj.as.ref.ref->value;
            if (inner->type == VAL_ARRAY) return eval_ok(value_int((int64_t)inner->as.array.len));
            if (inner->type == VAL_STR) return eval_ok(value_int((int64_t)strlen(inner->as.str_val)));
            if (inner->type == VAL_MAP) return eval_ok(value_int((int64_t)lat_map_len(inner->as.map.map)));
            if (inner->type == VAL_BUFFER) return eval_ok(value_int((int64_t)inner->as.buffer.len));
        }
        return eval_err(strdup(".len()/.length() is not defined on this type"));
    }
    /// @method Array.map(fn: Closure) -> Array
    /// @category Array Methods
    /// Apply a function to each element, returning a new array of results.
    /// @example [1, 2, 3].map(|x| { x * 2 })  // [2, 4, 6]
    if (strcmp(method, "map") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 1) return eval_err(strdup(".map() expects exactly 1 argument (a closure)"));
        if (args[0].type != VAL_CLOSURE) return eval_err(strdup(".map() argument must be a closure"));

        size_t n = obj.as.array.len;
        LatValue *results = malloc(n * sizeof(LatValue));
        for (size_t i = 0; i < n; i++) {
            LatValue elem = value_deep_clone(&obj.as.array.elems[i]);
            EvalResult r = call_closure(ev,
                args[0].as.closure.param_names,
                args[0].as.closure.param_count,
                args[0].as.closure.body,
                args[0].as.closure.captured_env,
                &elem, 1,
                args[0].as.closure.default_values, args[0].as.closure.has_variadic);
            if (!IS_OK(r)) {
                GC_POP_N(ev, i);  /* accumulated results */
                for (size_t j = 0; j < i; j++) value_free(&results[j]);
                free(results);
                return r;
            }
            results[i] = r.value;
            GC_PUSH(ev, &results[i]);
        }
        GC_POP_N(ev, n);  /* accumulated results */
        LatValue arr = value_array(results, n);
        free(results);
        return eval_ok(arr);
    }
    /// @method Array.join(sep?: String) -> String
    /// @category Array Methods
    /// Join array elements into a string with an optional separator.
    /// @example ["a", "b", "c"].join(", ")  // "a, b, c"
    if (strcmp(method, "join") == 0) {
        if (obj.type != VAL_ARRAY) return eval_err(strdup(".join() is not defined on non-array"));
        const char *sep = "";
        if (arg_count > 0) {
            if (args[0].type != VAL_STR) return eval_err(strdup(".join() separator must be a string"));
            sep = args[0].as.str_val;
        }
        size_t total = 0;
        char **parts = malloc(obj.as.array.len * sizeof(char *));
        for (size_t i = 0; i < obj.as.array.len; i++) {
            parts[i] = value_display(&obj.as.array.elems[i]);
            total += strlen(parts[i]);
        }
        size_t sep_len = strlen(sep);
        if (obj.as.array.len > 0) total += sep_len * (obj.as.array.len - 1);
        char *result = malloc(total + 1);
        size_t pos = 0;
        for (size_t i = 0; i < obj.as.array.len; i++) {
            if (i > 0) { memcpy(result + pos, sep, sep_len); pos += sep_len; }
            size_t pl = strlen(parts[i]);
            memcpy(result + pos, parts[i], pl);
            pos += pl;
            free(parts[i]);
        }
        result[pos] = '\0';
        free(parts);
        return eval_ok(value_string_owned(result));
    }
    /// @method Array.filter(fn: Closure) -> Array
    /// @category Array Methods
    /// Return a new array containing only elements for which fn returns true.
    /// @example [1, 2, 3, 4].filter(|x| { x > 2 })  // [3, 4]
    /* ── Array: filter ── */
    if (strcmp(method, "filter") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 1 || args[0].type != VAL_CLOSURE) return eval_err(strdup(".filter() expects 1 closure argument"));
        size_t n = obj.as.array.len;
        LatValue *results = malloc((n > 0 ? n : 1) * sizeof(LatValue));
        size_t rcount = 0;
        for (size_t i = 0; i < n; i++) {
            LatValue elem = value_deep_clone(&obj.as.array.elems[i]);
            EvalResult r = call_closure(ev,
                args[0].as.closure.param_names,
                args[0].as.closure.param_count,
                args[0].as.closure.body,
                args[0].as.closure.captured_env,
                &elem, 1,
                args[0].as.closure.default_values, args[0].as.closure.has_variadic);
            if (!IS_OK(r)) {
                GC_POP_N(ev, rcount);  /* accumulated results */
                for (size_t j = 0; j < rcount; j++) value_free(&results[j]);
                free(results);
                return r;
            }
            if (value_is_truthy(&r.value)) {
                results[rcount++] = value_deep_clone(&obj.as.array.elems[i]);
                GC_PUSH(ev, &results[rcount - 1]);
            }
            value_free(&r.value);
        }
        GC_POP_N(ev, rcount);  /* accumulated results */
        LatValue arr = value_array(results, rcount);
        free(results);
        return eval_ok(arr);
    }
    /// @method Array.for_each(fn: Closure) -> Unit
    /// @category Array Methods
    /// Call a function for each element (for side effects).
    /// @example [1, 2, 3].for_each(|x| { print(x) })
    /* ── Array: for_each ── */
    if (strcmp(method, "for_each") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 1 || args[0].type != VAL_CLOSURE) return eval_err(strdup(".for_each() expects 1 closure argument"));
        for (size_t i = 0; i < obj.as.array.len; i++) {
            LatValue elem = value_deep_clone(&obj.as.array.elems[i]);
            EvalResult r = call_closure(ev,
                args[0].as.closure.param_names,
                args[0].as.closure.param_count,
                args[0].as.closure.body,
                args[0].as.closure.captured_env,
                &elem, 1,
                args[0].as.closure.default_values, args[0].as.closure.has_variadic);
            if (!IS_OK(r)) return r;
            value_free(&r.value);
        }
        return eval_ok(value_unit());
    }
    /// @method Array.find(fn: Closure) -> Any|Unit
    /// @category Array Methods
    /// Return the first element for which fn returns true, or unit if not found.
    /// @example [1, 2, 3].find(|x| { x > 1 })  // 2
    /* ── Array: find ── */
    if (strcmp(method, "find") == 0) {
        if (obj.type != VAL_ARRAY) return eval_err(strdup(".find() is not defined on non-array"));
        if (arg_count != 1 || args[0].type != VAL_CLOSURE) return eval_err(strdup(".find() expects 1 closure argument"));
        for (size_t i = 0; i < obj.as.array.len; i++) {
            LatValue elem = value_deep_clone(&obj.as.array.elems[i]);
            EvalResult r = call_closure(ev,
                args[0].as.closure.param_names,
                args[0].as.closure.param_count,
                args[0].as.closure.body,
                args[0].as.closure.captured_env,
                &elem, 1,
                args[0].as.closure.default_values, args[0].as.closure.has_variadic);
            if (!IS_OK(r)) return r;
            if (value_is_truthy(&r.value)) {
                value_free(&r.value);
                return eval_ok(value_deep_clone(&obj.as.array.elems[i]));
            }
            value_free(&r.value);
        }
        return eval_ok(value_unit());
    }
    /// @method Array.contains(val: Any) -> Bool
    /// @category Array Methods
    /// Check if the array contains a value.
    /// @example [1, 2, 3].contains(2)  // true
    /* ── Array: contains ── */
    if (strcmp(method, "contains") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 1) return eval_err(strdup(".contains() expects 1 argument"));
        for (size_t i = 0; i < obj.as.array.len; i++) {
            if (value_eq(&obj.as.array.elems[i], &args[0])) {
                return eval_ok(value_bool(true));
            }
        }
        return eval_ok(value_bool(false));
    }
    /// @method Array.reverse() -> Array
    /// @category Array Methods
    /// Return a new array with elements in reverse order.
    /// @example [1, 2, 3].reverse()  // [3, 2, 1]
    /* ── Array: reverse ── */
    if (strcmp(method, "reverse") == 0 && obj.type == VAL_ARRAY) {
        size_t n = obj.as.array.len;
        LatValue *reversed = malloc((n > 0 ? n : 1) * sizeof(LatValue));
        for (size_t i = 0; i < n; i++) {
            reversed[i] = value_deep_clone(&obj.as.array.elems[n - 1 - i]);
        }
        LatValue arr = value_array(reversed, n);
        free(reversed);
        return eval_ok(arr);
    }
    /// @method Array.enumerate() -> Array
    /// @category Array Methods
    /// Return an array of [index, value] pairs.
    /// @example ["a", "b"].enumerate()  // [[0, "a"], [1, "b"]]
    /* ── Array: enumerate ── */
    if (strcmp(method, "enumerate") == 0) {
        if (obj.type != VAL_ARRAY) return eval_err(strdup(".enumerate() is not defined on non-array"));
        size_t n = obj.as.array.len;
        LatValue *pairs = malloc((n > 0 ? n : 1) * sizeof(LatValue));
        for (size_t i = 0; i < n; i++) {
            LatValue pair_elems[2];
            pair_elems[0] = value_int((int64_t)i);
            pair_elems[1] = value_deep_clone(&obj.as.array.elems[i]);
            pairs[i] = value_array(pair_elems, 2);
        }
        LatValue arr = value_array(pairs, n);
        free(pairs);
        return eval_ok(arr);
    }
    /// @method Array.sort() -> Array
    /// @category Array Methods
    /// Return a new sorted array (elements must be comparable).
    /// @example [3, 1, 2].sort()  // [1, 2, 3]
    /* ── Array: sort ── */
    if (strcmp(method, "sort") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 0) return eval_err(strdup(".sort() takes no arguments"));
        char *sort_err = NULL;
        LatValue sorted = array_sort(&obj, &sort_err);
        if (sort_err) return eval_err(sort_err);
        return eval_ok(sorted);
    }
    /// @method Array.flat() -> Array
    /// @category Array Methods
    /// Flatten one level of nested arrays.
    /// @example [[1, 2], [3, 4]].flat()  // [1, 2, 3, 4]
    /* ── Array: flat ── */
    if (strcmp(method, "flat") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 0) return eval_err(strdup(".flat() takes no arguments"));
        return eval_ok(array_flat(&obj));
    }
    /// @method Array.reduce(fn: Closure, init: Any) -> Any
    /// @category Array Methods
    /// Reduce an array to a single value by applying fn(acc, elem) for each element.
    /// @example [1, 2, 3].reduce(|a, b| { a + b }, 0)  // 6
    /* ── Array: reduce ── */
    if (strcmp(method, "reduce") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 2) return eval_err(strdup(".reduce() expects 2 arguments (closure, initial_value)"));
        if (args[0].type != VAL_CLOSURE) return eval_err(strdup(".reduce() first argument must be a closure"));
        LatValue acc = value_deep_clone(&args[1]);
        GC_PUSH(ev, &acc);
        for (size_t i = 0; i < obj.as.array.len; i++) {
            LatValue call_args[2];
            call_args[0] = acc;
            call_args[1] = value_deep_clone(&obj.as.array.elems[i]);
            EvalResult r = call_closure(ev,
                args[0].as.closure.param_names,
                args[0].as.closure.param_count,
                args[0].as.closure.body,
                args[0].as.closure.captured_env,
                call_args, 2,
                args[0].as.closure.default_values, args[0].as.closure.has_variadic);
            if (!IS_OK(r)) { GC_POP(ev); return r; }
            acc = r.value;
        }
        GC_POP(ev);
        return eval_ok(acc);
    }
    /// @method Array.slice(start: Int, end: Int) -> Array
    /// @category Array Methods
    /// Return a sub-array from start (inclusive) to end (exclusive).
    /// @example [1, 2, 3, 4, 5].slice(1, 4)  // [2, 3, 4]
    /* ── Array: slice ── */
    if (strcmp(method, "slice") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 2) return eval_err(strdup(".slice() expects 2 arguments (start, end)"));
        if (args[0].type != VAL_INT || args[1].type != VAL_INT)
            return eval_err(strdup(".slice() arguments must be integers"));
        char *slice_err = NULL;
        LatValue sliced = array_slice(&obj, args[0].as.int_val, args[1].as.int_val, &slice_err);
        if (slice_err) return eval_err(slice_err);
        return eval_ok(sliced);
    }
    /// @method Array.take(n: Int) -> Array
    /// @category Array Methods
    /// Return the first n elements of the array.
    /// @example [1, 2, 3, 4].take(2)  // [1, 2]
    /* ── Array: take ── */
    if (strcmp(method, "take") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 1 || args[0].type != VAL_INT)
            return eval_err(strdup(".take() expects 1 integer argument"));
        int64_t n = args[0].as.int_val;
        if (n <= 0) {
            return eval_ok(value_array(NULL, 0));
        }
        size_t take_count = (size_t)n;
        if (take_count > obj.as.array.len) take_count = obj.as.array.len;
        LatValue *elems = malloc((take_count > 0 ? take_count : 1) * sizeof(LatValue));
        for (size_t i = 0; i < take_count; i++) {
            elems[i] = value_deep_clone(&obj.as.array.elems[i]);
        }
        LatValue arr = value_array(elems, take_count);
        free(elems);
        return eval_ok(arr);
    }
    /// @method Array.drop(n: Int) -> Array
    /// @category Array Methods
    /// Return the array with the first n elements removed.
    /// @example [1, 2, 3, 4].drop(2)  // [3, 4]
    /* ── Array: drop ── */
    if (strcmp(method, "drop") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 1 || args[0].type != VAL_INT)
            return eval_err(strdup(".drop() expects 1 integer argument"));
        int64_t n = args[0].as.int_val;
        if (n <= 0) {
            LatValue *elems = malloc((obj.as.array.len > 0 ? obj.as.array.len : 1) * sizeof(LatValue));
            for (size_t i = 0; i < obj.as.array.len; i++) {
                elems[i] = value_deep_clone(&obj.as.array.elems[i]);
            }
            LatValue arr = value_array(elems, obj.as.array.len);
            free(elems);
            return eval_ok(arr);
        }
        size_t start = (size_t)n;
        if (start >= obj.as.array.len) {
            return eval_ok(value_array(NULL, 0));
        }
        size_t drop_count = obj.as.array.len - start;
        LatValue *elems = malloc(drop_count * sizeof(LatValue));
        for (size_t i = 0; i < drop_count; i++) {
            elems[i] = value_deep_clone(&obj.as.array.elems[start + i]);
        }
        LatValue arr = value_array(elems, drop_count);
        free(elems);
        return eval_ok(arr);
    }
    /// @method Array.pop() -> Any
    /// @category Array Methods
    /// Remove and return the last element of the array.
    /// @example [1, 2, 3].pop()  // 3
    /* ── Array: pop ── */
    if (strcmp(method, "pop") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 0) return eval_err(strdup(".pop() takes no arguments"));
        if (obj.as.array.len == 0) return eval_err(strdup("pop on empty array"));
        LatValue removed = value_deep_clone(&obj.as.array.elems[obj.as.array.len - 1]);
        return eval_ok(removed);
    }
    /// @method Array.index_of(val: Any) -> Int
    /// @category Array Methods
    /// Return the index of the first occurrence of val, or -1 if not found.
    /// @example [10, 20, 30].index_of(20)  // 1
    /* ── Array: index_of ── */
    if (strcmp(method, "index_of") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 1) return eval_err(strdup(".index_of() expects 1 argument"));
        for (size_t i = 0; i < obj.as.array.len; i++) {
            if (value_eq(&obj.as.array.elems[i], &args[0])) {
                return eval_ok(value_int((int64_t)i));
            }
        }
        return eval_ok(value_int(-1));
    }
    /// @method Array.any(fn: Closure) -> Bool
    /// @category Array Methods
    /// Return true if fn returns true for any element.
    /// @example [1, 2, 3].any(|x| { x > 2 })  // true
    /* ── Array: any ── */
    if (strcmp(method, "any") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 1 || args[0].type != VAL_CLOSURE)
            return eval_err(strdup(".any() expects 1 closure argument"));
        for (size_t i = 0; i < obj.as.array.len; i++) {
            LatValue elem = value_deep_clone(&obj.as.array.elems[i]);
            EvalResult r = call_closure(ev,
                args[0].as.closure.param_names,
                args[0].as.closure.param_count,
                args[0].as.closure.body,
                args[0].as.closure.captured_env,
                &elem, 1,
                args[0].as.closure.default_values, args[0].as.closure.has_variadic);
            if (!IS_OK(r)) return r;
            if (value_is_truthy(&r.value)) {
                value_free(&r.value);
                return eval_ok(value_bool(true));
            }
            value_free(&r.value);
        }
        return eval_ok(value_bool(false));
    }
    /// @method Array.all(fn: Closure) -> Bool
    /// @category Array Methods
    /// Return true if fn returns true for all elements.
    /// @example [2, 4, 6].all(|x| { x % 2 == 0 })  // true
    /* ── Array: all ── */
    if (strcmp(method, "all") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 1 || args[0].type != VAL_CLOSURE)
            return eval_err(strdup(".all() expects 1 closure argument"));
        for (size_t i = 0; i < obj.as.array.len; i++) {
            LatValue elem = value_deep_clone(&obj.as.array.elems[i]);
            EvalResult r = call_closure(ev,
                args[0].as.closure.param_names,
                args[0].as.closure.param_count,
                args[0].as.closure.body,
                args[0].as.closure.captured_env,
                &elem, 1,
                args[0].as.closure.default_values, args[0].as.closure.has_variadic);
            if (!IS_OK(r)) return r;
            if (!value_is_truthy(&r.value)) {
                value_free(&r.value);
                return eval_ok(value_bool(false));
            }
            value_free(&r.value);
        }
        return eval_ok(value_bool(true));
    }
    /// @method Array.zip(other: Array) -> Array
    /// @category Array Methods
    /// Combine two arrays into an array of [a, b] pairs.
    /// @example [1, 2].zip(["a", "b"])  // [[1, "a"], [2, "b"]]
    /* ── Array: zip ── */
    if (strcmp(method, "zip") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 1) return eval_err(strdup(".zip() expects 1 argument"));
        if (args[0].type != VAL_ARRAY) return eval_err(strdup(".zip() argument must be an array"));
        size_t n = obj.as.array.len < args[0].as.array.len
                 ? obj.as.array.len : args[0].as.array.len;
        LatValue *pairs = malloc((n > 0 ? n : 1) * sizeof(LatValue));
        for (size_t i = 0; i < n; i++) {
            LatValue pair_elems[2];
            pair_elems[0] = value_deep_clone(&obj.as.array.elems[i]);
            pair_elems[1] = value_deep_clone(&args[0].as.array.elems[i]);
            pairs[i] = value_array(pair_elems, 2);
        }
        LatValue arr = value_array(pairs, n);
        free(pairs);
        return eval_ok(arr);
    }
    /// @method Array.unique() -> Array
    /// @category Array Methods
    /// Return a new array with duplicate elements removed.
    /// @example [1, 2, 2, 3, 1].unique()  // [1, 2, 3]
    /* ── Array: unique ── */
    if (strcmp(method, "unique") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 0) return eval_err(strdup(".unique() takes no arguments"));
        size_t n = obj.as.array.len;
        LatValue *results = malloc((n > 0 ? n : 1) * sizeof(LatValue));
        size_t rcount = 0;
        for (size_t i = 0; i < n; i++) {
            bool found = false;
            for (size_t j = 0; j < rcount; j++) {
                if (value_eq(&obj.as.array.elems[i], &results[j])) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                results[rcount++] = value_deep_clone(&obj.as.array.elems[i]);
            }
        }
        LatValue arr = value_array(results, rcount);
        free(results);
        return eval_ok(arr);
    }
    /// @method Array.insert(index: Int, val: Any) -> Unit
    /// @category Array Methods
    /// Insert a value at the given index (mutates in place).
    /// @example arr.insert(1, "x")
    /* ── Array: insert ── */
    if (strcmp(method, "insert") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 2) return eval_err(strdup(".insert() expects 2 arguments (index, value)"));
        if (args[0].type != VAL_INT) return eval_err(strdup(".insert() index must be an integer"));
        int64_t idx = args[0].as.int_val;
        size_t len = obj.as.array.len;
        if (idx < 0 || (size_t)idx > len) {
            char *err = NULL;
            (void)asprintf(&err, ".insert() index %lld out of bounds (length %zu)",
                           (long long)idx, len);
            return eval_err(err);
        }
        return eval_ok(value_unit());
    }
    /// @method Array.remove_at(index: Int) -> Any
    /// @category Array Methods
    /// Remove and return the element at the given index.
    /// @example [1, 2, 3].remove_at(1)  // 2
    /* ── Array: remove_at ── */
    if (strcmp(method, "remove_at") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 1) return eval_err(strdup(".remove_at() expects 1 argument (index)"));
        if (args[0].type != VAL_INT) return eval_err(strdup(".remove_at() index must be an integer"));
        int64_t idx = args[0].as.int_val;
        size_t len = obj.as.array.len;
        if (idx < 0 || (size_t)idx >= len) {
            char *err = NULL;
            (void)asprintf(&err, ".remove_at() index %lld out of bounds (length %zu)",
                           (long long)idx, len);
            return eval_err(err);
        }
        LatValue removed = value_deep_clone(&obj.as.array.elems[(size_t)idx]);
        return eval_ok(removed);
    }
    /// @method Array.sort_by(cmp: Closure) -> Array
    /// @category Array Methods
    /// Sort using a custom comparator that returns a negative, zero, or positive Int.
    /// @example ["bb", "a", "ccc"].sort_by(|a, b| { len(a) - len(b) })  // ["a", "bb", "ccc"]
    /* ── Array: sort_by ── */
    if (strcmp(method, "sort_by") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 1 || args[0].type != VAL_CLOSURE)
            return eval_err(strdup(".sort_by() expects 1 closure argument"));
        size_t n = obj.as.array.len;
        /* Deep-clone elements into a working buffer */
        LatValue *buf = malloc((n > 0 ? n : 1) * sizeof(LatValue));
        for (size_t i = 0; i < n; i++) {
            buf[i] = value_deep_clone(&obj.as.array.elems[i]);
        }
        /* Insertion sort using the closure as comparator */
        for (size_t i = 1; i < n; i++) {
            LatValue key = buf[i];
            size_t j = i;
            while (j > 0) {
                LatValue call_args[2];
                call_args[0] = value_deep_clone(&key);
                call_args[1] = value_deep_clone(&buf[j - 1]);
                EvalResult r = call_closure(ev,
                    args[0].as.closure.param_names,
                    args[0].as.closure.param_count,
                    args[0].as.closure.body,
                    args[0].as.closure.captured_env,
                    call_args, 2,
                    args[0].as.closure.default_values, args[0].as.closure.has_variadic);
                if (!IS_OK(r)) {
                    for (size_t k = 0; k < j; k++) value_free(&buf[k]);
                    value_free(&key);
                    for (size_t k = j + 1; k < n; k++) value_free(&buf[k]);
                    free(buf);
                    return r;
                }
                if (r.value.type != VAL_INT) {
                    value_free(&r.value);
                    for (size_t k = 0; k < j; k++) value_free(&buf[k]);
                    value_free(&key);
                    for (size_t k = j + 1; k < n; k++) value_free(&buf[k]);
                    free(buf);
                    return eval_err(strdup(".sort_by() comparator must return an Int"));
                }
                int64_t cmp = r.value.as.int_val;
                value_free(&r.value);
                if (cmp >= 0) break;
                buf[j] = buf[j - 1];
                j--;
            }
            buf[j] = key;
        }
        LatValue arr = value_array(buf, n);
        free(buf);
        return eval_ok(arr);
    }
    /// @method Array.flat_map(fn: Closure) -> Array
    /// @category Array Methods
    /// Map each element to an array, then flatten one level.
    /// @example [1, 2].flat_map(|x| { [x, x * 10] })  // [1, 10, 2, 20]
    /* ── Array: flat_map ── */
    if (strcmp(method, "flat_map") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 1 || args[0].type != VAL_CLOSURE)
            return eval_err(strdup(".flat_map() expects 1 closure argument"));
        size_t n = obj.as.array.len;
        LatValue *mapped = malloc((n > 0 ? n : 1) * sizeof(LatValue));
        for (size_t i = 0; i < n; i++) {
            LatValue elem = value_deep_clone(&obj.as.array.elems[i]);
            EvalResult r = call_closure(ev,
                args[0].as.closure.param_names,
                args[0].as.closure.param_count,
                args[0].as.closure.body,
                args[0].as.closure.captured_env,
                &elem, 1,
                args[0].as.closure.default_values, args[0].as.closure.has_variadic);
            if (!IS_OK(r)) {
                for (size_t j = 0; j < i; j++) value_free(&mapped[j]);
                free(mapped);
                return r;
            }
            mapped[i] = r.value;
        }
        size_t total = 0;
        for (size_t i = 0; i < n; i++) {
            if (mapped[i].type == VAL_ARRAY)
                total += mapped[i].as.array.len;
            else
                total += 1;
        }
        LatValue *fm_buf = malloc((total > 0 ? total : 1) * sizeof(LatValue));
        size_t fm_pos = 0;
        for (size_t i = 0; i < n; i++) {
            if (mapped[i].type == VAL_ARRAY) {
                for (size_t j = 0; j < mapped[i].as.array.len; j++) {
                    fm_buf[fm_pos++] = value_deep_clone(&mapped[i].as.array.elems[j]);
                }
            } else {
                fm_buf[fm_pos++] = value_deep_clone(&mapped[i]);
            }
        }
        for (size_t i = 0; i < n; i++) value_free(&mapped[i]);
        free(mapped);
        LatValue fm_arr = value_array(fm_buf, fm_pos);
        free(fm_buf);
        return eval_ok(fm_arr);
    }
    /// @method Array.chunk(size: Int) -> Array
    /// @category Array Methods
    /// Split the array into sub-arrays of the given size.
    /// @example [1, 2, 3, 4, 5].chunk(2)  // [[1, 2], [3, 4], [5]]
    /* ── Array: chunk ── */
    if (strcmp(method, "chunk") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 1 || args[0].type != VAL_INT)
            return eval_err(strdup(".chunk() expects 1 integer argument"));
        int64_t chunk_size = args[0].as.int_val;
        if (chunk_size <= 0)
            return eval_err(strdup(".chunk() size must be positive"));
        size_t n = obj.as.array.len;
        size_t num_chunks = (n > 0) ? (n + (size_t)chunk_size - 1) / (size_t)chunk_size : 0;
        LatValue *chunks = malloc((num_chunks > 0 ? num_chunks : 1) * sizeof(LatValue));
        for (size_t ci = 0; ci < num_chunks; ci++) {
            size_t cstart = ci * (size_t)chunk_size;
            size_t cend = cstart + (size_t)chunk_size;
            if (cend > n) cend = n;
            size_t clen = cend - cstart;
            LatValue *celems = malloc(clen * sizeof(LatValue));
            for (size_t j = 0; j < clen; j++) {
                celems[j] = value_deep_clone(&obj.as.array.elems[cstart + j]);
            }
            chunks[ci] = value_array(celems, clen);
            free(celems);
        }
        LatValue chunk_arr = value_array(chunks, num_chunks);
        free(chunks);
        return eval_ok(chunk_arr);
    }
    /// @method Array.group_by(fn: Closure) -> Map
    /// @category Array Methods
    /// Group elements by the result of fn, returning a map of key to arrays.
    /// @example [1, 2, 3, 4].group_by(|x| { x % 2 })  // {0: [2, 4], 1: [1, 3]}
    /* ── Array: group_by ── */
    if (strcmp(method, "group_by") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 1 || args[0].type != VAL_CLOSURE)
            return eval_err(strdup(".group_by() expects 1 closure argument"));
        LatValue grp_map = value_map_new();
        for (size_t i = 0; i < obj.as.array.len; i++) {
            LatValue elem = value_deep_clone(&obj.as.array.elems[i]);
            EvalResult r = call_closure(ev,
                args[0].as.closure.param_names,
                args[0].as.closure.param_count,
                args[0].as.closure.body,
                args[0].as.closure.captured_env,
                &elem, 1,
                args[0].as.closure.default_values, args[0].as.closure.has_variadic);
            if (!IS_OK(r)) {
                value_free(&grp_map);
                return r;
            }
            char *grp_key = value_display(&r.value);
            value_free(&r.value);
            LatValue *existing = (LatValue *)lat_map_get(grp_map.as.map.map, grp_key);
            if (existing) {
                size_t old_len = existing->as.array.len;
                LatValue *new_elems = malloc((old_len + 1) * sizeof(LatValue));
                for (size_t j = 0; j < old_len; j++) {
                    new_elems[j] = value_deep_clone(&existing->as.array.elems[j]);
                }
                new_elems[old_len] = value_deep_clone(&obj.as.array.elems[i]);
                LatValue new_grp_arr = value_array(new_elems, old_len + 1);
                free(new_elems);
                lat_map_set(grp_map.as.map.map, grp_key, &new_grp_arr);
            } else {
                LatValue cloned = value_deep_clone(&obj.as.array.elems[i]);
                LatValue new_grp_arr = value_array(&cloned, 1);
                lat_map_set(grp_map.as.map.map, grp_key, &new_grp_arr);
            }
            free(grp_key);
        }
        return eval_ok(grp_map);
    }
    /// @method Array.sum() -> Int|Float
    /// @category Array Methods
    /// Return the sum of all numeric elements.
    /// @example [1, 2, 3].sum()  // 6
    /* ── Array: sum ── */
    if (strcmp(method, "sum") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 0) return eval_err(strdup(".sum() takes no arguments"));
        bool sum_has_float = false;
        int64_t isum = 0;
        double fsum = 0.0;
        for (size_t i = 0; i < obj.as.array.len; i++) {
            if (obj.as.array.elems[i].type == VAL_INT) {
                isum += obj.as.array.elems[i].as.int_val;
                fsum += (double)obj.as.array.elems[i].as.int_val;
            } else if (obj.as.array.elems[i].type == VAL_FLOAT) {
                sum_has_float = true;
                fsum += obj.as.array.elems[i].as.float_val;
            } else {
                return eval_err(strdup(".sum() requires all elements to be numeric"));
            }
        }
        if (sum_has_float) return eval_ok(value_float(fsum));
        return eval_ok(value_int(isum));
    }
    /// @method Array.min() -> Int|Float
    /// @category Array Methods
    /// Return the minimum element (all elements must be numeric).
    /// @example [3, 1, 2].min()  // 1
    /* ── Array: min ── */
    if (strcmp(method, "min") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 0) return eval_err(strdup(".min() takes no arguments"));
        if (obj.as.array.len == 0) return eval_err(strdup(".min() on empty array"));
        bool min_has_float = false;
        for (size_t i = 0; i < obj.as.array.len; i++) {
            if (obj.as.array.elems[i].type == VAL_FLOAT) min_has_float = true;
            else if (obj.as.array.elems[i].type != VAL_INT)
                return eval_err(strdup(".min() requires all elements to be numeric"));
        }
        if (min_has_float) {
            double fmin = (obj.as.array.elems[0].type == VAL_FLOAT)
                ? obj.as.array.elems[0].as.float_val
                : (double)obj.as.array.elems[0].as.int_val;
            for (size_t i = 1; i < obj.as.array.len; i++) {
                double v = (obj.as.array.elems[i].type == VAL_FLOAT)
                    ? obj.as.array.elems[i].as.float_val
                    : (double)obj.as.array.elems[i].as.int_val;
                if (v < fmin) fmin = v;
            }
            return eval_ok(value_float(fmin));
        }
        int64_t imin = obj.as.array.elems[0].as.int_val;
        for (size_t i = 1; i < obj.as.array.len; i++) {
            if (obj.as.array.elems[i].as.int_val < imin)
                imin = obj.as.array.elems[i].as.int_val;
        }
        return eval_ok(value_int(imin));
    }
    /// @method Array.max() -> Int|Float
    /// @category Array Methods
    /// Return the maximum element (all elements must be numeric).
    /// @example [3, 1, 2].max()  // 3
    /* ── Array: max ── */
    if (strcmp(method, "max") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 0) return eval_err(strdup(".max() takes no arguments"));
        if (obj.as.array.len == 0) return eval_err(strdup(".max() on empty array"));
        bool max_has_float = false;
        for (size_t i = 0; i < obj.as.array.len; i++) {
            if (obj.as.array.elems[i].type == VAL_FLOAT) max_has_float = true;
            else if (obj.as.array.elems[i].type != VAL_INT)
                return eval_err(strdup(".max() requires all elements to be numeric"));
        }
        if (max_has_float) {
            double fmax = (obj.as.array.elems[0].type == VAL_FLOAT)
                ? obj.as.array.elems[0].as.float_val
                : (double)obj.as.array.elems[0].as.int_val;
            for (size_t i = 1; i < obj.as.array.len; i++) {
                double v = (obj.as.array.elems[i].type == VAL_FLOAT)
                    ? obj.as.array.elems[i].as.float_val
                    : (double)obj.as.array.elems[i].as.int_val;
                if (v > fmax) fmax = v;
            }
            return eval_ok(value_float(fmax));
        }
        int64_t imax = obj.as.array.elems[0].as.int_val;
        for (size_t i = 1; i < obj.as.array.len; i++) {
            if (obj.as.array.elems[i].as.int_val > imax)
                imax = obj.as.array.elems[i].as.int_val;
        }
        return eval_ok(value_int(imax));
    }
    /// @method Array.first() -> Any|Unit
    /// @category Array Methods
    /// Return the first element, or unit if the array is empty.
    /// @example [1, 2, 3].first()  // 1
    /* ── Array: first ── */
    if (strcmp(method, "first") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 0) return eval_err(strdup(".first() takes no arguments"));
        if (obj.as.array.len == 0) return eval_ok(value_unit());
        return eval_ok(value_deep_clone(&obj.as.array.elems[0]));
    }
    /// @method Array.last() -> Any|Unit
    /// @category Array Methods
    /// Return the last element, or unit if the array is empty.
    /// @example [1, 2, 3].last()  // 3
    /* ── Array: last ── */
    if (strcmp(method, "last") == 0 && obj.type == VAL_ARRAY) {
        if (arg_count != 0) return eval_err(strdup(".last() takes no arguments"));
        if (obj.as.array.len == 0) return eval_ok(value_unit());
        return eval_ok(value_deep_clone(&obj.as.array.elems[obj.as.array.len - 1]));
    }
    /* ── Map methods ── */
    if (obj.type == VAL_MAP) {
        /// @method Map.get(key: String) -> Any|Unit
        /// @category Map Methods
        /// Get the value for a key, or unit if not found.
        /// @example m.get("name")  // "Alice"
        if (strcmp(method, "get") == 0) {
            if (arg_count != 1 || args[0].type != VAL_STR)
                return eval_err(strdup(".get() expects 1 string argument"));
            LatValue *found = (LatValue *)lat_map_get(obj.as.map.map, args[0].as.str_val);
            return eval_ok(found ? value_deep_clone(found) : value_nil());
        }
        /// @method Map.has(key: String) -> Bool
        /// @category Map Methods
        /// Check if the map contains the given key.
        /// @example m.has("name")  // true
        if (strcmp(method, "has") == 0) {
            if (arg_count != 1 || args[0].type != VAL_STR)
                return eval_err(strdup(".has() expects 1 string argument"));
            return eval_ok(value_bool(lat_map_contains(obj.as.map.map, args[0].as.str_val)));
        }
        /// @method Map.keys() -> Array
        /// @category Map Methods
        /// Return an array of all keys in the map.
        /// @example m.keys()  // ["name", "age"]
        if (strcmp(method, "keys") == 0) {
            size_t n = lat_map_len(obj.as.map.map);
            LatValue *keys = malloc((n > 0 ? n : 1) * sizeof(LatValue));
            size_t ki = 0;
            for (size_t i = 0; i < obj.as.map.map->cap; i++) {
                if (obj.as.map.map->entries[i].state == MAP_OCCUPIED) {
                    keys[ki++] = value_string(obj.as.map.map->entries[i].key);
                }
            }
            LatValue arr = value_array(keys, ki);
            free(keys);
            return eval_ok(arr);
        }
        /// @method Map.values() -> Array
        /// @category Map Methods
        /// Return an array of all values in the map.
        /// @example m.values()  // ["Alice", 30]
        if (strcmp(method, "values") == 0) {
            size_t n = lat_map_len(obj.as.map.map);
            LatValue *vals = malloc((n > 0 ? n : 1) * sizeof(LatValue));
            size_t vi = 0;
            for (size_t i = 0; i < obj.as.map.map->cap; i++) {
                if (obj.as.map.map->entries[i].state == MAP_OCCUPIED) {
                    vals[vi++] = value_deep_clone((LatValue *)obj.as.map.map->entries[i].value);
                }
            }
            LatValue arr = value_array(vals, vi);
            free(vals);
            return eval_ok(arr);
        }
        /// @method Map.len() -> Int
        /// @category Map Methods
        /// Return the number of key-value pairs in the map. Also available as .length().
        /// @example m.len()  // 2
        if (strcmp(method, "len") == 0 || strcmp(method, "length") == 0) {
            return eval_ok(value_int((int64_t)lat_map_len(obj.as.map.map)));
        }
        /// @method Map.entries() -> Array
        /// @category Map Methods
        /// Return an array of [key, value] pairs.
        /// @example m.entries()  // [["name", "Alice"], ["age", 30]]
        if (strcmp(method, "entries") == 0) {
            if (arg_count != 0) return eval_err(strdup(".entries() takes no arguments"));
            size_t n = lat_map_len(obj.as.map.map);
            LatValue *entries = malloc((n > 0 ? n : 1) * sizeof(LatValue));
            size_t ei = 0;
            for (size_t i = 0; i < obj.as.map.map->cap; i++) {
                if (obj.as.map.map->entries[i].state == MAP_OCCUPIED) {
                    LatValue pair_elems[2];
                    pair_elems[0] = value_string(obj.as.map.map->entries[i].key);
                    pair_elems[1] = value_deep_clone((LatValue *)obj.as.map.map->entries[i].value);
                    entries[ei++] = value_array(pair_elems, 2);
                }
            }
            LatValue arr = value_array(entries, ei);
            free(entries);
            return eval_ok(arr);
        }
        /// @method Map.merge(other: Map) -> Unit
        /// @category Map Methods
        /// Merge another map into this one (mutates in place).
        /// @example m.merge(other_map)
        if (strcmp(method, "merge") == 0) {
            if (arg_count != 1) return eval_err(strdup(".merge() expects exactly 1 argument"));
            if (args[0].type != VAL_MAP) return eval_err(strdup(".merge() argument must be a Map"));
            LatMap *other = args[0].as.map.map;
            for (size_t i = 0; i < other->cap; i++) {
                if (other->entries[i].state == MAP_OCCUPIED) {
                    LatValue cloned = value_deep_clone((LatValue *)other->entries[i].value);
                    lat_map_set(obj.as.map.map, other->entries[i].key, &cloned);
                }
            }
            return eval_ok(value_unit());
        }
        /// @method Map.for_each(fn: Closure) -> Unit
        /// @category Map Methods
        /// Call fn(key, value) for each entry in the map.
        /// @example m.for_each(|k, v| { print(k, v) })
        if (strcmp(method, "for_each") == 0) {
            if (arg_count != 1 || args[0].type != VAL_CLOSURE)
                return eval_err(strdup(".for_each() expects 1 closure argument"));
            for (size_t i = 0; i < obj.as.map.map->cap; i++) {
                if (obj.as.map.map->entries[i].state == MAP_OCCUPIED) {
                    LatValue call_args[2];
                    call_args[0] = value_string(obj.as.map.map->entries[i].key);
                    call_args[1] = value_deep_clone((LatValue *)obj.as.map.map->entries[i].value);
                    EvalResult r = call_closure(ev,
                        args[0].as.closure.param_names,
                        args[0].as.closure.param_count,
                        args[0].as.closure.body,
                        args[0].as.closure.captured_env,
                        call_args, 2,
                        args[0].as.closure.default_values, args[0].as.closure.has_variadic);
                    if (!IS_OK(r)) return r;
                    value_free(&r.value);
                }
            }
            return eval_ok(value_unit());
        }
        /// @method Map.filter(fn: Closure) -> Map
        /// @category Map Methods
        /// Return a new map with only entries where fn(key, value) returns true.
        /// @example m.filter(|k, v| { v > 0 })
        if (strcmp(method, "filter") == 0) {
            if (arg_count != 1 || args[0].type != VAL_CLOSURE)
                return eval_err(strdup(".filter() expects 1 closure argument"));
            LatValue result = value_map_new();
            for (size_t i = 0; i < obj.as.map.map->cap; i++) {
                if (obj.as.map.map->entries[i].state == MAP_OCCUPIED) {
                    LatValue call_args[2];
                    call_args[0] = value_string(obj.as.map.map->entries[i].key);
                    call_args[1] = value_deep_clone((LatValue *)obj.as.map.map->entries[i].value);
                    EvalResult r = call_closure(ev,
                        args[0].as.closure.param_names,
                        args[0].as.closure.param_count,
                        args[0].as.closure.body,
                        args[0].as.closure.captured_env,
                        call_args, 2,
                        args[0].as.closure.default_values, args[0].as.closure.has_variadic);
                    if (!IS_OK(r)) { value_free(&result); return r; }
                    if (value_is_truthy(&r.value)) {
                        LatValue cloned = value_deep_clone((LatValue *)obj.as.map.map->entries[i].value);
                        lat_map_set(result.as.map.map, obj.as.map.map->entries[i].key, &cloned);
                    }
                    value_free(&r.value);
                }
            }
            return eval_ok(result);
        }
        /// @method Map.map(fn: Closure) -> Map
        /// @category Map Methods
        /// Return a new map with values transformed by fn(key, value).
        /// @example m.map(|k, v| { v * 2 })
        if (strcmp(method, "map") == 0) {
            if (arg_count != 1 || args[0].type != VAL_CLOSURE)
                return eval_err(strdup(".map() expects 1 closure argument"));
            LatValue result = value_map_new();
            for (size_t i = 0; i < obj.as.map.map->cap; i++) {
                if (obj.as.map.map->entries[i].state == MAP_OCCUPIED) {
                    LatValue call_args[2];
                    call_args[0] = value_string(obj.as.map.map->entries[i].key);
                    call_args[1] = value_deep_clone((LatValue *)obj.as.map.map->entries[i].value);
                    EvalResult r = call_closure(ev,
                        args[0].as.closure.param_names,
                        args[0].as.closure.param_count,
                        args[0].as.closure.body,
                        args[0].as.closure.captured_env,
                        call_args, 2,
                        args[0].as.closure.default_values, args[0].as.closure.has_variadic);
                    if (!IS_OK(r)) { value_free(&result); return r; }
                    lat_map_set(result.as.map.map, obj.as.map.map->entries[i].key, &r.value);
                }
            }
            return eval_ok(result);
        }
    }
    /* ── String methods ── */
    if (obj.type == VAL_STR) {
        /// @method String.contains(substr: String) -> Bool
        /// @category String Methods
        /// Check if the string contains a substring.
        /// @example "hello world".contains("world")  // true
        if (strcmp(method, "contains") == 0) {
            if (arg_count != 1 || args[0].type != VAL_STR)
                return eval_err(strdup(".contains() expects 1 string argument"));
            return eval_ok(value_bool(lat_str_contains(obj.as.str_val, args[0].as.str_val)));
        }
        /// @method String.starts_with(prefix: String) -> Bool
        /// @category String Methods
        /// Check if the string starts with the given prefix.
        /// @example "hello".starts_with("he")  // true
        if (strcmp(method, "starts_with") == 0) {
            if (arg_count != 1 || args[0].type != VAL_STR)
                return eval_err(strdup(".starts_with() expects 1 string argument"));
            return eval_ok(value_bool(lat_str_starts_with(obj.as.str_val, args[0].as.str_val)));
        }
        /// @method String.ends_with(suffix: String) -> Bool
        /// @category String Methods
        /// Check if the string ends with the given suffix.
        /// @example "hello".ends_with("lo")  // true
        if (strcmp(method, "ends_with") == 0) {
            if (arg_count != 1 || args[0].type != VAL_STR)
                return eval_err(strdup(".ends_with() expects 1 string argument"));
            return eval_ok(value_bool(lat_str_ends_with(obj.as.str_val, args[0].as.str_val)));
        }
        /// @method String.trim() -> String
        /// @category String Methods
        /// Remove leading and trailing whitespace.
        /// @example "  hello  ".trim()  // "hello"
        if (strcmp(method, "trim") == 0) {
            return eval_ok(value_string_owned(lat_str_trim(obj.as.str_val)));
        }
        /// @method String.to_upper() -> String
        /// @category String Methods
        /// Convert the string to uppercase.
        /// @example "hello".to_upper()  // "HELLO"
        if (strcmp(method, "to_upper") == 0) {
            return eval_ok(value_string_owned(lat_str_to_upper(obj.as.str_val)));
        }
        /// @method String.to_lower() -> String
        /// @category String Methods
        /// Convert the string to lowercase.
        /// @example "HELLO".to_lower()  // "hello"
        if (strcmp(method, "to_lower") == 0) {
            return eval_ok(value_string_owned(lat_str_to_lower(obj.as.str_val)));
        }
        /// @method String.capitalize() -> String
        /// @category String Methods
        /// Capitalize the first letter, lowercase the rest.
        /// @example "hello world".capitalize()  // "Hello world"
        if (strcmp(method, "capitalize") == 0) {
            return eval_ok(value_string_owned(lat_str_capitalize(obj.as.str_val)));
        }
        /// @method String.title_case() -> String
        /// @category String Methods
        /// Capitalize the first letter of each word.
        /// @example "hello world".title_case()  // "Hello World"
        if (strcmp(method, "title_case") == 0) {
            return eval_ok(value_string_owned(lat_str_title_case(obj.as.str_val)));
        }
        /// @method String.snake_case() -> String
        /// @category String Methods
        /// Convert to snake_case.
        /// @example "helloWorld".snake_case()  // "hello_world"
        if (strcmp(method, "snake_case") == 0) {
            return eval_ok(value_string_owned(lat_str_snake_case(obj.as.str_val)));
        }
        /// @method String.camel_case() -> String
        /// @category String Methods
        /// Convert to camelCase.
        /// @example "hello_world".camel_case()  // "helloWorld"
        if (strcmp(method, "camel_case") == 0) {
            return eval_ok(value_string_owned(lat_str_camel_case(obj.as.str_val)));
        }
        /// @method String.kebab_case() -> String
        /// @category String Methods
        /// Convert to kebab-case.
        /// @example "helloWorld".kebab_case()  // "hello-world"
        if (strcmp(method, "kebab_case") == 0) {
            return eval_ok(value_string_owned(lat_str_kebab_case(obj.as.str_val)));
        }
        /// @method String.replace(old: String, new: String) -> String
        /// @category String Methods
        /// Replace all occurrences of a substring.
        /// @example "hello world".replace("world", "there")  // "hello there"
        if (strcmp(method, "replace") == 0) {
            if (arg_count != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR)
                return eval_err(strdup(".replace() expects 2 string arguments"));
            return eval_ok(value_string_owned(lat_str_replace(obj.as.str_val, args[0].as.str_val, args[1].as.str_val)));
        }
        /// @method String.split(sep: String) -> Array
        /// @category String Methods
        /// Split the string by a separator, returning an array of parts.
        /// @example "a,b,c".split(",")  // ["a", "b", "c"]
        if (strcmp(method, "split") == 0) {
            if (arg_count != 1 || args[0].type != VAL_STR)
                return eval_err(strdup(".split() expects 1 string argument"));
            size_t count;
            char **parts = lat_str_split(obj.as.str_val, args[0].as.str_val, &count);
            LatValue *elems = malloc(count * sizeof(LatValue));
            for (size_t i = 0; i < count; i++) {
                elems[i] = value_string_owned(parts[i]);
            }
            free(parts);
            LatValue arr = value_array(elems, count);
            free(elems);
            return eval_ok(arr);
        }
        /// @method String.index_of(substr: String) -> Int
        /// @category String Methods
        /// Return the index of the first occurrence of substr, or -1 if not found.
        /// @example "hello".index_of("ll")  // 2
        if (strcmp(method, "index_of") == 0) {
            if (arg_count != 1 || args[0].type != VAL_STR)
                return eval_err(strdup(".index_of() expects 1 string argument"));
            return eval_ok(value_int(lat_str_index_of(obj.as.str_val, args[0].as.str_val)));
        }
        /// @method String.substring(start: Int, end: Int) -> String
        /// @category String Methods
        /// Extract a substring from start (inclusive) to end (exclusive).
        /// @example "hello".substring(1, 4)  // "ell"
        if (strcmp(method, "substring") == 0) {
            if (arg_count != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT)
                return eval_err(strdup(".substring() expects 2 integer arguments"));
            return eval_ok(value_string_owned(lat_str_substring(obj.as.str_val, args[0].as.int_val, args[1].as.int_val)));
        }
        /// @method String.chars() -> Array
        /// @category String Methods
        /// Split the string into an array of single-character strings.
        /// @example "abc".chars()  // ["a", "b", "c"]
        if (strcmp(method, "chars") == 0) {
            size_t slen = strlen(obj.as.str_val);
            LatValue *elems = malloc((slen > 0 ? slen : 1) * sizeof(LatValue));
            for (size_t i = 0; i < slen; i++) {
                char buf[2] = { obj.as.str_val[i], '\0' };
                elems[i] = value_string(buf);
            }
            LatValue arr = value_array(elems, slen);
            free(elems);
            return eval_ok(arr);
        }
        /// @method String.bytes() -> Array
        /// @category String Methods
        /// Return an array of byte values (integers) for the string.
        /// @example "AB".bytes()  // [65, 66]
        if (strcmp(method, "bytes") == 0) {
            size_t slen = strlen(obj.as.str_val);
            LatValue *elems = malloc((slen > 0 ? slen : 1) * sizeof(LatValue));
            for (size_t i = 0; i < slen; i++) {
                elems[i] = value_int((int64_t)(unsigned char)obj.as.str_val[i]);
            }
            LatValue arr = value_array(elems, slen);
            free(elems);
            return eval_ok(arr);
        }
        /// @method String.reverse() -> String
        /// @category String Methods
        /// Return the string with characters in reverse order.
        /// @example "hello".reverse()  // "olleh"
        if (strcmp(method, "reverse") == 0) {
            return eval_ok(value_string_owned(lat_str_reverse(obj.as.str_val)));
        }
        /// @method String.repeat(n: Int) -> String
        /// @category String Methods
        /// Repeat the string n times.
        /// @example "ab".repeat(3)  // "ababab"
        if (strcmp(method, "repeat") == 0) {
            if (arg_count != 1 || args[0].type != VAL_INT)
                return eval_err(strdup(".repeat() expects 1 integer argument"));
            return eval_ok(value_string_owned(lat_str_repeat(obj.as.str_val, (size_t)args[0].as.int_val)));
        }
        /// @method String.trim_start() -> String
        /// @category String Methods
        /// Remove leading whitespace.
        /// @example "  hello".trim_start()  // "hello"
        if (strcmp(method, "trim_start") == 0) {
            if (arg_count != 0) return eval_err(strdup(".trim_start() takes no arguments"));
            const char *s = obj.as.str_val;
            size_t len = strlen(s);
            size_t start = 0;
            while (start < len && isspace((unsigned char)s[start])) start++;
            char *result = malloc(len - start + 1);
            memcpy(result, s + start, len - start);
            result[len - start] = '\0';
            return eval_ok(value_string_owned(result));
        }
        /// @method String.trim_end() -> String
        /// @category String Methods
        /// Remove trailing whitespace.
        /// @example "hello  ".trim_end()  // "hello"
        if (strcmp(method, "trim_end") == 0) {
            if (arg_count != 0) return eval_err(strdup(".trim_end() takes no arguments"));
            const char *s = obj.as.str_val;
            size_t len = strlen(s);
            size_t end = len;
            while (end > 0 && isspace((unsigned char)s[end - 1])) end--;
            char *result = malloc(end + 1);
            memcpy(result, s, end);
            result[end] = '\0';
            return eval_ok(value_string_owned(result));
        }
        /// @method String.pad_left(n: Int, ch: String) -> String
        /// @category String Methods
        /// Pad the string on the left to reach length n using character ch.
        /// @example "42".pad_left(5, "0")  // "00042"
        if (strcmp(method, "pad_left") == 0) {
            if (arg_count != 2) return eval_err(strdup(".pad_left() expects 2 arguments (n, ch)"));
            if (args[0].type != VAL_INT) return eval_err(strdup(".pad_left() first argument must be an integer"));
            if (args[1].type != VAL_STR) return eval_err(strdup(".pad_left() second argument must be a string"));
            if (strlen(args[1].as.str_val) != 1) return eval_err(strdup(".pad_left() padding must be a single character"));
            const char *s = obj.as.str_val;
            size_t slen = strlen(s);
            size_t target = (size_t)args[0].as.int_val;
            if (slen >= target) return eval_ok(value_string(s));
            size_t pad_count = target - slen;
            char *result = malloc(target + 1);
            char ch = args[1].as.str_val[0];
            memset(result, ch, pad_count);
            memcpy(result + pad_count, s, slen);
            result[target] = '\0';
            return eval_ok(value_string_owned(result));
        }
        /// @method String.pad_right(n: Int, ch: String) -> String
        /// @category String Methods
        /// Pad the string on the right to reach length n using character ch.
        /// @example "42".pad_right(5, "0")  // "42000"
        if (strcmp(method, "pad_right") == 0) {
            if (arg_count != 2) return eval_err(strdup(".pad_right() expects 2 arguments (n, ch)"));
            if (args[0].type != VAL_INT) return eval_err(strdup(".pad_right() first argument must be an integer"));
            if (args[1].type != VAL_STR) return eval_err(strdup(".pad_right() second argument must be a string"));
            if (strlen(args[1].as.str_val) != 1) return eval_err(strdup(".pad_right() padding must be a single character"));
            const char *s = obj.as.str_val;
            size_t slen = strlen(s);
            size_t target = (size_t)args[0].as.int_val;
            if (slen >= target) return eval_ok(value_string(s));
            size_t pad_count = target - slen;
            char *result = malloc(target + 1);
            memcpy(result, s, slen);
            char ch = args[1].as.str_val[0];
            memset(result + slen, ch, pad_count);
            result[target] = '\0';
            return eval_ok(value_string_owned(result));
        }
        /// @method String.count(substr: String) -> Int
        /// @category String Methods
        /// Count non-overlapping occurrences of a substring.
        /// @example "ababa".count("ab")  // 2
        if (strcmp(method, "count") == 0) {
            if (arg_count != 1 || args[0].type != VAL_STR)
                return eval_err(strdup(".count() expects 1 string argument"));
            const char *haystack = obj.as.str_val;
            const char *needle = args[0].as.str_val;
            size_t needle_len = strlen(needle);
            int64_t count = 0;
            if (needle_len == 0) {
                return eval_err(strdup(".count() substring must not be empty"));
            }
            const char *p = haystack;
            while ((p = strstr(p, needle)) != NULL) {
                count++;
                p += needle_len;
            }
            return eval_ok(value_int(count));
        }
        /// @method String.is_empty() -> Bool
        /// @category String Methods
        /// Check if the string is empty.
        /// @example "".is_empty()  // true
        if (strcmp(method, "is_empty") == 0) {
            if (arg_count != 0)
                return eval_err(strdup(".is_empty() takes no arguments"));
            return eval_ok(value_bool(obj.as.str_val[0] == '\0'));
        }
    }

    if (strcmp(method, "get") == 0 && obj.type != VAL_REF) {
        if (obj.type != VAL_STRUCT) return eval_err(strdup(".get() is not defined on non-struct"));
        if (arg_count != 1) return eval_err(strdup(".get() expects exactly 1 argument"));
        if (args[0].type != VAL_STR) return eval_err(strdup(".get() key must be a string"));
        for (size_t i = 0; i < obj.as.strct.field_count; i++) {
            if (obj.as.strct.field_names[i] == intern(args[0].as.str_val)) {
                return eval_ok(value_deep_clone(&obj.as.strct.field_values[i]));
            }
        }
        char *err = NULL;
        (void)asprintf(&err, "struct has no field '%s'", args[0].as.str_val);
        return eval_err(err);
    }

    /* ── Callable struct fields: obj.method(args) where field is a closure ── */
    if (obj.type == VAL_STRUCT) {
        for (size_t i = 0; i < obj.as.strct.field_count; i++) {
            if (obj.as.strct.field_names[i] == intern(method) &&
                obj.as.strct.field_values[i].type == VAL_CLOSURE) {
                LatValue *cl = &obj.as.strct.field_values[i];
                /* Prepend self (the struct) as the first argument */
                size_t total = 1 + arg_count;
                LatValue *full_args = malloc(total * sizeof(LatValue));
                full_args[0] = value_deep_clone(&obj);
                for (size_t j = 0; j < arg_count; j++) {
                    full_args[j + 1] = value_deep_clone(&args[j]);
                }
                EvalResult r = call_closure(ev,
                    cl->as.closure.param_names, cl->as.closure.param_count,
                    cl->as.closure.body, cl->as.closure.captured_env,
                    full_args, total,
                    cl->as.closure.default_values, cl->as.closure.has_variadic);
                /* call_closure takes ownership of args via env_define;
                   env_pop_scope frees them — do NOT value_free here */
                free(full_args);
                return r;
            }
        }
    }

    /* ── Channel methods ── */
    if (obj.type == VAL_CHANNEL) {
        LatChannel *ch = obj.as.channel.ch;
        /// @method Channel.send(val: Any) -> Unit
        /// @category Channel Methods
        /// Send a crystal (frozen) value on the channel.
        /// @example ch.send(freeze(42))
        if (strcmp(method, "send") == 0) {
            if (arg_count != 1)
                return eval_err(strdup(".send() expects exactly 1 argument"));
            if (!value_is_crystal(&args[0]) && args[0].type != VAL_INT &&
                args[0].type != VAL_FLOAT && args[0].type != VAL_BOOL &&
                args[0].type != VAL_UNIT) {
                return eval_err(strdup("can only send crystal (frozen) values on a channel"));
            }
            /* Deep-clone into malloc-backed memory (no heap/arena pointers) */
            DualHeap *saved_heap = ev->heap;
            value_set_heap(NULL);
            value_set_arena(NULL);
            LatValue detached = value_deep_clone(&args[0]);
            value_set_heap(saved_heap);
            bool ok = channel_send(ch, detached);
            if (!ok) return eval_err(strdup("cannot send on a closed channel"));
            return eval_ok(value_unit());
        }
        /// @method Channel.recv() -> Any|Unit
        /// @category Channel Methods
        /// Receive a value from the channel, blocking until available. Returns unit if closed.
        /// @example ch.recv()  // 42
        if (strcmp(method, "recv") == 0) {
            if (arg_count != 0)
                return eval_err(strdup(".recv() takes no arguments"));
            bool ok;
            LatValue val = channel_recv(ch, &ok);
            if (!ok) return eval_ok(value_unit());
            return eval_ok(val);
        }
        /// @method Channel.close() -> Unit
        /// @category Channel Methods
        /// Close the channel, preventing further sends.
        /// @example ch.close()
        if (strcmp(method, "close") == 0) {
            if (arg_count != 0)
                return eval_err(strdup(".close() takes no arguments"));
            channel_close(ch);
            return eval_ok(value_unit());
        }
    }

    /* Fallback: check for callable field (e.g. module map with closure values).
     * call_closure consumes args, but the caller also frees them, so we
     * deep-clone args before passing. */
    if (obj.type == VAL_MAP) {
        LatValue *field = (LatValue *)lat_map_get(obj.as.map.map, method);
        if (field && field->type == VAL_CLOSURE) {
            if (field->as.closure.native_fn && !field->as.closure.body
                && field->as.closure.default_values == (struct Expr **)(uintptr_t)0x1) {
                /* VM-style native from builtin module */
                typedef LatValue (*VMNativeFn)(LatValue *, int);
                VMNativeFn fn = (VMNativeFn)field->as.closure.native_fn;
                LatRuntime *prev_rt = lat_runtime_current();
                LatRuntime tmp_rt;
                if (!prev_rt) {
                    memset(&tmp_rt, 0, sizeof(tmp_rt));
                    lat_runtime_set_current(&tmp_rt);
                }
                LatRuntime *rt = lat_runtime_current();
                LatValue result = fn(args, (int)arg_count);
                EvalResult res;
                if (rt->error) {
                    char *msg = rt->error;
                    rt->error = NULL;
                    value_free(&result);
                    res = eval_err(msg);
                } else {
                    res = eval_ok(result);
                }
                if (!prev_rt) lat_runtime_set_current(NULL);
                return res;
            }
            LatValue *cloned_args = malloc(arg_count * sizeof(LatValue));
            for (size_t i = 0; i < arg_count; i++)
                cloned_args[i] = value_deep_clone(&args[i]);
            EvalResult res = call_closure(ev,
                field->as.closure.param_names,
                field->as.closure.param_count,
                field->as.closure.body,
                field->as.closure.captured_env,
                cloned_args, arg_count,
                field->as.closure.default_values, field->as.closure.has_variadic);
            free(cloned_args);
            return res;
        }
    } else if (obj.type == VAL_STRUCT) {
        for (size_t i = 0; i < obj.as.strct.field_count; i++) {
            if (obj.as.strct.field_names[i] == intern(method) &&
                obj.as.strct.field_values[i].type == VAL_CLOSURE) {
                LatValue *field = &obj.as.strct.field_values[i];
                LatValue *cloned_args = malloc(arg_count * sizeof(LatValue));
                for (size_t j = 0; j < arg_count; j++)
                    cloned_args[j] = value_deep_clone(&args[j]);
                EvalResult res = call_closure(ev,
                    field->as.closure.param_names,
                    field->as.closure.param_count,
                    field->as.closure.body,
                    field->as.closure.captured_env,
                    cloned_args, arg_count,
                    field->as.closure.default_values, field->as.closure.has_variadic);
                free(cloned_args);
                return res;
            }
        }
    }

    /* ── Trait impl method dispatch ── */
    {
        const char *type_name = NULL;
        if (obj.type == VAL_STRUCT) type_name = obj.as.strct.name;
        else if (obj.type == VAL_INT) type_name = "Int";
        else if (obj.type == VAL_FLOAT) type_name = "Float";
        else if (obj.type == VAL_STR) type_name = "String";
        else if (obj.type == VAL_BOOL) type_name = "Bool";
        else if (obj.type == VAL_ARRAY) type_name = "Array";
        else if (obj.type == VAL_MAP) type_name = "Map";

        if (type_name) {
            /* Search all impl blocks for this type */
            for (size_t i = 0; i < ev->impl_registry.cap; i++) {
                if (ev->impl_registry.entries[i].state != MAP_OCCUPIED) continue;
                ImplBlock **ib_ptr = (ImplBlock **)ev->impl_registry.entries[i].value;
                ImplBlock *ib = *ib_ptr;
                if (strcmp(ib->type_name, type_name) != 0) continue;
                for (size_t m = 0; m < ib->method_count; m++) {
                    if (strcmp(ib->methods[m].name, method) != 0) continue;
                    /* Found a matching impl method — call it with self as first arg.
                     * call_fn takes ownership of args via env_define, so do NOT free them. */
                    FnDecl *fn = &ib->methods[m];
                    size_t total = 1 + arg_count;
                    LatValue *full_args = malloc(total * sizeof(LatValue));
                    full_args[0] = value_deep_clone(&obj);
                    for (size_t j = 0; j < arg_count; j++)
                        full_args[j + 1] = value_deep_clone(&args[j]);
                    EvalResult r = call_fn(ev, fn, full_args, total, NULL);
                    free(full_args);
                    return r;
                }
            }
        }
    }

    /* ── Ref methods ── */
    if (obj.type == VAL_REF) {
        LatRef *ref = obj.as.ref.ref;
        LatValue *inner = &ref->value;

        /* Ref-specific methods */
        /// @method Ref.get() -> Any
        /// @category Ref Methods
        /// Return a deep clone of the wrapped value.
        /// @example r.get()
        if (strcmp(method, "get") == 0 && arg_count == 0) {
            return eval_ok(value_deep_clone(inner));
        }
        /// @method Ref.deref() -> Any
        /// @category Ref Methods
        /// Alias for get(). Return a deep clone of the wrapped value.
        /// @example r.deref()
        if (strcmp(method, "deref") == 0 && arg_count == 0) {
            return eval_ok(value_deep_clone(inner));
        }
        /// @method Ref.set(value: Any) -> Unit
        /// @category Ref Methods
        /// Replace the inner value (all holders see the change).
        /// @example r.set(42)
        if (strcmp(method, "set") == 0 && arg_count == 1) {
            if (obj.phase == VTAG_CRYSTAL)
                return eval_err(strdup("cannot set on a frozen Ref"));
            value_free(inner);
            *inner = value_deep_clone(&args[0]);
            return eval_ok(value_unit());
        }
        /// @method Ref.inner_type() -> String
        /// @category Ref Methods
        /// Return the type name of the wrapped value.
        /// @example r.inner_type()
        if (strcmp(method, "inner_type") == 0 && arg_count == 0) {
            return eval_ok(value_string(value_type_name(inner)));
        }

        /* Map proxy (when inner is VAL_MAP) */
        if (inner->type == VAL_MAP) {
            if (strcmp(method, "get") == 0 && arg_count == 1) {
                if (args[0].type != VAL_STR) return eval_ok(value_nil());
                LatValue *found = lat_map_get(inner->as.map.map, args[0].as.str_val);
                return eval_ok(found ? value_deep_clone(found) : value_nil());
            }
            if (strcmp(method, "set") == 0 && arg_count == 2) {
                if (obj.phase == VTAG_CRYSTAL)
                    return eval_err(strdup("cannot set on a frozen Ref"));
                if (args[0].type != VAL_STR)
                    return eval_err(strdup(".set() key must be a string"));
                LatValue *old = (LatValue *)lat_map_get(inner->as.map.map, args[0].as.str_val);
                if (old) value_free(old);
                LatValue cloned = value_deep_clone(&args[1]);
                lat_map_set(inner->as.map.map, args[0].as.str_val, &cloned);
                return eval_ok(value_unit());
            }
            if (strcmp(method, "has") == 0 && arg_count == 1) {
                bool found = args[0].type == VAL_STR && lat_map_contains(inner->as.map.map, args[0].as.str_val);
                return eval_ok(value_bool(found));
            }
            if (strcmp(method, "contains") == 0 && arg_count == 1) {
                bool found = false;
                for (size_t i = 0; i < inner->as.map.map->cap; i++) {
                    if (inner->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                    LatValue *mv = (LatValue *)inner->as.map.map->entries[i].value;
                    if (value_eq(mv, &args[0])) { found = true; break; }
                }
                return eval_ok(value_bool(found));
            }
            if (strcmp(method, "keys") == 0 && arg_count == 0) {
                size_t n = lat_map_len(inner->as.map.map);
                LatValue *elems = malloc((n > 0 ? n : 1) * sizeof(LatValue));
                size_t ei = 0;
                for (size_t i = 0; i < inner->as.map.map->cap; i++) {
                    if (inner->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                    elems[ei++] = value_string(inner->as.map.map->entries[i].key);
                }
                LatValue arr = value_array(elems, ei); free(elems);
                return eval_ok(arr);
            }
            if (strcmp(method, "values") == 0 && arg_count == 0) {
                size_t n = lat_map_len(inner->as.map.map);
                LatValue *elems = malloc((n > 0 ? n : 1) * sizeof(LatValue));
                size_t ei = 0;
                for (size_t i = 0; i < inner->as.map.map->cap; i++) {
                    if (inner->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
                    LatValue *mv = (LatValue *)inner->as.map.map->entries[i].value;
                    elems[ei++] = value_deep_clone(mv);
                }
                LatValue arr = value_array(elems, ei); free(elems);
                return eval_ok(arr);
            }
            if (strcmp(method, "entries") == 0 && arg_count == 0) {
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
                return eval_ok(arr);
            }
            if ((strcmp(method, "len") == 0 || strcmp(method, "length") == 0) && arg_count == 0) {
                return eval_ok(value_int((int64_t)lat_map_len(inner->as.map.map)));
            }
        }

        /* Array proxy */
        if (inner->type == VAL_ARRAY) {
            if ((strcmp(method, "len") == 0 || strcmp(method, "length") == 0) && arg_count == 0)
                return eval_ok(value_int((int64_t)inner->as.array.len));
            if (strcmp(method, "contains") == 0 && arg_count == 1) {
                for (size_t i = 0; i < inner->as.array.len; i++) {
                    if (value_eq(&inner->as.array.elems[i], &args[0]))
                        return eval_ok(value_bool(true));
                }
                return eval_ok(value_bool(false));
            }
        }

        char *rerr = NULL;
        const char *rsug = builtin_find_similar_method(VAL_REF, method);
        if (rsug)
            (void)asprintf(&rerr, "Ref has no method '%s' (did you mean '%s'?)", method, rsug);
        else
            (void)asprintf(&rerr, "Ref has no method '%s'", method);
        return eval_err(rerr);
    }

    char *err = NULL;
    const char *msug = builtin_find_similar_method(obj.type, method);
    if (msug)
        (void)asprintf(&err, "unknown method '.%s()' on %s (did you mean '%s'?)", method, value_type_name(&obj), msug);
    else
        (void)asprintf(&err, "unknown method '.%s()' on %s", method, value_type_name(&obj));
    return eval_err(err);
}

/* ── Evaluator lifecycle ── */

Evaluator *evaluator_new(void) {
    Evaluator *ev = calloc(1, sizeof(Evaluator));
    ev->env = env_new();
    ev->mode = MODE_CASUAL;
    ev->struct_defs = lat_map_new(sizeof(StructDecl *));
    ev->enum_defs = lat_map_new(sizeof(EnumDecl *));
    ev->fn_defs = lat_map_new(sizeof(FnDecl *));
    ev->trait_defs = lat_map_new(sizeof(TraitDecl *));
    ev->impl_registry = lat_map_new(sizeof(ImplBlock *));
    stats_init(&ev->stats);
    ev->heap = dual_heap_new();
    ev->gc_roots = lat_vec_new(sizeof(LatValue *));
    ev->saved_envs = lat_vec_new(sizeof(Env *));
    ev->gc_stress = false;
    ev->no_regions = false;
    ev->required_files = lat_map_new(sizeof(bool));
    ev->module_cache = lat_map_new(sizeof(LatValue));
    ev->loaded_extensions = lat_map_new(sizeof(LatValue));
    ev->module_exprs = lat_vec_new(sizeof(Expr *));
    ev->bonds = NULL;
    ev->bond_count = 0;
    ev->bond_cap = 0;
    ev->tracked_vars = NULL;
    ev->tracked_count = 0;
    ev->tracked_cap = 0;
    ev->reactions = NULL;
    ev->reaction_count = 0;
    ev->reaction_cap = 0;
    ev->defer_stack = NULL;
    ev->defer_count = 0;
    ev->defer_cap = 0;
    ev->assertions_enabled = true;
    value_set_heap(ev->heap);
    return ev;
}

void evaluator_free(Evaluator *ev) {
    if (!ev) return;
    net_tls_cleanup();
    env_free(ev->env);            /* lat_free → fluid_dealloc removes from heap */
    lat_map_free(&ev->struct_defs);
    lat_map_free(&ev->enum_defs);
    lat_map_free(&ev->fn_defs);
    lat_map_free(&ev->trait_defs);
    lat_map_free(&ev->impl_registry);
    lat_map_free(&ev->required_files);
    /* Free cached module maps */
    for (size_t i = 0; i < ev->module_cache.cap; i++) {
        if (ev->module_cache.entries[i].state == MAP_OCCUPIED) {
            LatValue *mv = (LatValue *)ev->module_cache.entries[i].value;
            value_free(mv);
        }
    }
    lat_map_free(&ev->module_cache);
    /* Free cached extension maps */
    for (size_t i = 0; i < ev->loaded_extensions.cap; i++) {
        if (ev->loaded_extensions.entries[i].state == MAP_OCCUPIED) {
            LatValue *mv = (LatValue *)ev->loaded_extensions.entries[i].value;
            value_free(mv);
        }
    }
    lat_map_free(&ev->loaded_extensions);
    /* Free body Expr wrappers kept alive for module closures */
    for (size_t i = 0; i < ev->module_exprs.len; i++) {
        Expr **ep = lat_vec_get(&ev->module_exprs, i);
        /* Only free the wrapper node, not the stmts it borrows */
        free(*ep);
    }
    lat_vec_free(&ev->module_exprs);
    free(ev->script_dir);
    /* Free bonds */
    for (size_t i = 0; i < ev->bond_count; i++) {
        free(ev->bonds[i].target);
        for (size_t j = 0; j < ev->bonds[i].dep_count; j++) {
            free(ev->bonds[i].deps[j]);
            if (ev->bonds[i].dep_strategies) free(ev->bonds[i].dep_strategies[j]);
        }
        free(ev->bonds[i].deps);
        free(ev->bonds[i].dep_strategies);
    }
    free(ev->bonds);
    /* Free tracked variable histories */
    for (size_t i = 0; i < ev->tracked_count; i++) {
        free(ev->tracked_vars[i].name);
        for (size_t j = 0; j < ev->tracked_vars[i].history.count; j++) {
            free(ev->tracked_vars[i].history.snapshots[j].phase_name);
            free(ev->tracked_vars[i].history.snapshots[j].fn_name);
            value_free(&ev->tracked_vars[i].history.snapshots[j].value);
        }
        free(ev->tracked_vars[i].history.snapshots);
    }
    free(ev->tracked_vars);
    /* Free phase reactions */
    for (size_t i = 0; i < ev->reaction_count; i++) {
        free(ev->reactions[i].var_name);
        for (size_t j = 0; j < ev->reactions[i].cb_count; j++)
            value_free(&ev->reactions[i].callbacks[j]);
        free(ev->reactions[i].callbacks);
    }
    free(ev->reactions);
    /* Free seed crystals */
    for (size_t i = 0; i < ev->seed_count; i++) {
        free(ev->seeds[i].var_name);
        value_free(&ev->seeds[i].contract);
    }
    free(ev->seeds);
    /* Free pressure entries */
    for (size_t i = 0; i < ev->pressure_count; i++) {
        free(ev->pressures[i].var_name);
        free(ev->pressures[i].mode);
    }
    free(ev->pressures);
    free(ev->defer_stack);
    free(ev->call_stack);
    value_set_heap(NULL);         /* disconnect before freeing the heap itself */
    dual_heap_free(ev->heap);     /* frees any remaining tracked allocs */
    lat_vec_free(&ev->gc_roots);
    lat_vec_free(&ev->saved_envs);
    free(ev);
}

void evaluator_set_gc_stress(Evaluator *ev, bool enabled) {
    ev->gc_stress = enabled;
}

void evaluator_set_no_regions(Evaluator *ev, bool enabled) {
    ev->no_regions = enabled;
}

void evaluator_set_script_dir(Evaluator *ev, const char *dir) {
    free(ev->script_dir);
    ev->script_dir = dir ? strdup(dir) : NULL;
}

void evaluator_set_argv(Evaluator *ev, int argc, char **argv) {
    ev->prog_argc = argc;
    ev->prog_argv = argv;
}

void evaluator_set_assertions(Evaluator *ev, bool enabled) {
    ev->assertions_enabled = enabled;
}

char *evaluator_run(Evaluator *ev, const Program *prog) {
    ev->mode = prog->mode;

    /* First pass: register structs, enums, functions, traits, and impls */
    for (size_t i = 0; i < prog->item_count; i++) {
        if (prog->items[i].tag == ITEM_STRUCT) {
            StructDecl *ptr = &prog->items[i].as.struct_decl;
            lat_map_set(&ev->struct_defs, ptr->name, &ptr);
        } else if (prog->items[i].tag == ITEM_ENUM) {
            EnumDecl *ptr = &prog->items[i].as.enum_decl;
            lat_map_set(&ev->enum_defs, ptr->name, &ptr);
        } else if (prog->items[i].tag == ITEM_FUNCTION) {
            FnDecl *ptr = &prog->items[i].as.fn_decl;
            register_fn_overload(&ev->fn_defs, ptr);
        } else if (prog->items[i].tag == ITEM_TRAIT) {
            TraitDecl *ptr = &prog->items[i].as.trait_decl;
            lat_map_set(&ev->trait_defs, ptr->name, &ptr);
        } else if (prog->items[i].tag == ITEM_IMPL) {
            ImplBlock *ptr = &prog->items[i].as.impl_block;
            char key[512];
            snprintf(key, sizeof(key), "%s::%s", ptr->type_name, ptr->trait_name);
            lat_map_set(&ev->impl_registry, key, &ptr);
        }
    }

    /* Second pass: execute top-level statements */
    for (size_t i = 0; i < prog->item_count; i++) {
        if (prog->items[i].tag == ITEM_STMT) {
            EvalResult r = eval_stmt(ev, prog->items[i].as.stmt);
            if (IS_ERR(r)) {
                r.error = ev_attach_trace(ev, r.error);
                ev->call_depth = 0;
                return r.error;
            }
            if (IS_SIGNAL(r)) return strdup("unexpected control flow at top level");
            value_free(&r.value);
        }
    }

    /* If there is a main() function, call it */
    FnDecl *main_fn = find_fn(ev, "main");
    if (main_fn) {
        EvalResult r = call_fn(ev, main_fn, NULL, 0, NULL);
        if (IS_ERR(r)) {
            r.error = ev_attach_trace(ev, r.error);
            ev->call_depth = 0;
            return r.error;
        }
        value_free(&r.value);
    }

    return NULL; /* success */
}

int evaluator_run_tests(Evaluator *ev, const Program *prog) {
    ev->mode = prog->mode;

    /* First pass: register structs, enums, functions, traits, and impls */
    for (size_t i = 0; i < prog->item_count; i++) {
        if (prog->items[i].tag == ITEM_STRUCT) {
            StructDecl *ptr = &prog->items[i].as.struct_decl;
            lat_map_set(&ev->struct_defs, ptr->name, &ptr);
        } else if (prog->items[i].tag == ITEM_ENUM) {
            EnumDecl *ptr = &prog->items[i].as.enum_decl;
            lat_map_set(&ev->enum_defs, ptr->name, &ptr);
        } else if (prog->items[i].tag == ITEM_FUNCTION) {
            FnDecl *ptr = &prog->items[i].as.fn_decl;
            register_fn_overload(&ev->fn_defs, ptr);
        } else if (prog->items[i].tag == ITEM_TRAIT) {
            TraitDecl *ptr = &prog->items[i].as.trait_decl;
            lat_map_set(&ev->trait_defs, ptr->name, &ptr);
        } else if (prog->items[i].tag == ITEM_IMPL) {
            ImplBlock *ptr = &prog->items[i].as.impl_block;
            char key[512];
            snprintf(key, sizeof(key), "%s::%s", ptr->type_name, ptr->trait_name);
            lat_map_set(&ev->impl_registry, key, &ptr);
        }
    }

    /* Second pass: execute top-level statements (setup code) */
    for (size_t i = 0; i < prog->item_count; i++) {
        if (prog->items[i].tag == ITEM_STMT) {
            EvalResult r = eval_stmt(ev, prog->items[i].as.stmt);
            if (IS_ERR(r)) {
                fprintf(stderr, "setup error: %s\n", r.error);
                free(r.error);
                return 1;
            }
            if (IS_SIGNAL(r)) {
                fprintf(stderr, "setup error: unexpected control flow at top level\n");
                return 1;
            }
            value_free(&r.value);
        }
    }

    /* Count tests */
    size_t test_count = 0;
    for (size_t i = 0; i < prog->item_count; i++) {
        if (prog->items[i].tag == ITEM_TEST)
            test_count++;
    }

    if (test_count == 0) {
        printf("No tests found.\n");
        return 0;
    }

    printf("Running %zu test%s...\n\n", test_count, test_count == 1 ? "" : "s");

    /* Third pass: run tests */
    size_t passed = 0, failed = 0;
    for (size_t i = 0; i < prog->item_count; i++) {
        if (prog->items[i].tag != ITEM_TEST) continue;
        TestDecl *td = &prog->items[i].as.test_decl;

        stats_scope_push(&ev->stats);
        env_push_scope(ev->env);

        bool ok = true;
        char *errmsg = NULL;
        for (size_t j = 0; j < td->body_count; j++) {
            EvalResult r = eval_stmt(ev, td->body[j]);
            if (IS_ERR(r)) {
                ok = false;
                errmsg = ev_attach_trace(ev, r.error);
                ev->call_depth = 0;
                break;
            }
            if (IS_SIGNAL(r)) {
                ok = false;
                errmsg = strdup("unexpected control flow in test");
                break;
            }
            value_free(&r.value);
        }

        env_pop_scope(ev->env);
        stats_scope_pop(&ev->stats);

        if (ok) {
            passed++;
            printf("  ok: %s\n", td->name);
        } else {
            failed++;
            printf("  FAIL: %s\n", td->name);
            if (errmsg) {
                printf("        %s\n", errmsg);
                free(errmsg);
            }
        }
    }

    printf("\nResults: %zu passed, %zu failed, %zu total\n", passed, failed, test_count);
    return failed > 0 ? 1 : 0;
}

char *evaluator_run_repl(Evaluator *ev, const Program *prog) {
    ev->mode = prog->mode;

    /* First pass: register structs, enums, functions, traits, and impls */
    for (size_t i = 0; i < prog->item_count; i++) {
        if (prog->items[i].tag == ITEM_STRUCT) {
            StructDecl *ptr = &prog->items[i].as.struct_decl;
            lat_map_set(&ev->struct_defs, ptr->name, &ptr);
        } else if (prog->items[i].tag == ITEM_ENUM) {
            EnumDecl *ptr = &prog->items[i].as.enum_decl;
            lat_map_set(&ev->enum_defs, ptr->name, &ptr);
        } else if (prog->items[i].tag == ITEM_FUNCTION) {
            FnDecl *ptr = &prog->items[i].as.fn_decl;
            register_fn_overload(&ev->fn_defs, ptr);
        } else if (prog->items[i].tag == ITEM_TRAIT) {
            TraitDecl *ptr = &prog->items[i].as.trait_decl;
            lat_map_set(&ev->trait_defs, ptr->name, &ptr);
        } else if (prog->items[i].tag == ITEM_IMPL) {
            ImplBlock *ptr = &prog->items[i].as.impl_block;
            char key[512];
            snprintf(key, sizeof(key), "%s::%s", ptr->type_name, ptr->trait_name);
            lat_map_set(&ev->impl_registry, key, &ptr);
        }
    }

    /* Second pass: execute top-level statements (no auto-main) */
    for (size_t i = 0; i < prog->item_count; i++) {
        if (prog->items[i].tag == ITEM_STMT) {
            EvalResult r = eval_stmt(ev, prog->items[i].as.stmt);
            if (IS_ERR(r)) {
                r.error = ev_attach_trace(ev, r.error);
                ev->call_depth = 0;
                return r.error;
            }
            if (IS_SIGNAL(r)) return strdup("unexpected control flow at top level");
            value_free(&r.value);
        }
    }

    return NULL; /* success */
}

EvalResult evaluator_run_repl_result(Evaluator *ev, const Program *prog) {
    ev->mode = prog->mode;

    /* First pass: register structs, enums, functions, traits, and impls */
    for (size_t i = 0; i < prog->item_count; i++) {
        if (prog->items[i].tag == ITEM_STRUCT) {
            StructDecl *ptr = &prog->items[i].as.struct_decl;
            lat_map_set(&ev->struct_defs, ptr->name, &ptr);
        } else if (prog->items[i].tag == ITEM_ENUM) {
            EnumDecl *ptr = &prog->items[i].as.enum_decl;
            lat_map_set(&ev->enum_defs, ptr->name, &ptr);
        } else if (prog->items[i].tag == ITEM_FUNCTION) {
            FnDecl *ptr = &prog->items[i].as.fn_decl;
            register_fn_overload(&ev->fn_defs, ptr);
        } else if (prog->items[i].tag == ITEM_TRAIT) {
            TraitDecl *ptr = &prog->items[i].as.trait_decl;
            lat_map_set(&ev->trait_defs, ptr->name, &ptr);
        } else if (prog->items[i].tag == ITEM_IMPL) {
            ImplBlock *ptr = &prog->items[i].as.impl_block;
            char key[512];
            snprintf(key, sizeof(key), "%s::%s", ptr->type_name, ptr->trait_name);
            lat_map_set(&ev->impl_registry, key, &ptr);
        }
    }

    /* Second pass: execute top-level statements, keep last result */
    LatValue last = value_unit();
    for (size_t i = 0; i < prog->item_count; i++) {
        if (prog->items[i].tag == ITEM_STMT) {
            EvalResult r = eval_stmt(ev, prog->items[i].as.stmt);
            if (IS_ERR(r)) {
                r.error = ev_attach_trace(ev, r.error);
                ev->call_depth = 0;
                return r;
            }
            if (IS_SIGNAL(r)) {
                value_free(&last);
                return eval_err(strdup("unexpected control flow at top level"));
            }
            value_free(&last);
            last = r.value;
        }
    }

    return eval_ok(last);
}

char *eval_repr(Evaluator *ev, const LatValue *v) {
    if (v->type == VAL_STRUCT) {
        /* Look for a "repr" closure field */
        for (size_t i = 0; i < v->as.strct.field_count; i++) {
            if (v->as.strct.field_names[i] == intern("repr") &&
                v->as.strct.field_values[i].type == VAL_CLOSURE) {
                const LatValue *cl = &v->as.strct.field_values[i];
                LatValue self_arg = value_deep_clone(v);
                EvalResult r = call_closure(ev,
                    cl->as.closure.param_names,
                    cl->as.closure.param_count,
                    cl->as.closure.body,
                    cl->as.closure.captured_env,
                    &self_arg, 1,
                    cl->as.closure.default_values,
                    cl->as.closure.has_variadic);
                if (IS_OK(r) && r.value.type == VAL_STR) {
                    char *result = strdup(r.value.as.str_val);
                    value_free(&r.value);
                    return result;
                }
                if (IS_OK(r)) value_free(&r.value);
                /* Fall through to default */
                break;
            }
        }
    }
    return value_repr(v);
}

const MemoryStats *evaluator_stats(const Evaluator *ev) {
    /* Finalize snapshot metrics from heap state */
    MemoryStats *s = (MemoryStats *)&ev->stats;
    s->fluid_peak_bytes = ev->heap->fluid->peak_bytes;
    s->fluid_live_bytes = ev->heap->fluid->total_bytes;
    s->fluid_cumulative_bytes = ev->heap->fluid->cumulative_bytes;
    s->region_peak_count = ev->heap->regions->peak_count;
    s->region_live_count = ev->heap->regions->count;
    s->region_live_data_bytes = region_live_data_bytes(ev->heap->regions);
    s->region_cumulative_data_bytes = ev->heap->regions->cumulative_data_bytes;
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
#ifdef __linux__
        s->rss_peak_kb = (size_t)ru.ru_maxrss;
#else
        s->rss_peak_kb = (size_t)ru.ru_maxrss / 1024;
#endif
    }
    return &ev->stats;
}

void memory_stats_print(const MemoryStats *s, FILE *out) {
    fprintf(out, "=== Memory Statistics ===\n\n");
    fprintf(out, "Phase transitions:\n");
    fprintf(out, "  freezes:      %zu\n", s->freezes);
    fprintf(out, "  thaws:        %zu\n", s->thaws);
    fprintf(out, "  deep clones:  %zu\n", s->deep_clones);
    fprintf(out, "\nAllocations:\n");
    fprintf(out, "  arrays:       %zu\n", s->array_allocs);
    fprintf(out, "  structs:      %zu\n", s->struct_allocs);
    fprintf(out, "  closures:     %zu\n", s->closure_allocs);
    fprintf(out, "  total:        %zu\n", s->array_allocs + s->struct_allocs + s->closure_allocs);
    fprintf(out, "\nMemory footprint:\n");
    fprintf(out, "  fluid peak:   %zu bytes (%.2f KB)\n",
        s->fluid_peak_bytes, (double)s->fluid_peak_bytes / 1024.0);
    fprintf(out, "  fluid live:   %zu bytes\n", s->fluid_live_bytes);
    fprintf(out, "  fluid total:  %zu bytes (%.2f KB)\n",
        s->fluid_cumulative_bytes, (double)s->fluid_cumulative_bytes / 1024.0);
    if (s->fluid_peak_bytes > 0)
        fprintf(out, "  churn ratio:  %.1fx\n",
            (double)s->fluid_cumulative_bytes / (double)s->fluid_peak_bytes);
    fprintf(out, "  region peak:  %zu\n", s->region_peak_count);
    fprintf(out, "  region live:  %zu (%zu bytes data)\n",
        s->region_live_count, s->region_live_data_bytes);
    fprintf(out, "  region total: %zu bytes data\n", s->region_cumulative_data_bytes);
    if (s->rss_peak_kb > 0)
        fprintf(out, "  RSS peak:     %zu KB\n", s->rss_peak_kb);
    fprintf(out, "\nScope lifecycle:\n");
    fprintf(out, "  pushes:       %zu\n", s->scope_pushes);
    fprintf(out, "  pops:         %zu\n", s->scope_pops);
    fprintf(out, "  peak depth:   %zu\n", s->peak_scope_depth);
    fprintf(out, "\nCalls & bindings:\n");
    fprintf(out, "  bindings:     %zu\n", s->bindings_created);
    fprintf(out, "  fn calls:     %zu\n", s->fn_calls);
    fprintf(out, "  closure calls:%zu\n", s->closure_calls);
    fprintf(out, "\nForge blocks:   %zu\n", s->forge_blocks);
    fprintf(out, "\nGarbage collection:\n");
    fprintf(out, "  gc cycles:    %zu\n", s->gc_cycles);
    fprintf(out, "  swept fluid:  %zu (%zu bytes)\n", s->gc_swept_fluid, s->gc_bytes_swept);
    fprintf(out, "  swept regions:%zu\n", s->gc_swept_regions);
    if (s->gc_cycles > 0)
        fprintf(out, "  avg/cycle:    %.2f KB swept\n",
            (double)s->gc_bytes_swept / 1024.0 / (double)s->gc_cycles);
    fprintf(out, "\nTiming:\n");
    fprintf(out, "  gc total:     %.3f ms\n", (double)s->gc_total_ns / 1e6);
    fprintf(out, "  freeze total: %.3f ms\n", (double)s->freeze_total_ns / 1e6);
    fprintf(out, "  thaw total:   %.3f ms\n", (double)s->thaw_total_ns / 1e6);
    if (s->gc_cycles > 0)
        fprintf(out, "  avg gc cycle: %.3f ms\n",
            (double)s->gc_total_ns / 1e6 / (double)s->gc_cycles);
}
