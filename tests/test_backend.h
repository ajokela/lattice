#ifndef TEST_BACKEND_H
#define TEST_BACKEND_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

typedef enum {
    BACKEND_TREE_WALK, /* Evaluator (legacy) */
    BACKEND_STACK_VM,  /* Bytecode stack VM (production default) */
    BACKEND_REG_VM,    /* Register VM (POC) */
} TestBackend;

extern TestBackend test_backend;

/* ── Helper: platform temp directory ── */
static inline const char *test_tmp(void) {
#ifdef _WIN32
    static char buf[260];
    if (!buf[0]) {
        const char *t = getenv("TEMP");
        if (!t) t = getenv("TMP");
        if (!t) t = ".";
        snprintf(buf, sizeof(buf), "%s", t);
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\\' || buf[len - 1] == '/')) buf[--len] = '\0';
        /* Normalize to forward slashes so paths embed safely in Lattice source strings */
        for (size_t i = 0; i < len; i++) {
            if (buf[i] == '\\') buf[i] = '/';
        }
    }
    return buf;
#else
    return "/tmp";
#endif
}

/* Portable tmpfile replacement. Writes path to path_out for cleanup with remove(). */
static inline FILE *test_tmpfile(char *path_out, size_t path_size) {
    snprintf(path_out, path_size, "%s/lat_test_XXXXXX", test_tmp());
#ifdef _WIN32
    _mktemp(path_out);
    return fopen(path_out, "w+b");
#else
    int fd = mkstemp(path_out);
    if (fd < 0) return NULL;
    return fdopen(fd, "w+b");
#endif
}

#endif /* TEST_BACKEND_H */
