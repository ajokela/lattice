#ifndef EVAL_H
#define EVAL_H

#include "ast.h"
#include "value.h"
#include "env.h"
#include "memory.h"
#include "ds/vec.h"
#include "ds/hashmap.h"

/* Control flow signals */
typedef enum {
    CF_NONE,
    CF_RETURN,
    CF_BREAK,
    CF_CONTINUE,
} ControlFlowTag;

typedef struct {
    ControlFlowTag tag;
    LatValue       value;  /* only meaningful for CF_RETURN */
} ControlFlow;

/* Memory statistics */
typedef struct {
    size_t freezes;
    size_t thaws;
    size_t deep_clones;
    size_t array_allocs;
    size_t struct_allocs;
    size_t closure_allocs;
    size_t scope_pushes;
    size_t scope_pops;
    size_t peak_scope_depth;
    size_t current_scope_depth;
    size_t bindings_created;
    size_t fn_calls;
    size_t closure_calls;
    size_t forge_blocks;
    size_t gc_cycles;
    size_t gc_swept_fluid;
    size_t gc_swept_regions;
    size_t gc_bytes_swept;
    uint64_t gc_total_ns;
    uint64_t freeze_total_ns;
    uint64_t thaw_total_ns;
    /* Memory footprint (finalized by evaluator_stats) */
    size_t fluid_peak_bytes;
    size_t fluid_live_bytes;
    size_t fluid_cumulative_bytes;
    size_t region_peak_count;
    size_t region_live_count;
    size_t region_live_data_bytes;
    size_t region_cumulative_data_bytes;
    size_t rss_peak_kb;
} MemoryStats;

/* Evaluator result */
typedef struct {
    bool     ok;
    LatValue value;    /* valid if ok == true */
    char    *error;    /* heap-allocated error if ok == false */
    ControlFlow cf;    /* control flow signal */
} EvalResult;

/* Bond entry: tracks variables bonded to a target for phase propagation */
typedef struct {
    char  *target;       /* target variable name */
    char **deps;         /* bonded dependency variable names */
    char **dep_strategies; /* per-dep strategy: "mirror", "inverse", "gate" (parallel to deps) */
    size_t dep_count;
    size_t dep_cap;
} BondEntry;

/* Seed entry: pending contracts to validate on freeze */
typedef struct {
    char    *var_name;
    LatValue contract;    /* closure value */
} SeedEntry;

/* Pressure entry: soft constraints on fluid variables */
typedef struct {
    char *var_name;
    char *mode;           /* "no_grow", "no_shrink", "no_resize", "read_heavy" */
} PressureEntry;

/* History snapshot for temporal values */
typedef struct {
    char     *phase_name;  /* "fluid", "crystal", "unphased" */
    LatValue  value;       /* deep clone of value at this point */
} HistorySnapshot;

/* History tracking for a single variable */
typedef struct {
    HistorySnapshot *snapshots;
    size_t           count;
    size_t           cap;
} VariableHistory;

/* Tracked variable entry */
typedef struct {
    char            *name;
    VariableHistory  history;
} TrackedVar;

/* Phase reaction entry: callbacks that fire on phase transitions */
typedef struct {
    char     *var_name;
    LatValue *callbacks;   /* array of closure values */
    size_t    cb_count;
    size_t    cb_cap;
} ReactionEntry;

/* Defer entry: deferred block registered at a given scope depth */
typedef struct {
    Stmt  **body;
    size_t  body_count;
    size_t  scope_depth;
} DeferEntry;

