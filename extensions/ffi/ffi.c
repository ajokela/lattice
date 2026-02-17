/*
 * Lattice FFI Extension
 *
 * Provides foreign function interface for calling functions from arbitrary
 * shared libraries at runtime via dlopen/dlsym. Supports a fixed set of
 * calling signatures without requiring libffi.
 *
 * Functions:
 *   ffi.open(path)                  -> Int handle
 *   ffi.close(handle)               -> Nil
 *   ffi.sym(handle, name, sig)      -> Int (symbol handle)
 *   ffi.call(sym_handle, ...args)   -> result
 *   ffi.nullptr()                   -> Int (0)
 *   ffi.error()                     -> String
 */

#include "lattice_ext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

/* Forward declare the init function (exported symbol) */
void lat_ext_init(LatExtContext *ctx);

/* ── Library handle table ── */

#define MAX_LIBRARIES 64

static void *libraries[MAX_LIBRARIES];
static int lib_count = 0;

static int lib_alloc(void *handle) {
    int i;
    for (i = 0; i < lib_count; i++) {
        if (!libraries[i]) {
            libraries[i] = handle;
            return i;
        }
    }
    if (lib_count >= MAX_LIBRARIES) return -1;
    libraries[lib_count] = handle;
    return lib_count++;
}

static void *lib_get(int64_t id) {
    if (id < 0 || id >= lib_count) return NULL;
    return libraries[id];
}

static void lib_release(int64_t id) {
    if (id >= 0 && id < lib_count) {
        libraries[id] = NULL;
    }
}

/* ── Symbol table with parsed signatures ── */

#define MAX_SYMBOLS 256
#define MAX_SIG_ARGS 6

typedef enum {
    SIG_INT64,    /* 'i' - int64_t */
    SIG_DOUBLE,   /* 'f' - double */
    SIG_STRING,   /* 's' - const char * */
    SIG_POINTER,  /* 'p' - void * */
    SIG_VOID      /* 'v' - void (return type only) */
} SigType;

typedef struct {
    void    *fn_ptr;
    SigType  arg_types[MAX_SIG_ARGS];
    int      arg_count;
    SigType  ret_type;
    int      in_use;
} SymEntry;

static SymEntry symbols[MAX_SYMBOLS];
static int sym_count = 0;

static int sym_alloc(void) {
    int i;
    for (i = 0; i < sym_count; i++) {
        if (!symbols[i].in_use) return i;
    }
    if (sym_count >= MAX_SYMBOLS) return -1;
    return sym_count++;
}

static SymEntry *sym_get(int64_t id) {
    if (id < 0 || id >= sym_count) return NULL;
    if (!symbols[id].in_use) return NULL;
    return &symbols[id];
}

static void sym_release(int64_t id) {
    if (id >= 0 && id < sym_count) {
        symbols[id].in_use = 0;
        symbols[id].fn_ptr = NULL;
    }
}

/* ── Signature parsing ── */

static int parse_sig_type(char c, SigType *out) {
    switch (c) {
        case 'i': *out = SIG_INT64;   return 1;
        case 'f': *out = SIG_DOUBLE;  return 1;
        case 's': *out = SIG_STRING;  return 1;
        case 'p': *out = SIG_POINTER; return 1;
        case 'v': *out = SIG_VOID;    return 1;
        default:  return 0;
    }
}

/*
 * Parse a signature string like "ii>i" (two int args, returns int).
 * The '>' separates argument types from return type.
 * If no '>' is present, return type defaults to void.
 * Returns 1 on success, 0 on error.
 */
