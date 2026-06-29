#include "ds/hashmap.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define INITIAL_CAP 16
#define LOAD_FACTOR 70 /* percent */

/* FNV-1a hash */
static size_t fnv1a(const char *key) {
    size_t hash = 14695981039346656037ULL;
    for (const char *p = key; *p; p++) {
        hash ^= (size_t)(unsigned char)*p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* ── Crystallized Map Index (CMI) helpers ── */

#define CMI_MAX_DISP   65535u
#define CMI_SEED_TRIES 3

static inline uint64_t cmi_mix64(uint64_t x) {
    /* splitmix64 finalizer (used for re-seeding; off the lookup path) */
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* Cheap lookup-path mix: one multiply. cmi_range consumes the HIGH 64 bits of
 * a 128-bit product, so a single multiply pushes input entropy where it is
 * needed. FNV-1a input is already well mixed; CHD build verifies quality and
 * re-seeds on failure. Keeping this short matters: the whole lookup is one
 * serial dependency chain (mix -> disp load -> mix -> packed load -> entry). */
static inline uint64_t cmi_quickmix(uint64_t x) { return x * 0x9E3779B97F4A7C15ULL; }

static inline uint64_t cmi_range(uint64_t x, uint64_t n) {
#ifdef __SIZEOF_INT128__
    return (uint64_t)(((__uint128_t)x * (__uint128_t)n) >> 64); /* fastrange: mulhi */
#else
    return x % n; /* portable fallback (wasm/mingw without __int128) */
#endif
}

static inline uint64_t cmi_bucket_of(uint64_t h, uint64_t seed, uint64_t nbuckets) {
    return cmi_range(cmi_quickmix(h ^ seed), nbuckets);
}

static inline uint64_t cmi_pos_of(uint64_t h, uint64_t seed, uint64_t d, uint64_t n) {
    /* The XOR between the two multiplies is essential: with a purely linear
     * d-term, all keys of a bucket shift as a rigid block as d varies and the
     * displacement search cannot fill a load-factor-1.0 table. */
    return cmi_range(cmi_quickmix(h ^ ((seed + d) * 0xff51afd7ed558ccdULL)), n);
}

/* Lookup in a crystallized map. One displacement load, one packed load
 * (32-bit hash filter + dense index together), then one strcmp on a
 * (almost-always-true after the filter) candidate hit. */
static void *cmi_get(const LatMap *m, const char *key, uint64_t h) {
    const CrystalMapHeader *hd = m->cmi;
    const char *base = (const char *)hd;
    const uint16_t *disp = (const uint16_t *)(base + hd->off_disp);
    uint64_t b = cmi_bucket_of(h, hd->seed, hd->nbuckets);
    uint64_t d = disp[b];
    if (d == 0) return NULL; /* empty bucket — no key hashes here */
    uint64_t p = cmi_pos_of(h, hd->seed, d, hd->n);
    uint64_t packed = ((const uint64_t *)(base + hd->off_packed))[p];
    if ((uint32_t)(packed >> 32) != (uint32_t)h) return NULL; /* miss filter */
    LatMapEntry *e = &m->entries[(uint32_t)packed];
    if (strcmp(e->key, key) != 0) return NULL; /* full compare for correctness */
    return e->value;
}

/* qsort comparator: (count<<32)|bucket, descending */
static int cmi_border_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x < y) - (x > y);
}

bool lat_map_crystallize(LatMap *m) {
    if (m->cmi) return true;
    size_t n = m->live;
    if (n == 0) return false;

    /* (1) Collect occupied slots in scan order — this IS the iteration order
     * to preserve — and measure key bytes. */
    size_t *slots = malloc(n * sizeof(size_t));
    if (!slots) return false;
    size_t keybytes = 0, k = 0;
    for (size_t i = 0; i < m->cap; i++) {
        if (m->entries[i].state == MAP_OCCUPIED) {
            slots[k++] = i;
            keybytes += strlen(m->entries[i].key) + 1;
        }
    }
    if (k != n) {
        free(slots);
        return false;
    }

    uint64_t nbuckets = (n + 3) / 4; /* CHD lambda = 4 */
    if (nbuckets == 0) nbuckets = 1;

    /* (2) Block layout (all sections 8-byte-friendly given entry alignment) */
    uint64_t off_entries = (sizeof(CrystalMapHeader) + 7u) & ~(uint64_t)7u;
    uint64_t off_packed = off_entries + (uint64_t)n * sizeof(LatMapEntry);
    uint64_t off_disp = off_packed + (uint64_t)n * sizeof(uint64_t);
    uint64_t off_keys = off_disp + nbuckets * sizeof(uint16_t);
    uint64_t total = off_keys + keybytes;

    char *block = malloc(total);
    if (!block) {
        free(slots);
        return false;
    }
    uint64_t *hashes = malloc(n * sizeof(uint64_t)); /* build-time only */
    if (!hashes) {
        free(block);
        free(slots);
        return false;
    }
    CrystalMapHeader *hd = (CrystalMapHeader *)block;
    hd->n = n;
    hd->nbuckets = nbuckets;
    hd->seed = 0;
    hd->refcount = 1;
    hd->total_size = total;
    hd->off_entries = off_entries;
    hd->off_packed = off_packed;
    hd->off_disp = off_disp;
    hd->off_keys = off_keys;

    LatMapEntry *dense = (LatMapEntry *)(block + off_entries);
    uint64_t *packed = (uint64_t *)(block + off_packed);
    uint16_t *disp = (uint16_t *)(block + off_disp);
    char *keyw = block + off_keys;

    /* (3) Dense copy in scan order; pack keys; record full hashes */
    for (size_t j = 0; j < n; j++) {
        LatMapEntry *src = &m->entries[slots[j]];
        dense[j].state = MAP_OCCUPIED;
        size_t klen = strlen(src->key) + 1;
        memcpy(keyw, src->key, klen);
        dense[j].key = keyw;
        keyw += klen;
        dense[j].value = dense[j]._ibuf;
        memcpy(dense[j]._ibuf, src->value, m->value_size);
        hashes[j] = (uint64_t)fnv1a(src->key);
    }

    /* (4) CHD displacement search */
    uint32_t *bcount = malloc(nbuckets * sizeof(uint32_t));
    uint32_t *bstart = malloc((nbuckets + 1) * sizeof(uint32_t));
    uint32_t *bitems = malloc(n * sizeof(uint32_t));
    uint64_t *border = malloc(nbuckets * sizeof(uint64_t)); /* (count<<32)|bucket, sorted desc */
    uint8_t *used = malloc((n + 7) / 8);
    uint64_t *tent = malloc(n * sizeof(uint64_t)); /* tentative positions for one bucket */
    bool ok = false;
    if (bcount && bstart && bitems && border && used && tent) {
        uint64_t seed = 0x5EEDC0DE5EEDC0DEULL;
        for (int attempt = 0; attempt < CMI_SEED_TRIES && !ok; attempt++, seed = cmi_mix64(seed + 1)) {
            memset(bcount, 0, nbuckets * sizeof(uint32_t));
            for (size_t j = 0; j < n; j++) bcount[cmi_bucket_of(hashes[j], seed, nbuckets)]++;
            bstart[0] = 0;
            for (uint64_t b = 0; b < nbuckets; b++) bstart[b + 1] = bstart[b] + bcount[b];
            {
                uint32_t *cursor = malloc(nbuckets * sizeof(uint32_t));
                if (!cursor) break;
                memcpy(cursor, bstart, nbuckets * sizeof(uint32_t));
                for (size_t j = 0; j < n; j++) {
                    uint64_t b = cmi_bucket_of(hashes[j], seed, nbuckets);
                    bitems[cursor[b]++] = (uint32_t)j;
                }
                free(cursor);
            }
            /* Process buckets largest-first */
            for (uint64_t b = 0; b < nbuckets; b++) border[b] = ((uint64_t)bcount[b] << 32) | b;
            qsort(border, nbuckets, sizeof(uint64_t), cmi_border_cmp);

            memset(used, 0, (n + 7) / 8);
            memset(disp, 0, nbuckets * sizeof(uint16_t));
            memset(packed, 0, n * sizeof(uint64_t));
            ok = true;
            for (uint64_t bi = 0; bi < nbuckets && ok; bi++) {
                uint64_t b = border[bi] & 0xFFFFFFFFu;
                uint32_t cnt = (uint32_t)(border[bi] >> 32);
                if (cnt == 0) break; /* sorted desc: rest are empty */
                bool placed = false;
                for (uint32_t d = 1; d <= CMI_MAX_DISP; d++) {
                    bool fits = true;
                    for (uint32_t t = 0; t < cnt && fits; t++) {
                        uint64_t p = cmi_pos_of(hashes[bitems[bstart[b] + t]], seed, d, n);
                        if (used[p >> 3] & (1u << (p & 7))) {
                            fits = false;
                            break;
                        }
                        for (uint32_t u = 0; u < t; u++)
                            if (tent[u] == p) {
                                fits = false;
                                break;
                            }
                        if (fits) tent[t] = p;
                    }
                    if (fits) {
                        for (uint32_t t = 0; t < cnt; t++) {
                            uint32_t j = bitems[bstart[b] + t];
                            used[tent[t] >> 3] |= (uint8_t)(1u << (tent[t] & 7));
                            packed[tent[t]] = ((uint64_t)(uint32_t)hashes[j] << 32) | j;
                        }
                        disp[b] = (uint16_t)d;
                        placed = true;
                        break;
                    }
                }
                if (!placed) ok = false;
            }
            if (ok) hd->seed = seed;
        }
    }
    free(bcount);
    free(bstart);
    free(bitems);
    free(border);
    free(used);
    free(tent);
    free(slots);
    free(hashes);

    if (!ok) {
        /* Pathological hash collisions (or OOM): keep the sparse layout.
         * Semantics identical, just no speedup. */
        free(block);
        return false;
    }

    /* (5) Swap in: free the old table (keys were copied into the block;
     * nested heap data inside the inline values transfers ownership). */
    for (size_t i = 0; i < m->cap; i++)
        if (m->entries[i].state == MAP_OCCUPIED) free(m->entries[i].key);
    free(m->entries);
    m->entries = dense;
    m->cap = n;
    m->count = n;
    m->live = n;
    m->cmi = hd;
    return true;
}

void lat_map_decrystallize(LatMap *m) {
    if (!m->cmi) return;
    CrystalMapHeader *hd = m->cmi;
    LatMapEntry *dense = m->entries;
    LatMap fresh = lat_map_new(m->value_size);
    /* Insert in dense (== original scan) order: reproduces exactly the table
     * layout today's thaw/clone rebuild produces. Value bytes are memcpy'd, so
     * nested heap data transfers ownership out of the block. */
    for (uint64_t i = 0; i < hd->n; i++) lat_map_set(&fresh, dense[i].key, dense[i].value);
    free(hd);
    *m = fresh;
}

CrystalMapHeader *lat_map_cmi_clone_block(const LatMap *src, LatMapEntry **entries_out) {
    if (!src->cmi) return NULL;
    const CrystalMapHeader *sh = src->cmi;
    char *block = malloc(sh->total_size);
    if (!block) return NULL;
    memcpy(block, sh, sh->total_size);
    CrystalMapHeader *hd = (CrystalMapHeader *)block;
    hd->refcount = 1;
    LatMapEntry *dense = (LatMapEntry *)(block + hd->off_entries);
    ptrdiff_t delta = block - (const char *)sh;
    for (uint64_t i = 0; i < hd->n; i++) {
        dense[i].key += delta;           /* rebase into this block's key section */
        dense[i].value = dense[i]._ibuf; /* values stay inline */
    }
    *entries_out = dense;
    return hd;
}

static void lat_map_alloc_entries(LatMap *m, size_t cap) {
    m->entries = calloc(cap, sizeof(LatMapEntry));
    if (!m->entries) return;
    /* Point each entry's value to its inline buffer */
    for (size_t i = 0; i < cap; i++) m->entries[i].value = m->entries[i]._ibuf;
    m->cap = cap;
    m->count = 0;
    m->live = 0;
}

LatMap lat_map_new(size_t value_size) {
    assert(value_size <= LAT_MAP_INLINE_MAX);
    LatMap m;
    m.value_size = value_size;
    m.cmi = NULL;
    lat_map_alloc_entries(&m, INITIAL_CAP);
    return m;
}

void lat_map_free(LatMap *m) {
    if (m->cmi) {
        /* Crystallized: entries and keys live inside the single CMI block.
         * MUST NOT fall through to the per-key free()/free(entries) below —
         * those pointers are block-interior. */
        free(m->cmi);
        m->cmi = NULL;
        m->entries = NULL;
        m->cap = 0;
        m->count = 0;
        m->live = 0;
        return;
    }
    for (size_t i = 0; i < m->cap; i++) {
        if (m->entries[i].state == MAP_OCCUPIED) {
            free(m->entries[i].key);
            /* value is inline — no free needed */
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
        if (e->state == MAP_EMPTY) { return (first_tombstone != (size_t)-1) ? first_tombstone : pos; }
        if (e->state == MAP_TOMBSTONE) {
            if (first_tombstone == (size_t)-1) first_tombstone = pos;
            continue;
        }
        /* MAP_OCCUPIED */
        if (strcmp(e->key, key) == 0) { return pos; }
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
            m->entries[pos].key = old[i].key;
            m->entries[pos].state = MAP_OCCUPIED;
            /* value pointer already points to _ibuf; copy inline data */
            memcpy(m->entries[pos]._ibuf, old[i]._ibuf, m->value_size);
            m->count++;
            m->live++;
        }
    }
    free(old);
}

bool lat_map_set(LatMap *m, const char *key, const void *value) {
    /* Crystallized backstop: MUST run before the load-factor check below —
     * a CMI map has count == cap, so lat_map_rehash would otherwise calloc a
     * new table and free() the block-interior entries pointer. */
    if (m->cmi) {
        void *existing = cmi_get(m, key, (uint64_t)fnv1a(key));
        if (existing) { /* in-place value update: layout untouched */
            memcpy(existing, value, m->value_size);
            return false;
        }
        lat_map_decrystallize(m); /* new key: degrade to the sparse layout */
    }
    if ((m->count + 1) * 100 > m->cap * LOAD_FACTOR) { lat_map_rehash(m); }

    size_t pos = lat_map_find_slot(m, key);
    LatMapEntry *e = &m->entries[pos];

    if (e->state == MAP_OCCUPIED) {
        /* Update existing — value is inline */
        memcpy(e->value, value, m->value_size);
        return false;
    }

    /* New entry — value stored inline (e->value already points to e->_ibuf) */
    bool was_empty = (e->state == MAP_EMPTY);
    e->key = strdup(key);
    memcpy(e->value, value, m->value_size);
    e->state = MAP_OCCUPIED;
    if (was_empty) m->count++;
    m->live++;
    return true;
}

void *lat_map_get(const LatMap *m, const char *key) {
    if (m->cmi) return cmi_get(m, key, (uint64_t)fnv1a(key));
    if (m->live == 0) return NULL;
    size_t idx = fnv1a(key) % m->cap;
    for (size_t i = 0; i < m->cap; i++) {
        size_t pos = (idx + i) % m->cap;
        LatMapEntry *e = &m->entries[pos];
        if (e->state == MAP_EMPTY) return NULL;
        if (e->state == MAP_OCCUPIED && strcmp(e->key, key) == 0) { return e->value; }
    }
    return NULL;
}

void *lat_map_get_prehashed(const LatMap *m, const char *key, size_t hash) {
    if (m->cmi) return cmi_get(m, key, (uint64_t)hash); /* same FNV-1a family */
    if (m->live == 0) return NULL;
    size_t idx = hash % m->cap;
    for (size_t i = 0; i < m->cap; i++) {
        size_t pos = (idx + i) % m->cap;
        LatMapEntry *e = &m->entries[pos];
        if (e->state == MAP_EMPTY) return NULL;
        if (e->state == MAP_OCCUPIED && strcmp(e->key, key) == 0) { return e->value; }
    }
    return NULL;
}

bool lat_map_remove(LatMap *m, const char *key) {
    if (m->cmi) lat_map_decrystallize(m); /* structural change: degrade first */
    if (m->live == 0) return false;
    size_t idx = fnv1a(key) % m->cap;
    for (size_t i = 0; i < m->cap; i++) {
        size_t pos = (idx + i) % m->cap;
        LatMapEntry *e = &m->entries[pos];
        if (e->state == MAP_EMPTY) return false;
        if (e->state == MAP_OCCUPIED && strcmp(e->key, key) == 0) {
            free(e->key);
            e->key = NULL;
            /* value is inline, no free needed */
            e->state = MAP_TOMBSTONE;
            m->live--;
            return true;
        }
    }
    return false;
}

bool lat_map_contains(const LatMap *m, const char *key) { return lat_map_get(m, key) != NULL; }

size_t lat_map_len(const LatMap *m) { return m->live; }

void lat_map_iter(const LatMap *m, LatMapIterFn fn, void *ctx) {
    for (size_t i = 0; i < m->cap; i++) {
        if (m->entries[i].state == MAP_OCCUPIED) { fn(m->entries[i].key, m->entries[i].value, ctx); }
    }
}
