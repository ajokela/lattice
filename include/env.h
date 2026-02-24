#ifndef ENV_H
#define ENV_H

#include "value.h"
#include "ds/hashmap.h"

/* A scope is a hash map from variable names to LatValue */
typedef LatMap Scope;

/* Environment: a stack of scopes implementing lexical scoping */
struct Env {
    Scope  *scopes;
    size_t  count;
    size_t  cap;
    size_t  refcount;
    bool    arena_backed;
};

/* Create a new environment with one (root) scope */
Env *env_new(void);

/* Free the environment and all its scopes */
void env_free(Env *env);

/* Increment reference count (for shared closure environments) */
void env_retain(Env *env);

/* Decrement reference count and free when it reaches 0 */
void env_release(Env *env);

/* Push a fresh scope */
void env_push_scope(Env *env);

/* Pop the top-most scope */
void env_pop_scope(Env *env);

/* Define a new binding in the current scope */
void env_define(Env *env, const char *name, LatValue value);

/* Define a new binding at a specific scope index */
void env_define_at(Env *env, size_t scope_idx, const char *name, LatValue value);

/* Look up a name. Returns a deep clone if found, or a zero-type (VAL_UNIT with type=-1 sentinel) if not. */
/* Returns true if found and writes to *out */
bool env_get(const Env *env, const char *name, LatValue *out);

/* Look up a name without cloning. Returns pointer to value or NULL if not found. */
LatValue *env_get_ref(const Env *env, const char *name);

/* Look up with pre-computed hash (avoids rehashing constant keys). */
LatValue *env_get_ref_prehashed(const Env *env, const char *name, size_t hash);

/* Update an existing binding. Returns false if not defined. */
bool env_set(Env *env, const char *name, LatValue value);

/* Remove a binding (consuming semantics for strict freeze). Returns true if found. */
bool env_remove(Env *env, const char *name, LatValue *out);

/* Deep-clone the entire environment (for closure capture) */
Env *env_clone(const Env *env);

/* Iterate all values in all scopes (for GC marking) */
typedef void (*EnvIterFn)(LatValue *value, void *ctx);
void env_iter_values(Env *env, EnvIterFn fn, void *ctx);

#endif /* ENV_H */
