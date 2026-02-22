#ifndef VM_H
#define VM_H

#include "chunk.h"
#include "value.h"
#include "env.h"

struct BumpArena;  /* forward declaration */

#define VM_STACK_MAX 4096
#define VM_FRAMES_MAX 256
#define VM_HANDLER_MAX 64
#define VM_DEFER_MAX 256

/* Upvalue representation for closed-over variables */
typedef struct ObjUpvalue {
    LatValue *location;        /* Points into stack when open, or &closed when closed */
    LatValue  closed;          /* Holds the value when closed */
    struct ObjUpvalue *next;   /* Linked list of open upvalues */
} ObjUpvalue;

typedef struct {
    Chunk    *chunk;
    uint8_t  *ip;      /* Instruction pointer */
    LatValue *slots;   /* Pointer to this frame's base on the value stack */
    ObjUpvalue **upvalues;  /* Array of upvalue pointers for closures */
    size_t    upvalue_count;
    LatValue *cleanup_base;  /* If non-NULL, OP_RETURN frees down to here (not slots).
                              * Used by defer bodies that share parent frame's slots. */
} CallFrame;

typedef struct {
    uint8_t  *ip;          /* Where to resume on catch */
    Chunk    *chunk;       /* Which chunk the handler is in */
    size_t    frame_index; /* Which call frame */
    LatValue *stack_top;   /* Stack top at handler registration */
} ExceptionHandler;

typedef struct {
    uint8_t  *ip;          /* Start of defer body */
    Chunk    *chunk;       /* Which chunk */
    size_t    frame_index; /* Which call frame */
    LatValue *slots;       /* Frame slots */
    uint8_t   scope_depth; /* Compiler scope depth at registration */
} VMDeferEntry;

typedef enum {
    VM_OK,
    VM_COMPILE_ERROR,
    VM_RUNTIME_ERROR,
} VMResult;

typedef struct {
    CallFrame  frames[VM_FRAMES_MAX];
    size_t     frame_count;
    LatValue   stack[VM_STACK_MAX];
    LatValue  *stack_top;
    Env       *env;        /* For global variable access */
    char      *error;      /* Runtime error message (heap-allocated) */
    ObjUpvalue *open_upvalues; /* Linked list of open upvalues */
    /* Exception handling */
    ExceptionHandler handlers[VM_HANDLER_MAX];
    size_t     handler_count;
    /* Defer stack */
    VMDeferEntry defers[VM_DEFER_MAX];
    size_t     defer_count;
    /* Struct metadata (name -> field names array) for OP_BUILD_STRUCT */
    Env       *struct_meta;
    /* Chunks allocated for functions (freed with VM) */
    Chunk    **fn_chunks;
    size_t     fn_chunk_count;
    size_t     fn_chunk_cap;
    /* Module import cache (path -> LatValue module map) */
    LatMap     module_cache;
    /* Require dedup cache (path -> bool, prevents circular requires) */
    LatMap     required_files;
    /* Extension cache (name -> LatValue map of native functions) */
    LatMap     loaded_extensions;
    /* Script directory for relative path resolution */
    char      *script_dir;
    /* Command-line arguments for args() */
    int        prog_argc;
    char     **prog_argv;
    /* Phase system: tracked variable history */
    struct {
        char *name;
        struct { char *phase; LatValue value; int line; char *fn_name; } *snapshots;
        size_t snap_count;
        size_t snap_cap;
    } *tracked_vars;
    size_t tracked_count;
    size_t tracked_cap;
    /* Phase system: pressure constraints */
    struct { char *name; char *mode; } *pressures;
    size_t pressure_count;
    size_t pressure_cap;
    /* Phase reactions: var_name → callback closures */
    struct {
        char *var_name;
        LatValue *callbacks;
        size_t cb_count;
        size_t cb_cap;
    } *reactions;
    size_t reaction_count;
    size_t reaction_cap;
    /* Phase bonds: target → deps with strategies */
    struct {
        char *target;
        char **deps;
        char **dep_strategies;
        size_t dep_count;
        size_t dep_cap;
    } *bonds;
    size_t bond_count;
    size_t bond_cap;
    /* Seed contracts: var_name → contract closure */
    struct {
        char *var_name;
        LatValue contract;
    } *seeds;
    size_t seed_count;
    size_t seed_cap;
    /* Pre-allocated buffer for native function call args (avoids malloc per call) */
    LatValue fast_args[16];
    /* Ephemeral bump arena for short-lived string temporaries */
    struct BumpArena *ephemeral;
    /* Fast cache of (tracked_count > 0) — avoids compound guard on hot path */
    bool tracking_active;
    /* True when ephemeral values exist on the stack (avoids scanning on every call) */
    bool ephemeral_on_stack;
    /* Pre-built wrapper chunk for vm_call_closure [OP_CALL, arg_count, OP_RETURN] */
    Chunk call_wrapper;
    /* Override for next vm_run frame's slots (used by defer to share parent locals) */
    LatValue *next_frame_slots;
} VM;

void vm_init(VM *vm);
void vm_free(VM *vm);

/* Print a stack trace to stderr from the VM's current call frames. */
void vm_print_stack_trace(VM *vm);

/* Run a compiled chunk. On success, returns VM_OK and sets *result.
 * On error, returns VM_RUNTIME_ERROR and vm->error is set. */
VMResult vm_run(VM *vm, Chunk *chunk, LatValue *result);

/* Clone a VM for running in a spawn thread. Clones env, shares struct_meta. */
VM *vm_clone_for_thread(VM *parent);

/* Free a child VM created by vm_clone_for_thread. Does NOT free fn_chunks or struct_meta. */
void vm_free_child(VM *child);

/* Track a dynamically compiled chunk in the VM's fn_chunks array for cleanup. */
void vm_track_chunk(VM *vm, Chunk *ch);

#endif /* VM_H */
