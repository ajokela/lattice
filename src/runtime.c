#include "runtime.h"
#include "stackvm.h" /* For StackVM, StackCallFrame, ObjUpvalue, stackvm_run, stackvm_track_chunk, Chunk types */
#include "regvm.h"   /* For RegVM, RegChunk, regvm_run, regvm_track_chunk */
#include "stackopcode.h"
#include "stackcompiler.h"
#include "intern.h"
#include "builtins.h"
#include "lattice.h"
#include "math_ops.h"
#include "fs_ops.h"
#include "path_ops.h"
#include "net.h"
#include "tls.h"
#include "http.h"
#include "json.h"
#include "toml_ops.h"
#include "yaml_ops.h"
#include "crypto_ops.h"
#include "regex_ops.h"
#include "time_ops.h"
#include "datetime_ops.h"
#include <time.h>
#include "env_ops.h"
#include "process_ops.h"
#include "format_ops.h"
#include "type_ops.h"
#include "string_ops.h"
#include "array_ops.h"
#include "channel.h"
#include "ext.h"
#include "lexer.h"
#include "parser.h"
#include "stackcompiler.h"
#include "latc.h"
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#ifndef __EMSCRIPTEN__
#include <pthread.h>
#endif
#include "memory.h"

/* Native function pointer for StackVM builtins. */
typedef LatValue (*VMNativeFn)(LatValue *args, int arg_count);

/* Sentinel to distinguish native C functions from compiled closures. */
#define VM_NATIVE_MARKER ((struct Expr **)(uintptr_t)0x1)
#define VM_EXT_MARKER    ((struct Expr **)(uintptr_t)0x2)

/* Thread-local runtime pointer for native function dispatch. */
static _Thread_local LatRuntime *current_rt = NULL;

void lat_runtime_set_current(LatRuntime *rt) { current_rt = rt; }
LatRuntime *lat_runtime_current(void) { return current_rt; }

/* ── Phase system functions ── */

void rt_record_history(LatRuntime *rt, const char *name, LatValue *val) {
    for (size_t i = 0; i < rt->tracked_count; i++) {
        if (strcmp(rt->tracked_vars[i].name, name) != 0) continue;
        if (rt->tracked_vars[i].snap_count >= rt->tracked_vars[i].snap_cap) {
            rt->tracked_vars[i].snap_cap = rt->tracked_vars[i].snap_cap ? rt->tracked_vars[i].snap_cap * 2 : 4;
            rt->tracked_vars[i].snapshots = realloc(
                rt->tracked_vars[i].snapshots, rt->tracked_vars[i].snap_cap * sizeof(*rt->tracked_vars[i].snapshots));
        }
        size_t si = rt->tracked_vars[i].snap_count++;
        const char *phase_name = builtin_phase_of_str(val);
        rt->tracked_vars[i].snapshots[si].phase = strdup(phase_name);
        rt->tracked_vars[i].snapshots[si].value = value_deep_clone(val);
        rt->tracked_vars[i].snapshots[si].line = rt->current_line ? rt->current_line(rt->active_vm) : 0;
        rt->tracked_vars[i].snapshots[si].fn_name = NULL;
        return;
    }
}

void rt_fire_reactions(LatRuntime *rt, const char *name, const char *phase) {
    for (size_t i = 0; i < rt->reaction_count; i++) {
        if (strcmp(rt->reactions[i].var_name, name) != 0) continue;
        LatValue cur;
        bool found = rt->get_var_by_name(rt->active_vm, name, &cur);
        if (!found) return;
        for (size_t j = 0; j < rt->reactions[i].cb_count; j++) {
            LatValue *cb = &rt->reactions[i].callbacks[j];
            LatValue args[2];
            args[0] = value_string(phase);
            args[1] = value_deep_clone(&cur);
            LatValue result = rt->call_closure(rt->active_vm, cb, args, 2);
            value_free(&args[0]);
            value_free(&args[1]);
            value_free(&result);
            if (rt->error) {
                char *wrapped = NULL;
                lat_asprintf(&wrapped, "reaction error: %s", rt->error);
                free(rt->error);
                rt->error = wrapped;
                value_free(&cur);
                return;
            }
        }
        value_free(&cur);
        return;
    }
}

void rt_freeze_cascade(LatRuntime *rt, const char *target_name) {
    for (size_t bi = 0; bi < rt->bond_count; bi++) {
        if (strcmp(rt->bonds[bi].target, target_name) != 0) continue;
        for (size_t di = 0; di < rt->bonds[bi].dep_count; di++) {
            const char *dep = rt->bonds[bi].deps[di];
            const char *strategy = rt->bonds[bi].dep_strategies ? rt->bonds[bi].dep_strategies[di] : "mirror";
            LatValue dval;
            if (!rt->get_var_by_name(rt->active_vm, dep, &dval)) continue;
            if (dval.type == VAL_CHANNEL) {
                value_free(&dval);
                continue;
            }

            if (strcmp(strategy, "mirror") == 0) {
                if (dval.phase == VTAG_CRYSTAL) {
                    value_free(&dval);
                    continue;
                }
                LatValue frozen = value_freeze(dval);
                rt->set_var_by_name(rt->active_vm, dep, value_deep_clone(&frozen));
                value_free(&frozen);
                rt_fire_reactions(rt, dep, "crystal");
                if (rt->error) return;
                rt_freeze_cascade(rt, dep);
                if (rt->error) return;
            } else if (strcmp(strategy, "inverse") == 0) {
                if (dval.phase != VTAG_CRYSTAL && dval.phase != VTAG_SUBLIMATED) {
                    value_free(&dval);
                    continue;
                }
                LatValue thawed = value_thaw(&dval);
                value_free(&dval);
                rt->set_var_by_name(rt->active_vm, dep, value_deep_clone(&thawed));
                value_free(&thawed);
                rt_fire_reactions(rt, dep, "fluid");
                if (rt->error) return;
            } else if (strcmp(strategy, "gate") == 0) {
                if (dval.phase != VTAG_CRYSTAL) {
                    value_free(&dval);
                    char *err = NULL;
                    lat_asprintf(&err, "gate bond: '%s' must be crystal before '%s' can freeze", dep, target_name);
                    rt->error = err;
                    return;
                }
                value_free(&dval);
            } else {
                value_free(&dval);
            }
        }
        /* Consume the bond entry (one-shot) */
        for (size_t di = 0; di < rt->bonds[bi].dep_count; di++) {
            free(rt->bonds[bi].deps[di]);
            if (rt->bonds[bi].dep_strategies) free(rt->bonds[bi].dep_strategies[di]);
        }
        free(rt->bonds[bi].deps);
        free(rt->bonds[bi].dep_strategies);
        free(rt->bonds[bi].target);
        rt->bonds[bi] = rt->bonds[--rt->bond_count];
        break;
    }
}

char *rt_validate_seeds(LatRuntime *rt, const char *name, LatValue *val, bool consume) {
    for (size_t si = 0; si < rt->seed_count; si++) {
        if (strcmp(rt->seeds[si].var_name, name) != 0) continue;
        LatValue check_val = value_deep_clone(val);
        LatValue result = rt->call_closure(rt->active_vm, &rt->seeds[si].contract, &check_val, 1);
        value_free(&check_val);
        if (rt->error) {
            char *msg = NULL;
            char *inner = rt->error;
            rt->error = NULL;
            lat_asprintf(&msg, "seed contract failed: %s", inner);
            free(inner);
            value_free(&result);
            return msg;
        }
        if (!value_is_truthy(&result)) {
            value_free(&result);
            if (consume) {
                free(rt->seeds[si].var_name);
                value_free(&rt->seeds[si].contract);
                rt->seeds[si] = rt->seeds[--rt->seed_count];
            }
            return strdup("grow() seed contract returned false");
        }
        value_free(&result);
        if (consume) {
            free(rt->seeds[si].var_name);
            value_free(&rt->seeds[si].contract);
            rt->seeds[si] = rt->seeds[--rt->seed_count];
            si--;
        }
    }
    return NULL;
}

/* ── Native builtins ── */

/* ── Native builtins ── */

static LatValue native_to_string(LatValue *args, int arg_count) {
    if (arg_count != 1) return value_string("to_string() expects 1 argument");
    char *s = builtin_to_string(&args[0]);
    LatValue r = value_string(s);
    free(s);
    return r;
}

static LatValue native_typeof(LatValue *args, int arg_count) {
    if (arg_count != 1) return value_string("typeof() expects 1 argument");
    return value_string(builtin_typeof_str(&args[0]));
}

static LatValue native_len(LatValue *args, int arg_count) {
    if (arg_count != 1) return value_int(0);
    LatValue *v = &args[0];
    if (v->type == VAL_REF) v = &v->as.ref.ref->value;
    if (v->type == VAL_ARRAY) return value_int((int64_t)v->as.array.len);
    if (v->type == VAL_STR) return value_int((int64_t)strlen(v->as.str_val));
    if (v->type == VAL_MAP) return value_int((int64_t)lat_map_len(v->as.map.map));
    if (v->type == VAL_BUFFER) return value_int((int64_t)v->as.buffer.len);
    return value_int(0);
}

static LatValue native_parse_int(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_STR) return value_nil();
    bool ok;
    int64_t v = builtin_parse_int(args[0].as.str_val, &ok);
    return ok ? value_int(v) : value_nil();
}

static LatValue native_parse_float(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_STR) return value_nil();
    bool ok;
    double v = builtin_parse_float(args[0].as.str_val, &ok);
    return ok ? value_float(v) : value_nil();
}

static LatValue native_ord(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_STR) return value_int(-1);
    return value_int(builtin_ord(args[0].as.str_val));
}

static LatValue native_chr(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_INT) return value_string("");
    char *s = builtin_chr(args[0].as.int_val);
    LatValue r = value_string(s);
    free(s);
    return r;
}

static LatValue native_abs(LatValue *args, int arg_count) {
    if (arg_count != 1) return value_int(0);
    if (args[0].type == VAL_INT) return value_int(args[0].as.int_val < 0 ? -args[0].as.int_val : args[0].as.int_val);
    if (args[0].type == VAL_FLOAT)
        return value_float(args[0].as.float_val < 0 ? -args[0].as.float_val : args[0].as.float_val);
    return value_int(0);
}

static LatValue native_floor(LatValue *args, int arg_count) {
    if (arg_count != 1) return value_int(0);
    if (args[0].type == VAL_FLOAT) return value_int((int64_t)args[0].as.float_val);
    if (args[0].type == VAL_INT) return args[0];
    return value_int(0);
}

static LatValue native_ceil(LatValue *args, int arg_count) {
    if (arg_count != 1) return value_int(0);
    if (args[0].type == VAL_FLOAT) {
        double v = args[0].as.float_val;
        int64_t i = (int64_t)v;
        return value_int(v > (double)i ? i + 1 : i);
    }
    if (args[0].type == VAL_INT) return args[0];
    return value_int(0);
}

static LatValue native_exit(LatValue *args, int arg_count) {
    int code = 0;
    if (arg_count >= 1 && args[0].type == VAL_INT) code = (int)args[0].as.int_val;
    exit(code);
    return value_unit();
}

static LatValue native_error(LatValue *args, int arg_count) {
    if (arg_count < 1) return value_nil();
    /* Create an error map: {"tag" => "err", "value" => arg} */
    LatValue map_val = value_map_new();
    LatValue tag = value_string("err");
    lat_map_set(map_val.as.map.map, "tag", &tag);
    LatValue val = value_deep_clone(&args[0]);
    lat_map_set(map_val.as.map.map, "value", &val);
    return map_val;
}

static LatValue native_is_error(LatValue *args, int arg_count) {
    if (arg_count < 1) return value_bool(false);
    if (args[0].type != VAL_MAP) return value_bool(false);
    LatValue *tag = lat_map_get(args[0].as.map.map, "tag");
    if (!tag || tag->type != VAL_STR) return value_bool(false);
    return value_bool(strcmp(tag->as.str_val, "err") == 0);
}

static LatValue native_map_new(LatValue *args, int arg_count) {
    (void)args;
    (void)arg_count;
    return value_map_new();
}

static LatValue native_set_new(LatValue *args, int arg_count) {
    (void)args;
    (void)arg_count;
    return value_set_new();
}

static LatValue native_set_from(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_ARRAY) return value_set_new();
    LatValue set = value_set_new();
    for (size_t i = 0; i < args[0].as.array.len; i++) {
        char *key = value_display(&args[0].as.array.elems[i]);
        LatValue clone = value_deep_clone(&args[0].as.array.elems[i]);
        lat_map_set(set.as.set.map, key, &clone);
        free(key);
    }
    return set;
}

static LatValue native_channel_new(LatValue *args, int arg_count) {
    (void)args;
    (void)arg_count;
    LatChannel *ch = channel_new();
    LatValue val = value_channel(ch);
    channel_release(ch);
    return val;
}

/* ── Buffer constructors ── */

static LatValue native_buffer_new(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_INT) return value_buffer_alloc(0);
    int64_t size = args[0].as.int_val;
    if (size < 0) size = 0;
    return value_buffer_alloc((size_t)size);
}

static LatValue native_buffer_from(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_ARRAY) return value_buffer(NULL, 0);
    size_t len = args[0].as.array.len;
    uint8_t *data = malloc(len > 0 ? len : 1);
    if (!data) return value_unit();
    for (size_t i = 0; i < len; i++) {
        if (args[0].as.array.elems[i].type == VAL_INT) data[i] = (uint8_t)(args[0].as.array.elems[i].as.int_val & 0xFF);
        else data[i] = 0;
    }
    LatValue buf = value_buffer(data, len);
    free(data);
    return buf;
}

static LatValue native_buffer_from_string(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_STR) return value_buffer(NULL, 0);
    const char *s = args[0].as.str_val;
    size_t len = strlen(s);
    return value_buffer((const uint8_t *)s, len);
}

static LatValue native_ref_new(LatValue *args, int arg_count) {
    if (arg_count != 1) return value_nil();
    return value_ref(args[0]);
}

static LatValue native_read_file_bytes(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_nil();
    FILE *f = fopen(args[0].as.str_val, "rb");
    if (!f) return value_nil();
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen < 0) {
        fclose(f);
        return value_nil();
    }
    uint8_t *data = malloc((size_t)flen);
    if (!data) return value_unit();
    size_t nread = fread(data, 1, (size_t)flen, f);
    fclose(f);
    LatValue buf = value_buffer(data, nread);
    free(data);
    return buf;
}

static LatValue native_write_file_bytes(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_BUFFER) return value_bool(false);
    FILE *f = fopen(args[0].as.str_val, "wb");
    if (!f) return value_bool(false);
    size_t written = fwrite(args[1].as.buffer.data, 1, args[1].as.buffer.len, f);
    fclose(f);
    return value_bool(written == args[1].as.buffer.len);
}

/* ── Phase 6: Phase system native functions ── */

static LatValue native_track(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR || !current_rt) return value_unit();
    const char *name = args[0].as.str_val;
    /* Check if already tracked */
    for (size_t i = 0; i < current_rt->tracked_count; i++) {
        if (strcmp(current_rt->tracked_vars[i].name, name) == 0) return value_unit();
    }
    /* Find the variable's current value (try env first, then locals) */
    LatValue val;
    bool found = env_get(current_rt->env, name, &val);
    if (!found) found = current_rt->find_local_value(current_rt->active_vm, name, &val);
    if (!found) {
        char *msg = NULL;
        lat_asprintf(&msg, "track: undefined variable '%s'", name);
        current_rt->error = msg;
        return value_unit();
    }
    /* Register tracking */
    if (current_rt->tracked_count >= current_rt->tracked_cap) {
        current_rt->tracked_cap = current_rt->tracked_cap ? current_rt->tracked_cap * 2 : 4;
        current_rt->tracked_vars =
            realloc(current_rt->tracked_vars, current_rt->tracked_cap * sizeof(*current_rt->tracked_vars));
    }
    size_t idx = current_rt->tracked_count++;
    current_rt->tracking_active = true;
    current_rt->tracked_vars[idx].name = strdup(name);
    current_rt->tracked_vars[idx].snapshots = NULL;
    current_rt->tracked_vars[idx].snap_count = 0;
    current_rt->tracked_vars[idx].snap_cap = 0;
    /* Record initial snapshot */
    rt_record_history(current_rt, name, &val);
    value_free(&val);
    return value_unit();
}

static LatValue native_phases(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR || !current_rt) { return value_array(NULL, 0); }
    const char *name = args[0].as.str_val;
    for (size_t i = 0; i < current_rt->tracked_count; i++) {
        if (strcmp(current_rt->tracked_vars[i].name, name) != 0) continue;
        size_t n = current_rt->tracked_vars[i].snap_count;
        LatValue *elems = malloc(n * sizeof(LatValue));
        if (!elems) return value_unit();
        for (size_t j = 0; j < n; j++) {
            LatValue m = value_map_new();
            LatValue phase_val = value_string(current_rt->tracked_vars[i].snapshots[j].phase);
            LatValue val_clone = value_deep_clone(&current_rt->tracked_vars[i].snapshots[j].value);
            LatValue line_val = value_int(current_rt->tracked_vars[i].snapshots[j].line);
            LatValue fn_val = current_rt->tracked_vars[i].snapshots[j].fn_name
                                  ? value_string(current_rt->tracked_vars[i].snapshots[j].fn_name)
                                  : value_nil();
            lat_map_set(m.as.map.map, "phase", &phase_val);
            lat_map_set(m.as.map.map, "value", &val_clone);
            lat_map_set(m.as.map.map, "line", &line_val);
            lat_map_set(m.as.map.map, "fn", &fn_val);
            elems[j] = m;
        }
        LatValue arr = value_array(elems, n);
        free(elems);
        return arr;
    }
    return value_array(NULL, 0);
}

