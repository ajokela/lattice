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

/* Evaluator state */
typedef struct Evaluator {
    Env        *env;
    AstMode     mode;
    LatMap      struct_defs;   /* char* -> StructDecl */
    LatMap      fn_defs;       /* char* -> FnDecl */
    MemoryStats stats;
    DualHeap   *heap;
    LatVec      gc_roots;      /* shadow stack of LatValue* */
    LatVec      saved_envs;    /* stack of Env* saved during closure calls */
    bool        gc_stress;
    bool        no_regions;    /* baseline mode: skip region registration */
    size_t      lat_eval_scope; /* when > 0, top-level lat_eval bindings go here */
    LatMap      required_files; /* set of resolved paths already require()'d */
    char       *script_dir;    /* directory of the main script (for require) */
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

/* Evaluate a program. Returns heap-allocated error string or NULL on success. */
char *evaluator_run(Evaluator *ev, const Program *prog);

/* Evaluate a program incrementally (REPL mode): registers declarations and
 * executes statements, but does NOT auto-call main(). The evaluator persists
 * state between calls so bindings/functions/structs accumulate. */
char *evaluator_run_repl(Evaluator *ev, const Program *prog);

/* Get memory stats */
const MemoryStats *evaluator_stats(const Evaluator *ev);

/* Print memory stats to stream */
void memory_stats_print(const MemoryStats *stats, FILE *out);

#endif /* EVAL_H */
