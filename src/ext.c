#include "ext.h"
#include "lattice_ext.h"
#include "eval.h"
#include "env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __EMSCRIPTEN__
#include <dlfcn.h>
#endif

/* ── LatExtValue: thin wrapper around LatValue ── */

struct LatExtValue {
    LatValue val;
};

/* ── LatExtContext: collects registered functions during init ── */

typedef struct {
    char    **names;
    LatExtFn *fns;
    size_t    count;
    size_t    cap;
} ExtRegistry;

struct LatExtContext {
    ExtRegistry reg;
};

/* ── Registration ── */

void lat_ext_register(LatExtContext *ctx, const char *name, LatExtFn fn) {
    if (ctx->reg.count >= ctx->reg.cap) {
        ctx->reg.cap = ctx->reg.cap ? ctx->reg.cap * 2 : 8;
        ctx->reg.names = realloc(ctx->reg.names, ctx->reg.cap * sizeof(char *));
        ctx->reg.fns = realloc(ctx->reg.fns, ctx->reg.cap * sizeof(LatExtFn));
    }
    ctx->reg.names[ctx->reg.count] = strdup(name);
    ctx->reg.fns[ctx->reg.count] = fn;
    ctx->reg.count++;
}

/* ── Constructors ── */

LatExtValue *lat_ext_int(int64_t v) {
    LatExtValue *ev = malloc(sizeof(LatExtValue));
    ev->val = value_int(v);
    return ev;
}

LatExtValue *lat_ext_float(double v) {
    LatExtValue *ev = malloc(sizeof(LatExtValue));
    ev->val = value_float(v);
    return ev;
}

LatExtValue *lat_ext_bool(bool v) {
    LatExtValue *ev = malloc(sizeof(LatExtValue));
    ev->val = value_bool(v);
    return ev;
}

LatExtValue *lat_ext_string(const char *s) {
    LatExtValue *ev = malloc(sizeof(LatExtValue));
    ev->val = value_string(s);
    return ev;
}

LatExtValue *lat_ext_nil(void) {
    LatExtValue *ev = malloc(sizeof(LatExtValue));
    ev->val = value_nil();
    return ev;
}

LatExtValue *lat_ext_array(LatExtValue **elems, size_t len) {
    LatValue *vals = malloc(len * sizeof(LatValue));
    for (size_t i = 0; i < len; i++) {
        vals[i] = value_deep_clone(&elems[i]->val);
    }
    LatExtValue *ev = malloc(sizeof(LatExtValue));
    ev->val = value_array(vals, len);
    free(vals);
    return ev;
}

LatExtValue *lat_ext_map_new(void) {
    LatExtValue *ev = malloc(sizeof(LatExtValue));
    ev->val = value_map_new();
    return ev;
}

void lat_ext_map_set(LatExtValue *map, const char *key, LatExtValue *val) {
    if (map->val.type != VAL_MAP) return;
    LatValue v = value_deep_clone(&val->val);
    lat_map_set(map->val.as.map.map, key, &v);
}

LatExtValue *lat_ext_error(const char *msg) {
    /* Errors are strings prefixed with "EVAL_ERROR:" */
    char *err = NULL;
    (void)asprintf(&err, "EVAL_ERROR:%s", msg);
    LatExtValue *ev = malloc(sizeof(LatExtValue));
    ev->val = value_string_owned(err);
    return ev;
}

/* ── Type query ── */

LatExtType lat_ext_type(const LatExtValue *v) {
    switch (v->val.type) {
        case VAL_INT:    return LAT_EXT_INT;
        case VAL_FLOAT:  return LAT_EXT_FLOAT;
        case VAL_BOOL:   return LAT_EXT_BOOL;
        case VAL_STR:    return LAT_EXT_STRING;
        case VAL_ARRAY:  return LAT_EXT_ARRAY;
        case VAL_MAP:    return LAT_EXT_MAP;
        case VAL_NIL:    return LAT_EXT_NIL;
        default:         return LAT_EXT_OTHER;
    }
}

/* ── Accessors ── */

int64_t lat_ext_as_int(const LatExtValue *v) {
    return v->val.as.int_val;
}

double lat_ext_as_float(const LatExtValue *v) {
    return v->val.as.float_val;
}

bool lat_ext_as_bool(const LatExtValue *v) {
    return v->val.as.bool_val;
}

const char *lat_ext_as_string(const LatExtValue *v) {
    return v->val.as.str_val;
}

size_t lat_ext_array_len(const LatExtValue *v) {
    if (v->val.type != VAL_ARRAY) return 0;
    return v->val.as.array.len;
}

LatExtValue *lat_ext_array_get(const LatExtValue *v, size_t index) {
    if (v->val.type != VAL_ARRAY || index >= v->val.as.array.len) return NULL;
    LatExtValue *ev = malloc(sizeof(LatExtValue));
    ev->val = value_deep_clone(&v->val.as.array.elems[index]);
    return ev;
}