/// @builtin history(name: String) -> Array
/// @category Temporal
/// Returns the full enriched timeline of a tracked variable as an array of Maps
/// with keys: phase, value, line, fn.
/// @example history(x)  // [{phase: "fluid", value: 10, line: 3, fn: "main"}, ...]
static LatValue native_history(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR || !current_rt) return value_array(NULL, 0);
    const char *name = args[0].as.str_val;
    for (size_t i = 0; i < current_rt->tracked_count; i++) {
        if (strcmp(current_rt->tracked_vars[i].name, name) != 0) continue;
        size_t n = current_rt->tracked_vars[i].snap_count;
        LatValue *elems = malloc(n * sizeof(LatValue));
        if (!elems) return value_unit();
        for (size_t j = 0; j < n; j++) {
            LatValue m = value_map_new();
            LatValue phase_val = value_string(current_rt->tracked_vars[i].snapshots[j].phase);
            LatValue val_clone = value_deep_clone(&current_rt->tracked_vars[i].snapshots[j].value);
            LatValue line_val = value_int(current_rt->tracked_vars[i].snapshots[j].line);
            LatValue fn_val = current_rt->tracked_vars[i].snapshots[j].fn_name
                                  ? value_string(current_rt->tracked_vars[i].snapshots[j].fn_name)
                                  : value_nil();
            lat_map_set(m.as.map.map, "phase", &phase_val);
            lat_map_set(m.as.map.map, "value", &val_clone);
            lat_map_set(m.as.map.map, "line", &line_val);
            lat_map_set(m.as.map.map, "fn", &fn_val);
            elems[j] = m;
        }
        LatValue arr = value_array(elems, n);
        free(elems);
        return arr;
    }
    return value_array(NULL, 0);
}

static LatValue native_rewind(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_INT || !current_rt) return value_nil();
    const char *name = args[0].as.str_val;
    int64_t steps = args[1].as.int_val;
    for (size_t i = 0; i < current_rt->tracked_count; i++) {
        if (strcmp(current_rt->tracked_vars[i].name, name) != 0) continue;
        int64_t idx = (int64_t)current_rt->tracked_vars[i].snap_count - 1 - steps;
        if (idx < 0 || idx >= (int64_t)current_rt->tracked_vars[i].snap_count) return value_nil();
        return value_deep_clone(&current_rt->tracked_vars[i].snapshots[idx].value);
    }
    return value_nil();
}

static LatValue native_pressurize(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR || !current_rt) return value_unit();
    const char *name = args[0].as.str_val;
    const char *mode = args[1].as.str_val;
    /* Validate mode */
    if (strcmp(mode, "no_grow") != 0 && strcmp(mode, "no_shrink") != 0 && strcmp(mode, "no_resize") != 0 &&
        strcmp(mode, "read_heavy") != 0)
        return value_unit();
    /* Update existing or add new */
    for (size_t i = 0; i < current_rt->pressure_count; i++) {
        if (strcmp(current_rt->pressures[i].name, name) == 0) {
            free(current_rt->pressures[i].mode);
            current_rt->pressures[i].mode = strdup(mode);
            return value_unit();
        }
    }
    if (current_rt->pressure_count >= current_rt->pressure_cap) {
        current_rt->pressure_cap = current_rt->pressure_cap ? current_rt->pressure_cap * 2 : 4;
        current_rt->pressures =
            realloc(current_rt->pressures, current_rt->pressure_cap * sizeof(*current_rt->pressures));
    }
    size_t idx = current_rt->pressure_count++;
    current_rt->pressures[idx].name = strdup(name);
    current_rt->pressures[idx].mode = strdup(mode);
    return value_unit();
}

static LatValue native_depressurize(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR || !current_rt) return value_unit();
    const char *name = args[0].as.str_val;
    for (size_t i = 0; i < current_rt->pressure_count; i++) {
        if (strcmp(current_rt->pressures[i].name, name) == 0) {
            free(current_rt->pressures[i].name);
            free(current_rt->pressures[i].mode);
            current_rt->pressures[i] = current_rt->pressures[--current_rt->pressure_count];
            return value_unit();
        }
    }
    return value_unit();
}

static LatValue native_pressure_of(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR || !current_rt) return value_nil();
    const char *name = args[0].as.str_val;
    for (size_t i = 0; i < current_rt->pressure_count; i++) {
        if (strcmp(current_rt->pressures[i].name, name) == 0) return value_string(current_rt->pressures[i].mode);
    }
    return value_nil();
}
/* Full grow() implementation as native function */
static LatValue native_grow(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR || !current_rt) return value_nil();
    const char *vname = args[0].as.str_val;
    LatValue val;
    if (!current_rt->get_var_by_name(current_rt->active_vm, vname, &val)) return value_nil();

    /* Validate and consume all seeds */
    char *err = rt_validate_seeds(current_rt, vname, &val, true);
    if (err) {
        value_free(&val);
        /* Report error through StackVM */
        current_rt->error = err;
        return value_nil();
    }

    /* Freeze */
    LatValue frozen = value_freeze(val);
    LatValue ret = value_deep_clone(&frozen);
    current_rt->set_var_by_name(current_rt->active_vm, vname, value_deep_clone(&frozen));
    rt_record_history(current_rt, vname, &frozen);
    value_free(&frozen);

    /* Cascade and reactions */

    rt_freeze_cascade(current_rt, vname);
    rt_fire_reactions(current_rt, vname, "crystal");

    return ret;
}

static LatValue native_phase_of(LatValue *args, int arg_count) {
    if (arg_count != 1) return value_string("unknown");
    return value_string(builtin_phase_of_str(&args[0]));
}

static LatValue native_assert(LatValue *args, int arg_count) {
    if (arg_count < 1) return value_unit();
    bool ok = false;
    if (args[0].type == VAL_BOOL) ok = args[0].as.bool_val;
    else if (args[0].type == VAL_INT) ok = args[0].as.int_val != 0;
    else ok = args[0].type != VAL_NIL;
    if (!ok) {
        const char *msg = (arg_count >= 2 && args[1].type == VAL_STR) ? args[1].as.str_val : "assertion failed";
        /* Use StackVM error mechanism instead of exit() so tests can catch failures */
        if (current_rt) {
            char *err = NULL;
            lat_asprintf(&err, "assertion failed: %s", msg);
            current_rt->error = err;
        } else {
            fprintf(stderr, "assertion failed: %s\n", msg);
            exit(1);
        }
    }
    return value_unit();
}

static LatValue native_version(LatValue *args, int arg_count) {
    (void)args;
    (void)arg_count;
    return value_string(LATTICE_VERSION);
}

static LatValue native_input(LatValue *args, int arg_count) {
    const char *prompt = NULL;
    if (arg_count >= 1 && args[0].type == VAL_STR) prompt = args[0].as.str_val;
    char *line = builtin_input(prompt);
    if (!line) return value_nil();
    LatValue r = value_string(line);
    free(line);
    return r;
}

/* ── Math natives (via math_ops.h) ── */

#define MATH1(cname, mathfn)                        \
    static LatValue cname(LatValue *args, int ac) { \
        if (ac != 1) return value_nil();            \
        char *err = NULL;                           \
        LatValue r = mathfn(&args[0], &err);        \
        if (err) {                                  \
            current_rt->error = err;                \
            return value_nil();                     \
        }                                           \
        return r;                                   \
    }

#define MATH2(cname, mathfn)                           \
    static LatValue cname(LatValue *args, int ac) {    \
        if (ac != 2) return value_nil();               \
        char *err = NULL;                              \
        LatValue r = mathfn(&args[0], &args[1], &err); \
        if (err) {                                     \
            current_rt->error = err;                   \
            return value_nil();                        \
        }                                              \
        return r;                                      \
    }

#define MATH3(cname, mathfn)                                     \
    static LatValue cname(LatValue *args, int ac) {              \
        if (ac != 3) return value_nil();                         \
        char *err = NULL;                                        \
        LatValue r = mathfn(&args[0], &args[1], &args[2], &err); \
        if (err) {                                               \
            current_rt->error = err;                             \
            return value_nil();                                  \
        }                                                        \
        return r;                                                \
    }

MATH1(native_round, math_round)
MATH1(native_sqrt, math_sqrt)
MATH2(native_pow, math_pow)
MATH2(native_min, math_min)
MATH2(native_max, math_max)
MATH1(native_log, math_log)
MATH1(native_log2, math_log2)
MATH1(native_log10, math_log10)
MATH1(native_sin, math_sin)
MATH1(native_cos, math_cos)
MATH1(native_tan, math_tan)
MATH1(native_asin, math_asin)
MATH1(native_acos, math_acos)
MATH1(native_atan, math_atan)
MATH2(native_atan2, math_atan2)
MATH1(native_exp, math_exp)
MATH1(native_sign, math_sign)
MATH2(native_gcd, math_gcd)
MATH2(native_lcm, math_lcm)
MATH1(native_is_nan, math_is_nan)
MATH1(native_is_inf, math_is_inf)
MATH1(native_sinh, math_sinh)
MATH1(native_cosh, math_cosh)
MATH1(native_tanh, math_tanh)
MATH3(native_lerp, math_lerp)
MATH3(native_clamp, math_clamp)

static LatValue native_random(LatValue *args, int ac) {
    (void)args;
    (void)ac;
    return math_random();
}
static LatValue native_random_int(LatValue *args, int ac) {
    if (ac != 2) return value_nil();
    char *err = NULL;
    LatValue r = math_random_int(&args[0], &args[1], &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return r;
}
static LatValue native_math_pi(LatValue *args, int ac) {
    (void)args;
    (void)ac;
    return math_pi();
}
static LatValue native_math_e(LatValue *args, int ac) {
    (void)args;
    (void)ac;
    return math_e();
}

#undef MATH1
#undef MATH2
#undef MATH3

/* ── File system natives ── */

static LatValue native_read_file(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("read_file() expects (path: String)");
        return value_nil();
    }
    char *contents = builtin_read_file(args[0].as.str_val);
    if (!contents) return value_nil();
    return value_string_owned(contents);
}
static LatValue native_write_file(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_rt->error = strdup("write_file() expects (path: String, data: String)");
        return value_bool(false);
    }
    return value_bool(builtin_write_file(args[0].as.str_val, args[1].as.str_val));
}
static LatValue native_file_exists(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("file_exists() expects (path: String)");
        return value_bool(false);
    }
    return value_bool(fs_file_exists(args[0].as.str_val));
}
static LatValue native_delete_file(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("delete_file: expected (path: Str)");
        return value_bool(false);
    }
    char *err = NULL;
    bool ok = fs_delete_file(args[0].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_bool(false);
    }
    return value_bool(ok);
}
static LatValue native_list_dir(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("list_dir: expected (path: Str)");
        return value_array(NULL, 0);
    }
    char *err = NULL;
    size_t count = 0;
    char **entries = fs_list_dir(args[0].as.str_val, &count, &err);
    if (err) {
        current_rt->error = err;
        return value_array(NULL, 0);
    }
    if (!entries) return value_array(NULL, 0);
    LatValue *elems = malloc((count > 0 ? count : 1) * sizeof(LatValue));
    if (!elems) return value_unit();
    for (size_t i = 0; i < count; i++) {
        elems[i] = value_string(entries[i]);
        free(entries[i]);
    }
    free(entries);
    LatValue r = value_array(elems, count);
    free(elems);
    return r;
}
static LatValue native_append_file(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_rt->error = strdup("append_file() expects (path: String, data: String)");
        return value_bool(false);
    }
    char *err = NULL;
    bool ok = fs_append_file(args[0].as.str_val, args[1].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_bool(false);
    }
    return value_bool(ok);
}
static LatValue native_mkdir(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_bool(false);
    char *err = NULL;
    bool ok = fs_mkdir(args[0].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_bool(false);
    }
    return value_bool(ok);
}
static LatValue native_fs_rename(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_rt->error = strdup("rename: expected (from: Str, to: Str)");
        return value_bool(false);
    }
    char *err = NULL;
    bool ok = fs_rename(args[0].as.str_val, args[1].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_bool(false);
    }
    return value_bool(ok);
}
static LatValue native_is_dir(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_bool(false);
    return value_bool(fs_is_dir(args[0].as.str_val));
}
static LatValue native_is_file(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_bool(false);
    return value_bool(fs_is_file(args[0].as.str_val));
}
static LatValue native_rmdir(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("rmdir: expected (path: Str)");
        return value_bool(false);
    }
    char *err = NULL;
    bool ok = fs_rmdir(args[0].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_bool(false);
    }
    return value_bool(ok);
}
static LatValue native_glob(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_array(NULL, 0);
    char *err = NULL;
    size_t count = 0;
    char **matches = fs_glob(args[0].as.str_val, &count, &err);
    if (err) {
        current_rt->error = err;
        return value_array(NULL, 0);
    }
    if (!matches) return value_array(NULL, 0);
    LatValue *elems = malloc((count > 0 ? count : 1) * sizeof(LatValue));
    if (!elems) return value_unit();
    for (size_t i = 0; i < count; i++) {
        elems[i] = value_string(matches[i]);
        free(matches[i]);
    }
    free(matches);
    LatValue r = value_array(elems, count);
    free(elems);
    return r;
}
static LatValue native_stat(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("stat: expected (path: Str)");
        return value_nil();
    }
    int64_t sz, mt, md;
    const char *tp;
    char *err = NULL;
    if (!fs_stat(args[0].as.str_val, &sz, &mt, &md, &tp, &err)) {
        if (err) current_rt->error = err;
        return value_nil();
    }
    LatValue map = value_map_new();
    LatValue v_sz = value_int(sz);
    lat_map_set(map.as.map.map, "size", &v_sz);
    LatValue v_mt = value_int(mt);
    lat_map_set(map.as.map.map, "mtime", &v_mt);
    LatValue v_md = value_int(md);
    lat_map_set(map.as.map.map, "mode", &v_md);
    LatValue v_tp = value_string(tp);
    lat_map_set(map.as.map.map, "type", &v_tp);
    LatValue v_pm = value_int(md);
    lat_map_set(map.as.map.map, "permissions", &v_pm);
    return map;
}
static LatValue native_copy_file(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_rt->error = strdup("copy_file: expected (src: Str, dst: Str)");
        return value_bool(false);
    }
    char *err = NULL;
    bool ok = fs_copy_file(args[0].as.str_val, args[1].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_bool(false);
    }
    return value_bool(ok);
}
static LatValue native_realpath(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("realpath: expected (path: Str)");
        return value_nil();
    }
    char *err = NULL;
    char *r = fs_realpath(args[0].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    if (!r) return value_nil();
    return value_string_owned(r);
}
static LatValue native_tempdir(LatValue *args, int ac) {
    (void)args;
    (void)ac;
    char *err = NULL;
    char *r = fs_tempdir(&err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    if (!r) return value_nil();
    return value_string_owned(r);
}
static LatValue native_tempfile(LatValue *args, int ac) {
    (void)args;
    (void)ac;
    char *err = NULL;
    char *r = fs_tempfile(&err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    if (!r) return value_nil();
    return value_string_owned(r);
}
static LatValue native_chmod(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_INT) return value_bool(false);
    char *err = NULL;
    bool ok = fs_chmod(args[0].as.str_val, (int)args[1].as.int_val, &err);
    if (err) {
        current_rt->error = err;
        return value_bool(false);
    }
    return value_bool(ok);
}
static LatValue native_file_size(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("file_size: expected (path: Str)");
        return value_int(-1);
    }
    char *err = NULL;
    int64_t sz = fs_file_size(args[0].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_int(-1);
    }
    return value_int(sz);
}

/* ── Path natives ── */

static LatValue native_path_join(LatValue *args, int ac) {
    if (ac < 1) {
        current_rt->error = strdup("path_join() expects at least 1 argument");
        return value_string("");
    }
    for (int i = 0; i < ac; i++) {
        if (args[i].type != VAL_STR) {
            current_rt->error = strdup("path_join() expects (String...)");
            return value_string("");
        }
    }
    const char **parts = malloc((size_t)ac * sizeof(char *));
    if (!parts) return value_unit();
    for (int i = 0; i < ac; i++) parts[i] = args[i].as.str_val;
    char *r = path_join(parts, (size_t)ac);
    free(parts);
    return value_string_owned(r);
}
static LatValue native_path_dir(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("path_dir() expects (path: String)");
        return value_string(".");
    }
    return value_string_owned(path_dir(args[0].as.str_val));
}
static LatValue native_path_base(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("path_base() expects (path: String)");
        return value_string("");
    }
    return value_string_owned(path_base(args[0].as.str_val));
}
static LatValue native_path_ext(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("path_ext() expects (path: String)");
        return value_string("");
    }
    return value_string_owned(path_ext(args[0].as.str_val));
}

/* ── Network TCP natives ── */

