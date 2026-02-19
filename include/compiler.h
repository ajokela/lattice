#ifndef COMPILER_H
#define COMPILER_H

#include "ast.h"
#include "chunk.h"

typedef struct {
    char *name;
    int   depth;       /* Scope depth (-1 = uninitialized) */
    bool  is_captured; /* True if captured as upvalue by an inner function */
} Local;

typedef struct {
    uint8_t index;     /* Stack slot in enclosing function */
    bool    is_local;  /* true = local in immediate enclosing, false = upvalue in enclosing */
} CompilerUpvalue;

typedef enum { FUNC_SCRIPT, FUNC_FUNCTION, FUNC_CLOSURE } FunctionType;

typedef struct Compiler {
    struct Compiler *enclosing;  /* enclosing compiler (for upvalue resolution) */
    Chunk       *chunk;
    FunctionType type;
    char        *func_name;     /* name of the function being compiled (NULL for script) */
    int          arity;         /* parameter count */
    Local       *locals;
    size_t       local_count;
    size_t       local_cap;
    CompilerUpvalue *upvalues;
    size_t       upvalue_count;
    int          scope_depth;
    /* Break/continue tracking */
    size_t      *break_jumps;
    size_t       break_count;
    size_t       break_cap;
    size_t       loop_start;
    int          loop_depth;
    size_t       loop_break_local_count;    /* locals to keep on break */
    size_t       loop_continue_local_count; /* locals to keep on continue */
    /* Ensure contracts (postconditions) for the current function */
    ContractClause *contracts;
    size_t       contract_count;
} Compiler;

/* Compile a program to bytecode. Returns a Chunk on success.
 * On error, returns NULL and sets *error to a heap-allocated message. */
Chunk *compile(const Program *prog, char **error);

/* Compile a module (does not auto-call main). */
Chunk *compile_module(const Program *prog, char **error);

/* Compile for REPL: like compile_module but keeps last expression on stack
 * as the return value, and does NOT free known enums (they persist across
 * REPL iterations). Call compiler_free_known_enums() on REPL exit. */
Chunk *compile_repl(const Program *prog, char **error);

/* Free the compiler's known-enum table. Call once when the bytecode REPL exits. */
void compiler_free_known_enums(void);

#endif /* COMPILER_H */