/* Evaluator state */
typedef struct Evaluator {
    Env        *env;
    AstMode     mode;
    LatMap      struct_defs;   /* char* -> StructDecl */
    LatMap      enum_defs;     /* char* -> EnumDecl */
    LatMap      fn_defs;       /* char* -> FnDecl */
    LatMap      trait_defs;    /* char* -> TraitDecl* */
    LatMap      impl_registry; /* "Type::Trait" -> ImplBlock* */
    MemoryStats stats;
    DualHeap   *heap;
    LatVec      gc_roots;      /* shadow stack of LatValue* */
    LatVec      saved_envs;    /* stack of Env* saved during closure calls */
    bool        gc_stress;
    bool        no_regions;    /* baseline mode: skip region registration */
    size_t      lat_eval_scope; /* when > 0, top-level lat_eval bindings go here */
    LatMap      required_files; /* set of resolved paths already require()'d */
    LatMap      module_cache;  /* char* -> LatValue (cached module Maps) */
    LatMap      loaded_extensions; /* char* -> LatValue (cached extension Maps) */
    LatVec      module_exprs;  /* Expr* body wrappers kept alive for module closures */
    char       *script_dir;    /* directory of the main script (for require) */
    int         prog_argc;     /* argc from main() for args() builtin */
    char      **prog_argv;     /* argv from main() for args() builtin */
    /* Phase propagation bonds */
    BondEntry  *bonds;
    size_t      bond_count;
    size_t      bond_cap;
    /* Phase history / temporal values */
    TrackedVar *tracked_vars;
    size_t      tracked_count;
    size_t      tracked_cap;
    /* Phase reactions */
    ReactionEntry *reactions;
    size_t         reaction_count;
    size_t         reaction_cap;
    /* Seed crystals (deferred contracts) */
    SeedEntry     *seeds;
    size_t         seed_count;
    size_t         seed_cap;
    /* Phase pressure constraints */
    PressureEntry *pressures;
    size_t         pressure_count;
    size_t         pressure_cap;
    /* Defer stack */
    DeferEntry    *defer_stack;
    size_t         defer_count;
    size_t         defer_cap;
    /* Contract/assertion control */
    bool           assertions_enabled;
} Evaluator;

/* Create a new evaluator */
Evaluator *evaluator_new(void);

/* Free the evaluator */
void evaluator_free(Evaluator *ev);

/* Enable GC stress mode (GC after every allocation) */
void evaluator_set_gc_stress(Evaluator *ev, bool enabled);

/* Enable baseline mode (no region allocator, crystal stays in fluid heap) */
void evaluator_set_no_regions(Evaluator *ev, bool enabled);

/* Set the script directory for resolving require() paths */
void evaluator_set_script_dir(Evaluator *ev, const char *dir);

/* Store argc/argv for the args() builtin */
void evaluator_set_argv(Evaluator *ev, int argc, char **argv);

/* Enable/disable debug_assert() and contracts */
void evaluator_set_assertions(Evaluator *ev, bool enabled);

/* Evaluate a program. Returns heap-allocated error string or NULL on success. */
char *evaluator_run(Evaluator *ev, const Program *prog);

/* Evaluate a program incrementally (REPL mode): registers declarations and
 * executes statements, but does NOT auto-call main(). The evaluator persists
 * state between calls so bindings/functions/structs accumulate. */
char *evaluator_run_repl(Evaluator *ev, const Program *prog);

/* Like evaluator_run_repl, but returns the last statement's result value
 * instead of freeing it.  Caller must value_free the returned result.
 * On error, result.ok == false and result.error is set. */
EvalResult evaluator_run_repl_result(Evaluator *ev, const Program *prog);

/* Return the repr string for a value.  For structs with a "repr" closure
 * field, calls the closure.  Otherwise falls back to value_repr().
 * Caller must free() the returned string. */
char *eval_repr(Evaluator *ev, const LatValue *v);

/* Run tests from a parsed program. Returns exit code (0 = all pass). */
int evaluator_run_tests(Evaluator *ev, const Program *prog);

/* Get memory stats */
const MemoryStats *evaluator_stats(const Evaluator *ev);

/* Print memory stats to stream */
void memory_stats_print(const MemoryStats *stats, FILE *out);

#endif /* EVAL_H */