static LatValue native_tcp_listen(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_INT) {
        current_rt->error = strdup("tcp_listen: expected (host: Str, port: Int)");
        return value_int(-1);
    }
    char *err = NULL;
    int fd = net_tcp_listen(args[0].as.str_val, (int)args[1].as.int_val, &err);
    if (err) {
        current_rt->error = err;
        return value_int(-1);
    }
    return value_int(fd);
}
static LatValue native_tcp_accept(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("tcp_accept: expected (fd: Int)");
        return value_int(-1);
    }
    char *err = NULL;
    int fd = net_tcp_accept((int)args[0].as.int_val, &err);
    if (err) {
        current_rt->error = err;
        return value_int(-1);
    }
    return value_int(fd);
}
static LatValue native_tcp_connect(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_INT) {
        current_rt->error = strdup("tcp_connect: expected (host: Str, port: Int)");
        return value_int(-1);
    }
    char *err = NULL;
    int fd = net_tcp_connect(args[0].as.str_val, (int)args[1].as.int_val, &err);
    if (err) {
        current_rt->error = err;
        return value_int(-1);
    }
    return value_int(fd);
}
static LatValue native_tcp_read(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("tcp_read: expected (fd: Int)");
        return value_string("");
    }
    char *err = NULL;
    char *data = net_tcp_read((int)args[0].as.int_val, &err);
    if (err) {
        current_rt->error = err;
        return value_string("");
    }
    if (!data) return value_string("");
    return value_string_owned(data);
}
static LatValue native_tcp_read_bytes(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT) return value_string("");
    char *err = NULL;
    char *data = net_tcp_read_bytes((int)args[0].as.int_val, (size_t)args[1].as.int_val, &err);
    if (err) {
        current_rt->error = err;
        return value_string("");
    }
    if (!data) return value_string("");
    return value_string_owned(data);
}
static LatValue native_tcp_write(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_INT || args[1].type != VAL_STR) return value_bool(false);
    char *err = NULL;
    bool ok = net_tcp_write((int)args[0].as.int_val, args[1].as.str_val, strlen(args[1].as.str_val), &err);
    if (err) {
        current_rt->error = err;
        return value_bool(false);
    }
    return value_bool(ok);
}
static LatValue native_tcp_close(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) return value_unit();
    net_tcp_close((int)args[0].as.int_val);
    return value_unit();
}
static LatValue native_tcp_peer_addr(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) return value_nil();
    char *err = NULL;
    char *addr = net_tcp_peer_addr((int)args[0].as.int_val, &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    if (!addr) return value_nil();
    return value_string_owned(addr);
}
static LatValue native_tcp_set_timeout(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT) return value_bool(false);
    char *err = NULL;
    bool ok = net_tcp_set_timeout((int)args[0].as.int_val, (int)args[1].as.int_val, &err);
    if (err) {
        current_rt->error = err;
        return value_bool(false);
    }
    return value_bool(ok);
}

/* ── TLS natives ── */

static LatValue native_tls_connect(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_INT) {
        current_rt->error = strdup("tls_connect: expected (host: Str, port: Int)");
        return value_int(-1);
    }
    char *err = NULL;
    int fd = net_tls_connect(args[0].as.str_val, (int)args[1].as.int_val, &err);
    if (err) {
        current_rt->error = err;
        return value_int(-1);
    }
    return value_int(fd);
}
static LatValue native_tls_read(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("tls_read() expects (fd: Int)");
        return value_string("");
    }
    char *err = NULL;
    char *data = net_tls_read((int)args[0].as.int_val, &err);
    if (err) {
        current_rt->error = err;
        return value_string("");
    }
    if (!data) return value_string("");
    return value_string_owned(data);
}
static LatValue native_tls_read_bytes(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT) {
        current_rt->error = strdup("tls_read_bytes() expects (fd: Int, n: Int)");
        return value_string("");
    }
    char *err = NULL;
    char *data = net_tls_read_bytes((int)args[0].as.int_val, (size_t)args[1].as.int_val, &err);
    if (err) {
        current_rt->error = err;
        return value_string("");
    }
    if (!data) return value_string("");
    return value_string_owned(data);
}
static LatValue native_tls_write(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_INT || args[1].type != VAL_STR) {
        current_rt->error = strdup("tls_write() expects (fd: Int, data: String)");
        return value_bool(false);
    }
    char *err = NULL;
    bool ok = net_tls_write((int)args[0].as.int_val, args[1].as.str_val, strlen(args[1].as.str_val), &err);
    if (err) {
        current_rt->error = err;
        return value_bool(false);
    }
    return value_bool(ok);
}
static LatValue native_tls_close(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("tls_close() expects (fd: Int)");
        return value_unit();
    }
    net_tls_close((int)args[0].as.int_val);
    return value_unit();
}
static LatValue native_tls_available(LatValue *args, int ac) {
    (void)args;
    (void)ac;
    return value_bool(net_tls_available());
}

/* ── HTTP natives ── */

static LatValue vm_build_http_response(HttpResponse *resp) {
    LatValue map = value_map_new();
    LatValue st = value_int(resp->status_code);
    lat_map_set(map.as.map.map, "status", &st);
    LatValue bd = value_string(resp->body ? resp->body : "");
    lat_map_set(map.as.map.map, "body", &bd);
    LatValue hdr = value_map_new();
    for (size_t i = 0; i < resp->header_count; i++) {
        LatValue hv = value_string(resp->header_values[i]);
        lat_map_set(hdr.as.map.map, resp->header_keys[i], &hv);
    }
    lat_map_set(map.as.map.map, "headers", &hdr);
    http_response_free(resp);
    return map;
}
static LatValue native_http_get(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("http_get() expects (url: String)");
        return value_nil();
    }
    HttpRequest req = {0};
    req.method = "GET";
    req.url = args[0].as.str_val;
    char *err = NULL;
    HttpResponse *resp = http_execute(&req, &err);
    if (!resp) {
        current_rt->error = err ? err : strdup("http_get: request failed");
        return value_nil();
    }
    return vm_build_http_response(resp);
}
static LatValue native_http_post(LatValue *args, int ac) {
    if (ac < 1 || ac > 2 || args[0].type != VAL_STR) {
        current_rt->error = strdup("http_post() expects (url: String, options?: Map)");
        return value_nil();
    }
    HttpRequest req = {0};
    req.method = "POST";
    req.url = args[0].as.str_val;
    if (ac >= 2 && args[1].type == VAL_STR) {
        req.body = args[1].as.str_val;
        req.body_len = strlen(args[1].as.str_val);
    } else if (ac >= 2 && args[1].type == VAL_MAP) {
        LatValue *bv = lat_map_get(args[1].as.map.map, "body");
        if (bv && bv->type == VAL_STR) {
            req.body = bv->as.str_val;
            req.body_len = strlen(bv->as.str_val);
        }
    }
    char *err = NULL;
    HttpResponse *resp = http_execute(&req, &err);
    if (!resp) {
        current_rt->error = err ? err : strdup("http_post: request failed");
        return value_nil();
    }
    return vm_build_http_response(resp);
}
static LatValue native_http_request(LatValue *args, int ac) {
    if (ac < 2 || ac > 3 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_rt->error = strdup("http_request() expects (method: String, url: String, options?: Map)");
        return value_nil();
    }
    HttpRequest req = {0};
    req.method = args[0].as.str_val;
    req.url = args[1].as.str_val;
    if (ac == 3 && args[2].type == VAL_MAP) {
        LatValue *bv = lat_map_get(args[2].as.map.map, "body");
        if (bv && bv->type == VAL_STR) {
            req.body = bv->as.str_val;
            req.body_len = strlen(bv->as.str_val);
        }
    }
    char *err = NULL;
    HttpResponse *resp = http_execute(&req, &err);
    if (!resp) {
        current_rt->error = err ? err : strdup("http_request: request failed");
        return value_nil();
    }
    return vm_build_http_response(resp);
}

/* ── JSON/TOML/YAML natives ── */

static LatValue native_json_parse(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("json_parse: expected (str: Str)");
        return value_nil();
    }
    char *err = NULL;
    LatValue r = json_parse(args[0].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return r;
}
static LatValue native_json_stringify(LatValue *args, int ac) {
    if (ac != 1) {
        current_rt->error = strdup("json_stringify: expected 1 argument");
        return value_nil();
    }
    char *err = NULL;
    char *r = json_stringify(&args[0], &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return value_string_owned(r);
}
static LatValue native_toml_parse(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("toml_parse() expects (String)");
        return value_nil();
    }
    char *err = NULL;
    LatValue r = toml_ops_parse(args[0].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return r;
}
static LatValue native_toml_stringify(LatValue *args, int ac) {
    if (ac != 1) {
        current_rt->error = strdup("toml_stringify: expected 1 argument");
        return value_nil();
    }
    char *err = NULL;
    char *r = toml_ops_stringify(&args[0], &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    if (!r) return value_nil();
    return value_string_owned(r);
}
static LatValue native_yaml_parse(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("yaml_parse() expects (String)");
        return value_nil();
    }
    char *err = NULL;
    LatValue r = yaml_ops_parse(args[0].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return r;
}
static LatValue native_yaml_stringify(LatValue *args, int ac) {
    if (ac != 1) {
        current_rt->error = strdup("yaml_stringify: expected 1 argument");
        return value_nil();
    }
    if (args[0].type != VAL_MAP && args[0].type != VAL_ARRAY) {
        current_rt->error = strdup("yaml_stringify: value must be a Map or Array");
        return value_nil();
    }
    char *err = NULL;
    char *r = yaml_ops_stringify(&args[0], &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    if (!r) return value_nil();
    return value_string_owned(r);
}

/* ── Crypto natives ── */

static LatValue native_sha256(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("sha256: expected (str: Str)");
        return value_nil();
    }
    char *err = NULL;
    char *r = crypto_sha256(args[0].as.str_val, strlen(args[0].as.str_val), &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return value_string_owned(r);
}
static LatValue native_md5(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("md5: expected (str: Str)");
        return value_nil();
    }
    char *err = NULL;
    char *r = crypto_md5(args[0].as.str_val, strlen(args[0].as.str_val), &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return value_string_owned(r);
}
static LatValue native_base64_encode(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("base64_encode: expected (str: Str)");
        return value_nil();
    }
    return value_string_owned(crypto_base64_encode(args[0].as.str_val, strlen(args[0].as.str_val)));
}
static LatValue native_base64_decode(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("base64_decode: expected (str: Str)");
        return value_nil();
    }
    char *err = NULL;
    size_t dl = 0;
    char *r = crypto_base64_decode(args[0].as.str_val, strlen(args[0].as.str_val), &dl, &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return value_string_owned(r);
}

static LatValue native_sha512(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("sha512: expected (str: Str)");
        return value_nil();
    }
    char *err = NULL;
    char *r = crypto_sha512(args[0].as.str_val, strlen(args[0].as.str_val), &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return value_string_owned(r);
}
static LatValue native_hmac_sha256(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_rt->error = strdup("hmac_sha256: expected (key: Str, data: Str)");
        return value_nil();
    }
    char *err = NULL;
    char *r = crypto_hmac_sha256(args[0].as.str_val, strlen(args[0].as.str_val), args[1].as.str_val,
                                 strlen(args[1].as.str_val), &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return value_string_owned(r);
}
static LatValue native_random_bytes(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("random_bytes: expected (n: Int)");
        return value_nil();
    }
    int64_t n = args[0].as.int_val;
    if (n < 0 || n > 1048576) {
        current_rt->error = strdup("random_bytes: n must be 0..1048576");
        return value_nil();
    }
    char *err = NULL;
    uint8_t *buf = crypto_random_bytes((size_t)n, &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    LatValue result = value_buffer(buf, (size_t)n);
    free(buf);
    return result;
}

/* ── Regex natives ── */

static LatValue native_regex_match(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_rt->error = strdup("regex_match: expected (pattern: Str, input: Str)");
        return value_bool(false);
    }
    char *err = NULL;
    LatValue r = regex_match(args[0].as.str_val, args[1].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_bool(false);
    }
    return r;
}
static LatValue native_regex_find_all(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_rt->error = strdup("regex_find_all: expected (pattern: Str, input: Str)");
        return value_array(NULL, 0);
    }
    char *err = NULL;
    LatValue r = regex_find_all(args[0].as.str_val, args[1].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_array(NULL, 0);
    }
    return r;
}
static LatValue native_regex_replace(LatValue *args, int ac) {
    if (ac != 3 || args[0].type != VAL_STR || args[1].type != VAL_STR || args[2].type != VAL_STR) return value_nil();
    char *err = NULL;
    char *r = regex_replace(args[0].as.str_val, args[1].as.str_val, args[2].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    if (!r) return value_nil();
    return value_string_owned(r);
}

/* ── Time/DateTime natives ── */

static LatValue native_time(LatValue *args, int ac) {
    (void)args;
    if (ac != 0) {
        current_rt->error = strdup("time() expects no arguments");
        return value_int(0);
    }
    return value_int(time_now_ms());
}
static LatValue native_sleep(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("sleep() expects (ms: Int)");
        return value_unit();
    }
    char *err = NULL;
    time_sleep_ms(args[0].as.int_val, &err);
    if (err) {
        current_rt->error = err;
        return value_unit();
    }
    return value_unit();
}
static LatValue native_time_format(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_INT || args[1].type != VAL_STR) {
        current_rt->error = strdup("time_format: expected (timestamp: Int, format: Str)");
        return value_nil();
    }
    char *err = NULL;
    char *r = datetime_format(args[0].as.int_val, args[1].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return value_string_owned(r);
}
static LatValue native_time_parse(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_rt->error = strdup("time_parse: expected (str: Str, format: Str)");
        return value_nil();
    }
    char *err = NULL;
    int64_t r = datetime_parse(args[0].as.str_val, args[1].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return value_int(r);
}
static LatValue native_time_year(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("time_year: expected (timestamp: Int)");
        return value_nil();
    }
    return value_int(datetime_year(args[0].as.int_val));
}
static LatValue native_time_month(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("time_month: expected (timestamp: Int)");
        return value_nil();
    }
    return value_int(datetime_month(args[0].as.int_val));
}
static LatValue native_time_day(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("time_day: expected (timestamp: Int)");
        return value_nil();
    }
    return value_int(datetime_day(args[0].as.int_val));
}
static LatValue native_time_hour(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("time_hour: expected (timestamp: Int)");
        return value_nil();
    }
    return value_int(datetime_hour(args[0].as.int_val));
}
static LatValue native_time_minute(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("time_minute: expected (timestamp: Int)");
        return value_nil();
    }
    return value_int(datetime_minute(args[0].as.int_val));
}
static LatValue native_time_second(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("time_second: expected (timestamp: Int)");
        return value_nil();
    }
    return value_int(datetime_second(args[0].as.int_val));
}
static LatValue native_time_weekday(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("time_weekday: expected (timestamp: Int)");
        return value_nil();
    }
    return value_int(datetime_weekday(args[0].as.int_val));
}
static LatValue native_time_add(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT) {
        current_rt->error = strdup("time_add: expected (timestamp: Int, delta_ms: Int)");
        return value_nil();
    }
    return value_int(datetime_add(args[0].as.int_val, args[1].as.int_val));
}
static LatValue native_is_leap_year(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("is_leap_year: expected (year: Int)");
        return value_nil();
    }
    return value_bool(datetime_is_leap_year((int)args[0].as.int_val));
}

/* ── Duration helpers ── */

static LatValue make_duration_map(int64_t total_ms) {
    int64_t ms = total_ms % 1000;
    if (ms < 0) ms = -ms;
    int64_t rem = total_ms / 1000;
    int64_t s = rem % 60;
    if (s < 0) s = -s;
    rem /= 60;
    int64_t m = rem % 60;
    if (m < 0) m = -m;
    int64_t h = rem / 60;

    LatValue map = value_map_new();
    LatValue vh = value_int(h);
    LatValue vm_ = value_int(m);
    LatValue vs = value_int(s);
    LatValue vms = value_int(ms);
    LatValue vtot = value_int(total_ms);
    lat_map_set(map.as.map.map, "hours", &vh);
    lat_map_set(map.as.map.map, "minutes", &vm_);
    lat_map_set(map.as.map.map, "seconds", &vs);
    lat_map_set(map.as.map.map, "millis", &vms);
    lat_map_set(map.as.map.map, "total_ms", &vtot);
    return map;
}

/// @builtin duration(hours: Int, minutes: Int, seconds: Int, millis: Int) -> Map
/// @category Date & Time
/// Create a Duration map with hours, minutes, seconds, millis fields.
/// @example duration(2, 30, 15, 0)  // {hours: 2, minutes: 30, seconds: 15, millis: 0, total_ms: 9015000}
static LatValue native_duration(LatValue *args, int ac) {
    if (ac != 4 || args[0].type != VAL_INT || args[1].type != VAL_INT || args[2].type != VAL_INT ||
        args[3].type != VAL_INT) {
        current_rt->error = strdup("duration: expected (hours: Int, minutes: Int, seconds: Int, millis: Int)");
        return value_nil();
    }
    int64_t total =
        args[0].as.int_val * 3600000 + args[1].as.int_val * 60000 + args[2].as.int_val * 1000 + args[3].as.int_val;
    return make_duration_map(total);
}

/// @builtin duration_from_seconds(s: Int) -> Map
/// @category Date & Time
/// Create a Duration from total seconds.
/// @example duration_from_seconds(3661)  // {hours: 1, minutes: 1, seconds: 1, ...}
static LatValue native_duration_from_seconds(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("duration_from_seconds: expected (seconds: Int)");
        return value_nil();
    }
    return make_duration_map(args[0].as.int_val * 1000);
}

