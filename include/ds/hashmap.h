#ifndef LAT_MAP_H
#define LAT_MAP_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Hash map entry states */
typedef enum {
    MAP_EMPTY,
    MAP_OCCUPIED,
    MAP_TOMBSTONE,
} LatMapState;

/* Max value size for inline storage in map entries.
 * Values <= this size are stored inline (no heap allocation per entry).
 * value pointer always points to ibuf for inline values. */
#define LAT_MAP_INLINE_MAX 72

/* A single map entry */
typedef struct {
    char *key;
    void *value; /* points to ibuf (inline storage, no separate heap alloc) */
    LatMapState state;
    char _ibuf[LAT_MAP_INLINE_MAX] __attribute__((aligned(8)));
} LatMapEntry;

/* ── Crystallized Map Index (CMI) ──
 * Read-optimized layout for frozen maps: a single malloc'd block holding a
 * CHD minimal perfect hash over a dense, order-preserving copy of the entries.
 *
 * Block layout (offsets are from the header/block start):
 *   CrystalMapHeader
 *   [off_entries] LatMapEntry dense[n]   — original slot-scan order, all OCCUPIED
 *   [off_packed]  uint64_t packed[n]     — perfect-hash position -> (hash32 << 32) | dense index
 *                                          (one load yields both the miss filter and the entry)
 *   [off_disp]    uint16_t disp[nbuckets]— CHD displacements (0 = empty bucket)
 *   [off_keys]    packed NUL-terminated key bytes
 *
 * When cmi != NULL, m->entries points INSIDE the block (do not free it
 * separately) and cap == count == live == n. Because dense[] is a legal,
 * 100%-occupied entries array, every existing entries[]-scanning iteration
 * site works unchanged and in the same order as the source table.
 *
 * IMPORTANT: any future code that derives a slot from a hash on a possibly
 * crystallized map must go through lat_map_get/lat_map_get_prehashed.
 *
 * The header reserves refcount headroom for the sibling "crystal values
 * shared by reference" effort: the block is one allocation with rebasable
 * interior pointers, so aliasing later is "bump refcount, share pointer".
 *
 * NOTE (32-bit/wasm): fnv1a here is size_t-wide; on 32-bit targets the
 * key_hash pre-filter loses entropy and CHD build quality degrades — the
 * build then falls back to the sparse layout (correctness unaffected). */
typedef struct CrystalMapHeader {
    uint64_t n;          /* live key count */
    uint64_t nbuckets;   /* CHD buckets (~n/4) */
    uint64_t seed;       /* hash-mix seed */
    uint64_t refcount;   /* reserved for crystal-by-ref aliasing (unused, init 1) */
    uint64_t total_size; /* whole block size in bytes (enables memcpy clone) */
    uint64_t off_entries, off_packed, off_disp, off_keys;
} CrystalMapHeader;

/* Open-addressing hash map with string keys */
typedef struct {
    LatMapEntry *entries;
    size_t cap;   /* total slots */
    size_t count; /* occupied + tombstones (for load factor) */
    size_t live;  /* occupied only */
    size_t value_size;
    CrystalMapHeader *cmi; /* NULL = normal open-addressing layout */
} LatMap;

/* Create a new map */
LatMap lat_map_new(size_t value_size);

/* Free all entries and the map */
void lat_map_free(LatMap *m);

/* Insert or update. Returns true if key was new. */
bool lat_map_set(LatMap *m, const char *key, const void *value);

/* Get a pointer to the stored value, or NULL if not found */
void *lat_map_get(const LatMap *m, const char *key);

/* Get with pre-computed hash (avoids rehashing known constant keys) */
void *lat_map_get_prehashed(const LatMap *m, const char *key, size_t hash);

/* Remove a key. Returns true if it was present. */
bool lat_map_remove(LatMap *m, const char *key);

/* Check if key exists */
bool lat_map_contains(const LatMap *m, const char *key);

/* Number of live entries */
size_t lat_map_len(const LatMap *m);

/* Iteration: call fn for each (key, value) pair */
typedef void (*LatMapIterFn)(const char *key, void *value, void *ctx);
void lat_map_iter(const LatMap *m, LatMapIterFn fn, void *ctx);

/* ── Crystallized layout (frozen-map read optimization) ── */

/* Rebuild m into the CMI layout (raw malloc block; matches the raw-calloc
 * ownership of normal entries). Returns false (layout unchanged) on alloc
 * failure or if the CHD build fails after 3 seeds. No-op if already
 * crystallized. Caller is responsible for eligibility (phase, key_phases). */
bool lat_map_crystallize(LatMap *m);

/* Restore the normal open-addressing layout. Inserts dense entries in order
 * into a fresh table (INITIAL_CAP + deterministic growth), reproducing the
 * exact layout a thaw/rebuild produces today. No-op if not crystallized. */
void lat_map_decrystallize(LatMap *m);

/* Clone the CMI block of a crystallized map: one memcpy + interior pointer
 * rebase. The cloned entries' inline values still SHARE nested heap data with
 * the source; the caller must re-clone each entry value in place. Returns the
 * new block and stores the rebased dense entries pointer in *entries_out.
 * Returns NULL on alloc failure (caller should fall back to a rebuild). */
CrystalMapHeader *lat_map_cmi_clone_block(const LatMap *src, LatMapEntry **entries_out);

#endif /* LAT_MAP_H */
