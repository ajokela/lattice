#ifndef REGVM_H
#define REGVM_H

#include "regopcode.h"
#include "value.h"
#include "env.h"
#include "chunk.h"
#include "ast.h"
#include "memory.h"
#include "runtime.h"

/* ── Register VM structures ── */

#define REGVM_REG_MAX     256
#define REGVM_FRAMES_MAX  64
#define REGVM_CONST_MAX   65536
#define REGVM_HANDLER_MAX 64
#define REGVM_DEFER_MAX   256

/* Forward declarations */
struct ObjUpvalue;

/* A "RegChunk" stores 32-bit instructions + a constant pool.
 * We reuse LatValue constants from the existing Chunk infra. */
#define REGCHUNK_MAGIC 0x524C4154 /* "RLAT" */

typedef struct RegChunk {
    uint32_t magic; /* REGCHUNK_MAGIC — distinguishes from stack-VM Chunk */
    RegInstr *code; /* 32-bit instruction array */
    size_t code_len;
    size_t code_cap;
    LatValue *constants; /* Constant pool */
    size_t const_len;
    size_t const_cap;
    int *lines; /* Line number per instruction (parallel to code) */
    size_t lines_len;
    size_t lines_cap;
    char **local_names; /* Debug: register -> variable name */
    size_t local_name_cap;
    char *name;            /* Debug: function name */
    uint8_t *param_phases; /* Phase constraints per parameter (AstPhase enum) */
    int param_phase_count;
    int default_count;   /* LAT-451: # params with defaults, for cross-backend arity check */
    char **export_names; /* Module export list (NULL = export-all) */
    size_t export_count;
    bool has_exports; /* true if module uses explicit exports */
    /* Imported modules keep their globals private while their exported
     * closures retain access to them. Nested function chunks borrow this
     * environment; only the module's root chunk owns it. */
    Env *module_env;
    bool owns_module_env;
    uint8_t max_reg; /* High-water register count (for bounded init/cleanup) */
    PICTable pic;    /* Polymorphic inline cache for method dispatch */
} RegChunk;

RegChunk *regchunk_new(void);
void regchunk_free(RegChunk *c);
size_t regchunk_write(RegChunk *c, RegInstr instr, int line);
size_t regchunk_add_constant(RegChunk *c, LatValue val);
void regchunk_set_local_name(RegChunk *c, size_t reg, const char *name);

/* Call frame for the register VM */
typedef struct {
    RegChunk *chunk;
    RegInstr *ip;     /* Instruction pointer */
    LatValue *regs;   /* Base of register window (points into reg stack) */
    size_t reg_count; /* Number of registers used in this frame */
    struct ObjUpvalue **upvalues;
    size_t upvalue_count;
    uint8_t caller_result_reg; /* Register in CALLER's frame to store return value */
} RegCallFrame;

typedef enum {
    REGVM_OK,
    REGVM_COMPILE_ERROR,
    REGVM_RUNTIME_ERROR,
} RegVMResult;

/* Exception handler entry */
typedef struct {
    RegInstr *ip; /* Catch block IP */
    RegChunk *chunk;
    size_t frame_index;
    size_t reg_stack_top;
    uint8_t error_reg;  /* Register to store error value */
    size_t defer_count; /* Defer stack depth at handler registration */
} RegHandler;

/* Defer entry */
typedef struct {
    LatValue closure; /* Hidden zero-argument cleanup closure */
    size_t frame_index;
    int scope_depth; /* Scope depth when defer was registered */
} RegDefer;

/* The register VM */
typedef struct RegVM {
    RegCallFrame frames[REGVM_FRAMES_MAX];
    int frame_count;
    LatValue reg_stack[REGVM_REG_MAX * REGVM_FRAMES_MAX];
    size_t reg_stack_top; /* Next available register slot in reg_stack */
    Env *env;             /* Global environment */
    char *error;          /* Runtime error message (heap-allocated) */
    struct ObjUpvalue *open_upvalues;
    /* Struct metadata (name -> field names) */
    Env *struct_meta;
    /* Function chunks allocated (freed with VM) */
    RegChunk **fn_chunks;
    size_t fn_chunk_count;
    size_t fn_chunk_cap;
    /* Exception handlers */
    RegHandler handlers[REGVM_HANDLER_MAX];
    size_t handler_count;
    /* Defer stack */
    RegDefer defers[REGVM_DEFER_MAX];
    size_t defer_count;
    bool unwinding_defers;
    /* Per-VM module cache (for OP_IMPORT in dispatch loop) */
    LatMap *module_cache;
    /* Ephemeral bump arena */
    BumpArena *ephemeral;
    /* Shared runtime (not owned by RegVM) */
    LatRuntime *rt;
} RegVM;

void regvm_init(RegVM *vm, LatRuntime *rt);
void regvm_free(RegVM *vm);
RegVMResult regvm_run(RegVM *vm, RegChunk *chunk, LatValue *result);
RegVMResult regvm_run_repl(RegVM *vm, RegChunk *chunk, LatValue *result);
void regvm_track_chunk(RegVM *vm, RegChunk *ch);

/* Clone a RegVM for running in a spawn/async thread. Clones env, shares struct_meta. */
RegVM *regvm_clone_for_thread(RegVM *parent);

/* Free a child RegVM created by regvm_clone_for_thread. */
void regvm_free_child(RegVM *child);

/* Free the child's live register stack (and nil the slots) on the worker thread,
 * before its heap is torn down — avoids the spawn-teardown UAF (LAT-538/541). */
void regvm_free_child_registers(RegVM *child);

/* ── Register compiler ── */

/* Compile a program to register-based bytecode. */
RegChunk *reg_compile(const Program *prog, char **error);

/* Compile for REPL (keeps last expression value). */
RegChunk *reg_compile_repl(const Program *prog, char **error);

/* Compile a module (no auto-call main). */
RegChunk *reg_compile_module(const Program *prog, char **error);

/* Free known enums (call on exit). */
void reg_compiler_free_known_enums(void);

/* Verify a deserialized (untrusted) register chunk before execution. Returns
 * NULL if the chunk and all of its sub-chunks are well-formed; otherwise a
 * heap-allocated error string (caller frees) describing the first violation. */
char *regchunk_verify(const RegChunk *c);

#endif /* REGVM_H */