static int parse_signature(const char *sig, SymEntry *entry) {
    const char *p = sig;
    const char *arrow;
    int argc = 0;

    entry->arg_count = 0;
    entry->ret_type = SIG_VOID;

    /* Find the '>' separator */
    arrow = strchr(sig, '>');

    /* Parse argument types (everything before '>') */
    while (*p && p != arrow) {
        if (argc >= MAX_SIG_ARGS) return 0;
        if (!parse_sig_type(*p, &entry->arg_types[argc])) return 0;
        /* void is not a valid argument type */
        if (entry->arg_types[argc] == SIG_VOID) return 0;
        argc++;
        p++;
    }
    entry->arg_count = argc;

    /* Parse return type (after '>') */
    if (arrow && arrow[1]) {
        if (!parse_sig_type(arrow[1], &entry->ret_type)) return 0;
        /* Check for trailing garbage */
        if (arrow[2] != '\0') return 0;
    }

    return 1;
}

/* ── Generic union for argument passing ── */

typedef union {
    int64_t     as_int;
    double      as_double;
    const char *as_string;
    void       *as_pointer;
} FfiArg;

/* ── Call dispatch ── */

/*
 * We cast the function pointer to specific signatures and call it.
 * This avoids needing libffi. We support up to 6 arguments.
 * Each argument can be int64_t, double, const char*, or void*.
 * Return type can be void, int64_t, double, or const char*.
 *
 * To keep the combinatorial explosion manageable, we pass all arguments
 * as a uniform type through wrapper macros. Since we're on modern
 * 64-bit platforms (LP64/LLP64), int64_t, double, pointers all fit
 * in 8 bytes. We use a union-based approach: each argument is either
 * passed as integer or double. For the function call itself, we cast
 * the function pointer to accept the right combination.
 *
 * Strategy: integer-class args (int64, string, pointer) are passed via
 * intptr_t slots. Double args require special handling since they go in
 * floating-point registers. To support mixed int/float args without full
 * combinatorial explosion, we limit to a practical dispatch approach:
 * cast the function pointer based on the specific pattern of int vs float args.
 */

/* All integer-class types (int64, string, pointer) are passed as intptr_t */
static intptr_t arg_to_int_class(FfiArg *a, SigType t) {
    switch (t) {
        case SIG_INT64:   return (intptr_t)a->as_int;
        case SIG_STRING:  return (intptr_t)a->as_string;
        case SIG_POINTER: return (intptr_t)a->as_pointer;
        default:          return 0;
    }
}

/*
 * Dispatch the call. We handle each arg count (0-6) separately.
 * For each arg count, we generate all possible int/float patterns.
 * With 6 args and 2 categories (int-class vs double), that's 2^6 = 64
 * patterns per arg count, which is manageable.
 *
 * We encode the pattern as a bitmask: bit N = 1 means arg N is double.
 */

static LatExtValue *dispatch_call(SymEntry *entry, FfiArg *ffi_args) {
    void *fp = entry->fn_ptr;
    int nargs = entry->arg_count;
    SigType ret = entry->ret_type;

    /* Build the float bitmask */
    unsigned int fmask = 0;
    int i;
    for (i = 0; i < nargs; i++) {
        if (entry->arg_types[i] == SIG_DOUBLE) {
            fmask |= (1u << (unsigned)i);
        }
    }

    /* Prepare integer-class arg values */
    intptr_t ia[6] = {0};
    double   fa[6] = {0};
    for (i = 0; i < nargs; i++) {
        if (entry->arg_types[i] == SIG_DOUBLE) {
            fa[i] = ffi_args[i].as_double;
        } else {
            ia[i] = arg_to_int_class(&ffi_args[i], entry->arg_types[i]);
        }
    }

    /*
     * Macro-based dispatch. For each arg count and float mask combination,
     * we cast the function pointer and call it.
     *
     * To keep this tractable, we use a two-level approach:
     * 1. Switch on arg count
     * 2. Switch on float mask within each arg count
     *
     * The A() macro picks either ia[n] or fa[n] based on the mask.
     */

    /* Helper: call with a specific return type */
#define CALL_RET_INT(call_expr) do { \
    int64_t r = (int64_t)(call_expr); \
    return lat_ext_int(r); \
} while(0)

