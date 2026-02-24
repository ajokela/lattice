#include "env.h"
#include "memory.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_SCOPE_CAP 8

static Scope scope_new(void) {
    return lat_map_new(sizeof(LatValue));
}

static void scope_free_values(const char *key, void *value, void *ctx) {
    (void)key; (void)ctx;
    LatValue *v = (LatValue *)value;
    value_free(v);
}

static void scope_free(Scope *s) {
    lat_map_iter(s, scope_free_values, NULL);
    lat_map_free(s);
}

Env *env_new(void) {
    Env *env = malloc(sizeof(Env));
    env->cap = INITIAL_SCOPE_CAP;
    env->count = 1;
    env->refcount = 1;
    env->arena_backed = false;
    env->scopes = malloc(env->cap * sizeof(Scope));
    env->scopes[0] = scope_new();
    return env;
}

void env_retain(Env *env) {
    if (env) env->refcount++;
}

void env_release(Env *env) {
    if (!env) return;
    if (--env->refcount == 0)
        env_free(env);
}

void env_free(Env *env) {
    if (!env) return;
    if (env->arena_backed) return;
    for (size_t i = 0; i < env->count; i++) {
        scope_free(&env->scopes[i]);
    }
    free(env->scopes);
    free(env);
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
    if (existing) {
        value_free(existing);
    }
    lat_map_set(scope, name, &value);
}

bool env_get(const Env *env, const char *name, LatValue *out) {
    if (env->count == 1) {
        LatValue *v = lat_map_get(&env->scopes[0], name);
        if (v) { *out = value_deep_clone(v); return true; }
        return false;
    }
    for (size_t i = env->count; i > 0; i--) {
        LatValue *v = lat_map_get(&env->scopes[i - 1], name);
        if (v) {
            *out = value_deep_clone(v);
            return true;
        }
    }
    return false;
}

LatValue *env_get_ref(const Env *env, const char *name) {
    if (env->count == 1)
        return lat_map_get(&env->scopes[0], name);
    for (size_t i = env->count; i > 0; i--) {
        LatValue *v = lat_map_get(&env->scopes[i - 1], name);
        if (v) return v;
    }
    return NULL;
}

LatValue *env_get_ref_prehashed(const Env *env, const char *name, size_t hash) {
    if (env->count == 1)
        return lat_map_get_prehashed(&env->scopes[0], name, hash);
    for (size_t i = env->count; i > 0; i--) {
        LatValue *v = lat_map_get_prehashed(&env->scopes[i - 1], name, hash);
        if (v) return v;
    }
    return NULL;
}

bool env_set(Env *env, const char *name, LatValue value) {
    if (env->count == 1) {
        LatValue *existing = lat_map_get(&env->scopes[0], name);
        if (existing) {
            value_free(existing);
            lat_map_set(&env->scopes[0], name, &value);
            return true;
        }
        return false;
    }
    for (size_t i = env->count; i > 0; i--) {
        LatValue *existing = lat_map_get(&env->scopes[i - 1], name);
        if (existing) {
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
    LatValue cloned = value_deep_clone(v);
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
        dst->count = src->count;  /* preserve tombstone count for probe chains */
        dst->live = src->live;
        dst->entries = lat_calloc_routed(src->cap, sizeof(LatMapEntry));
        for (size_t j = 0; j < src->cap; j++) {
            /* Point value to inline buffer for all entries */
            dst->entries[j].value = dst->entries[j]._ibuf;
            if (src->entries[j].state == MAP_OCCUPIED) {
                dst->entries[j].state = MAP_OCCUPIED;
                dst->entries[j].key = lat_strdup_routed(src->entries[j].key);
                LatValue *sv = (LatValue *)src->entries[j].value;
                LatValue cloned = value_deep_clone(sv);
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
    new_env->cap = env->cap;
    new_env->count = env->count;
    new_env->refcount = 1;
    new_env->arena_backed = false;
    new_env->scopes = malloc(new_env->cap * sizeof(Scope));

    CloneCtx ctx;
    ctx.dest = new_env;
    for (size_t i = 0; i < env->count; i++) {
        new_env->scopes[i] = scope_new();
        ctx.scope_idx = i;
        lat_map_iter(&env->scopes[i], clone_entry, &ctx);
    }
    return new_env;
}

static void iter_scope_values(const char *key, void *value, void *ctx) {
    (void)key;
    void **args = (void **)ctx;
    EnvIterFn fn = (EnvIterFn)args[0];
    void *user_ctx = args[1];
    fn((LatValue *)value, user_ctx);
}

void env_iter_values(Env *env, EnvIterFn fn, void *ctx) {
    void *args[2] = { (void *)fn, ctx };
    for (size_t i = 0; i < env->count; i++) {
        lat_map_iter(&env->scopes[i], iter_scope_values, args);
    }
}
