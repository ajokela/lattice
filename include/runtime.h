#ifndef RUNTIME_H
#define RUNTIME_H

#include "value.h"
#include "env.h"

/* ── Phase system types ──
 * Named typedefs for the anonymous structs in stackvm.h/regvm.h. */

typedef struct {
    char     *phase;
    LatValue  value;
    int       line;
    char     *fn_name;
} RTPhaseSnap;

typedef struct {
    char        *name;
    RTPhaseSnap *snapshots;
    size_t       snap_count;
    size_t       snap_cap;
} RTTrackedVar;

typedef struct {
    char *name;
    char *mode;
} RTPressure;

typedef struct {
    char     *var_name;
    LatValue *callbacks;
    size_t    cb_count;
    size_t    cb_cap;
} RTReaction;

typedef struct {
    char    *target;
    char   **deps;
    char   **dep_strategies;
    size_t   dep_count;
    size_t   dep_cap;
} RTBond;

typedef struct {
    char     *var_name;
    LatValue  contract;
} RTSeed;

/* ── LatRuntime: shared services consumed by both VMs ── */

typedef enum {
    RT_BACKEND_STACK_VM,
    RT_BACKEND_REG_VM,
} RTBackend;

typedef struct LatRuntime {
    Env  *env;            /* Global environment (native functions + globals) */
    Env  *struct_meta;    /* Struct metadata (name -> field names array) */
    char *error;          /* Error accumulator (set by natives, read by VMs) */

    /* Phase system: tracked variable history */
    RTTrackedVar *tracked_vars;
    size_t tracked_count;
    size_t tracked_cap;
    bool   tracking_active;

    /* Phase system: pressure constraints */
    RTPressure *pressures;
    size_t pressure_count;
    size_t pressure_cap;

    /* Phase reactions: var_name -> callback closures */
    RTReaction *reactions;
    size_t reaction_count;
    size_t reaction_cap;

    /* Phase bonds: target -> deps with strategies */
    RTBond *bonds;
    size_t bond_count;
    size_t bond_cap;

    /* Seed contracts: var_name -> contract closure */
    RTSeed *seeds;
    size_t seed_count;
    size_t seed_cap;

    /* Module system */
    char  *script_dir;
    LatMap module_cache;
    LatMap required_files;
    LatMap loaded_extensions;

    /* Program arguments */
    int    prog_argc;
    char **prog_argv;

    /* VM dispatch -- set by whichever VM is currently executing.
     * These allow runtime functions (natives, phase system) to call back
     * into the active VM without knowing its concrete type. */
    RTBackend backend;
    void    *active_vm;
    LatValue (*call_closure)(void *vm, LatValue *closure, LatValue *args, int argc);
    bool     (*find_local_value)(void *vm, const char *name, LatValue *out);
    int      (*current_line)(void *vm);
    bool     (*get_var_by_name)(void *vm, const char *name, LatValue *out);
    bool     (*set_var_by_name)(void *vm, const char *name, LatValue val);
} LatRuntime;

/* ── Lifecycle ── */
void lat_runtime_init(LatRuntime *rt);
void lat_runtime_free(LatRuntime *rt);

/* ── Built-in module lookup (no LatRuntime dependency) ── */
bool rt_try_builtin_import(const char *name, LatValue *out);

/* ── Thread-local current runtime ── */
void        lat_runtime_set_current(LatRuntime *rt);
LatRuntime *lat_runtime_current(void);

/* ── Phase system (shared between VMs) ── */
void  rt_record_history(LatRuntime *rt, const char *name, LatValue *val);
void  rt_fire_reactions(LatRuntime *rt, const char *name, const char *phase);
void  rt_freeze_cascade(LatRuntime *rt, const char *target_name);
char *rt_validate_seeds(LatRuntime *rt, const char *name, LatValue *val, bool consume);

#endif /* RUNTIME_H */