LatExtValue *lat_ext_map_get(const LatExtValue *v, const char *key) {
    if (v->val.type != VAL_MAP) return NULL;
    LatValue *found = (LatValue *)lat_map_get(v->val.as.map.map, key);
    if (!found) return NULL;
    LatExtValue *ev = malloc(sizeof(LatExtValue));
    ev->val = value_deep_clone(found);
    return ev;
}

/* ── Cleanup ── */

void lat_ext_free(LatExtValue *v) {
    if (!v) return;
    value_free(&v->val);
    free(v);
}

/* ── Native closure trampoline ── */
/*
 * This is the function pointer stored in closure.native_fn.
 * It wraps calling an LatExtFn: converts args to LatExtValue**,
 * calls the extension function, and unwraps the result.
 *
 * We don't call it directly from call_closure; instead, eval.c's
 * native dispatch calls ext_call_native() which does the wrapping.
 */

LatValue ext_call_native(void *fn_ptr, LatValue *args, size_t argc) {
    LatExtFn fn = (LatExtFn)fn_ptr;

    /* Wrap args as LatExtValue pointers (stack-allocated wrappers) */
    LatExtValue *ext_args_storage = malloc(argc * sizeof(LatExtValue));
    LatExtValue **ext_args = malloc(argc * sizeof(LatExtValue *));
    for (size_t i = 0; i < argc; i++) {
        ext_args_storage[i].val = args[i];
        ext_args[i] = &ext_args_storage[i];
    }

    LatExtValue *result = fn(ext_args, argc);

    free(ext_args);
    free(ext_args_storage);

    if (!result) return value_nil();
    LatValue ret = value_deep_clone(&result->val);
    lat_ext_free(result);
    return ret;
}

/* ── Extension loader ── */

#ifndef __EMSCRIPTEN__

LatValue ext_load(Evaluator *ev, const char *name, char **err) {
    (void)ev;  /* for future use (e.g., passing evaluator context) */

    /* Determine library suffix */
#ifdef __APPLE__
    const char *suffix = ".dylib";
#else
    const char *suffix = ".so";
#endif

    /* Build search paths */
    char paths[5][1024];
    int path_count = 0;

    /* 1. ./extensions/<name><suffix> */
    snprintf(paths[path_count++], sizeof(paths[0]),
             "./extensions/%s%s", name, suffix);

    /* 2. ./extensions/<name>/<name><suffix> */
    snprintf(paths[path_count++], sizeof(paths[0]),
             "./extensions/%s/%s%s", name, name, suffix);

    /* 3. ~/.lattice/ext/<name><suffix> */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(paths[path_count++], sizeof(paths[0]),
                 "%s/.lattice/ext/%s%s", home, name, suffix);
    }

    /* 4. $LATTICE_EXT_PATH/<name><suffix> */
    const char *ext_path = getenv("LATTICE_EXT_PATH");
    if (ext_path) {
        snprintf(paths[path_count++], sizeof(paths[0]),
                 "%s/%s%s", ext_path, name, suffix);
    }

    /* Try each path */
    void *handle = NULL;
    for (int i = 0; i < path_count; i++) {
        handle = dlopen(paths[i], RTLD_NOW);
        if (handle) break;
    }

    if (!handle) {
        (void)asprintf(err, "require_ext: cannot find extension '%s' "
                       "(searched ./extensions/, ./extensions/%s/, ~/.lattice/ext/, $LATTICE_EXT_PATH)",
                       name, name);
        return value_nil();
    }

    /* Look up init function */
    LatExtInitFn init_fn = (LatExtInitFn)dlsym(handle, "lat_ext_init");
    if (!init_fn) {
        (void)asprintf(err, "require_ext: extension '%s' has no lat_ext_init()", name);
        dlclose(handle);
        return value_nil();
    }

    /* Create context and call init */
    LatExtContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    init_fn(&ctx);

    /* Build a Map of native closures */
    LatValue map = value_map_new();
    for (size_t i = 0; i < ctx.reg.count; i++) {
        /* Create a native closure: no params (variadic), no body, no env */
        char *pname = strdup("args");
        char **param_names = malloc(sizeof(char *));
        param_names[0] = pname;
        LatValue closure = value_closure(param_names, 1, NULL, NULL, NULL, true);
        closure.as.closure.native_fn = (void *)ctx.reg.fns[i];
        free(param_names);

        lat_map_set(map.as.map.map, ctx.reg.names[i], &closure);
        free(ctx.reg.names[i]);
    }
    free(ctx.reg.names);
    free(ctx.reg.fns);

    /* NOTE: we intentionally do NOT dlclose(handle) -- the library must stay
     * loaded while its functions are callable. The handle leaks, which is
     * acceptable for the lifetime of the process. */

    return map;
}

#else /* __EMSCRIPTEN__ */

LatValue ext_load(Evaluator *ev, const char *name, char **err) {
    (void)ev;
    (void)asprintf(err, "require_ext: native extensions not available in WASM (tried '%s')", name);
    return value_nil();
}

#endif /* __EMSCRIPTEN__ */
