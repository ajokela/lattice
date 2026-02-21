#ifndef LAT_MAP_H
#define LAT_MAP_H

#include <stddef.h>
#include <stdbool.h>

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
    char       *key;
    void       *value;      /* points to ibuf (inline storage, no separate heap alloc) */
    LatMapState state;
    char        _ibuf[LAT_MAP_INLINE_MAX] __attribute__((aligned(8)));
} LatMapEntry;

/* Open-addressing hash map with string keys */
typedef struct {
    LatMapEntry *entries;
    size_t       cap;       /* total slots */
    size_t       count;     /* occupied + tombstones (for load factor) */
    size_t       live;      /* occupied only */
    size_t       value_size;
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

#endif /* LAT_MAP_H */
