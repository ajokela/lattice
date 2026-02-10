#include "ds/hashmap.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAP  16
#define LOAD_FACTOR  70  /* percent */

/* FNV-1a hash */
static size_t fnv1a(const char *key) {
    size_t hash = 14695981039346656037ULL;
    for (const char *p = key; *p; p++) {
        hash ^= (size_t)(unsigned char)*p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void lat_map_alloc_entries(LatMap *m, size_t cap) {
    m->entries = calloc(cap, sizeof(LatMapEntry));
    m->cap = cap;
    m->count = 0;
    m->live = 0;
}

LatMap lat_map_new(size_t value_size) {
    LatMap m;
    m.value_size = value_size;
    lat_map_alloc_entries(&m, INITIAL_CAP);
    return m;
}

void lat_map_free(LatMap *m) {
    for (size_t i = 0; i < m->cap; i++) {
        if (m->entries[i].state == MAP_OCCUPIED) {
            free(m->entries[i].key);
            free(m->entries[i].value);
        }
    }
    free(m->entries);
    m->entries = NULL;
    m->cap = 0;
    m->count = 0;
    m->live = 0;
}

static size_t lat_map_find_slot(const LatMap *m, const char *key) {
    size_t idx = fnv1a(key) % m->cap;
    size_t first_tombstone = (size_t)-1;
    for (size_t i = 0; i < m->cap; i++) {
        size_t pos = (idx + i) % m->cap;
        LatMapEntry *e = &m->entries[pos];
        if (e->state == MAP_EMPTY) {
            return (first_tombstone != (size_t)-1) ? first_tombstone : pos;
        }
        if (e->state == MAP_TOMBSTONE) {
            if (first_tombstone == (size_t)-1) first_tombstone = pos;
            continue;
        }
        /* MAP_OCCUPIED */
        if (strcmp(e->key, key) == 0) {
            return pos;
        }
    }
    return (first_tombstone != (size_t)-1) ? first_tombstone : 0;
}

static void lat_map_rehash(LatMap *m) {
    size_t old_cap = m->cap;
    LatMapEntry *old = m->entries;

    lat_map_alloc_entries(m, old_cap * 2);

    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].state == MAP_OCCUPIED) {
            size_t pos = lat_map_find_slot(m, old[i].key);
            m->entries[pos] = old[i];
            m->count++;
            m->live++;
        }
    }
    free(old);
}

bool lat_map_set(LatMap *m, const char *key, const void *value) {
    if ((m->count + 1) * 100 > m->cap * LOAD_FACTOR) {
        lat_map_rehash(m);
    }

    size_t pos = lat_map_find_slot(m, key);
    LatMapEntry *e = &m->entries[pos];

    if (e->state == MAP_OCCUPIED) {
        /* Update existing */
        memcpy(e->value, value, m->value_size);
        return false;
    }

    /* New entry */
    bool was_empty = (e->state == MAP_EMPTY);
    e->key = strdup(key);
    e->value = malloc(m->value_size);
    memcpy(e->value, value, m->value_size);
    e->state = MAP_OCCUPIED;
    if (was_empty) m->count++;
    m->live++;
    return true;
}

void *lat_map_get(const LatMap *m, const char *key) {
    if (m->live == 0) return NULL;
    size_t idx = fnv1a(key) % m->cap;
    for (size_t i = 0; i < m->cap; i++) {
        size_t pos = (idx + i) % m->cap;
        LatMapEntry *e = &m->entries[pos];
        if (e->state == MAP_EMPTY) return NULL;
        if (e->state == MAP_OCCUPIED && strcmp(e->key, key) == 0) {
            return e->value;
        }
    }
    return NULL;
}

bool lat_map_remove(LatMap *m, const char *key) {
    if (m->live == 0) return false;
    size_t idx = fnv1a(key) % m->cap;
    for (size_t i = 0; i < m->cap; i++) {
        size_t pos = (idx + i) % m->cap;
        LatMapEntry *e = &m->entries[pos];
        if (e->state == MAP_EMPTY) return false;
        if (e->state == MAP_OCCUPIED && strcmp(e->key, key) == 0) {
            free(e->key);
            free(e->value);
            e->key = NULL;
            e->value = NULL;
            e->state = MAP_TOMBSTONE;
            m->live--;
            return true;
        }
    }
    return false;
}

bool lat_map_contains(const LatMap *m, const char *key) {
    return lat_map_get(m, key) != NULL;
}

size_t lat_map_len(const LatMap *m) {
    return m->live;
}

void lat_map_iter(const LatMap *m, LatMapIterFn fn, void *ctx) {
    for (size_t i = 0; i < m->cap; i++) {
        if (m->entries[i].state == MAP_OCCUPIED) {
            fn(m->entries[i].key, m->entries[i].value, ctx);
        }
    }
}