/// @builtin duration_from_millis(ms: Int) -> Map
/// @category Date & Time
/// Create a Duration from total milliseconds.
/// @example duration_from_millis(5000)  // {hours: 0, minutes: 0, seconds: 5, ...}
static LatValue native_duration_from_millis(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("duration_from_millis: expected (millis: Int)");
        return value_nil();
    }
    return make_duration_map(args[0].as.int_val);
}

static int64_t duration_map_to_ms(LatValue *d) {
    if (d->type != VAL_MAP) return 0;
    LatValue *tot = lat_map_get(d->as.map.map, "total_ms");
    if (tot && tot->type == VAL_INT) return tot->as.int_val;
    return 0;
}

/// @builtin duration_add(d1: Map, d2: Map) -> Map
/// @category Date & Time
/// Add two Duration maps.
static LatValue native_duration_add(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_MAP || args[1].type != VAL_MAP) {
        current_rt->error = strdup("duration_add: expected (d1: Map, d2: Map)");
        return value_nil();
    }
    int64_t t = duration_map_to_ms(&args[0]) + duration_map_to_ms(&args[1]);
    return make_duration_map(t);
}

/// @builtin duration_sub(d1: Map, d2: Map) -> Map
/// @category Date & Time
/// Subtract Duration d2 from d1.
static LatValue native_duration_sub(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_MAP || args[1].type != VAL_MAP) {
        current_rt->error = strdup("duration_sub: expected (d1: Map, d2: Map)");
        return value_nil();
    }
    int64_t t = duration_map_to_ms(&args[0]) - duration_map_to_ms(&args[1]);
    return make_duration_map(t);
}

/// @builtin duration_to_string(d: Map) -> String
/// @category Date & Time
/// Format a Duration map as a human-readable string like "2h 30m 15s".
/// @example duration_to_string(duration(2, 30, 15, 0))  // "2h 30m 15s"
static LatValue native_duration_to_string(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_MAP) {
        current_rt->error = strdup("duration_to_string: expected (d: Map)");
        return value_nil();
    }
    int64_t total = duration_map_to_ms(&args[0]);
    int64_t ms = total % 1000;
    if (ms < 0) ms = -ms;
    int64_t rem = total / 1000;
    int64_t s = rem % 60;
    if (s < 0) s = -s;
    rem /= 60;
    int64_t m = rem % 60;
    if (m < 0) m = -m;
    int64_t h = rem / 60;

    char buf[128];
    if (ms > 0) {
        snprintf(buf, sizeof(buf), "%lldh %lldm %llds %lldms", (long long)h, (long long)m, (long long)s, (long long)ms);
    } else {
        snprintf(buf, sizeof(buf), "%lldh %lldm %llds", (long long)h, (long long)m, (long long)s);
    }
    return value_string(buf);
}

/// @builtin duration_hours(d: Map) -> Int
/// @category Date & Time
/// Extract the hours component from a Duration.
static LatValue native_duration_hours(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_MAP) {
        current_rt->error = strdup("duration_hours: expected (d: Map)");
        return value_nil();
    }
    LatValue *v = lat_map_get(args[0].as.map.map, "hours");
    return (v && v->type == VAL_INT) ? value_int(v->as.int_val) : value_int(0);
}

/// @builtin duration_minutes(d: Map) -> Int
/// @category Date & Time
/// Extract the minutes component from a Duration.
static LatValue native_duration_minutes(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_MAP) {
        current_rt->error = strdup("duration_minutes: expected (d: Map)");
        return value_nil();
    }
    LatValue *v = lat_map_get(args[0].as.map.map, "minutes");
    return (v && v->type == VAL_INT) ? value_int(v->as.int_val) : value_int(0);
}

/// @builtin duration_seconds(d: Map) -> Int
/// @category Date & Time
/// Extract the seconds component from a Duration.
static LatValue native_duration_seconds(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_MAP) {
        current_rt->error = strdup("duration_seconds: expected (d: Map)");
        return value_nil();
    }
    LatValue *v = lat_map_get(args[0].as.map.map, "seconds");
    return (v && v->type == VAL_INT) ? value_int(v->as.int_val) : value_int(0);
}

/// @builtin duration_millis(d: Map) -> Int
/// @category Date & Time
/// Extract the millis component from a Duration.
static LatValue native_duration_millis(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_MAP) {
        current_rt->error = strdup("duration_millis: expected (d: Map)");
        return value_nil();
    }
    LatValue *v = lat_map_get(args[0].as.map.map, "millis");
    return (v && v->type == VAL_INT) ? value_int(v->as.int_val) : value_int(0);
}

/* ── DateTime map helpers ── */

static LatValue make_datetime_map(int year, int month, int day, int hour, int minute, int second, int tz_offset_sec) {
    LatValue map = value_map_new();
    LatValue vy = value_int(year);
    LatValue vmo = value_int(month);
    LatValue vd = value_int(day);
    LatValue vh = value_int(hour);
    LatValue vmi = value_int(minute);
    LatValue vs = value_int(second);
    LatValue vtz = value_int(tz_offset_sec);
    lat_map_set(map.as.map.map, "year", &vy);
    lat_map_set(map.as.map.map, "month", &vmo);
    lat_map_set(map.as.map.map, "day", &vd);
    lat_map_set(map.as.map.map, "hour", &vh);
    lat_map_set(map.as.map.map, "minute", &vmi);
    lat_map_set(map.as.map.map, "second", &vs);
    lat_map_set(map.as.map.map, "tz_offset", &vtz);
    return map;
}

static bool datetime_map_extract(LatValue *dt, int *year, int *month, int *day, int *hour, int *minute, int *second,
                                 int *tz_offset) {
    if (dt->type != VAL_MAP) return false;
    LatValue *vy = lat_map_get(dt->as.map.map, "year");
    LatValue *vmo = lat_map_get(dt->as.map.map, "month");
    LatValue *vd = lat_map_get(dt->as.map.map, "day");
    LatValue *vh = lat_map_get(dt->as.map.map, "hour");
    LatValue *vmi = lat_map_get(dt->as.map.map, "minute");
    LatValue *vs = lat_map_get(dt->as.map.map, "second");
    LatValue *vtz = lat_map_get(dt->as.map.map, "tz_offset");
    if (!vy || vy->type != VAL_INT || !vmo || vmo->type != VAL_INT || !vd || vd->type != VAL_INT) return false;
    *year = (int)vy->as.int_val;
    *month = (int)vmo->as.int_val;
    *day = (int)vd->as.int_val;
    *hour = (vh && vh->type == VAL_INT) ? (int)vh->as.int_val : 0;
    *minute = (vmi && vmi->type == VAL_INT) ? (int)vmi->as.int_val : 0;
    *second = (vs && vs->type == VAL_INT) ? (int)vs->as.int_val : 0;
    *tz_offset = (vtz && vtz->type == VAL_INT) ? (int)vtz->as.int_val : 0;
    return true;
}

/// @builtin datetime_now() -> Map
/// @category Date & Time
/// Returns a DateTime map with current local time components.
/// @example datetime_now()  // {year: 2026, month: 2, day: 24, hour: 10, ...}
static LatValue native_datetime_now(LatValue *args, int ac) {
    (void)args;
    if (ac != 0) {
        current_rt->error = strdup("datetime_now: expects no arguments");
        return value_nil();
    }
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    int tz_off = datetime_tz_offset_seconds();
    return make_datetime_map(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                             local.tm_sec, tz_off);
}

/// @builtin datetime_from_epoch(epoch_seconds: Int) -> Map
/// @category Date & Time
/// Create a DateTime map from epoch seconds (UTC).
/// @example datetime_from_epoch(0)  // {year: 1970, month: 1, day: 1, hour: 0, ...}
static LatValue native_datetime_from_epoch(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_INT) {
        current_rt->error = strdup("datetime_from_epoch: expected (epoch_seconds: Int)");
        return value_nil();
    }
    int y, mo, d, h, mi, s;
    datetime_to_utc_components(args[0].as.int_val, &y, &mo, &d, &h, &mi, &s);
    return make_datetime_map(y, mo, d, h, mi, s, 0);
}

/// @builtin datetime_to_epoch(dt: Map) -> Int
/// @category Date & Time
/// Convert a DateTime map to epoch seconds.
static LatValue native_datetime_to_epoch(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_MAP) {
        current_rt->error = strdup("datetime_to_epoch: expected (dt: Map)");
        return value_nil();
    }
    int y, mo, d, h, mi, s, tz;
    if (!datetime_map_extract(&args[0], &y, &mo, &d, &h, &mi, &s, &tz)) {
        current_rt->error = strdup("datetime_to_epoch: invalid DateTime map");
        return value_nil();
    }
    int64_t epoch = datetime_from_components(y, mo, d, h, mi, s, tz);
    return value_int(epoch);
}

/// @builtin datetime_from_iso(str: String) -> Map
/// @category Date & Time
/// Parse an ISO 8601 string into a DateTime map.
/// @example datetime_from_iso("2026-02-24T10:30:00Z")
static LatValue native_datetime_from_iso(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("datetime_from_iso: expected (str: String)");
        return value_nil();
    }
    char *err = NULL;
    int64_t epoch = datetime_parse_iso(args[0].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    int y, mo, d, h, mi, s;
    datetime_to_utc_components(epoch, &y, &mo, &d, &h, &mi, &s);
    return make_datetime_map(y, mo, d, h, mi, s, 0);
}

/// @builtin datetime_to_iso(dt: Map) -> String
/// @category Date & Time
/// Format a DateTime map as an ISO 8601 string.
/// @example datetime_to_iso(datetime_from_epoch(0))  // "1970-01-01T00:00:00Z"
static LatValue native_datetime_to_iso(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_MAP) {
        current_rt->error = strdup("datetime_to_iso: expected (dt: Map)");
        return value_nil();
    }
    int y, mo, d, h, mi, s, tz;
    if (!datetime_map_extract(&args[0], &y, &mo, &d, &h, &mi, &s, &tz)) {
        current_rt->error = strdup("datetime_to_iso: invalid DateTime map");
        return value_nil();
    }
    int64_t epoch = datetime_from_components(y, mo, d, h, mi, s, tz);
    char *iso = datetime_to_iso(epoch);
    return value_string_owned(iso);
}

/// @builtin datetime_add_duration(dt: Map, dur: Map) -> Map
/// @category Date & Time
/// Add a Duration to a DateTime, returning a new DateTime.
static LatValue native_datetime_add_duration(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_MAP || args[1].type != VAL_MAP) {
        current_rt->error = strdup("datetime_add_duration: expected (dt: Map, dur: Map)");
        return value_nil();
    }
    int y, mo, d, h, mi, s, tz;
    if (!datetime_map_extract(&args[0], &y, &mo, &d, &h, &mi, &s, &tz)) {
        current_rt->error = strdup("datetime_add_duration: invalid DateTime map");
        return value_nil();
    }
    int64_t epoch = datetime_from_components(y, mo, d, h, mi, s, tz);
    int64_t dur_ms = duration_map_to_ms(&args[1]);
    epoch += dur_ms / 1000;

    /* Convert back, preserving original tz_offset */
    int64_t utc_epoch = epoch + (int64_t)tz;
    int ny, nmo, nd, nh, nmi, ns;
    datetime_to_utc_components(utc_epoch, &ny, &nmo, &nd, &nh, &nmi, &ns);
    return make_datetime_map(ny, nmo, nd, nh, nmi, ns, tz);
}

/// @builtin datetime_sub(dt1: Map, dt2: Map) -> Map
/// @category Date & Time
/// Subtract two DateTimes, returning a Duration.
static LatValue native_datetime_sub(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_MAP || args[1].type != VAL_MAP) {
        current_rt->error = strdup("datetime_sub: expected (dt1: Map, dt2: Map)");
        return value_nil();
    }
    int y1, mo1, d1, h1, mi1, s1, tz1;
    int y2, mo2, d2, h2, mi2, s2, tz2;
    if (!datetime_map_extract(&args[0], &y1, &mo1, &d1, &h1, &mi1, &s1, &tz1) ||
        !datetime_map_extract(&args[1], &y2, &mo2, &d2, &h2, &mi2, &s2, &tz2)) {
        current_rt->error = strdup("datetime_sub: invalid DateTime map");
        return value_nil();
    }
    int64_t e1 = datetime_from_components(y1, mo1, d1, h1, mi1, s1, tz1);
    int64_t e2 = datetime_from_components(y2, mo2, d2, h2, mi2, s2, tz2);
    return make_duration_map((e1 - e2) * 1000);
}

/// @builtin datetime_format(dt: Map, fmt: String) -> String
/// @category Date & Time
/// Format a DateTime map using strftime-style format.
/// @example datetime_format(datetime_from_epoch(0), "%Y-%m-%d")  // "1970-01-01"
static LatValue native_datetime_fmt(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_MAP || args[1].type != VAL_STR) {
        current_rt->error = strdup("datetime_format: expected (dt: Map, fmt: String)");
        return value_nil();
    }
    int y, mo, d, h, mi, s, tz;
    if (!datetime_map_extract(&args[0], &y, &mo, &d, &h, &mi, &s, &tz)) {
        current_rt->error = strdup("datetime_format: invalid DateTime map");
        return value_nil();
    }
    int64_t epoch = datetime_from_components(y, mo, d, h, mi, s, tz);
    /* Use epoch_ms interface (multiply by 1000) with gmtime-based formatting */
    struct tm tm;
    time_t t = (time_t)epoch;
    gmtime_r(&t, &tm);
    char buf[512];
    size_t n = strftime(buf, sizeof(buf), args[1].as.str_val, &tm);
    if (n == 0) {
        current_rt->error = strdup("datetime_format: format produced empty string or exceeded buffer");
        return value_nil();
    }
    return value_string(buf);
}

/// @builtin datetime_to_utc(dt: Map) -> Map
/// @category Date & Time
/// Convert a DateTime to UTC (tz_offset becomes 0).
static LatValue native_datetime_to_utc(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_MAP) {
        current_rt->error = strdup("datetime_to_utc: expected (dt: Map)");
        return value_nil();
    }
    int y, mo, d, h, mi, s, tz;
    if (!datetime_map_extract(&args[0], &y, &mo, &d, &h, &mi, &s, &tz)) {
        current_rt->error = strdup("datetime_to_utc: invalid DateTime map");
        return value_nil();
    }
    int64_t epoch = datetime_from_components(y, mo, d, h, mi, s, tz);
    int ny, nmo, nd, nh, nmi, ns;
    datetime_to_utc_components(epoch, &ny, &nmo, &nd, &nh, &nmi, &ns);
    return make_datetime_map(ny, nmo, nd, nh, nmi, ns, 0);
}

/// @builtin datetime_to_local(dt: Map) -> Map
/// @category Date & Time
/// Convert a DateTime to the local timezone.
static LatValue native_datetime_to_local(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_MAP) {
        current_rt->error = strdup("datetime_to_local: expected (dt: Map)");
        return value_nil();
    }
    int y, mo, d, h, mi, s, tz;
    if (!datetime_map_extract(&args[0], &y, &mo, &d, &h, &mi, &s, &tz)) {
        current_rt->error = strdup("datetime_to_local: invalid DateTime map");
        return value_nil();
    }
    int64_t epoch = datetime_from_components(y, mo, d, h, mi, s, tz);
    time_t t = (time_t)epoch;
    struct tm local;
    localtime_r(&t, &local);
    int local_tz = datetime_tz_offset_seconds();
    return make_datetime_map(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                             local.tm_sec, local_tz);
}

/// @builtin timezone_offset() -> Int
/// @category Date & Time
/// Returns the current local timezone offset from UTC in seconds.
static LatValue native_timezone_offset(LatValue *args, int ac) {
    (void)args;
    if (ac != 0) {
        current_rt->error = strdup("timezone_offset: expects no arguments");
        return value_nil();
    }
    return value_int(datetime_tz_offset_seconds());
}

/// @builtin days_in_month(year: Int, month: Int) -> Int
/// @category Date & Time
/// Returns the number of days in the given month of the given year.
/// @example days_in_month(2024, 2)  // 29
static LatValue native_days_in_month(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_INT || args[1].type != VAL_INT) {
        current_rt->error = strdup("days_in_month: expected (year: Int, month: Int)");
        return value_nil();
    }
    int r = datetime_days_in_month((int)args[0].as.int_val, (int)args[1].as.int_val);
    if (r < 0) {
        current_rt->error = strdup("days_in_month: month must be 1-12");
        return value_nil();
    }
    return value_int(r);
}

/// @builtin day_of_week(year: Int, month: Int, day: Int) -> Int
/// @category Date & Time
/// Returns the day of week (0=Sunday, 6=Saturday).
/// @example day_of_week(2026, 2, 24)  // 2 (Tuesday)
static LatValue native_day_of_week(LatValue *args, int ac) {
    if (ac != 3 || args[0].type != VAL_INT || args[1].type != VAL_INT || args[2].type != VAL_INT) {
        current_rt->error = strdup("day_of_week: expected (year: Int, month: Int, day: Int)");
        return value_nil();
    }
    return value_int(datetime_day_of_week((int)args[0].as.int_val, (int)args[1].as.int_val, (int)args[2].as.int_val));
}

/// @builtin day_of_year(year: Int, month: Int, day: Int) -> Int
/// @category Date & Time
/// Returns the day of year (1-366).
/// @example day_of_year(2026, 2, 24)  // 55
static LatValue native_day_of_year(LatValue *args, int ac) {
    if (ac != 3 || args[0].type != VAL_INT || args[1].type != VAL_INT || args[2].type != VAL_INT) {
        current_rt->error = strdup("day_of_year: expected (year: Int, month: Int, day: Int)");
        return value_nil();
    }
    int r = datetime_day_of_year((int)args[0].as.int_val, (int)args[1].as.int_val, (int)args[2].as.int_val);
    if (r < 0) {
        current_rt->error = strdup("day_of_year: month must be 1-12");
        return value_nil();
    }
    return value_int(r);
}