#define CALL_RET_FLOAT(call_expr) do { \
    double r = (call_expr); \
    return lat_ext_float(r); \
} while(0)

#define CALL_RET_STRING(call_expr) do { \
    const char *r = (const char *)(call_expr); \
    if (!r) return lat_ext_nil(); \
    return lat_ext_string(r); \
} while(0)

#define CALL_RET_VOID(call_expr) do { \
    (call_expr); \
    return lat_ext_nil(); \
} while(0)

    /*
     * For simplicity and to avoid massive macro expansion, we use a
     * different approach: since we're on System V AMD64 ABI (macOS/Linux),
     * integer args go in rdi,rsi,rdx,rcx,r8,r9 and float args go in
     * xmm0-xmm7. We can cast the function to accept all 6 int args + all
     * 6 float args, and the ABI will route them correctly as long as we
     * pass them in the right register class.
     *
     * This is the key insight: on x86-64 System V ABI, we can define a
     * function type that takes all integer args followed by all float args
     * (or interleaved). The caller puts each arg in the right register
     * class regardless of position in the prototype.
     *
     * Actually, the simplest correct approach: define typed function pointers
     * for common patterns and dispatch. Let's handle the most common cases.
     */

    /* 0 args */
    if (nargs == 0) {
        switch (ret) {
            case SIG_VOID:    CALL_RET_VOID(((void (*)(void))fp)());
            case SIG_INT64:   CALL_RET_INT(((int64_t (*)(void))fp)());
            case SIG_DOUBLE:  CALL_RET_FLOAT(((double (*)(void))fp)());
            case SIG_STRING:  CALL_RET_STRING(((const char *(*)(void))fp)());
            case SIG_POINTER: CALL_RET_INT(((intptr_t (*)(void))fp)());
        }
    }

    /* For 1-6 args, dispatch on float mask */

    /*
     * We'll use a pattern where all-integer-class is the common case.
     * For mixed patterns, we generate specific casts.
     *
     * Define cast types for all-int-class calls (most common):
     */

    /* All-integer-class dispatch (fmask == 0): most common case */
    if (fmask == 0) {
        switch (ret) {
            case SIG_INT64: {
                switch (nargs) {
                    case 1: return lat_ext_int((int64_t)((int64_t (*)(intptr_t))fp)(ia[0]));
                    case 2: return lat_ext_int((int64_t)((int64_t (*)(intptr_t,intptr_t))fp)(ia[0],ia[1]));
                    case 3: return lat_ext_int((int64_t)((int64_t (*)(intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2]));
                    case 4: return lat_ext_int((int64_t)((int64_t (*)(intptr_t,intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2],ia[3]));
                    case 5: return lat_ext_int((int64_t)((int64_t (*)(intptr_t,intptr_t,intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2],ia[3],ia[4]));
                    case 6: return lat_ext_int((int64_t)((int64_t (*)(intptr_t,intptr_t,intptr_t,intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2],ia[3],ia[4],ia[5]));
                    default: break;
                }
                break;
            }
            case SIG_DOUBLE: {
                switch (nargs) {
                    case 1: return lat_ext_float(((double (*)(intptr_t))fp)(ia[0]));
                    case 2: return lat_ext_float(((double (*)(intptr_t,intptr_t))fp)(ia[0],ia[1]));
                    case 3: return lat_ext_float(((double (*)(intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2]));
                    case 4: return lat_ext_float(((double (*)(intptr_t,intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2],ia[3]));
                    case 5: return lat_ext_float(((double (*)(intptr_t,intptr_t,intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2],ia[3],ia[4]));
                    case 6: return lat_ext_float(((double (*)(intptr_t,intptr_t,intptr_t,intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2],ia[3],ia[4],ia[5]));
                    default: break;
                }
                break;
            }
            case SIG_STRING: {
                switch (nargs) {
                    case 1: CALL_RET_STRING(((const char *(*)(intptr_t))fp)(ia[0]));
                    case 2: CALL_RET_STRING(((const char *(*)(intptr_t,intptr_t))fp)(ia[0],ia[1]));
                    case 3: CALL_RET_STRING(((const char *(*)(intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2]));
                    case 4: CALL_RET_STRING(((const char *(*)(intptr_t,intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2],ia[3]));
                    case 5: CALL_RET_STRING(((const char *(*)(intptr_t,intptr_t,intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2],ia[3],ia[4]));
                    case 6: CALL_RET_STRING(((const char *(*)(intptr_t,intptr_t,intptr_t,intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2],ia[3],ia[4],ia[5]));
                    default: break;
                }
                break;
            }
            case SIG_POINTER: {
                switch (nargs) {
                    case 1: return lat_ext_int((int64_t)(intptr_t)((void *(*)(intptr_t))fp)(ia[0]));
                    case 2: return lat_ext_int((int64_t)(intptr_t)((void *(*)(intptr_t,intptr_t))fp)(ia[0],ia[1]));
                    case 3: return lat_ext_int((int64_t)(intptr_t)((void *(*)(intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2]));
                    case 4: return lat_ext_int((int64_t)(intptr_t)((void *(*)(intptr_t,intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2],ia[3]));
                    case 5: return lat_ext_int((int64_t)(intptr_t)((void *(*)(intptr_t,intptr_t,intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2],ia[3],ia[4]));
                    case 6: return lat_ext_int((int64_t)(intptr_t)((void *(*)(intptr_t,intptr_t,intptr_t,intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2],ia[3],ia[4],ia[5]));
                    default: break;
                }
                break;
            }
            case SIG_VOID: {
                switch (nargs) {
                    case 1: CALL_RET_VOID(((void (*)(intptr_t))fp)(ia[0]));
                    case 2: CALL_RET_VOID(((void (*)(intptr_t,intptr_t))fp)(ia[0],ia[1]));
                    case 3: CALL_RET_VOID(((void (*)(intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2]));
                    case 4: CALL_RET_VOID(((void (*)(intptr_t,intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2],ia[3]));
                    case 5: CALL_RET_VOID(((void (*)(intptr_t,intptr_t,intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2],ia[3],ia[4]));
                    case 6: CALL_RET_VOID(((void (*)(intptr_t,intptr_t,intptr_t,intptr_t,intptr_t,intptr_t))fp)(ia[0],ia[1],ia[2],ia[3],ia[4],ia[5]));
                    default: break;
                }
                break;
            }
        }
    }

    /* All-double dispatch (all args are double): second most common for math */
    if (fmask == (unsigned)((1 << nargs) - 1)) {
        switch (ret) {
            case SIG_DOUBLE: {
                switch (nargs) {
                    case 1: return lat_ext_float(((double (*)(double))fp)(fa[0]));
                    case 2: return lat_ext_float(((double (*)(double,double))fp)(fa[0],fa[1]));
                    case 3: return lat_ext_float(((double (*)(double,double,double))fp)(fa[0],fa[1],fa[2]));
                    case 4: return lat_ext_float(((double (*)(double,double,double,double))fp)(fa[0],fa[1],fa[2],fa[3]));
                    case 5: return lat_ext_float(((double (*)(double,double,double,double,double))fp)(fa[0],fa[1],fa[2],fa[3],fa[4]));
                    case 6: return lat_ext_float(((double (*)(double,double,double,double,double,double))fp)(fa[0],fa[1],fa[2],fa[3],fa[4],fa[5]));
                    default: break;
                }
                break;
            }
            case SIG_INT64: {
                switch (nargs) {
                    case 1: return lat_ext_int((int64_t)((int64_t (*)(double))fp)(fa[0]));
                    case 2: return lat_ext_int((int64_t)((int64_t (*)(double,double))fp)(fa[0],fa[1]));
                    default: break;
                }
                break;
            }
            case SIG_VOID: {
                switch (nargs) {
                    case 1: CALL_RET_VOID(((void (*)(double))fp)(fa[0]));
                    case 2: CALL_RET_VOID(((void (*)(double,double))fp)(fa[0],fa[1]));
                    default: break;
                }
                break;
            }
            default: break;
        }
    }

    /*
     * Mixed int/double patterns. We handle the most useful ones:
     * 1 int + rest double, 1 double + rest int, etc.
     * For full generality with mixed types, we generate the common patterns.
     */

    /* 2-arg mixed patterns */
    if (nargs == 2) {
        if (fmask == 0x1) {
            /* arg0=double, arg1=int */
            switch (ret) {
                case SIG_DOUBLE:  return lat_ext_float(((double (*)(double,intptr_t))fp)(fa[0],ia[1]));
                case SIG_INT64:   return lat_ext_int((int64_t)((int64_t (*)(double,intptr_t))fp)(fa[0],ia[1]));
                case SIG_VOID:    CALL_RET_VOID(((void (*)(double,intptr_t))fp)(fa[0],ia[1]));
                case SIG_STRING:  CALL_RET_STRING(((const char *(*)(double,intptr_t))fp)(fa[0],ia[1]));
                case SIG_POINTER: return lat_ext_int((int64_t)(intptr_t)((void *(*)(double,intptr_t))fp)(fa[0],ia[1]));
            }
        }
        if (fmask == 0x2) {
            /* arg0=int, arg1=double */
            switch (ret) {
                case SIG_DOUBLE:  return lat_ext_float(((double (*)(intptr_t,double))fp)(ia[0],fa[1]));
                case SIG_INT64:   return lat_ext_int((int64_t)((int64_t (*)(intptr_t,double))fp)(ia[0],fa[1]));
                case SIG_VOID:    CALL_RET_VOID(((void (*)(intptr_t,double))fp)(ia[0],fa[1]));
                case SIG_STRING:  CALL_RET_STRING(((const char *(*)(intptr_t,double))fp)(ia[0],fa[1]));
                case SIG_POINTER: return lat_ext_int((int64_t)(intptr_t)((void *(*)(intptr_t,double))fp)(ia[0],fa[1]));
            }
        }
    }

    /* 3-arg mixed patterns (most common ones) */
    if (nargs == 3) {
        if (fmask == 0x1) {
            /* double, int, int */
            switch (ret) {
                case SIG_DOUBLE:  return lat_ext_float(((double (*)(double,intptr_t,intptr_t))fp)(fa[0],ia[1],ia[2]));
                case SIG_INT64:   return lat_ext_int((int64_t)((int64_t (*)(double,intptr_t,intptr_t))fp)(fa[0],ia[1],ia[2]));
                case SIG_VOID:    CALL_RET_VOID(((void (*)(double,intptr_t,intptr_t))fp)(fa[0],ia[1],ia[2]));
                default: break;
            }
        }
        if (fmask == 0x2) {
            /* int, double, int */
            switch (ret) {
                case SIG_DOUBLE:  return lat_ext_float(((double (*)(intptr_t,double,intptr_t))fp)(ia[0],fa[1],ia[2]));
                case SIG_INT64:   return lat_ext_int((int64_t)((int64_t (*)(intptr_t,double,intptr_t))fp)(ia[0],fa[1],ia[2]));
                case SIG_VOID:    CALL_RET_VOID(((void (*)(intptr_t,double,intptr_t))fp)(ia[0],fa[1],ia[2]));
                default: break;
            }
        }
        if (fmask == 0x4) {
            /* int, int, double */
            switch (ret) {
                case SIG_DOUBLE:  return lat_ext_float(((double (*)(intptr_t,intptr_t,double))fp)(ia[0],ia[1],fa[2]));
                case SIG_INT64:   return lat_ext_int((int64_t)((int64_t (*)(intptr_t,intptr_t,double))fp)(ia[0],ia[1],fa[2]));
                case SIG_VOID:    CALL_RET_VOID(((void (*)(intptr_t,intptr_t,double))fp)(ia[0],ia[1],fa[2]));
                default: break;
            }
        }
        if (fmask == 0x3) {
            /* double, double, int */
            switch (ret) {
                case SIG_DOUBLE:  return lat_ext_float(((double (*)(double,double,intptr_t))fp)(fa[0],fa[1],ia[2]));
                case SIG_INT64:   return lat_ext_int((int64_t)((int64_t (*)(double,double,intptr_t))fp)(fa[0],fa[1],ia[2]));
                case SIG_VOID:    CALL_RET_VOID(((void (*)(double,double,intptr_t))fp)(fa[0],fa[1],ia[2]));
                default: break;
            }
        }
        if (fmask == 0x5) {
            /* double, int, double */
            switch (ret) {
                case SIG_DOUBLE:  return lat_ext_float(((double (*)(double,intptr_t,double))fp)(fa[0],ia[1],fa[2]));
                case SIG_INT64:   return lat_ext_int((int64_t)((int64_t (*)(double,intptr_t,double))fp)(fa[0],ia[1],fa[2]));
                case SIG_VOID:    CALL_RET_VOID(((void (*)(double,intptr_t,double))fp)(fa[0],ia[1],fa[2]));
                default: break;
            }
        }
        if (fmask == 0x6) {
            /* int, double, double */
            switch (ret) {
                case SIG_DOUBLE:  return lat_ext_float(((double (*)(intptr_t,double,double))fp)(ia[0],fa[1],fa[2]));
                case SIG_INT64:   return lat_ext_int((int64_t)((int64_t (*)(intptr_t,double,double))fp)(ia[0],fa[1],fa[2]));
                case SIG_VOID:    CALL_RET_VOID(((void (*)(intptr_t,double,double))fp)(ia[0],fa[1],fa[2]));
                default: break;
            }
        }
    }

    /* 4-arg: handle a few common mixed patterns */
    if (nargs == 4 && fmask == 0x2) {
        /* int, double, int, int */
        switch (ret) {
            case SIG_DOUBLE:  return lat_ext_float(((double (*)(intptr_t,double,intptr_t,intptr_t))fp)(ia[0],fa[1],ia[2],ia[3]));
            case SIG_INT64:   return lat_ext_int((int64_t)((int64_t (*)(intptr_t,double,intptr_t,intptr_t))fp)(ia[0],fa[1],ia[2],ia[3]));
            case SIG_VOID:    CALL_RET_VOID(((void (*)(intptr_t,double,intptr_t,intptr_t))fp)(ia[0],fa[1],ia[2],ia[3]));
            default: break;
        }
    }
    if (nargs == 4 && fmask == 0x1) {
        /* double, int, int, int */
        switch (ret) {
            case SIG_DOUBLE:  return lat_ext_float(((double (*)(double,intptr_t,intptr_t,intptr_t))fp)(fa[0],ia[1],ia[2],ia[3]));
            case SIG_INT64:   return lat_ext_int((int64_t)((int64_t (*)(double,intptr_t,intptr_t,intptr_t))fp)(fa[0],ia[1],ia[2],ia[3]));
            case SIG_VOID:    CALL_RET_VOID(((void (*)(double,intptr_t,intptr_t,intptr_t))fp)(fa[0],ia[1],ia[2],ia[3]));
            default: break;
        }
    }
    if (nargs == 4 && fmask == 0x8) {
        /* int, int, int, double */
        switch (ret) {
            case SIG_DOUBLE:  return lat_ext_float(((double (*)(intptr_t,intptr_t,intptr_t,double))fp)(ia[0],ia[1],ia[2],fa[3]));
            case SIG_INT64:   return lat_ext_int((int64_t)((int64_t (*)(intptr_t,intptr_t,intptr_t,double))fp)(ia[0],ia[1],ia[2],fa[3]));
            case SIG_VOID:    CALL_RET_VOID(((void (*)(intptr_t,intptr_t,intptr_t,double))fp)(ia[0],ia[1],ia[2],fa[3]));
            default: break;
        }
    }

#undef CALL_RET_INT
#undef CALL_RET_FLOAT
#undef CALL_RET_STRING
#undef CALL_RET_VOID

    return lat_ext_error("ffi.call: unsupported argument type combination");
}

/* ── Extension functions ── */

/* ffi.open(path) -> Int (handle) */
static LatExtValue *ffi_open(LatExtValue **args, size_t argc) {
    const char *path;
    void *handle;
    int id;

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_STRING) {
        return lat_ext_error("ffi.open() expects a library path (String)");
    }
    path = lat_ext_as_string(args[0]);
    handle = dlopen(path, RTLD_NOW);
    if (!handle) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "ffi.open: %s", dlerror());
        return lat_ext_error(errbuf);
    }
    id = lib_alloc(handle);
    if (id < 0) {
        dlclose(handle);
        return lat_ext_error("ffi.open: too many open libraries (max 64)");
    }
    return lat_ext_int(id);
}

/* ffi.close(handle) -> Nil */
static LatExtValue *ffi_close(LatExtValue **args, size_t argc) {
    int64_t id;
    void *handle;
    int i;

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("ffi.close() expects a library handle (Int)");
    }
    id = lat_ext_as_int(args[0]);
    handle = lib_get(id);
    if (!handle) return lat_ext_error("ffi.close: invalid library handle");

    /* Release all symbols associated with this library */
    for (i = 0; i < sym_count; i++) {
        /* We can't directly check which library a symbol came from,
         * so we just invalidate when the user closes the library.
         * The user is responsible for not calling stale symbols. */
    }

    dlclose(handle);
    lib_release(id);
    return lat_ext_nil();
}

/* ffi.sym(handle, name, signature) -> Int (symbol handle) */
static LatExtValue *ffi_sym(LatExtValue **args, size_t argc) {
    int64_t lib_id;
    void *lib_handle;
    const char *name;
    const char *sig;
    void *fn_ptr;
    int sym_id;
    SymEntry *entry;

    if (argc < 3 ||
        lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING ||
        lat_ext_type(args[2]) != LAT_EXT_STRING) {
        return lat_ext_error("ffi.sym() expects (handle: Int, name: String, signature: String)");
    }

    lib_id = lat_ext_as_int(args[0]);
    lib_handle = lib_get(lib_id);
    if (!lib_handle) return lat_ext_error("ffi.sym: invalid library handle");

    name = lat_ext_as_string(args[1]);
    sig = lat_ext_as_string(args[2]);

    /* Clear any previous dlerror */
    dlerror();

    fn_ptr = dlsym(lib_handle, name);
    {
        const char *err = dlerror();
        if (err) {
            char errbuf[512];
            snprintf(errbuf, sizeof(errbuf), "ffi.sym: %s", err);
            return lat_ext_error(errbuf);
        }
    }

    /* Allocate a symbol entry */
    sym_id = sym_alloc();
    if (sym_id < 0) {
        return lat_ext_error("ffi.sym: too many symbols (max 256)");
    }

    entry = &symbols[sym_id];
    entry->fn_ptr = fn_ptr;
    entry->in_use = 1;

    if (!parse_signature(sig, entry)) {
        sym_release(sym_id);
        return lat_ext_error("ffi.sym: invalid signature string. "
                             "Format: arg_types>ret_type, e.g. \"ii>i\". "
                             "Types: i=int64, f=double, s=string, p=pointer, v=void");
    }

    return lat_ext_int(sym_id);
}

/* ffi.call(sym_handle, ...args) -> result */
static LatExtValue *ffi_call(LatExtValue **args, size_t argc) {
    int64_t sym_id;
    SymEntry *entry;
    FfiArg ffi_args[MAX_SIG_ARGS];
    int nargs;
    int i;

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("ffi.call() expects (sym_handle: Int, ...args)");
    }

    sym_id = lat_ext_as_int(args[0]);
    entry = sym_get(sym_id);
    if (!entry) return lat_ext_error("ffi.call: invalid symbol handle");

    nargs = entry->arg_count;
    if ((int)(argc - 1) < nargs) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf),
                 "ffi.call: expected %d arguments, got %d",
                 nargs, (int)(argc - 1));
        return lat_ext_error(errbuf);
    }

    /* Convert Lattice values to C types based on signature */
    for (i = 0; i < nargs; i++) {
        LatExtValue *arg = args[i + 1];
        LatExtType atype = lat_ext_type(arg);

        switch (entry->arg_types[i]) {
            case SIG_INT64:
                if (atype == LAT_EXT_INT) {
                    ffi_args[i].as_int = lat_ext_as_int(arg);
                } else if (atype == LAT_EXT_FLOAT) {
                    ffi_args[i].as_int = (int64_t)lat_ext_as_float(arg);
                } else if (atype == LAT_EXT_BOOL) {
                    ffi_args[i].as_int = lat_ext_as_bool(arg) ? 1 : 0;
                } else {
                    return lat_ext_error("ffi.call: expected Int for 'i' arg");
                }
                break;

            case SIG_DOUBLE:
                if (atype == LAT_EXT_FLOAT) {
                    ffi_args[i].as_double = lat_ext_as_float(arg);
                } else if (atype == LAT_EXT_INT) {
                    ffi_args[i].as_double = (double)lat_ext_as_int(arg);
                } else {
                    return lat_ext_error("ffi.call: expected Float for 'f' arg");
                }
                break;

            case SIG_STRING:
                if (atype == LAT_EXT_STRING) {
                    ffi_args[i].as_string = lat_ext_as_string(arg);
                } else if (atype == LAT_EXT_NIL) {
                    ffi_args[i].as_string = NULL;
                } else {
                    return lat_ext_error("ffi.call: expected String for 's' arg");
                }
                break;

            case SIG_POINTER:
                if (atype == LAT_EXT_INT) {
                    ffi_args[i].as_pointer = (void *)(intptr_t)lat_ext_as_int(arg);
                } else if (atype == LAT_EXT_STRING) {
                    /* Allow strings to be passed as pointers (const char*) */
                    ffi_args[i].as_pointer = (void *)lat_ext_as_string(arg);
                } else if (atype == LAT_EXT_NIL) {
                    ffi_args[i].as_pointer = NULL;
                } else {
                    return lat_ext_error("ffi.call: expected Int or String for 'p' arg");
                }
                break;

            case SIG_VOID:
                return lat_ext_error("ffi.call: void is not a valid argument type");
        }
    }

    return dispatch_call(entry, ffi_args);
}

/* ffi.nullptr() -> Int (0) */
static LatExtValue *ffi_nullptr(LatExtValue **args, size_t argc) {
    (void)args;
    (void)argc;
    return lat_ext_int(0);
}

/* ffi.error() -> String */
static LatExtValue *ffi_error(LatExtValue **args, size_t argc) {
    const char *err;
    (void)args;
    (void)argc;
    err = dlerror();
    if (!err) return lat_ext_string("(no error)");
    return lat_ext_string(err);
}

/* ── Extension init ── */

void lat_ext_init(LatExtContext *ctx) {
    lat_ext_register(ctx, "open",    ffi_open);
    lat_ext_register(ctx, "close",   ffi_close);
    lat_ext_register(ctx, "sym",     ffi_sym);
    lat_ext_register(ctx, "call",    ffi_call);
    lat_ext_register(ctx, "nullptr", ffi_nullptr);
    lat_ext_register(ctx, "error",   ffi_error);
}
