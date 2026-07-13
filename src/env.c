#include "env.h"
#include "string_ops.h"
#include "memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h> /* LAT-482: atomic Env refcount (shared across threads) */

#define INITIAL_SCOPE_CAP 8

/* Tree-walk closures capture lexical bindings through shared cells.  The
 * marker is intentionally outside PhaseTag's public values and never escapes
 * through env_get/env_get_ref; user-created Refs remain ordinary values. */
#define CAPTURE_CELL_PHASE ((PhaseTag)0x7f)
#define WEAK_CLOSURE_PHASE ((PhaseTag)0x7e)

static bool is_capture_cell(const LatValue *v) { return v && v->type == VAL_REF && v->phase == CAPTURE_CELL_PHASE; }

static bool is_weak_closure(const LatValue *v) { return v && v->type == VAL_CLOSURE && v->phase == WEAK_CLOSURE_PHASE; }

static LatValue capture_cell_clone(const LatValue *cell) {
    ref_lock(cell->as.ref.ref);
    LatValue value = value_deep_clone(&cell->as.ref.ref->value);
    ref_unlock(cell->as.ref.ref);
    return value;
}

/* Consumes value while preserving the capture-cell wrapper shared by every
 * closure that closed over this binding.  Cell contents stay detached from a
 * particular evaluator's fluid heap because they can outlive its scope. */
static void capture_cell_set(LatValue *cell, LatValue value) {
    LatValue detached = value_detach(&value);
    value_free(&value);
    ref_lock(cell->as.ref.ref);
    value_free(&cell->as.ref.ref->value);
    cell->as.ref.ref->value = detached;
    ref_unlock(cell->as.ref.ref);
}

static Scope scope_new(void) { return lat_map_new(sizeof(LatValue)); }

static void scope_free_values(const char *key, void *value, void *ctx) {
    (void)key;
    (void)ctx;
    LatValue *v = (LatValue *)value;
    value_free(v);
}

static void scope_free(Scope *s) {
    lat_map_iter(s, scope_free_values, NULL);
    lat_map_free(s);
}

Env *env_new(void) {
    Env *env = malloc(sizeof(Env));
    if (!env) return NULL;
    env->cap = INITIAL_SCOPE_CAP;
    env->count = 1;
    env->refcount = 1;
    env->arena_backed = false;
    env->scopes = malloc(env->cap * sizeof(Scope));
    if (!env->scopes) {
        free(env);
        return NULL;
    }
    env->scopes[0] = scope_new();
    return env;
}

void env_retain(Env *env) {
    /* Relaxed: a retain only happens while holding a live handle, so its
     * visibility piggybacks on whatever published the handle (pthread_create,
     * channel/mutex). Mirrors ref_retain / crystal_region_retain. */
    if (env) atomic_fetch_add_explicit(&env->refcount, 1, memory_order_relaxed);
}

void env_release(Env *env) {
    if (!env) return;
    /* acq_rel so every prior write through this env happens-before the free,
     * plus an acquire fence on the final release before tearing it down. */
    if (atomic_fetch_sub_explicit(&env->refcount, 1, memory_order_acq_rel) == 1) {
        atomic_thread_fence(memory_order_acquire);
        env_free(env);
    }
}

void env_free(Env *env) {
    if (!env) return;
    if (env->arena_backed) return;
    for (size_t i = 0; i < env->count; i++) { scope_free(&env->scopes[i]); }
    free(env->scopes);
    free(env);
}

/* Replace every bound value with a thread-independent (malloc-backed) copy.
 * A spawned thread stores values whose backing lives in its thread-local fluid
 * heap into its (cloned) env; that heap is freed when the thread exits, while
 * the env is freed later by the parent. Detaching the values before the heap is
 * torn down keeps them valid (avoids use-after-free / double-free). */