/* ── Environment natives ── */

static LatValue native_env(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("env() expects (key: String)");
        return value_unit();
    }
    char *val = envvar_get(args[0].as.str_val);
    if (!val) return value_unit();
    return value_string_owned(val);
}
static LatValue native_env_set(LatValue *args, int ac) {
    if (ac != 2 || args[0].type != VAL_STR || args[1].type != VAL_STR) {
        current_rt->error = strdup("env_set: expected (key: Str, value: Str)");
        return value_bool(false);
    }
    char *err = NULL;
    bool ok = envvar_set(args[0].as.str_val, args[1].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_bool(false);
    }
    return value_bool(ok);
}
static LatValue native_env_keys(LatValue *args, int ac) {
    (void)args;
    (void)ac;
    char **keys = NULL;
    size_t count = 0;
    envvar_keys(&keys, &count);
    LatValue *elems = malloc((count > 0 ? count : 1) * sizeof(LatValue));
    if (!elems) return value_unit();
    for (size_t i = 0; i < count; i++) {
        elems[i] = value_string(keys[i]);
        free(keys[i]);
    }
    free(keys);
    LatValue r = value_array(elems, count);
    free(elems);
    return r;
}

/* ── Process natives ── */

static LatValue native_cwd(LatValue *args, int ac) {
    (void)args;
    (void)ac;
    char *err = NULL;
    char *r = process_cwd(&err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    if (!r) return value_nil();
    return value_string_owned(r);
}
static LatValue native_exec_cmd(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_nil();
    char *err = NULL;
    LatValue r = process_exec(args[0].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return r;
}
static LatValue native_shell(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_nil();
    char *err = NULL;
    LatValue r = process_shell(args[0].as.str_val, &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return r;
}
static LatValue native_platform(LatValue *args, int ac) {
    (void)args;
    (void)ac;
    return value_string(process_platform());
}
static LatValue native_hostname(LatValue *args, int ac) {
    (void)args;
    (void)ac;
    char *err = NULL;
    char *r = process_hostname(&err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    if (!r) return value_nil();
    return value_string_owned(r);
}
static LatValue native_pid(LatValue *args, int ac) {
    (void)args;
    (void)ac;
    return value_int(process_pid());
}

/* ── Type/utility natives ── */

static LatValue native_to_int(LatValue *args, int ac) {
    if (ac != 1) return value_nil();
    char *err = NULL;
    LatValue r = type_to_int(&args[0], &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return r;
}
static LatValue native_to_float(LatValue *args, int ac) {
    if (ac != 1) return value_nil();
    char *err = NULL;
    LatValue r = type_to_float(&args[0], &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return r;
}
static LatValue native_struct_name(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STRUCT) return value_nil();
    return value_string(args[0].as.strct.name);
}
static LatValue native_struct_fields(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STRUCT) return value_array(NULL, 0);
    size_t fc = args[0].as.strct.field_count;
    LatValue *elems = malloc((fc > 0 ? fc : 1) * sizeof(LatValue));
    if (!elems) return value_unit();
    for (size_t i = 0; i < fc; i++) elems[i] = value_string(args[0].as.strct.field_names[i]);
    LatValue r = value_array(elems, fc);
    free(elems);
    return r;
}
static LatValue native_struct_to_map(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STRUCT) return value_nil();
    LatValue map = value_map_new();
    for (size_t i = 0; i < args[0].as.strct.field_count; i++) {
        LatValue v = value_deep_clone(&args[0].as.strct.field_values[i]);
        lat_map_set(map.as.map.map, args[0].as.strct.field_names[i], &v);
    }
    return map;
}
static LatValue native_repr(LatValue *args, int ac) {
    if (ac != 1) return value_nil();
    /* Check for custom repr closure on structs */
    if (args[0].type == VAL_STRUCT) {
        for (size_t i = 0; i < args[0].as.strct.field_count; i++) {
            if (strcmp(args[0].as.strct.field_names[i], "repr") == 0 &&
                args[0].as.strct.field_values[i].type == VAL_CLOSURE) {
                LatValue self = value_deep_clone(&args[0]);
                LatValue result =
                    current_rt->call_closure(current_rt->active_vm, &args[0].as.strct.field_values[i], &self, 1);
                value_free(&self);
                if (result.type == VAL_STR) return result;
                value_free(&result);
                break; /* fall through to default */
            }
        }
    }
    return value_string_owned(value_repr(&args[0]));
}
static LatValue native_format(LatValue *args, int ac) {
    if (ac < 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("format: expected (fmt: Str, ...)");
        return value_nil();
    }
    char *err = NULL;
    char *r = format_string(args[0].as.str_val, args + 1, (size_t)(ac - 1), &err);
    if (err) {
        current_rt->error = err;
        return value_nil();
    }
    return value_string_owned(r);
}
static LatValue native_range(LatValue *args, int ac) {
    if (ac < 2 || ac > 3 || args[0].type != VAL_INT || args[1].type != VAL_INT) {
        current_rt->error = strdup("range() expects (start: Int, end: Int, step?: Int)");
        return value_array(NULL, 0);
    }
    int64_t rstart = args[0].as.int_val, rend = args[1].as.int_val;
    int64_t rstep = (rstart <= rend) ? 1 : -1;
    if (ac == 3) {
        if (args[2].type != VAL_INT) {
            current_rt->error = strdup("range() step must be Int");
            return value_array(NULL, 0);
        }
        rstep = args[2].as.int_val;
    }
    if (rstep == 0) {
        current_rt->error = strdup("range() step cannot be zero");
        return value_array(NULL, 0);
    }
    size_t rcount = 0;
    if (rstep > 0 && rstart < rend) rcount = (size_t)((rend - rstart + rstep - 1) / rstep);
    else if (rstep < 0 && rstart > rend) rcount = (size_t)((rstart - rend + (-rstep) - 1) / (-rstep));
    LatValue *relems = malloc((rcount > 0 ? rcount : 1) * sizeof(LatValue));
    if (!relems) return value_unit();
    int64_t rcur = rstart;
    for (size_t i = 0; i < rcount; i++) {
        relems[i] = value_int(rcur);
        rcur += rstep;
    }
    LatValue r = value_array(relems, rcount);
    free(relems);
    return r;
}
static LatValue native_print_raw(LatValue *args, int ac) {
    for (int i = 0; i < ac; i++) {
        if (i > 0) printf(" ");
        if (args[i].type == VAL_STR) printf("%s", args[i].as.str_val);
        else {
            char *s = value_display(&args[i]);
            printf("%s", s);
            free(s);
        }
    }
    fflush(stdout);
    return value_unit();
}
static LatValue native_eprint(LatValue *args, int ac) {
    for (int i = 0; i < ac; i++) {
        if (i > 0) fprintf(stderr, " ");
        if (args[i].type == VAL_STR) fprintf(stderr, "%s", args[i].as.str_val);
        else {
            char *s = value_display(&args[i]);
            fprintf(stderr, "%s", s);
            free(s);
        }
    }
    fprintf(stderr, "\n");
    return value_unit();
}
static LatValue native_identity(LatValue *args, int ac) {
    if (ac != 1) return value_nil();
    return value_deep_clone(&args[0]);
}
static LatValue native_debug_assert(LatValue *args, int ac) {
    if (ac < 1) return value_unit();
    bool ok = (args[0].type == VAL_BOOL)  ? args[0].as.bool_val
              : (args[0].type == VAL_INT) ? args[0].as.int_val != 0
                                          : args[0].type != VAL_NIL;
    if (!ok) {
        const char *msg = (ac >= 2 && args[1].type == VAL_STR) ? args[1].as.str_val : "debug assertion failed";
        if (current_rt) {
            char *err = NULL;
            lat_asprintf(&err, "debug assertion failed: %s", msg);
            current_rt->error = err;
        } else {
            fprintf(stderr, "debug assertion failed: %s\n", msg);
            exit(1);
        }
    }
    return value_unit();
}

static LatValue native_panic(LatValue *args, int ac) {
    const char *msg = (ac >= 1 && args[0].type == VAL_STR) ? args[0].as.str_val : "panic";
    current_rt->error = strdup(msg);
    return value_unit();
}

/* ── Assertion helpers ── */

static void rt_assert_fail(const char *fmt, ...) {
    if (!current_rt) return;
    va_list ap;
    va_start(ap, fmt);
    lat_vasprintf(&current_rt->error, fmt, ap);
    va_end(ap);
}

static LatValue native_assert_eq(LatValue *args, int ac) {
    if (ac < 2) {
        rt_assert_fail("assert_eq() expects 2 arguments");
        return value_unit();
    }
    if (!value_eq(&args[0], &args[1])) {
        char *actual = value_display(&args[0]);
        char *expected = value_display(&args[1]);
        rt_assert_fail("assert_eq: expected %s, got %s", expected, actual);
        free(actual);
        free(expected);
    }
    return value_unit();
}

static LatValue native_assert_ne(LatValue *args, int ac) {
    if (ac < 2) {
        rt_assert_fail("assert_ne() expects 2 arguments");
        return value_unit();
    }
    if (value_eq(&args[0], &args[1])) {
        char *val = value_display(&args[0]);
        rt_assert_fail("assert_ne: values are equal: %s", val);
        free(val);
    }
    return value_unit();
}

static LatValue native_assert_true(LatValue *args, int ac) {
    if (ac < 1) {
        rt_assert_fail("assert_true() expects 1 argument");
        return value_unit();
    }
    if (args[0].type != VAL_BOOL || !args[0].as.bool_val) {
        char *val = value_display(&args[0]);
        rt_assert_fail("assert_true: expected true, got %s", val);
        free(val);
    }
    return value_unit();
}

static LatValue native_assert_false(LatValue *args, int ac) {
    if (ac < 1) {
        rt_assert_fail("assert_false() expects 1 argument");
        return value_unit();
    }
    if (args[0].type != VAL_BOOL || args[0].as.bool_val) {
        char *val = value_display(&args[0]);
        rt_assert_fail("assert_false: expected false, got %s", val);
        free(val);
    }
    return value_unit();
}

static LatValue native_assert_nil(LatValue *args, int ac) {
    if (ac < 1) {
        rt_assert_fail("assert_nil() expects 1 argument");
        return value_unit();
    }
    if (args[0].type != VAL_NIL) {
        char *val = value_display(&args[0]);
        rt_assert_fail("assert_nil: expected nil, got %s", val);
        free(val);
    }
    return value_unit();
}

static LatValue native_assert_throws(LatValue *args, int ac) {
    if (ac < 1 || args[0].type != VAL_CLOSURE) {
        rt_assert_fail("assert_throws() expects a closure argument");
        return value_unit();
    }
    if (!current_rt || !current_rt->call_closure || !current_rt->active_vm) {
        rt_assert_fail("assert_throws: no active VM");
        return value_unit();
    }
    /* Call the closure — pass nil if it expects a parameter (convention: |_| { ... }) */
    LatValue nil_arg = value_nil();
    int call_argc = (args[0].as.closure.param_count > 0) ? 1 : 0;
    LatValue *call_args = (call_argc > 0) ? &nil_arg : NULL;
    LatValue result = current_rt->call_closure(current_rt->active_vm, &args[0], call_args, call_argc);
    /* If the VM set an error, that's success — the closure threw */
    if (current_rt->error) {
        char *err_msg = current_rt->error;
        current_rt->error = NULL;
        LatValue ret = value_string(err_msg);
        free(err_msg);
        value_free(&result);
        return ret;
    }
    /* Check for error() return value: Map with {tag: "err", value: ...} */
    if (result.type == VAL_MAP) {
        LatValue *tag = lat_map_get(result.as.map.map, "tag");
        if (tag && tag->type == VAL_STR && strcmp(tag->as.str_val, "err") == 0) {
            LatValue *val = lat_map_get(result.as.map.map, "value");
            LatValue ret;
            if (val && val->type == VAL_STR) ret = value_string(val->as.str_val);
            else if (val) ret = value_deep_clone(val);
            else ret = value_string("error");
            value_free(&result);
            return ret;
        }
    }
    /* Check for EVAL_ERROR: string (tree-walker convention) */
    if (result.type == VAL_STR && strncmp(result.as.str_val, "EVAL_ERROR:", 11) == 0) {
        LatValue ret = value_string(result.as.str_val + 11);
        value_free(&result);
        return ret;
    }
    /* No error was thrown — assertion fails */
    value_free(&result);
    rt_assert_fail("assert_throws: expected an error but none was thrown");
    return value_unit();
}

static LatValue native_assert_contains(LatValue *args, int ac) {
    if (ac < 2) {
        rt_assert_fail("assert_contains() expects 2 arguments");
        return value_unit();
    }
    bool found = false;
    if (args[0].type == VAL_STR && args[1].type == VAL_STR) {
        found = strstr(args[0].as.str_val, args[1].as.str_val) != NULL;
    } else if (args[0].type == VAL_ARRAY) {
        for (size_t i = 0; i < args[0].as.array.len; i++) {
            if (value_eq(&args[0].as.array.elems[i], &args[1])) {
                found = true;
                break;
            }
        }
    } else {
        rt_assert_fail("assert_contains: first argument must be a String or Array");
        return value_unit();
    }
    if (!found) {
        char *haystack = value_display(&args[0]);
        char *needle = value_display(&args[1]);
        rt_assert_fail("assert_contains: %s does not contain %s", haystack, needle);
        free(haystack);
        free(needle);
    }
    return value_unit();
}

static LatValue native_assert_type(LatValue *args, int ac) {
    if (ac < 2 || args[1].type != VAL_STR) {
        rt_assert_fail("assert_type() expects (value, type_name_string)");
        return value_unit();
    }
    const char *actual_type = builtin_typeof_str(&args[0]);
    const char *expected_type = args[1].as.str_val;
    /* Also check struct name for Struct types */
    if (args[0].type == VAL_STRUCT) {
        if (strcmp(args[0].as.strct.name, expected_type) == 0) return value_unit();
    }
    if (strcmp(actual_type, expected_type) != 0) {
        rt_assert_fail("assert_type: expected type %s, got %s", expected_type, actual_type);
    }
    return value_unit();
}

/* Native require: load and execute a file in the global scope (no isolation) */
static LatValue native_require(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("require: expected a string argument");
        return value_bool(false);
    }
    const char *raw_path = args[0].as.str_val;

    /* Resolve file path: append .lat if not present */
    size_t plen = strlen(raw_path);
    char *file_path;
    if (plen >= 4 && strcmp(raw_path + plen - 4, ".lat") == 0) {
        file_path = strdup(raw_path);
    } else {
        file_path = malloc(plen + 5);
        if (!file_path) return value_unit();
        memcpy(file_path, raw_path, plen);
        memcpy(file_path + plen, ".lat", 5);
    }

    /* Resolve to absolute path: try CWD first, then script_dir */
    char resolved[PATH_MAX];
    bool found = (realpath(file_path, resolved) != NULL);
    if (!found && current_rt->script_dir && file_path[0] != '/') {
        char script_rel[PATH_MAX];
        snprintf(script_rel, sizeof(script_rel), "%s/%s", current_rt->script_dir, file_path);
        found = (realpath(script_rel, resolved) != NULL);
    }
    if (!found) {
        lat_asprintf(&current_rt->error, "require: cannot find '%s'", raw_path);
        free(file_path);
        return value_bool(false);
    }
    free(file_path);

    /* Dedup: skip if already required */
    if (lat_map_get(&current_rt->required_files, resolved)) { return value_bool(true); }

    /* Mark as loaded before execution (prevents circular requires) */
    bool loaded = true;
    lat_map_set(&current_rt->required_files, resolved, &loaded);

    /* Read the file */
    char *source = builtin_read_file(resolved);
    if (!source) {
        lat_asprintf(&current_rt->error, "require: cannot read '%s'", resolved);
        return value_bool(false);
    }

    /* Lex */
    Lexer req_lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec req_toks = lexer_tokenize(&req_lex, &lex_err);
    free(source);
    if (lex_err) {
        lat_asprintf(&current_rt->error, "require '%s': %s", resolved, lex_err);
        free(lex_err);
        lat_vec_free(&req_toks);
        return value_bool(false);
    }

    /* Parse */
    Parser req_parser = parser_new(&req_toks);
    char *parse_err = NULL;
    Program req_prog = parser_parse(&req_parser, &parse_err);
    if (parse_err) {
        lat_asprintf(&current_rt->error, "require '%s': %s", resolved, parse_err);
        free(parse_err);
        program_free(&req_prog);
        for (size_t ti = 0; ti < req_toks.len; ti++) token_free(lat_vec_get(&req_toks, ti));
        lat_vec_free(&req_toks);
        return value_bool(false);
    }

    /* Dispatch to the active VM backend */
    if (current_rt->backend == RT_BACKEND_REG_VM) {
        char *comp_err = NULL;
        RegChunk *rchunk = reg_compile_module(&req_prog, &comp_err);
        program_free(&req_prog);
        for (size_t ti = 0; ti < req_toks.len; ti++) token_free(lat_vec_get(&req_toks, ti));
        lat_vec_free(&req_toks);
        if (!rchunk) {
            lat_asprintf(&current_rt->error, "require '%s': %s", resolved, comp_err ? comp_err : "compile error");
            free(comp_err);
            return value_bool(false);
        }
        RegVM *rvm = (RegVM *)current_rt->active_vm;
        regvm_track_chunk(rvm, rchunk);
        LatValue req_result;
        RegVMResult rr = regvm_run(rvm, rchunk, &req_result);
        if (rr != REGVM_OK) {
            lat_asprintf(&current_rt->error, "require '%s': runtime error: %s", resolved,
                         rvm->error ? rvm->error : "(unknown)");
            return value_bool(false);
        }
        value_free(&req_result);
    } else {
        char *comp_err = NULL;
        Chunk *req_chunk = stack_compile_module(&req_prog, &comp_err);
        program_free(&req_prog);
        for (size_t ti = 0; ti < req_toks.len; ti++) token_free(lat_vec_get(&req_toks, ti));
        lat_vec_free(&req_toks);
        if (!req_chunk) {
            lat_asprintf(&current_rt->error, "require '%s': %s", resolved, comp_err ? comp_err : "compile error");
            free(comp_err);
            return value_bool(false);
        }
        StackVM *vm = (StackVM *)current_rt->active_vm;
        /* Track the chunk for proper lifetime management */
        if (vm->fn_chunk_count >= vm->fn_chunk_cap) {
            vm->fn_chunk_cap = vm->fn_chunk_cap ? vm->fn_chunk_cap * 2 : 8;
            vm->fn_chunks = realloc(vm->fn_chunks, vm->fn_chunk_cap * sizeof(Chunk *));
        }
        vm->fn_chunks[vm->fn_chunk_count++] = req_chunk;
        LatValue req_result;
        StackVMResult req_r = stackvm_run(vm, req_chunk, &req_result);
        if (req_r != STACKVM_OK) {
            lat_asprintf(&current_rt->error, "require '%s': runtime error: %s", resolved,
                         vm->error ? vm->error : "(unknown)");
            return value_bool(false);
        }
        value_free(&req_result);
    }

    return value_bool(true);
}

/* Native require_ext: load a native extension (.dylib/.so) and return a Map */
static LatValue native_require_ext(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_STR) {
        current_rt->error = strdup("require_ext: expected a string argument");
        return value_nil();
    }
    const char *ext_name = args[0].as.str_val;

    /* Check cache */
    LatValue *cached = (LatValue *)lat_map_get(&current_rt->loaded_extensions, ext_name);
    if (cached) { return value_deep_clone(cached); }

    /* Load extension */
    char *ext_err = NULL;
    LatValue ext_map = ext_load(NULL, ext_name, &ext_err);
    if (ext_err) {
        lat_asprintf(&current_rt->error, "require_ext: %s", ext_err);
        free(ext_err);
        return value_nil();
    }

    /* Mark extension closures with VM_EXT_MARKER so the StackVM dispatches them
     * through ext_call_native() instead of treating native_fn as a Chunk*. */
    if (ext_map.type == VAL_MAP && ext_map.as.map.map) {
        for (size_t i = 0; i < ext_map.as.map.map->cap; i++) {
            if (ext_map.as.map.map->entries[i].state != MAP_OCCUPIED) continue;
            LatValue *v = (LatValue *)ext_map.as.map.map->entries[i].value;
            if (v->type == VAL_CLOSURE && v->as.closure.native_fn && !v->as.closure.body) {
                v->as.closure.default_values = VM_EXT_MARKER;
            }
        }
    }

    /* Cache the extension */
    LatValue cached_copy = value_deep_clone(&ext_map);
    lat_map_set(&current_rt->loaded_extensions, ext_name, &cached_copy);

    return ext_map;
}

/* ── Missing native builtins ── */

static LatValue native_args(LatValue *args, int arg_count) {
    (void)args;
    (void)arg_count;
    int ac = current_rt->prog_argc;
    char **av = current_rt->prog_argv;
    LatValue *elems = NULL;
    if (ac > 0) {
        elems = malloc((size_t)ac * sizeof(LatValue));
        if (!elems) return value_unit();
        for (int i = 0; i < ac; i++) elems[i] = value_string(av[i]);
    }
    LatValue arr = value_array(elems, (size_t)ac);
    free(elems);
    return arr;
}

static LatValue native_struct_from_map(LatValue *args, int arg_count) {
    if (arg_count < 2 || args[0].type != VAL_STR || args[1].type != VAL_MAP) {
        current_rt->error = strdup("struct_from_map: expected (name: Str, fields: Map)");
        return value_nil();
    }
    const char *sname = args[0].as.str_val;
    /* Look up struct field names from env metadata */
    char meta_key[256];
    snprintf(meta_key, sizeof(meta_key), "__struct_%s", sname);
    LatValue meta;
    if (!env_get(current_rt->env, meta_key, &meta)) {
        lat_asprintf(&current_rt->error, "struct_from_map: unknown struct '%s'", sname);
        return value_nil();
    }
    if (meta.type != VAL_ARRAY) {
        value_free(&meta);
        return value_nil();
    }
    size_t fc = meta.as.array.len;
    char **names = malloc(fc * sizeof(char *));
    if (!names) return value_unit();
    LatValue *vals = malloc(fc * sizeof(LatValue));
    if (!vals) return value_unit();
    for (size_t j = 0; j < fc; j++) {
        names[j] = meta.as.array.elems[j].as.str_val;
        LatValue *found = (LatValue *)lat_map_get(args[1].as.map.map, names[j]);
        vals[j] = found ? value_deep_clone(found) : value_nil();
    }
    LatValue st = value_struct(sname, names, vals, fc);
    free(names);
    free(vals);
    value_free(&meta);
    return st;
}

static LatValue native_url_encode(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_STR) return value_nil();
    const char *src = args[0].as.str_val;
    size_t slen = strlen(src);
    size_t cap = slen * 3 + 1;
    char *out = malloc(cap);
    if (!out) return value_unit();
    size_t j = 0;
    for (size_t i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out[j++] = (char)c;
        } else {
            snprintf(out + j, 4, "%%%02X", c);
            j += 3;
        }
    }
    out[j] = '\0';
    return value_string_owned(out);
}

