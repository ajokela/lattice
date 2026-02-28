#ifndef LATTICE_H
#define LATTICE_H

#define LATTICE_VERSION        "0.3.28"
#define LATTICE_MAX_CALL_DEPTH 1000

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* MinGW lacks vasprintf — provide an inline implementation. */
#ifdef _WIN32
static inline int vasprintf(char **strp, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int len = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (len < 0) {
        *strp = NULL;
        return -1;
    }
    *strp = (char *)malloc((size_t)len + 1);
    if (!*strp) return -1;
    return vsnprintf(*strp, (size_t)len + 1, fmt, ap);
}
#endif

/* GCC's warn_unused_result cannot be silenced by (void) casts on asprintf.
   These wrappers properly consume the return value. */
#ifndef _WIN32
__attribute__((format(printf, 2, 3)))
#endif
static inline void lat_asprintf(char **strp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (vasprintf(strp, fmt, ap) < 0) *strp = NULL;
    va_end(ap);
}
static inline void lat_vasprintf(char **strp, const char *fmt, va_list ap) {
    if (vasprintf(strp, fmt, ap) < 0) *strp = NULL;
}

/* Forward declarations */
typedef struct LatValue LatValue;
typedef struct Env Env;
typedef struct Evaluator Evaluator;
typedef struct FluidHeap FluidHeap;
typedef struct CrystalRegion CrystalRegion;
typedef struct RegionManager RegionManager;
typedef struct DualHeap DualHeap;

#endif /* LATTICE_H */
