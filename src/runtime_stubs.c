/* runtime_stubs.c — stub implementations for symbols referenced by runtime.o,
 * stackvm.o, and latc.o that come from files excluded from the thin runtime
 * (lexer, parser, ast, compilers, regvm, debugger, package).
 *
 * These functions are registered as native callbacks or compiled into code paths
 * that are unreachable when executing pre-compiled bytecode.  The stubs satisfy
 * the linker; if any is ever called at runtime it aborts with an error message.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "value.h"
#include "ds/vec.h"
#include "chunk.h"

/* strdup helper that works in strict C11 mode */
static char *stub_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* Forward-declare opaque types used in signatures */
typedef struct {
    const char *source;
    size_t len;
    size_t pos;
    size_t line;
    size_t col;
} Lexer;
typedef struct {
    LatVec *tokens;
    size_t pos;
} Parser;
typedef struct Program Program;
typedef struct Token Token;
typedef int TokenType;
typedef struct RegChunk RegChunk;
typedef struct RegVM RegVM;
typedef uint32_t RegInstr;
typedef struct Debugger Debugger;

typedef enum { REGVM_OK_STUB, REGVM_ERROR_STUB } RegVMResult;

static void stub_abort(const char *name) {
    fprintf(stderr, "fatal: %s called in thin runtime (not supported)\n", name);
    abort();
}

/* ── Lexer / Token stubs ── */

Lexer lexer_new(const char *source) {
    (void)source;
    stub_abort("lexer_new");
    Lexer l = {0};
    return l;
}

LatVec lexer_tokenize(Lexer *lex, char **err) {
    (void)lex;
    (void)err;
    stub_abort("lexer_tokenize");
    LatVec v = {0};
    return v;
}

void token_free(Token *t) {
    (void)t;
    stub_abort("token_free");
}

const char *token_type_name(TokenType type) {
    (void)type;
    return "<unknown>";
}

/* ── Parser / AST stubs ── */

Parser parser_new(LatVec *tokens) {
    (void)tokens;
    stub_abort("parser_new");
    Parser p = {0};
    return p;
}

/* parser_parse returns a Program struct by value.  We can't know the size
 * without pulling in the full AST header, so we return a zeroed blob.
 * This code is unreachable in normal operation. */
/* Provide a minimal Program struct for the stub. The real Program is large,
 * but we only need it to compile; the function aborts before returning. */
struct Program {
    void *stmts;
    size_t count;
};

Program parser_parse(Parser *p, char **err) {
    (void)p;
    (void)err;
    stub_abort("parser_parse");
    Program prog = {0};
    return prog;
}

void program_free(Program *p) {
    (void)p;
    /* No-op: nothing to free in the stub */
}

bool module_should_export(const char *name, const char **export_names, size_t export_count, bool has_exports) {
    (void)name;
    (void)export_names;
    (void)export_count;
    (void)has_exports;
    return true; /* In runtime, all names are exported */
}

/* ── Stack compiler stubs ── */

Chunk *stack_compile(const Program *prog, char **error) {
    (void)prog;
    if (error) *error = stub_strdup("source compilation not available in thin runtime");
    return NULL;
}

Chunk *stack_compile_module(const Program *prog, char **error) {
    (void)prog;
    if (error) *error = stub_strdup("source compilation not available in thin runtime");
    return NULL;
}

Chunk *stack_compile_repl(const Program *prog, char **error) {
    (void)prog;
    if (error) *error = stub_strdup("source compilation not available in thin runtime");
    return NULL;
}

/* ── Register compiler stubs ── */

void *reg_compile_module(const Program *prog, char **error) {
    (void)prog;
    if (error) *error = stub_strdup("regvm compilation not available in thin runtime");
    return NULL;
}

void *reg_compile_repl(const Program *prog, char **error) {
    (void)prog;
    if (error) *error = stub_strdup("regvm compilation not available in thin runtime");
    return NULL;
}

/* ── RegVM stubs ── */

RegChunk *regchunk_new(void) {
    stub_abort("regchunk_new");
    return NULL;
}

void regchunk_free(RegChunk *c) {
    (void)c;
    stub_abort("regchunk_free");
}

size_t regchunk_write(RegChunk *c, RegInstr instr, int line) {
    (void)c;
    (void)instr;
    (void)line;
    stub_abort("regchunk_write");
    return 0;
}

size_t regchunk_add_constant(RegChunk *c, LatValue val) {
    (void)c;
    (void)val;
    stub_abort("regchunk_add_constant");
    return 0;
}

void regchunk_set_local_name(RegChunk *c, size_t reg, const char *name) {
    (void)c;
    (void)reg;
    (void)name;
    stub_abort("regchunk_set_local_name");
}

RegVMResult regvm_run(RegVM *vm, RegChunk *chunk, LatValue *result) {
    (void)vm;
    (void)chunk;
    (void)result;
    stub_abort("regvm_run");
    return REGVM_ERROR_STUB;
}

void regvm_track_chunk(RegVM *vm, RegChunk *ch) {
    (void)vm;
    (void)ch;
    stub_abort("regvm_track_chunk");
}

RegVM *regvm_clone_for_thread(RegVM *parent) {
    (void)parent;
    stub_abort("regvm_clone_for_thread");
    return NULL;
}

void regvm_free_child(RegVM *child) {
    (void)child;
    stub_abort("regvm_free_child");
}

/* ── Package manager stub ── */

char *pkg_resolve_module(const char *name, const char *project_dir) {
    (void)name;
    (void)project_dir;
    return NULL; /* No package resolution in thin runtime */
}

/* ── Debugger stub ── */

bool debugger_check(Debugger *dbg, void *vm, void *frame, size_t frame_count) {
    (void)dbg;
    (void)vm;
    (void)frame;
    (void)frame_count;
    return true; /* Continue execution */
}
