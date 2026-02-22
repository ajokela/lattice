#ifndef CHUNK_H
#define CHUNK_H

#include "value.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t  *code;        /* Bytecode stream */
    size_t    code_len;
    size_t    code_cap;
    LatValue *constants;   /* Constants pool */
    size_t   *const_hashes; /* Pre-computed FNV-1a hashes for string constants (0 for non-strings) */
    size_t    const_len;
    size_t    const_cap;
    int      *lines;       /* Line number per bytecode byte (parallel to code) */
    size_t    lines_len;
    size_t    lines_cap;
    char    **local_names;  /* Debug: slot index -> variable name (for tracking) */
    size_t    local_name_cap;
    char     *name;        /* Debug: function name (NULL for top-level script) */
    LatValue *default_values; /* Default param values (NULL if none) */
    int       default_count;  /* Number of params with defaults */
    bool      fn_has_variadic; /* Whether last param is variadic */
    uint8_t  *param_phases;   /* Per-param phase constraint (AstPhase values, NULL if none) */
    int       param_phase_count;
} Chunk;

Chunk *chunk_new(void);
void chunk_free(Chunk *c);

/* Write a byte to the chunk, returns its offset */
size_t chunk_write(Chunk *c, uint8_t byte, int line);

/* Add a constant to the pool, returns its index */
size_t chunk_add_constant(Chunk *c, LatValue val);

/* Emit a 2-byte operand (big-endian) */
void chunk_write_u16(Chunk *c, uint16_t val, int line);

/* Record a local variable name for a given stack slot (debug/tracking info) */
void chunk_set_local_name(Chunk *c, size_t slot, const char *name);

/* Disassemble chunk to stderr for debugging */
void chunk_disassemble(const Chunk *c, const char *name);

/* Disassemble a single instruction, returns next offset */
size_t chunk_disassemble_instruction(const Chunk *c, size_t offset);

#endif /* CHUNK_H */