static void env_detach_entry(const char *key, void *value, void *ctx) {
    (void)key;
    (void)ctx;
    LatValue *v = (LatValue *)value;
    /* LAT-538: a VAL_ITERATOR cannot be made thread-independent by value_detach —
     * value_clone_impl only bumps the iterator refcount (shallow share), so a
     * "detached" copy still aliases this thread's heap-backed iterator state,
     * which dual_heap_free then frees -> UAF when the parent later frees this
     * env. The env being detached belongs to a dying thread (discarded at
     * teardown), so free the value HERE (refcount-aware: a handle inherited from
     * the parent merely decrements; a child-created iterator's state is released
     * while this heap is still active) and nil the slot so env_free is a no-op. */
    if (v->type == VAL_ITERATOR) {
        value_free(v);
        *v = value_nil();
        return;
    }
    LatValue detached = value_detach(v);
    value_free(v);
    *v = detached;
}
void env_detach_values(Env *env) {
    if (!env || env->arena_backed) return;
    for (size_t i = 0; i < env->count; i++) { lat_map_iter(&env->scopes[i], env_detach_entry, NULL); }
}

void env_push_scope(Env *env) {
    if (env->count >= env->cap) {
        env->cap *= 2;
        env->scopes = realloc(env->scopes, env->cap * sizeof(Scope));
    }
    env->scopes[env->count++] = scope_new();
}

void env_pop_scope(Env *env) {
    if (env->count <= 1) return;
    env->count--;
    scope_free(&env->scopes[env->count]);
}

void env_define(Env *env, const char *name, LatValue value) {
    if (env->count == 0) return;
    env_define_at(env, env->count - 1, name, value);
}

void env_define_at(Env *env, size_t scope_idx, const char *name, LatValue value) {
    if (scope_idx >= env->count) return;
    Scope *scope = &env->scopes[scope_idx];
    LatValue *existing = lat_map_get(scope, name);
    if (is_capture_cell(existing)) {
        capture_cell_set(existing, value);
        return;
    }
    if (existing) { value_free(existing); }
    lat_map_set(scope, name, &value);
}

bool env_get(const Env *env, const char *name, LatValue *out) {
    if (env->count == 1) {
        LatValue *v = lat_map_get(&env->scopes[0], name);
        if (v) {
            *out = is_capture_cell(v) ? capture_cell_clone(v) : value_deep_clone(v);
            return true;
        }
        return false;
    }
    for (size_t i = env->count; i > 0; i--) {
        LatValue *v = lat_map_get(&env->scopes[i - 1], name);
        if (v) {
            *out = is_capture_cell(v) ? capture_cell_clone(v) : value_deep_clone(v);
            return true;
        }
    }
    return false;
}

LatValue *env_get_ref(const Env *env, const char *name) {
    if (env->count == 1) {
        LatValue *v = lat_map_get(&env->scopes[0], name);
        return is_capture_cell(v) ? &v->as.ref.ref->value : v;
    }
    for (size_t i = env->count; i > 0; i--) {
        LatValue *v = lat_map_get(&env->scopes[i - 1], name);
        if (v) return is_capture_cell(v) ? &v->as.ref.ref->value : v;
    }
    return NULL;
}

LatValue *env_get_ref_prehashed(const Env *env, const char *name, size_t hash) {
    if (env->count == 1) {
        LatValue *v = lat_map_get_prehashed(&env->scopes[0], name, hash);
        return is_capture_cell(v) ? &v->as.ref.ref->value : v;
    }
    for (size_t i = env->count; i > 0; i--) {
        LatValue *v = lat_map_get_prehashed(&env->scopes[i - 1], name, hash);
        if (v) return is_capture_cell(v) ? &v->as.ref.ref->value : v;
    }
    return NULL;
}

bool env_set(Env *env, const char *name, LatValue value) {
    if (env->count == 1) {
        LatValue *existing = lat_map_get(&env->scopes[0], name);
        if (existing) {
            if (is_capture_cell(existing)) {
                capture_cell_set(existing, value);
                return true;
            }
            value_free(existing);
            lat_map_set(&env->scopes[0], name, &value);
            return true;
        }
        return false;
    }
    for (size_t i = env->count; i > 0; i--) {
        LatValue *existing = lat_map_get(&env->scopes[i - 1], name);
        if (existing) {
            if (is_capture_cell(existing)) {
                capture_cell_set(existing, value);
                return true;
            }
            value_free(existing);
            lat_map_set(&env->scopes[i - 1], name, &value);
            return true;
        }
    }
    return false;
}

