#ifndef STACKVM_H
#define STACKVM_H

#include "chunk.h"
#include "value.h"
#include "env.h"
#include "runtime.h"

struct BumpArena; /* forward declaration */
struct Debugger;  /* forward declaration */

#define STACKVM_STACK_MAX   4096
#define STACKVM_FRAMES_MAX  256
#define STACKVM_HANDLER_MAX 64
#define STACKVM_DEFER_MAX   256

/* Upvalue representation for closed-over variables */
typedef struct ObjUpvalue {
    LatValue *location;      /* Points into stack when open, or &closed when closed */
    LatValue closed;         /* Holds the value when closed */
    struct ObjUpvalue *next; /* Linked list of open upvalues */
} ObjUpvalue;

typedef struct {
    Chunk *chunk;
    uint8_t *ip;           /* Instruction pointer */
    LatValue *slots;       /* Pointer to this frame's base on the value stack */
    ObjUpvalue **upvalues; /* Array of upvalue pointers for closures */
    size_t upvalue_count;
    LatValue *cleanup_base; /* If non-NULL, OP_RETURN frees down to here (not slots).
                             * Used by defer bodies that share parent frame's slots. */
} StackCallFrame;

typedef struct {
    uint8_t *ip;         /* Where to resume on catch */
    Chunk *chunk;        /* Which chunk the handler is in */
    size_t frame_index;  /* Which call frame */
    LatValue *stack_top; /* Stack top at handler registration */
} StackExceptionHandler;

typedef struct {
    uint8_t *ip;         /* Start of defer body */
    Chunk *chunk;        /* Which chunk */
    size_t frame_index;  /* Which call frame */
    LatValue *slots;     /* Frame slots */
    uint8_t scope_depth; /* Compiler scope depth at registration */
} StackDeferEntry;

typedef enum {
    STACKVM_OK,
    STACKVM_COMPILE_ERROR,
    STACKVM_RUNTIME_ERROR,
} StackVMResult;

typedef struct {
    StackCallFrame frames[STACKVM_FRAMES_MAX];
    size_t frame_count;
    LatValue stack[STACKVM_STACK_MAX];
    LatValue *stack_top;
    Env *env;                  /* For global variable access */
    char *error;               /* Runtime error message (heap-allocated) */
    ObjUpvalue *open_upvalues; /* Linked list of open upvalues */
    /* Exception handling */
    StackExceptionHandler handlers[STACKVM_HANDLER_MAX];
    size_t handler_count;
    /* Defer stack */
    StackDeferEntry defers[STACKVM_DEFER_MAX];
    size_t defer_count;
    /* Struct metadata (name -> field names array) for OP_BUILD_STRUCT */
    Env *struct_meta;
    /* Chunks allocated for functions (freed with StackVM) */
    Chunk **fn_chunks;
    size_t fn_chunk_count;
    size_t fn_chunk_cap;
    /* Module import cache (path -> LatValue module map), per-StackVM for thread isolation */
    LatMap module_cache;
    /* Pre-allocated buffer for native function call args (avoids malloc per call) */
    LatValue fast_args[16];
    /* Ephemeral bump arena for short-lived string temporaries */
    struct BumpArena *ephemeral;
    /* True when ephemeral values exist on the stack (avoids scanning on every call) */
    bool ephemeral_on_stack;
    /* Pre-built wrapper chunk for stackvm_call_closure [OP_CALL, arg_count, OP_RETURN] */
    Chunk call_wrapper;
    /* Override for next stackvm_run frame's slots (used by defer to share parent locals) */
    LatValue *next_frame_slots;
    /* Shared runtime (not owned by StackVM) */
    LatRuntime *rt;
    /* Interactive debugger (NULL when not debugging) */
    struct Debugger *debugger;
} StackVM;

void stackvm_init(StackVM *vm, LatRuntime *rt);
void stackvm_free(StackVM *vm);

/* Print a stack trace to stderr from the StackVM's current call frames. */
void stackvm_print_stack_trace(StackVM *vm);

/* Run a compiled chunk. On success, returns STACKVM_OK and sets *result.
 * On error, returns STACKVM_RUNTIME_ERROR and vm->error is set. */
StackVMResult stackvm_run(StackVM *vm, Chunk *chunk, LatValue *result);

/* Clone a StackVM for running in a spawn thread. Clones env, shares struct_meta. */
StackVM *stackvm_clone_for_thread(StackVM *parent);

/* Free a child StackVM created by stackvm_clone_for_thread. Does NOT free fn_chunks or struct_meta. */
void stackvm_free_child(StackVM *child);

/* Track a dynamically compiled chunk in the StackVM's fn_chunks array for cleanup. */
void stackvm_track_chunk(StackVM *vm, Chunk *ch);

#endif /* STACKVM_H */
