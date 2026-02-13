#ifndef LATTICE_EXT_H
#define LATTICE_EXT_H

/*
 * Lattice Native Extension API
 *
 * Extensions are shared libraries (.dylib/.so) that export lat_ext_init().
 * The runtime loads them via dlopen, calls init to register functions,
 * then builds a Map of callable native closures returned to user code.
 *
 * Extensions compile against this header only — internal LatValue layout
 * is hidden behind opaque LatExtValue pointers.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define LATTICE_EXT_API_VERSION 1

/* Opaque value wrapper (hides internal LatValue) */
typedef struct LatExtValue LatExtValue;

/* Opaque registration context passed to lat_ext_init */
typedef struct LatExtContext LatExtContext;

/* Extension function signature */
typedef LatExtValue *(*LatExtFn)(LatExtValue **args, size_t argc);

/* ── Registration ── */

/* Register a named function in the extension context */
void lat_ext_register(LatExtContext *ctx, const char *name, LatExtFn fn);

/* ── Constructors (return heap-allocated LatExtValue) ── */

LatExtValue *lat_ext_int(int64_t v);
LatExtValue *lat_ext_float(double v);
LatExtValue *lat_ext_bool(bool v);
LatExtValue *lat_ext_string(const char *s);
LatExtValue *lat_ext_nil(void);
LatExtValue *lat_ext_array(LatExtValue **elems, size_t len);
LatExtValue *lat_ext_map_new(void);
void         lat_ext_map_set(LatExtValue *map, const char *key, LatExtValue *val);

/* ── Error ── */
LatExtValue *lat_ext_error(const char *msg);

/* ── Type query ── */
typedef enum {
    LAT_EXT_INT, LAT_EXT_FLOAT, LAT_EXT_BOOL, LAT_EXT_STRING,
    LAT_EXT_ARRAY, LAT_EXT_MAP, LAT_EXT_NIL, LAT_EXT_OTHER
} LatExtType;

LatExtType lat_ext_type(const LatExtValue *v);

/* ── Accessors ── */

int64_t     lat_ext_as_int(const LatExtValue *v);
double      lat_ext_as_float(const LatExtValue *v);
bool        lat_ext_as_bool(const LatExtValue *v);
const char *lat_ext_as_string(const LatExtValue *v);

/* Array accessors */
size_t       lat_ext_array_len(const LatExtValue *v);
LatExtValue *lat_ext_array_get(const LatExtValue *v, size_t index);

/* Map accessor */
LatExtValue *lat_ext_map_get(const LatExtValue *v, const char *key);

/* ── Cleanup ── */
void lat_ext_free(LatExtValue *v);

/* ── Init entry point (extensions must export this) ── */
typedef void (*LatExtInitFn)(LatExtContext *ctx);

#endif /* LATTICE_EXT_H */