static LatValue native_url_decode(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_STR) return value_nil();
    const char *src = args[0].as.str_val;
    size_t slen = strlen(src);
    char *out = malloc(slen + 1);
    if (!out) return value_unit();
    size_t j = 0;
    for (size_t i = 0; i < slen; i++) {
        if (src[i] == '%' && i + 2 < slen) {
            char hex[3] = {src[i + 1], src[i + 2], '\0'};
            char *end = NULL;
            unsigned long val = strtoul(hex, &end, 16);
            if (end == hex + 2) {
                out[j++] = (char)val;
                i += 2;
            } else {
                out[j++] = src[i];
            }
        } else if (src[i] == '+') {
            out[j++] = ' ';
        } else {
            out[j++] = src[i];
        }
    }
    out[j] = '\0';
    return value_string_owned(out);
}

static LatValue native_csv_parse(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_STR) return value_nil();
    const char *input = args[0].as.str_val;
    size_t pos = 0, input_len = strlen(input);
    size_t rows_cap = 8, rows_len = 0;
    LatValue *rows = malloc(rows_cap * sizeof(LatValue));
    if (!rows) return value_unit();

    while (pos < input_len) {
        size_t fields_cap = 8, fields_len = 0;
        LatValue *fields = malloc(fields_cap * sizeof(LatValue));
        if (!fields) return value_unit();
        for (;;) {
            size_t field_cap = 64, field_len = 0;
            char *field = malloc(field_cap);
            if (!field) return value_unit();
            if (pos < input_len && input[pos] == '"') {
                pos++;
                for (;;) {
                    if (pos >= input_len) break;
                    if (input[pos] == '"') {
                        if (pos + 1 < input_len && input[pos + 1] == '"') {
                            if (field_len + 1 >= field_cap) {
                                field_cap *= 2;
                                field = realloc(field, field_cap);
                            }
                            field[field_len++] = '"';
                            pos += 2;
                        } else {
                            pos++;
                            break;
                        }
                    } else {
                        if (field_len + 1 >= field_cap) {
                            field_cap *= 2;
                            field = realloc(field, field_cap);
                        }
                        field[field_len++] = input[pos++];
                    }
                }
            } else {
                while (pos < input_len && input[pos] != ',' && input[pos] != '\n' && input[pos] != '\r') {
                    if (field_len + 1 >= field_cap) {
                        field_cap *= 2;
                        field = realloc(field, field_cap);
                    }
                    field[field_len++] = input[pos++];
                }
            }
            field[field_len] = '\0';
            if (fields_len >= fields_cap) {
                fields_cap *= 2;
                fields = realloc(fields, fields_cap * sizeof(LatValue));
            }
            fields[fields_len++] = value_string_owned(field);
            if (pos < input_len && input[pos] == ',') {
                pos++;
            } else break;
        }
        if (pos < input_len && input[pos] == '\r') pos++;
        if (pos < input_len && input[pos] == '\n') pos++;
        LatValue row = value_array(fields, fields_len);
        free(fields);
        if (rows_len >= rows_cap) {
            rows_cap *= 2;
            rows = realloc(rows, rows_cap * sizeof(LatValue));
        }
        rows[rows_len++] = row;
    }
    LatValue result = value_array(rows, rows_len);
    free(rows);
    return result;
}

static LatValue native_csv_stringify(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_ARRAY) return value_nil();
    LatValue *data = &args[0];
    size_t out_cap = 256, out_len = 0;
    char *out = malloc(out_cap);
    if (!out) return value_nil();
    for (size_t r = 0; r < data->as.array.len; r++) {
        LatValue *row = &data->as.array.elems[r];
        if (row->type != VAL_ARRAY) {
            free(out);
            return value_nil();
        }
        for (size_t c = 0; c < row->as.array.len; c++) {
            if (c > 0) {
                if (out_len + 1 >= out_cap) {
                    out_cap *= 2;
                    out = realloc(out, out_cap);
                }
                out[out_len++] = ',';
            }
            char *field_str = value_display(&row->as.array.elems[c]);
            size_t flen = strlen(field_str);
            bool needs_quote = false;
            for (size_t k = 0; k < flen; k++) {
                if (field_str[k] == ',' || field_str[k] == '"' || field_str[k] == '\n' || field_str[k] == '\r') {
                    needs_quote = true;
                    break;
                }
            }
            if (needs_quote) {
                size_t extra = 0;
                for (size_t k = 0; k < flen; k++) {
                    if (field_str[k] == '"') extra++;
                }
                size_t needed = flen + extra + 2;
                while (out_len + needed >= out_cap) {
                    out_cap *= 2;
                    out = realloc(out, out_cap);
                }
                out[out_len++] = '"';
                for (size_t k = 0; k < flen; k++) {
                    if (field_str[k] == '"') out[out_len++] = '"';
                    out[out_len++] = field_str[k];
                }
                out[out_len++] = '"';
            } else {
                while (out_len + flen >= out_cap) {
                    out_cap *= 2;
                    out = realloc(out, out_cap);
                }
                memcpy(out + out_len, field_str, flen);
                out_len += flen;
            }
            free(field_str);
        }
        if (out_len + 1 >= out_cap) {
            out_cap *= 2;
            out = realloc(out, out_cap);
        }
        out[out_len++] = '\n';
    }
    out[out_len] = '\0';
    return value_string_owned(out);
}

static LatValue native_is_complete(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_STR) return value_bool(false);
    const char *source = args[0].as.str_val;
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec toks = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        free(lex_err);
        lat_vec_free(&toks);
        return value_bool(false);
    }
    int depth = 0;
    for (size_t j = 0; j < toks.len; j++) {
        Token *t = lat_vec_get(&toks, j);
        switch (t->type) {
            case TOK_LBRACE:
            case TOK_LPAREN:
            case TOK_LBRACKET: depth++; break;
            case TOK_RBRACE:
            case TOK_RPAREN:
            case TOK_RBRACKET: depth--; break;
            default: break;
        }
    }
    for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
    lat_vec_free(&toks);
    return value_bool(depth <= 0);
}

static LatValue native_float_to_bits(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_FLOAT) return value_nil();
    double d = args[0].as.float_val;
    uint64_t bits;
    memcpy(&bits, &d, 8);
    return value_int((int64_t)bits);
}

static LatValue native_bits_to_float(LatValue *args, int arg_count) {
    if (arg_count != 1 || args[0].type != VAL_INT) return value_nil();
    uint64_t bits = (uint64_t)args[0].as.int_val;
    double d;
    memcpy(&d, &bits, 8);
    return value_float(d);
}

static LatValue native_tokenize(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_STR) return value_nil();
    const char *source = args[0].as.str_val;
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec toks = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        free(lex_err);
        lat_vec_free(&toks);
        return value_nil();
    }
    size_t tok_count = toks.len > 0 ? toks.len - 1 : 0;
    LatValue *elems = malloc((tok_count > 0 ? tok_count : 1) * sizeof(LatValue));
    if (!elems) return value_unit();
    for (size_t j = 0; j < tok_count; j++) {
        Token *t = lat_vec_get(&toks, j);
        const char *type_str = token_type_name(t->type);
        char *text;
        if (t->type == TOK_IDENT || t->type == TOK_STRING_LIT || t->type == TOK_MODE_DIRECTIVE ||
            t->type == TOK_INTERP_START || t->type == TOK_INTERP_MID || t->type == TOK_INTERP_END) {
            text = strdup(t->as.str_val);
        } else if (t->type == TOK_INT_LIT) {
            lat_asprintf(&text, "%lld", (long long)t->as.int_val);
        } else if (t->type == TOK_FLOAT_LIT) {
            lat_asprintf(&text, "%g", t->as.float_val);
        } else {
            text = strdup(token_type_name(t->type));
        }
        char *fnames[3] = {"type", "text", "line"};
        LatValue fvals[3];
        fvals[0] = value_string(type_str);
        fvals[1] = value_string_owned(text);
        fvals[2] = value_int((int64_t)t->line);
        elems[j] = value_struct("Token", fnames, fvals, 3);
    }
    for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
    lat_vec_free(&toks);
    LatValue arr = value_array(elems, tok_count);
    free(elems);
    return arr;
}

static LatValue native_lat_eval(LatValue *args, int arg_count) {
    if (arg_count < 1 || args[0].type != VAL_STR) return value_nil();
    const char *source = args[0].as.str_val;
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec toks = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        lat_asprintf(&current_rt->error, "lat_eval: %s", lex_err);
        free(lex_err);
        lat_vec_free(&toks);
        return value_nil();
    }
    Parser parser = parser_new(&toks);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    if (parse_err) {
        lat_asprintf(&current_rt->error, "lat_eval: %s", parse_err);
        free(parse_err);
        program_free(&prog);
        for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
        lat_vec_free(&toks);
        return value_nil();
    }

    if (current_rt->backend == RT_BACKEND_REG_VM) {
        char *comp_err = NULL;
        RegChunk *rchunk = reg_compile_repl(&prog, &comp_err);
        program_free(&prog);
        for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
        lat_vec_free(&toks);
        if (!rchunk) {
            lat_asprintf(&current_rt->error, "lat_eval: %s", comp_err ? comp_err : "compile error");
            free(comp_err);
            return value_nil();
        }
        RegVM *rvm = (RegVM *)current_rt->active_vm;
        regvm_track_chunk(rvm, rchunk);
        LatValue result;
        RegVMResult rr = regvm_run(rvm, rchunk, &result);
        if (rr != REGVM_OK) return value_nil();
        return result;
    }

    char *comp_err = NULL;
    Chunk *chunk = stack_compile_repl(&prog, &comp_err);
    program_free(&prog);
    for (size_t j = 0; j < toks.len; j++) token_free(lat_vec_get(&toks, j));
    lat_vec_free(&toks);
    if (!chunk) {
        lat_asprintf(&current_rt->error, "lat_eval: %s", comp_err ? comp_err : "compile error");
        free(comp_err);
        return value_nil();
    }
    StackVM *vm = (StackVM *)current_rt->active_vm;
    if (vm->fn_chunk_count >= vm->fn_chunk_cap) {
        vm->fn_chunk_cap = vm->fn_chunk_cap ? vm->fn_chunk_cap * 2 : 8;
        vm->fn_chunks = realloc(vm->fn_chunks, vm->fn_chunk_cap * sizeof(Chunk *));
    }
    vm->fn_chunks[vm->fn_chunk_count++] = chunk;
    LatValue result;
    StackVMResult r = stackvm_run(vm, chunk, &result);
    if (r != STACKVM_OK) return value_nil();
    return result;
}

static LatValue native_pipe(LatValue *args, int arg_count) {
    if (arg_count < 2) return value_nil();
    (void)0; /* uses dispatch */
    LatValue current = value_deep_clone(&args[0]);
    for (int i = 1; i < arg_count; i++) {
        if (args[i].type != VAL_CLOSURE) {
            value_free(&current);
            return value_nil();
        }
        LatValue result = current_rt->call_closure(current_rt->active_vm, &args[i], &current, 1);
        value_free(&current);
        current = result;
    }
    return current;
}