bool env_remove(Env *env, const char *name, LatValue *out) {
    for (size_t i = env->count; i > 0; i--) {
        LatValue *existing = lat_map_get(&env->scopes[i - 1], name);
        if (existing) {
            if (is_capture_cell(existing)) {
                LatValue copy = capture_cell_clone(existing);
                value_free(existing);
                *existing = value_unit();
                lat_map_remove(&env->scopes[i - 1], name);
                if (out) *out = copy;
                else value_free(&copy);
                return true;
            }
            if (out) *out = *existing;
            /* Remove without freeing the value (caller takes ownership) */
            /* We need to copy the value out before removing */
            LatValue copy = *existing;
            /* Set to unit so map_remove doesn't free our data */
            memset(existing, 0, sizeof(LatValue));
            existing->type = VAL_UNIT;
            lat_map_remove(&env->scopes[i - 1], name);
            if (out) *out = copy;
            else value_free(&copy);
            return true;
        }
    }
    return false;
}

/* Deep clone helper — normal (non-arena) path */
typedef struct {
    Env *dest;
    size_t scope_idx;
} CloneCtx;

static void clone_entry(const char *key, void *value, void *ctx) {
    CloneCtx *cc = (CloneCtx *)ctx;
    LatValue *v = (LatValue *)value;
    LatValue cloned = is_capture_cell(v) ? capture_cell_clone(v) : value_deep_clone(v);
    lat_map_set(&cc->dest->scopes[cc->scope_idx], key, &cloned);
}

/* Arena-routed clone: build scope map internals directly via arena alloc */
static Env *env_clone_arena(const Env *env) {
    Env *new_env = lat_alloc_routed(sizeof(Env));
    new_env->cap = env->cap;
    new_env->count = env->count;
    new_env->refcount = 1;
    new_env->scopes = lat_alloc_routed(new_env->cap * sizeof(Scope));

    new_env->arena_backed = true;
    for (size_t i = 0; i < env->count; i++) {
        const LatMap *src = &env->scopes[i];
        LatMap *dst = &new_env->scopes[i];
        dst->value_size = src->value_size;
        dst->cap = src->cap;
        dst->count = src->count; /* preserve tombstone count for probe chains */
        dst->live = src->live;
        dst->cmi = NULL; /* env scopes are never crystallized */
        dst->entries = lat_calloc_routed(src->cap, sizeof(LatMapEntry));
        for (size_t j = 0; j < src->cap; j++) {
            /* Point value to inline buffer for all entries */
            dst->entries[j].value = dst->entries[j]._ibuf;
            if (src->entries[j].state == MAP_OCCUPIED) {
                dst->entries[j].state = MAP_OCCUPIED;
                dst->entries[j].key = lat_strdup_routed(src->entries[j].key);
                LatValue *sv = (LatValue *)src->entries[j].value;
                LatValue cloned = is_capture_cell(sv) ? capture_cell_clone(sv) : value_deep_clone(sv);
                *(LatValue *)dst->entries[j].value = cloned;
            } else if (src->entries[j].state == MAP_TOMBSTONE) {
                dst->entries[j].state = MAP_TOMBSTONE;
            }
        }
    }
    return new_env;
}

Env *env_clone(const Env *env) {
    if (value_get_arena()) return env_clone_arena(env);

    Env *new_env = malloc(sizeof(Env));
    if (!new_env) return NULL;
    new_env->cap = env->cap;
    new_env->count = env->count;
    new_env->refcount = 1;
    new_env->arena_backed = false;
    new_env->scopes = malloc(new_env->cap * sizeof(Scope));
    if (!new_env->scopes) {
        free(new_env);
        return NULL;
    }

    CloneCtx ctx;
    ctx.dest = new_env;
    for (size_t i = 0; i < env->count; i++) {
        new_env->scopes[i] = scope_new();
        ctx.scope_idx = i;
        lat_map_iter(&env->scopes[i], clone_entry, &ctx);
    }
    return new_env;
}

/* Capture the current lexical environment without changing Env's public
 * representation. Existing bindings are promoted in place to shared cells;
 * the returned snapshot retains those same cells, so later outer writes and
 * sibling closures observe one binding. Unlike env_clone, this is deliberately
 * aliasing and is used only by the tree-walk closure constructor. */
