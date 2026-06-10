#include "intern.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The intern table is a process-global hash map. The bytecode VM runs spawn
 * bodies as concurrent threads that intern strings (struct construction, field
 * access, deep-clone, short-string promotion), so every mutation — including
 * intern_grow(), which frees and rebuilds the entries array — must be
 * serialized, or peer threads race the table (data race / use-after-free). */
#ifndef __EMSCRIPTEN__
#include <pthread.h>
static pthread_mutex_t g_intern_mutex = PTHREAD_MUTEX_INITIALIZER;
#define INTERN_LOCK()   pthread_mutex_lock(&g_intern_mutex)
#define INTERN_UNLOCK() pthread_mutex_unlock(&g_intern_mutex)
#else
#define INTERN_LOCK()   ((void)0)
#define INTERN_UNLOCK() ((void)0)
#endif

#define INTERN_INIT_CAP 256

typedef struct {
    char **entries;
    size_t count;
    size_t cap;
} InternTable;

static InternTable g_intern = {0};

void intern_init(void) {
    g_intern.cap = INTERN_INIT_CAP;
    g_intern.count = 0;
    g_intern.entries = calloc(g_intern.cap, sizeof(char *));
    /* if calloc fails, intern() will call intern_init() again on next use */
}

void intern_free(void) {
    for (size_t i = 0; i < g_intern.cap; i++) free(g_intern.entries[i]);
    free(g_intern.entries);
    g_intern.entries = NULL;
    g_intern.count = 0;
    g_intern.cap = 0;
}

static uint32_t intern_hash(const char *s) {
    uint32_t h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

static void intern_grow(void) {
    size_t new_cap = g_intern.cap * 2;
    char **new_entries = calloc(new_cap, sizeof(char *));
    if (!new_entries) return;
    for (size_t i = 0; i < g_intern.cap; i++) {
        if (!g_intern.entries[i]) continue;
        uint32_t h = intern_hash(g_intern.entries[i]) & (uint32_t)(new_cap - 1);
        while (new_entries[h]) h = (h + 1) & (uint32_t)(new_cap - 1);
        new_entries[h] = g_intern.entries[i];
    }
    free(g_intern.entries);
    g_intern.entries = new_entries;
    g_intern.cap = new_cap;
}

const char *intern(const char *s) {
    if (!s) return NULL;

    INTERN_LOCK();
    if (!g_intern.entries) intern_init();

    if (g_intern.count * 2 >= g_intern.cap) intern_grow();

    uint32_t mask = (uint32_t)(g_intern.cap - 1);
    uint32_t h = intern_hash(s) & mask;
    while (g_intern.entries[h]) {
        if (strcmp(g_intern.entries[h], s) == 0) {
            const char *found = g_intern.entries[h];
            INTERN_UNLOCK();
            return found;
        }
        h = (h + 1) & mask;
    }
    g_intern.entries[h] = strdup(s);
    g_intern.count++;
    const char *result = g_intern.entries[h];
    INTERN_UNLOCK();
    return result;
}
