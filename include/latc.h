#ifndef LATC_H
#define LATC_H

#include "chunk.h"
#include "regvm.h"
#include <stddef.h>
#include <stdint.h>

#define LATC_MAGIC   "LATC"
#define LATC_FORMAT   1
#define RLATC_MAGIC  "RLAT"
#define RLATC_FORMAT  2

/* Save a compiled chunk to a .latc file. Returns 0 on success, -1 on error. */
int chunk_save(const Chunk *c, const char *path);

/* Load a compiled chunk from a .latc file.
 * On success returns a Chunk* (caller owns it).
 * On error returns NULL and sets *err to a heap-allocated message. */
Chunk *chunk_load(const char *path, char **err);

/* Serialize a chunk to an in-memory byte buffer.
 * Returns a malloc'd buffer and sets *out_len.
 * Caller must free() the returned pointer. */
uint8_t *chunk_serialize(const Chunk *c, size_t *out_len);

/* Deserialize a chunk from an in-memory byte buffer.
 * On success returns a Chunk* (caller owns it).
 * On error returns NULL and sets *err to a heap-allocated message. */
Chunk *chunk_deserialize(const uint8_t *data, size_t len, char **err);

/* ── Register VM bytecode serialization (.rlatc) ── */

/* Serialize a register chunk to an in-memory byte buffer. */
uint8_t *regchunk_serialize(const RegChunk *c, size_t *out_len);

/* Deserialize a register chunk from an in-memory byte buffer. */
RegChunk *regchunk_deserialize(const uint8_t *data, size_t len, char **err);

/* Save/load register chunks to/from files. */
int regchunk_save(const RegChunk *c, const char *path);
RegChunk *regchunk_load(const char *path, char **err);

#endif /* LATC_H */