Env *env_capture(Env *env) {
    Env *captured = malloc(sizeof(Env));
    if (!captured) return NULL;
    captured->cap = env->cap;
    captured->count = 0;
    captured->refcount = 1;
    captured->arena_backed = false;
    captured->scopes = malloc(captured->cap * sizeof(Scope));
    if (!captured->scopes) {
        free(captured);
        return NULL;
    }

    for (size_t i = 0; i < env->count; i++) {
        captured->scopes[i] = scope_new();
        captured->count++;
        Scope *source = &env->scopes[i];
        for (size_t j = 0; j < source->cap; j++) {
            if (source->entries[j].state != MAP_OCCUPIED) continue;
            LatValue *binding = (LatValue *)source->entries[j].value;
            if (is_weak_closure(binding)) {
                LatValue strong = value_deep_clone(binding);
                lat_map_set(&captured->scopes[i], source->entries[j].key, &strong);
                continue;
            }
            if (!is_capture_cell(binding)) {
                LatValue original = *binding;
                LatValue cell = value_ref(original);
                if (cell.type != VAL_REF) {
                    env_free(captured);
                    return NULL;
                }
                value_free(&original);
                cell.phase = CAPTURE_CELL_PHASE;
                *binding = cell;
            }
            LatValue alias = value_deep_clone(binding);
            lat_map_set(&captured->scopes[i], source->entries[j].key, &alias);
        }
    }
    return captured;
}

/* Replace a recursive closure's self-cell in its captured environment with a
 * non-owning closure alias. Reads clone that alias into a normal owning
 * closure, while the stored alias breaks captured_env -> closure ->
 * captured_env reference cycles at scope exit. */
void env_weaken_recursive_binding(Env *captured, size_t scope_idx, const char *name, const LatValue *closure) {
    if (!captured || scope_idx >= captured->count || !closure || closure->type != VAL_CLOSURE) return;
    LatValue *slot = (LatValue *)lat_map_get(&captured->scopes[scope_idx], name);
    if (!is_capture_cell(slot)) return;

    /* The alias lives behind a detached capture cell graph that the fluid GC
     * intentionally does not traverse, so its metadata must be detached too. */
    LatValue weak = value_detach(closure);
    if (weak.type != VAL_CLOSURE) return;
    if (weak.as.closure.captured_env) env_release(weak.as.closure.captured_env); /* alias borrows */
    weak.as.closure.upvalue_count = (uint32_t)closure->phase;
    weak.phase = WEAK_CLOSURE_PHASE;
    value_free(slot);
    *slot = weak;
}

static void iter_scope_values(const char *key, void *value, void *ctx) {
    (void)key;
    void **args = (void **)ctx;
    EnvIterFn fn = (EnvIterFn)args[0];
    void *user_ctx = args[1];
    fn((LatValue *)value, user_ctx);
}

void env_iter_values(Env *env, EnvIterFn fn, void *ctx) {
    void *args[2] = {(void *)fn, ctx};
    for (size_t i = 0; i < env->count; i++) { lat_map_iter(&env->scopes[i], iter_scope_values, args); }
}

/* ── Spellcheck suggestion for undefined variables ── */

typedef struct {
    const char *target;
    const char *best;
    int best_dist;
} EnvSimilarCtx;

static void env_similar_iter(const char *key, void *value, void *ctx) {
    (void)value;
    EnvSimilarCtx *sc = (EnvSimilarCtx *)ctx;
    /* Skip internal names */
    if (key[0] == '_' && key[1] == '_') return;
    int d = lat_levenshtein(sc->target, key);
    if (d > 0 && d < sc->best_dist) {
        sc->best_dist = d;
        sc->best = key;
    }
}

const char *env_find_similar_name(const Env *env, const char *name) {
    if (!env || !name) return NULL;
    EnvSimilarCtx ctx = {.target = name, .best = NULL, .best_dist = 3};
    for (size_t i = 0; i < env->count; i++) { lat_map_iter(&env->scopes[i], env_similar_iter, &ctx); }
    return ctx.best;
}