static LatValue native_compose(LatValue *args, int arg_count) {
    if (arg_count < 2 || args[0].type != VAL_CLOSURE || args[1].type != VAL_CLOSURE) return value_nil();
    StackVM *vm = (StackVM *)current_rt->active_vm;

    /* Store f and g in env with unique names so the composed closure can find them */
    static int compose_counter = 0;
    char f_name[64], g_name[64];
    snprintf(f_name, sizeof(f_name), "__compose_f_%d", compose_counter);
    snprintf(g_name, sizeof(g_name), "__compose_g_%d", compose_counter);
    compose_counter++;
    env_define(current_rt->env, f_name, value_deep_clone(&args[0]));
    env_define(current_rt->env, g_name, value_deep_clone(&args[1]));

    /* Build chunk: GET_GLOBAL f, GET_GLOBAL g, GET_LOCAL 1(x), CALL 1, CALL 1, RETURN */
    Chunk *chunk = chunk_new();
    size_t f_idx = chunk_add_constant(chunk, value_string(f_name));
    size_t g_idx = chunk_add_constant(chunk, value_string(g_name));
    chunk_write(chunk, OP_GET_GLOBAL, 0);
    chunk_write(chunk, (uint8_t)f_idx, 0);
    chunk_write(chunk, OP_GET_GLOBAL, 0);
    chunk_write(chunk, (uint8_t)g_idx, 0);
    chunk_write(chunk, OP_GET_LOCAL, 0);
    chunk_write(chunk, 1, 0);
    chunk_write(chunk, OP_CALL, 0);
    chunk_write(chunk, 1, 0);
    chunk_write(chunk, OP_CALL, 0);
    chunk_write(chunk, 1, 0);
    chunk_write(chunk, OP_RETURN, 0);

    if (vm->fn_chunk_count >= vm->fn_chunk_cap) {
        vm->fn_chunk_cap = vm->fn_chunk_cap ? vm->fn_chunk_cap * 2 : 8;
        vm->fn_chunks = realloc(vm->fn_chunks, vm->fn_chunk_cap * sizeof(Chunk *));
    }
    vm->fn_chunks[vm->fn_chunk_count++] = chunk;

    /* Build a compiled closure with 1 parameter */
    char **params = malloc(sizeof(char *));
    if (!params) return value_unit();
    params[0] = strdup("x");
    LatValue closure = value_closure(params, 1, NULL, NULL, NULL, false);
    closure.as.closure.native_fn = (void *)chunk;
    free(params);
    return closure;
}

/* ── Bytecode compilation/loading builtins ── */

static char *latc_read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)len, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static LatValue native_compile_file(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_nil();
    const char *path = args[0].as.str_val;

    char *source = latc_read_file(path);
    if (!source) return value_nil();

    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        free(lex_err);
        lat_vec_free(&tokens);
        free(source);
        return value_nil();
    }

    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    if (parse_err) {
        free(parse_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        free(source);
        return value_nil();
    }

    char *comp_err = NULL;
    Chunk *chunk = stack_compile(&prog, &comp_err);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    free(source);

    if (!chunk) {
        free(comp_err);
        return value_nil();
    }

    size_t buf_len;
    uint8_t *buf = chunk_serialize(chunk, &buf_len);
    chunk_free(chunk);

    LatValue result = value_buffer(buf, buf_len);
    free(buf);
    return result;
}

static LatValue native_load_bytecode(LatValue *args, int ac) {
    if (ac != 1 || args[0].type != VAL_STR) return value_nil();
    const char *path = args[0].as.str_val;

    char *err = NULL;
    Chunk *chunk = chunk_load(path, &err);
    if (!chunk) {
        free(err);
        return value_nil();
    }

    if (!current_rt->active_vm) {
        chunk_free(chunk);
        return value_nil();
    }

    /* Stack-VM bytecode (.latc) can only run on the stack VM */
    StackVM *vm = (StackVM *)current_rt->active_vm;
    LatValue result;
    StackVMResult res = stackvm_run(vm, chunk, &result);
    chunk_free(chunk);

    if (res != STACKVM_OK) {
        free(vm->error);
        vm->error = NULL;
        return value_nil();
    }
    return result;
}

/* ── Built-in module helpers ── */

static void mod_set_native(LatValue *mod, const char *name, VMNativeFn fn) {
    LatValue v;
    v.type = VAL_CLOSURE;
    v.phase = VTAG_UNPHASED;
    v.as.closure.param_names = NULL;
    v.as.closure.param_count = 0;
    v.as.closure.body = NULL;
    v.as.closure.captured_env = NULL;
    v.as.closure.default_values = VM_NATIVE_MARKER;
    v.as.closure.has_variadic = false;
    v.as.closure.native_fn = (void *)fn;
    v.region_id = REGION_NONE;
    lat_map_set(mod->as.map.map, name, &v);
}

static LatValue build_math_module(void) {
    LatValue m = value_map_new();
    /* Trig */
    mod_set_native(&m, "sin", native_sin);
    mod_set_native(&m, "cos", native_cos);
    mod_set_native(&m, "tan", native_tan);
    mod_set_native(&m, "asin", native_asin);
    mod_set_native(&m, "acos", native_acos);
    mod_set_native(&m, "atan", native_atan);
    mod_set_native(&m, "atan2", native_atan2);
    mod_set_native(&m, "sinh", native_sinh);
    mod_set_native(&m, "cosh", native_cosh);
    mod_set_native(&m, "tanh", native_tanh);
    /* Core */
    mod_set_native(&m, "sqrt", native_sqrt);
    mod_set_native(&m, "pow", native_pow);
    mod_set_native(&m, "abs", native_abs);
    mod_set_native(&m, "floor", native_floor);
    mod_set_native(&m, "ceil", native_ceil);
    mod_set_native(&m, "round", native_round);
    mod_set_native(&m, "log", native_log);
    mod_set_native(&m, "log2", native_log2);
    mod_set_native(&m, "log10", native_log10);
    mod_set_native(&m, "exp", native_exp);
    mod_set_native(&m, "min", native_min);
    mod_set_native(&m, "max", native_max);
    mod_set_native(&m, "clamp", native_clamp);
    mod_set_native(&m, "sign", native_sign);
    mod_set_native(&m, "gcd", native_gcd);
    mod_set_native(&m, "lcm", native_lcm);
    mod_set_native(&m, "lerp", native_lerp);
    mod_set_native(&m, "is_nan", native_is_nan);
    mod_set_native(&m, "is_inf", native_is_inf);
    mod_set_native(&m, "random", native_random);
    mod_set_native(&m, "random_int", native_random_int);
    /* Constants (as zero-arg closures) */
    mod_set_native(&m, "PI", native_math_pi);
    mod_set_native(&m, "E", native_math_e);
    return m;
}

static LatValue build_fs_module(void) {
    LatValue m = value_map_new();
    mod_set_native(&m, "read_file", native_read_file);
    mod_set_native(&m, "write_file", native_write_file);
    mod_set_native(&m, "read_file_bytes", native_read_file_bytes);
    mod_set_native(&m, "write_file_bytes", native_write_file_bytes);
    mod_set_native(&m, "append_file", native_append_file);
    mod_set_native(&m, "copy_file", native_copy_file);
    mod_set_native(&m, "file_exists", native_file_exists);
    mod_set_native(&m, "is_file", native_is_file);
    mod_set_native(&m, "is_dir", native_is_dir);
    mod_set_native(&m, "file_size", native_file_size);
    mod_set_native(&m, "delete_file", native_delete_file);
    mod_set_native(&m, "mkdir", native_mkdir);
    mod_set_native(&m, "rmdir", native_rmdir);
    mod_set_native(&m, "rename", native_fs_rename);
    mod_set_native(&m, "chmod", native_chmod);
    mod_set_native(&m, "list_dir", native_list_dir);
    mod_set_native(&m, "glob", native_glob);
    mod_set_native(&m, "stat", native_stat);
    mod_set_native(&m, "realpath", native_realpath);
    mod_set_native(&m, "tempdir", native_tempdir);
    mod_set_native(&m, "tempfile", native_tempfile);
    return m;
}

static LatValue build_path_module(void) {
    LatValue m = value_map_new();
    mod_set_native(&m, "join", native_path_join);
    mod_set_native(&m, "dir", native_path_dir);
    mod_set_native(&m, "base", native_path_base);
    mod_set_native(&m, "ext", native_path_ext);
    return m;
}

static LatValue build_json_module(void) {
    LatValue m = value_map_new();
    mod_set_native(&m, "parse", native_json_parse);
    mod_set_native(&m, "stringify", native_json_stringify);
    return m;
}

static LatValue build_toml_module(void) {
    LatValue m = value_map_new();
    mod_set_native(&m, "parse", native_toml_parse);
    mod_set_native(&m, "stringify", native_toml_stringify);
    return m;
}

static LatValue build_yaml_module(void) {
    LatValue m = value_map_new();
    mod_set_native(&m, "parse", native_yaml_parse);
    mod_set_native(&m, "stringify", native_yaml_stringify);
    return m;
}

static LatValue build_crypto_module(void) {
    LatValue m = value_map_new();
    mod_set_native(&m, "sha256", native_sha256);
    mod_set_native(&m, "md5", native_md5);
    mod_set_native(&m, "base64_encode", native_base64_encode);
    mod_set_native(&m, "base64_decode", native_base64_decode);
    mod_set_native(&m, "sha512", native_sha512);
    mod_set_native(&m, "hmac_sha256", native_hmac_sha256);
    mod_set_native(&m, "random_bytes", native_random_bytes);
    mod_set_native(&m, "url_encode", native_url_encode);
    mod_set_native(&m, "url_decode", native_url_decode);
    return m;
}

static LatValue build_http_module(void) {
    LatValue m = value_map_new();
    mod_set_native(&m, "get", native_http_get);
    mod_set_native(&m, "post", native_http_post);
    mod_set_native(&m, "request", native_http_request);
    return m;
}

static LatValue build_net_module(void) {
    LatValue m = value_map_new();
    /* TCP */
    mod_set_native(&m, "tcp_listen", native_tcp_listen);
    mod_set_native(&m, "tcp_accept", native_tcp_accept);
    mod_set_native(&m, "tcp_connect", native_tcp_connect);
    mod_set_native(&m, "tcp_read", native_tcp_read);
    mod_set_native(&m, "tcp_read_bytes", native_tcp_read_bytes);
    mod_set_native(&m, "tcp_write", native_tcp_write);
    mod_set_native(&m, "tcp_close", native_tcp_close);
    mod_set_native(&m, "tcp_peer_addr", native_tcp_peer_addr);
    mod_set_native(&m, "tcp_set_timeout", native_tcp_set_timeout);
    /* TLS */
    mod_set_native(&m, "tls_connect", native_tls_connect);
    mod_set_native(&m, "tls_read", native_tls_read);
    mod_set_native(&m, "tls_read_bytes", native_tls_read_bytes);
    mod_set_native(&m, "tls_write", native_tls_write);
    mod_set_native(&m, "tls_close", native_tls_close);
    mod_set_native(&m, "tls_available", native_tls_available);
    return m;
}

static LatValue build_os_module(void) {
    LatValue m = value_map_new();
    mod_set_native(&m, "exec", native_exec_cmd);
    mod_set_native(&m, "shell", native_shell);
    mod_set_native(&m, "env", native_env);
    mod_set_native(&m, "env_set", native_env_set);
    mod_set_native(&m, "env_keys", native_env_keys);
    mod_set_native(&m, "cwd", native_cwd);
    mod_set_native(&m, "platform", native_platform);
    mod_set_native(&m, "hostname", native_hostname);
    mod_set_native(&m, "pid", native_pid);
    mod_set_native(&m, "args", native_args);
    return m;
}

static LatValue build_time_module(void) {
    LatValue m = value_map_new();
    mod_set_native(&m, "now", native_time);
    mod_set_native(&m, "sleep", native_sleep);
    mod_set_native(&m, "format", native_time_format);
    mod_set_native(&m, "parse", native_time_parse);
    mod_set_native(&m, "year", native_time_year);
    mod_set_native(&m, "month", native_time_month);
    mod_set_native(&m, "day", native_time_day);
    mod_set_native(&m, "hour", native_time_hour);
    mod_set_native(&m, "minute", native_time_minute);
    mod_set_native(&m, "second", native_time_second);
    mod_set_native(&m, "weekday", native_time_weekday);
    mod_set_native(&m, "add", native_time_add);
    mod_set_native(&m, "is_leap_year", native_is_leap_year);
    /* Duration */
    mod_set_native(&m, "duration", native_duration);
    mod_set_native(&m, "duration_from_seconds", native_duration_from_seconds);
    mod_set_native(&m, "duration_from_millis", native_duration_from_millis);
    mod_set_native(&m, "duration_add", native_duration_add);
    mod_set_native(&m, "duration_sub", native_duration_sub);
    mod_set_native(&m, "duration_to_string", native_duration_to_string);
    mod_set_native(&m, "duration_hours", native_duration_hours);
    mod_set_native(&m, "duration_minutes", native_duration_minutes);
    mod_set_native(&m, "duration_seconds", native_duration_seconds);
    mod_set_native(&m, "duration_millis", native_duration_millis);
    /* DateTime */
    mod_set_native(&m, "datetime_now", native_datetime_now);
    mod_set_native(&m, "datetime_from_epoch", native_datetime_from_epoch);
    mod_set_native(&m, "datetime_to_epoch", native_datetime_to_epoch);
    mod_set_native(&m, "datetime_from_iso", native_datetime_from_iso);
    mod_set_native(&m, "datetime_to_iso", native_datetime_to_iso);
    mod_set_native(&m, "datetime_add_duration", native_datetime_add_duration);
    mod_set_native(&m, "datetime_sub", native_datetime_sub);
    mod_set_native(&m, "datetime_format", native_datetime_fmt);
    mod_set_native(&m, "datetime_to_utc", native_datetime_to_utc);
    mod_set_native(&m, "datetime_to_local", native_datetime_to_local);
    mod_set_native(&m, "timezone_offset", native_timezone_offset);
    /* Calendar */
    mod_set_native(&m, "days_in_month", native_days_in_month);
    mod_set_native(&m, "day_of_week", native_day_of_week);
    mod_set_native(&m, "day_of_year", native_day_of_year);
    return m;
}

static LatValue build_regex_module(void) {
    LatValue m = value_map_new();
    mod_set_native(&m, "match", native_regex_match);
    mod_set_native(&m, "find_all", native_regex_find_all);
    mod_set_native(&m, "replace", native_regex_replace);
    return m;
}

bool rt_try_builtin_import(const char *name, LatValue *out) {
    /* Paths with directory separators or leading dot are never built-in */
    if (strchr(name, '/') || strchr(name, '\\') || name[0] == '.') return false;

    if (strcmp(name, "math") == 0) {
        *out = build_math_module();
        return true;
    }
    if (strcmp(name, "fs") == 0) {
        *out = build_fs_module();
        return true;
    }
    if (strcmp(name, "path") == 0) {
        *out = build_path_module();
        return true;
    }
    if (strcmp(name, "json") == 0) {
        *out = build_json_module();
        return true;
    }
    if (strcmp(name, "toml") == 0) {
        *out = build_toml_module();
        return true;
    }
    if (strcmp(name, "yaml") == 0) {
        *out = build_yaml_module();
        return true;
    }
    if (strcmp(name, "crypto") == 0) {
        *out = build_crypto_module();
        return true;
    }
    if (strcmp(name, "http") == 0) {
        *out = build_http_module();
        return true;
    }
    if (strcmp(name, "net") == 0) {
        *out = build_net_module();
        return true;
    }
    if (strcmp(name, "os") == 0) {
        *out = build_os_module();
        return true;
    }
    if (strcmp(name, "time") == 0) {
        *out = build_time_module();
        return true;
    }
    if (strcmp(name, "regex") == 0) {
        *out = build_regex_module();
        return true;
    }
    return false;
}

/* ── Registration helper ── */

static void rt_register_native(LatRuntime *rt, const char *name, VMNativeFn fn, int arity) {
    (void)arity;
    LatValue v;
    v.type = VAL_CLOSURE;
    v.phase = VTAG_UNPHASED;
    v.as.closure.param_names = NULL;
    v.as.closure.param_count = 0;
    v.as.closure.body = NULL;
    v.as.closure.captured_env = NULL;
    v.as.closure.default_values = VM_NATIVE_MARKER;
    v.as.closure.has_variadic = false;
    v.as.closure.native_fn = (void *)fn;
    v.region_id = REGION_NONE;
    env_define(rt->env, name, v);
}

/* ── Runtime lifecycle ── */

