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
    bool        gc_stress;
} Evaluator;

/* Create a new evaluator */
Evaluator *evaluator_new(void);

/* Free the evaluator */
void evaluator_free(Evaluator *ev);

/* Enable GC stress mode (GC after every allocation) */
void evaluator_set_gc_stress(Evaluator *ev, bool enabled);

/* Evaluate a program. Returns heap-allocated error string or NULL on success. */
char *evaluator_run(Evaluator *ev, const Program *prog);

/* Get memory stats */
const MemoryStats *evaluator_stats(const Evaluator *ev);

/* Print memory stats to stream */
void memory_stats_print(const MemoryStats *stats, FILE *out);

#endif /* EVAL_H */
