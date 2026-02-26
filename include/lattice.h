#ifndef LATTICE_H
#define LATTICE_H

#define LATTICE_VERSION "0.3.25"
#define LATTICE_MAX_CALL_DEPTH 1000

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* GCC's warn_unused_result cannot be silenced by (void) casts on asprintf.
   These wrappers properly consume the return value. */
__attribute__((format(printf, 2, 3)))
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