void lat_runtime_init(LatRuntime *rt) {
    memset(rt, 0, sizeof(LatRuntime));
    rt->env = env_new();
    rt->struct_meta = NULL;
    rt->error = NULL;
    rt->tracked_vars = NULL;
    rt->tracked_count = 0;
    rt->tracked_cap = 0;
    rt->tracking_active = false;
    rt->pressures = NULL;
    rt->pressure_count = 0;
    rt->pressure_cap = 0;
    rt->reactions = NULL;
    rt->reaction_count = 0;
    rt->reaction_cap = 0;
    rt->bonds = NULL;
    rt->bond_count = 0;
    rt->bond_cap = 0;
    rt->seeds = NULL;
    rt->seed_count = 0;
    rt->seed_cap = 0;
    rt->module_cache = lat_map_new(sizeof(LatValue));
    rt->required_files = lat_map_new(sizeof(bool));
    rt->loaded_extensions = lat_map_new(sizeof(LatValue));
    rt->script_dir = NULL;
    rt->prog_argc = 0;
    rt->prog_argv = NULL;
    rt->active_vm = NULL;
    rt->call_closure = NULL;
    rt->find_local_value = NULL;
    rt->current_line = NULL;
    rt->get_var_by_name = NULL;
    rt->set_var_by_name = NULL;

    /* Register builtin functions */
    rt_register_native(rt, "to_string", native_to_string, 1);
    rt_register_native(rt, "typeof", native_typeof, 1);
    rt_register_native(rt, "len", native_len, 1);
    rt_register_native(rt, "parse_int", native_parse_int, 1);
    rt_register_native(rt, "parse_float", native_parse_float, 1);
    rt_register_native(rt, "ord", native_ord, 1);
    rt_register_native(rt, "chr", native_chr, 1);
    rt_register_native(rt, "abs", native_abs, 1);
    rt_register_native(rt, "floor", native_floor, 1);
    rt_register_native(rt, "ceil", native_ceil, 1);
    rt_register_native(rt, "exit", native_exit, 1);
    rt_register_native(rt, "error", native_error, 1);
    rt_register_native(rt, "is_error", native_is_error, 1);
    rt_register_native(rt, "Map::new", native_map_new, 0);
    rt_register_native(rt, "Set::new", native_set_new, 0);
    rt_register_native(rt, "Set::from", native_set_from, 1);
    rt_register_native(rt, "Channel::new", native_channel_new, 0);
    rt_register_native(rt, "Buffer::new", native_buffer_new, 1);
    rt_register_native(rt, "Buffer::from", native_buffer_from, 1);
    rt_register_native(rt, "Buffer::from_string", native_buffer_from_string, 1);
    rt_register_native(rt, "Ref::new", native_ref_new, 1);
    rt_register_native(rt, "phase_of", native_phase_of, 1);
    rt_register_native(rt, "assert", native_assert, 2);
    rt_register_native(rt, "version", native_version, 0);
    rt_register_native(rt, "input", native_input, 1);

    /* Phase system */
    rt_register_native(rt, "track", native_track, 1);
    rt_register_native(rt, "phases", native_phases, 1);
    rt_register_native(rt, "history", native_history, 1);
    rt_register_native(rt, "rewind", native_rewind, 2);
    rt_register_native(rt, "pressurize", native_pressurize, 2);
    rt_register_native(rt, "depressurize", native_depressurize, 1);
    rt_register_native(rt, "pressure_of", native_pressure_of, 1);
    rt_register_native(rt, "grow", native_grow, 1);

    /* Math */
    rt_register_native(rt, "round", native_round, 1);
    rt_register_native(rt, "sqrt", native_sqrt, 1);
    rt_register_native(rt, "pow", native_pow, 2);
    rt_register_native(rt, "min", native_min, 2);
    rt_register_native(rt, "max", native_max, 2);
    rt_register_native(rt, "random", native_random, 0);
    rt_register_native(rt, "random_int", native_random_int, 2);
    rt_register_native(rt, "log", native_log, 1);
    rt_register_native(rt, "log2", native_log2, 1);
    rt_register_native(rt, "log10", native_log10, 1);
    rt_register_native(rt, "sin", native_sin, 1);
    rt_register_native(rt, "cos", native_cos, 1);
    rt_register_native(rt, "tan", native_tan, 1);
    rt_register_native(rt, "asin", native_asin, 1);
    rt_register_native(rt, "acos", native_acos, 1);
    rt_register_native(rt, "atan", native_atan, 1);
    rt_register_native(rt, "atan2", native_atan2, 2);
    rt_register_native(rt, "exp", native_exp, 1);
    rt_register_native(rt, "sign", native_sign, 1);
    rt_register_native(rt, "gcd", native_gcd, 2);
    rt_register_native(rt, "lcm", native_lcm, 2);
    rt_register_native(rt, "is_nan", native_is_nan, 1);
    rt_register_native(rt, "is_inf", native_is_inf, 1);
    rt_register_native(rt, "sinh", native_sinh, 1);
    rt_register_native(rt, "cosh", native_cosh, 1);
    rt_register_native(rt, "tanh", native_tanh, 1);
    rt_register_native(rt, "lerp", native_lerp, 3);
    rt_register_native(rt, "clamp", native_clamp, 3);
    rt_register_native(rt, "math_pi", native_math_pi, 0);
    rt_register_native(rt, "math_e", native_math_e, 0);

    /* File system */
    rt_register_native(rt, "read_file", native_read_file, 1);
    rt_register_native(rt, "write_file", native_write_file, 2);
    rt_register_native(rt, "read_file_bytes", native_read_file_bytes, 1);
    rt_register_native(rt, "write_file_bytes", native_write_file_bytes, 2);
    rt_register_native(rt, "file_exists", native_file_exists, 1);
    rt_register_native(rt, "delete_file", native_delete_file, 1);
    rt_register_native(rt, "list_dir", native_list_dir, 1);
    rt_register_native(rt, "append_file", native_append_file, 2);
    rt_register_native(rt, "mkdir", native_mkdir, 1);
    rt_register_native(rt, "rename", native_fs_rename, 2);
    rt_register_native(rt, "is_dir", native_is_dir, 1);
    rt_register_native(rt, "is_file", native_is_file, 1);
    rt_register_native(rt, "rmdir", native_rmdir, 1);
    rt_register_native(rt, "glob", native_glob, 1);
    rt_register_native(rt, "stat", native_stat, 1);
    rt_register_native(rt, "copy_file", native_copy_file, 2);
    rt_register_native(rt, "realpath", native_realpath, 1);
    rt_register_native(rt, "tempdir", native_tempdir, 0);
    rt_register_native(rt, "tempfile", native_tempfile, 0);
    rt_register_native(rt, "chmod", native_chmod, 2);
    rt_register_native(rt, "file_size", native_file_size, 1);

    /* Bytecode compilation/loading */
    rt_register_native(rt, "compile_file", native_compile_file, 1);
    rt_register_native(rt, "load_bytecode", native_load_bytecode, 1);

    /* Path */
    rt_register_native(rt, "path_join", native_path_join, -1);
    rt_register_native(rt, "path_dir", native_path_dir, 1);
    rt_register_native(rt, "path_base", native_path_base, 1);
    rt_register_native(rt, "path_ext", native_path_ext, 1);

    /* Network TCP */
    rt_register_native(rt, "tcp_listen", native_tcp_listen, 2);
    rt_register_native(rt, "tcp_accept", native_tcp_accept, 1);
    rt_register_native(rt, "tcp_connect", native_tcp_connect, 2);
    rt_register_native(rt, "tcp_read", native_tcp_read, 1);
    rt_register_native(rt, "tcp_read_bytes", native_tcp_read_bytes, 2);
    rt_register_native(rt, "tcp_write", native_tcp_write, 2);
    rt_register_native(rt, "tcp_close", native_tcp_close, 1);
    rt_register_native(rt, "tcp_peer_addr", native_tcp_peer_addr, 1);
    rt_register_native(rt, "tcp_set_timeout", native_tcp_set_timeout, 2);

    /* TLS */
    rt_register_native(rt, "tls_connect", native_tls_connect, 2);
    rt_register_native(rt, "tls_read", native_tls_read, 1);
    rt_register_native(rt, "tls_read_bytes", native_tls_read_bytes, 2);
    rt_register_native(rt, "tls_write", native_tls_write, 2);
    rt_register_native(rt, "tls_close", native_tls_close, 1);
    rt_register_native(rt, "tls_available", native_tls_available, 0);

    /* HTTP */
    rt_register_native(rt, "http_get", native_http_get, 1);
    rt_register_native(rt, "http_post", native_http_post, 2);
    rt_register_native(rt, "http_request", native_http_request, 3);

    /* JSON/TOML/YAML */
    rt_register_native(rt, "json_parse", native_json_parse, 1);
    rt_register_native(rt, "json_stringify", native_json_stringify, 1);
    rt_register_native(rt, "toml_parse", native_toml_parse, 1);
    rt_register_native(rt, "toml_stringify", native_toml_stringify, 1);
    rt_register_native(rt, "yaml_parse", native_yaml_parse, 1);
    rt_register_native(rt, "yaml_stringify", native_yaml_stringify, 1);

    /* Crypto */
    rt_register_native(rt, "sha256", native_sha256, 1);
    rt_register_native(rt, "md5", native_md5, 1);
    rt_register_native(rt, "base64_encode", native_base64_encode, 1);
    rt_register_native(rt, "base64_decode", native_base64_decode, 1);
    rt_register_native(rt, "sha512", native_sha512, 1);
    rt_register_native(rt, "hmac_sha256", native_hmac_sha256, 2);
    rt_register_native(rt, "random_bytes", native_random_bytes, 1);

    /* Regex */
    rt_register_native(rt, "regex_match", native_regex_match, 2);
    rt_register_native(rt, "regex_find_all", native_regex_find_all, 2);
    rt_register_native(rt, "regex_replace", native_regex_replace, 3);

    /* Time/DateTime */
    rt_register_native(rt, "time", native_time, 0);
    rt_register_native(rt, "sleep", native_sleep, 1);
    rt_register_native(rt, "time_format", native_time_format, 2);
    rt_register_native(rt, "time_parse", native_time_parse, 2);
    rt_register_native(rt, "time_year", native_time_year, 1);
    rt_register_native(rt, "time_month", native_time_month, 1);
    rt_register_native(rt, "time_day", native_time_day, 1);
    rt_register_native(rt, "time_hour", native_time_hour, 1);
    rt_register_native(rt, "time_minute", native_time_minute, 1);
    rt_register_native(rt, "time_second", native_time_second, 1);
    rt_register_native(rt, "time_weekday", native_time_weekday, 1);
    rt_register_native(rt, "time_add", native_time_add, 2);
    rt_register_native(rt, "is_leap_year", native_is_leap_year, 1);

    /* Duration */
    rt_register_native(rt, "duration", native_duration, 4);
    rt_register_native(rt, "duration_from_seconds", native_duration_from_seconds, 1);
    rt_register_native(rt, "duration_from_millis", native_duration_from_millis, 1);
    rt_register_native(rt, "duration_add", native_duration_add, 2);
    rt_register_native(rt, "duration_sub", native_duration_sub, 2);
    rt_register_native(rt, "duration_to_string", native_duration_to_string, 1);
    rt_register_native(rt, "duration_hours", native_duration_hours, 1);
    rt_register_native(rt, "duration_minutes", native_duration_minutes, 1);
    rt_register_native(rt, "duration_seconds", native_duration_seconds, 1);
    rt_register_native(rt, "duration_millis", native_duration_millis, 1);

    /* DateTime */
    rt_register_native(rt, "datetime_now", native_datetime_now, 0);
    rt_register_native(rt, "datetime_from_epoch", native_datetime_from_epoch, 1);
    rt_register_native(rt, "datetime_to_epoch", native_datetime_to_epoch, 1);
    rt_register_native(rt, "datetime_from_iso", native_datetime_from_iso, 1);
    rt_register_native(rt, "datetime_to_iso", native_datetime_to_iso, 1);
    rt_register_native(rt, "datetime_add_duration", native_datetime_add_duration, 2);
    rt_register_native(rt, "datetime_sub", native_datetime_sub, 2);
    rt_register_native(rt, "datetime_format", native_datetime_fmt, 2);
    rt_register_native(rt, "datetime_to_utc", native_datetime_to_utc, 1);
    rt_register_native(rt, "datetime_to_local", native_datetime_to_local, 1);
    rt_register_native(rt, "timezone_offset", native_timezone_offset, 0);

    /* Calendar */
    rt_register_native(rt, "days_in_month", native_days_in_month, 2);
    rt_register_native(rt, "day_of_week", native_day_of_week, 3);
    rt_register_native(rt, "day_of_year", native_day_of_year, 3);

    /* Environment */
    rt_register_native(rt, "env", native_env, 1);
    rt_register_native(rt, "env_set", native_env_set, 2);
    rt_register_native(rt, "env_keys", native_env_keys, 0);

    /* Process */
    rt_register_native(rt, "cwd", native_cwd, 0);
    rt_register_native(rt, "exec", native_exec_cmd, 1);
    rt_register_native(rt, "shell", native_shell, 1);
    rt_register_native(rt, "platform", native_platform, 0);
    rt_register_native(rt, "hostname", native_hostname, 0);
    rt_register_native(rt, "pid", native_pid, 0);

    /* Type/utility */
    rt_register_native(rt, "to_int", native_to_int, 1);
    rt_register_native(rt, "to_float", native_to_float, 1);
    rt_register_native(rt, "struct_name", native_struct_name, 1);
    rt_register_native(rt, "struct_fields", native_struct_fields, 1);
    rt_register_native(rt, "struct_to_map", native_struct_to_map, 1);
    rt_register_native(rt, "repr", native_repr, 1);
    rt_register_native(rt, "format", native_format, -1);
    rt_register_native(rt, "range", native_range, -1);
    rt_register_native(rt, "print_raw", native_print_raw, -1);
    rt_register_native(rt, "eprint", native_eprint, -1);
    rt_register_native(rt, "identity", native_identity, 1);
    rt_register_native(rt, "debug_assert", native_debug_assert, 2);
    rt_register_native(rt, "panic", native_panic, 1);

    /* Assertion helpers */
    rt_register_native(rt, "assert_eq", native_assert_eq, 2);
    rt_register_native(rt, "assert_ne", native_assert_ne, 2);
    rt_register_native(rt, "assert_true", native_assert_true, 1);
    rt_register_native(rt, "assert_false", native_assert_false, 1);
    rt_register_native(rt, "assert_nil", native_assert_nil, 1);
    rt_register_native(rt, "assert_throws", native_assert_throws, 1);
    rt_register_native(rt, "assert_contains", native_assert_contains, 2);
    rt_register_native(rt, "assert_type", native_assert_type, 2);

    /* Module loading */
    rt_register_native(rt, "require", native_require, 1);
    rt_register_native(rt, "require_ext", native_require_ext, 1);

    /* Metaprogramming/reflection */
    rt_register_native(rt, "args", native_args, 0);
    rt_register_native(rt, "struct_from_map", native_struct_from_map, 2);
    rt_register_native(rt, "is_complete", native_is_complete, 1);
    rt_register_native(rt, "tokenize", native_tokenize, 1);
    rt_register_native(rt, "lat_eval", native_lat_eval, 1);

    /* Bitwise float conversion (for bytecode serialization) */
    rt_register_native(rt, "float_to_bits", native_float_to_bits, 1);
    rt_register_native(rt, "bits_to_float", native_bits_to_float, 1);

    /* URL encoding */
    rt_register_native(rt, "url_encode", native_url_encode, 1);
    rt_register_native(rt, "url_decode", native_url_decode, 1);

    /* CSV */
    rt_register_native(rt, "csv_parse", native_csv_parse, 1);
    rt_register_native(rt, "csv_stringify", native_csv_stringify, 1);

    /* Functional */
    rt_register_native(rt, "pipe", native_pipe, -1);
    rt_register_native(rt, "compose", native_compose, 2);

    intern_init();

    intern_init();
}

void lat_runtime_free(LatRuntime *rt) {
    if (rt->env) env_free(rt->env);
    if (rt->struct_meta) env_free(rt->struct_meta);
    free(rt->error);

    /* Free module cache */
    for (size_t i = 0; i < rt->module_cache.cap; i++) {
        if (rt->module_cache.entries[i].state == MAP_OCCUPIED) {
            LatValue *v = (LatValue *)rt->module_cache.entries[i].value;
            value_free(v);
        }
    }
    lat_map_free(&rt->module_cache);
    lat_map_free(&rt->required_files);

    /* Free extension cache */
    for (size_t i = 0; i < rt->loaded_extensions.cap; i++) {
        if (rt->loaded_extensions.entries[i].state == MAP_OCCUPIED) {
            LatValue *v = (LatValue *)rt->loaded_extensions.entries[i].value;
            value_free(v);
        }
    }
    lat_map_free(&rt->loaded_extensions);
    free(rt->script_dir);

    /* Free tracked vars */
    for (size_t i = 0; i < rt->tracked_count; i++) {
        free(rt->tracked_vars[i].name);
        for (size_t j = 0; j < rt->tracked_vars[i].snap_count; j++) {
            free(rt->tracked_vars[i].snapshots[j].phase);
            free(rt->tracked_vars[i].snapshots[j].fn_name);
            value_free(&rt->tracked_vars[i].snapshots[j].value);
        }
        free(rt->tracked_vars[i].snapshots);
    }
    free(rt->tracked_vars);

    /* Free pressures */
    for (size_t i = 0; i < rt->pressure_count; i++) {
        free(rt->pressures[i].name);
        free(rt->pressures[i].mode);
    }
    free(rt->pressures);

    /* Free reactions */
    for (size_t i = 0; i < rt->reaction_count; i++) {
        free(rt->reactions[i].var_name);
        for (size_t j = 0; j < rt->reactions[i].cb_count; j++) value_free(&rt->reactions[i].callbacks[j]);
        free(rt->reactions[i].callbacks);
    }
    free(rt->reactions);

    /* Free bonds */
    for (size_t i = 0; i < rt->bond_count; i++) {
        free(rt->bonds[i].target);
        for (size_t j = 0; j < rt->bonds[i].dep_count; j++) {
            free(rt->bonds[i].deps[j]);
            if (rt->bonds[i].dep_strategies) free(rt->bonds[i].dep_strategies[j]);
        }
        free(rt->bonds[i].deps);
        free(rt->bonds[i].dep_strategies);
    }
    free(rt->bonds);

    /* Free seeds */
    for (size_t i = 0; i < rt->seed_count; i++) {
        free(rt->seeds[i].var_name);
        value_free(&rt->seeds[i].contract);
    }
    free(rt->seeds);

    intern_free();
}
