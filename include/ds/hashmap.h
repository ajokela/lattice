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

/* A single map entry */
typedef struct {
    char       *key;
    void       *value;      /* heap-allocated copy of value_size bytes */
    LatMapState state;
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
