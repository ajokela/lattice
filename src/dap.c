#include "dap.h"
#include "debugger.h"
#include "stackvm.h"
#include "chunk.h"
#include "value.h"
#include "env.h"
#include "ds/hashmap.h"
#include "../vendor/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Variable reference management ── */

/* Variable references for structured data (arrays, structs, maps).
 * Cleared on each stop event. IDs start at 1000 to avoid conflicts. */
#define DAP_VARREF_BASE    1000
#define DAP_VARREF_LOCALS  1 /* Locals scope */
#define DAP_VARREF_GLOBALS 2 /* Globals scope */

typedef struct {
    int ref_id;
    LatValue value; /* shallow copy — do NOT free */
} DapVarRef;

static DapVarRef *s_varrefs = NULL;
static size_t s_varref_count = 0;
static size_t s_varref_cap = 0;

static void varrefs_clear(void) { s_varref_count = 0; }

static int varrefs_add(LatValue val) {
    if (s_varref_count >= s_varref_cap) {
        s_varref_cap = s_varref_cap ? s_varref_cap * 2 : 32;
        s_varrefs = realloc(s_varrefs, s_varref_cap * sizeof(DapVarRef));
    }
    int id = DAP_VARREF_BASE + (int)s_varref_count;
    s_varrefs[s_varref_count].ref_id = id;
    s_varrefs[s_varref_count].value = val;
    s_varref_count++;
    return id;
}

static DapVarRef *varrefs_find(int ref_id) {
    if (ref_id < DAP_VARREF_BASE) return NULL;
    int idx = ref_id - DAP_VARREF_BASE;
    if (idx < 0 || (size_t)idx >= s_varref_count) return NULL;
    return &s_varrefs[idx];
}

/* ── DAP message I/O ── */

cJSON *dap_read_message(FILE *in) {
    char header[256];
    int content_length = -1;

    while (fgets(header, sizeof(header), in)) {
        if (header[0] == '\r' || header[0] == '\n') break;
        if (strncmp(header, "Content-Length:", 15) == 0) { content_length = atoi(header + 15); }
    }

    if (content_length <= 0) return NULL;

    char *body = malloc((size_t)content_length + 1);
    if (!body) return NULL;

    size_t nread = fread(body, 1, (size_t)content_length, in);
    if ((int)nread != content_length) {
        free(body);
        return NULL;
    }
    body[content_length] = '\0';

    cJSON *json = cJSON_Parse(body);
    free(body);
    return json;
}

void dap_write_message(cJSON *msg, FILE *out) {
    char *body = cJSON_PrintUnformatted(msg);
    if (!body) return;

    fprintf(out, "Content-Length: %zu\r\n\r\n%s", strlen(body), body);
    fflush(out);
    cJSON_free(body);
}

/* ── DAP response/event helpers ── */

void dap_send_response(Debugger *dbg, int request_seq, const char *command, cJSON *body) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddNumberToObject(msg, "seq", dbg->dap_seq++);
    cJSON_AddStringToObject(msg, "type", "response");
    cJSON_AddNumberToObject(msg, "request_seq", request_seq);
    cJSON_AddBoolToObject(msg, "success", 1);
    cJSON_AddStringToObject(msg, "command", command);
    if (body) cJSON_AddItemToObject(msg, "body", body);
    dap_write_message(msg, dbg->dap_out);
    cJSON_Delete(msg);
}

void dap_send_event(Debugger *dbg, const char *event, cJSON *body) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddNumberToObject(msg, "seq", dbg->dap_seq++);
    cJSON_AddStringToObject(msg, "type", "event");
    cJSON_AddStringToObject(msg, "event", event);
    if (body) cJSON_AddItemToObject(msg, "body", body);
    dap_write_message(msg, dbg->dap_out);
    cJSON_Delete(msg);
}

void dap_send_error(Debugger *dbg, int request_seq, const char *command, const char *message) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddNumberToObject(msg, "seq", dbg->dap_seq++);
    cJSON_AddStringToObject(msg, "type", "response");
    cJSON_AddNumberToObject(msg, "request_seq", request_seq);
    cJSON_AddBoolToObject(msg, "success", 0);
    cJSON_AddStringToObject(msg, "command", command);
    cJSON_AddStringToObject(msg, "message", message);
    dap_write_message(msg, dbg->dap_out);
    cJSON_Delete(msg);
}

/* ── Output capture callback for DAP mode ── */

static void dap_print_callback(const char *text, void *userdata) {
    Debugger *dbg = (Debugger *)userdata;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "category", "stdout");
    cJSON_AddStringToObject(body, "output", text);
    dap_send_event(dbg, "output", body);
}

/* ── Helper: get variable reference for compound values ── */

static int get_varref_for_value(LatValue *val) {
    if (val->type == VAL_ARRAY || val->type == VAL_STRUCT || val->type == VAL_MAP || val->type == VAL_TUPLE) {
        return varrefs_add(*val);
    }
    return 0;
}

/* ── Helper: build variables array for a scope ── */

static cJSON *build_locals_variables(StackVM *vm, StackCallFrame *frame) {
    cJSON *vars = cJSON_CreateArray();
    Chunk *chunk = frame->chunk;
    if (!chunk || !chunk->local_names) return vars;

    LatValue *frame_top = vm->stack_top;
    size_t slot_count = (size_t)(frame_top - frame->slots);
    for (size_t i = 0; i < chunk->local_name_cap && i < slot_count; i++) {
        if (chunk->local_names[i] && chunk->local_names[i][0] != '\0') {
            cJSON *var = cJSON_CreateObject();
            cJSON_AddStringToObject(var, "name", chunk->local_names[i]);
            char *repr = value_repr(&frame->slots[i]);
            cJSON_AddStringToObject(var, "value", repr);
            free(repr);
            /* Add type */
            const char *type_name = "unknown";
            switch (frame->slots[i].type) {
                case VAL_INT: type_name = "Int"; break;
                case VAL_FLOAT: type_name = "Float"; break;
                case VAL_BOOL: type_name = "Bool"; break;
                case VAL_STR: type_name = "String"; break;
                case VAL_ARRAY: type_name = "Array"; break;
                case VAL_STRUCT: type_name = "Struct"; break;
                case VAL_MAP: type_name = "Map"; break;
                case VAL_CLOSURE: type_name = "Function"; break;
                case VAL_NIL: type_name = "Nil"; break;
                case VAL_UNIT: type_name = "Unit"; break;
                case VAL_TUPLE: type_name = "Tuple"; break;
                case VAL_ENUM: type_name = "Enum"; break;
                case VAL_SET: type_name = "Set"; break;
                case VAL_RANGE: type_name = "Range"; break;
                case VAL_BUFFER: type_name = "Buffer"; break;
                case VAL_CHANNEL: type_name = "Channel"; break;
                case VAL_REF: type_name = "Ref"; break;
                case VAL_ITERATOR: type_name = "Iterator"; break;
                default: break;
            }
            cJSON_AddStringToObject(var, "type", type_name);
            int vref = get_varref_for_value(&frame->slots[i]);
            cJSON_AddNumberToObject(var, "variablesReference", vref);
            cJSON_AddItemToArray(vars, var);
        }
    }
    return vars;
}

/* Callback context for iterating globals */
typedef struct {
    cJSON *vars;
} GlobalIterCtx;

static void globals_iter_fn(const char *key, void *value, void *ctx) {
    GlobalIterCtx *gctx = (GlobalIterCtx *)ctx;
    LatValue *val = (LatValue *)value;
    cJSON *var = cJSON_CreateObject();
    cJSON_AddStringToObject(var, "name", key);
    char *repr = value_repr(val);
    cJSON_AddStringToObject(var, "value", repr);
    free(repr);
    int vref = get_varref_for_value(val);
    cJSON_AddNumberToObject(var, "variablesReference", vref);
    cJSON_AddItemToArray(gctx->vars, var);
}

static cJSON *build_globals_variables(StackVM *vm) {
    cJSON *vars = cJSON_CreateArray();
    if (!vm->env) return vars;

    /* Iterate the root scope (scope 0 = globals) */
    Env *e = vm->env;
    if (e->count > 0) {
        GlobalIterCtx ctx = {.vars = vars};
        lat_map_iter(&e->scopes[0], globals_iter_fn, &ctx);
    }
    return vars;
}

static cJSON *build_compound_variables(LatValue *val) {
    cJSON *vars = cJSON_CreateArray();

    if (val->type == VAL_ARRAY) {
        for (size_t i = 0; i < val->as.array.len; i++) {
            cJSON *var = cJSON_CreateObject();
            char idx[32];
            snprintf(idx, sizeof(idx), "[%zu]", i);
            cJSON_AddStringToObject(var, "name", idx);
            char *repr = value_repr(&val->as.array.elems[i]);
            cJSON_AddStringToObject(var, "value", repr);
            free(repr);
            int vref = get_varref_for_value(&val->as.array.elems[i]);
            cJSON_AddNumberToObject(var, "variablesReference", vref);
            cJSON_AddItemToArray(vars, var);
        }
    } else if (val->type == VAL_STRUCT) {
        for (size_t i = 0; i < val->as.strct.field_count; i++) {
            cJSON *var = cJSON_CreateObject();
            cJSON_AddStringToObject(var, "name", val->as.strct.field_names[i]);
            char *repr = value_repr(&val->as.strct.field_values[i]);
            cJSON_AddStringToObject(var, "value", repr);
            free(repr);
            int vref = get_varref_for_value(&val->as.strct.field_values[i]);
            cJSON_AddNumberToObject(var, "variablesReference", vref);
            cJSON_AddItemToArray(vars, var);
        }
    } else if (val->type == VAL_MAP) {
        if (val->as.map.map) {
            /* Use iterator callback for LatMap */
            GlobalIterCtx ctx = {.vars = vars};
            lat_map_iter(val->as.map.map, globals_iter_fn, &ctx);
        }
    } else if (val->type == VAL_TUPLE) {
        for (size_t i = 0; i < val->as.tuple.len; i++) {
            cJSON *var = cJSON_CreateObject();
            char idx[32];
            snprintf(idx, sizeof(idx), "[%zu]", i);
            cJSON_AddStringToObject(var, "name", idx);
            char *repr = value_repr(&val->as.tuple.elems[i]);
            cJSON_AddStringToObject(var, "value", repr);
            free(repr);
            int vref = get_varref_for_value(&val->as.tuple.elems[i]);
            cJSON_AddNumberToObject(var, "variablesReference", vref);
            cJSON_AddItemToArray(vars, var);
        }
    }

    return vars;
}

/* ── Helper: get line from frame ── */

static int dap_frame_line(StackCallFrame *frame) {
    if (!frame->chunk || !frame->chunk->lines || frame->chunk->lines_len == 0) return 0;
    size_t offset = (size_t)(frame->ip - frame->chunk->code);
    if (offset > 0) offset--;
    if (offset >= frame->chunk->lines_len) offset = frame->chunk->lines_len - 1;
    return frame->chunk->lines[offset];
}

/* ── DAP request handlers ── */

static void handle_threads(Debugger *dbg, int req_seq) {
    cJSON *body = cJSON_CreateObject();
    cJSON *threads = cJSON_CreateArray();
    cJSON *thread = cJSON_CreateObject();
    cJSON_AddNumberToObject(thread, "id", 1);
    cJSON_AddStringToObject(thread, "name", "main");
    cJSON_AddItemToArray(threads, thread);
    cJSON_AddItemToObject(body, "threads", threads);
    dap_send_response(dbg, req_seq, "threads", body);
}

static void handle_stack_trace(Debugger *dbg, StackVM *vm, int req_seq) {
    cJSON *body = cJSON_CreateObject();
    cJSON *frames = cJSON_CreateArray();

    /* Build stack trace from top to bottom */
    for (int i = (int)vm->frame_count - 1; i >= 0; i--) {
        StackCallFrame *f = &vm->frames[i];
        if (!f->chunk) continue;

        cJSON *sf = cJSON_CreateObject();
        cJSON_AddNumberToObject(sf, "id", i);
        const char *name = f->chunk->name;
        cJSON_AddStringToObject(sf, "name", (name && name[0]) ? name : "<script>");
        cJSON_AddNumberToObject(sf, "line", dap_frame_line(f));
        cJSON_AddNumberToObject(sf, "column", 1);

        if (dbg->source_path) {
            cJSON *source = cJSON_CreateObject();
            cJSON_AddStringToObject(source, "path", dbg->source_path);
            /* Extract filename from path */
            const char *slash = strrchr(dbg->source_path, '/');
            cJSON_AddStringToObject(source, "name", slash ? slash + 1 : dbg->source_path);
            cJSON_AddItemToObject(sf, "source", source);
        }

        cJSON_AddItemToArray(frames, sf);
    }

    cJSON_AddItemToObject(body, "stackFrames", frames);
    cJSON_AddNumberToObject(body, "totalFrames", (double)vm->frame_count);
    dap_send_response(dbg, req_seq, "stackTrace", body);
}

static void handle_scopes(Debugger *dbg, int req_seq, int frame_id) {
    (void)frame_id;
    cJSON *body = cJSON_CreateObject();
    cJSON *scopes = cJSON_CreateArray();

    /* Locals scope */
    cJSON *locals_scope = cJSON_CreateObject();
    cJSON_AddStringToObject(locals_scope, "name", "Locals");
    cJSON_AddStringToObject(locals_scope, "presentationHint", "locals");
    cJSON_AddNumberToObject(locals_scope, "variablesReference", DAP_VARREF_LOCALS);
    cJSON_AddBoolToObject(locals_scope, "expensive", 0);
    cJSON_AddItemToArray(scopes, locals_scope);

    /* Globals scope */
    cJSON *globals_scope = cJSON_CreateObject();
    cJSON_AddStringToObject(globals_scope, "name", "Globals");
    cJSON_AddStringToObject(globals_scope, "presentationHint", "globals");
    cJSON_AddNumberToObject(globals_scope, "variablesReference", DAP_VARREF_GLOBALS);
    cJSON_AddBoolToObject(globals_scope, "expensive", 1);
    cJSON_AddItemToArray(scopes, globals_scope);

    cJSON_AddItemToObject(body, "scopes", scopes);
    dap_send_response(dbg, req_seq, "scopes", body);
}

static void handle_variables(Debugger *dbg, StackVM *vm, StackCallFrame *frame, int req_seq, int var_ref) {
    cJSON *body = cJSON_CreateObject();
    cJSON *variables;

    if (var_ref == DAP_VARREF_LOCALS) {
        variables = build_locals_variables(vm, frame);
    } else if (var_ref == DAP_VARREF_GLOBALS) {
        variables = build_globals_variables(vm);
    } else {
        /* Compound variable expansion */
        DapVarRef *ref = varrefs_find(var_ref);
        if (ref) {
            variables = build_compound_variables(&ref->value);
        } else {
            variables = cJSON_CreateArray();
        }
    }

    cJSON_AddItemToObject(body, "variables", variables);
    dap_send_response(dbg, req_seq, "variables", body);
}

static void handle_set_breakpoints(Debugger *dbg, int req_seq, cJSON *args) {
    /* Clear existing line breakpoints for this source */
    for (size_t i = 0; i < dbg->bp_count;) {
        if (dbg->breakpoints[i].type == BP_LINE) {
            free(dbg->breakpoints[i].func_name);
            free(dbg->breakpoints[i].condition);
            dbg->breakpoints[i] = dbg->breakpoints[--dbg->bp_count];
        } else {
            i++;
        }
    }

    cJSON *body = cJSON_CreateObject();
    cJSON *bps_out = cJSON_CreateArray();
    cJSON *bps_in = cJSON_GetObjectItem(args, "breakpoints");
    if (bps_in) {
        cJSON *bp_item;
        cJSON_ArrayForEach(bp_item, bps_in) {
            cJSON *line_json = cJSON_GetObjectItem(bp_item, "line");
            if (!line_json) continue;
            int line = line_json->valueint;
            cJSON *cond_json = cJSON_GetObjectItem(bp_item, "condition");
            const char *cond = cond_json ? cond_json->valuestring : NULL;

            int id = debugger_add_breakpoint_line(dbg, line, cond);

            cJSON *bp_out = cJSON_CreateObject();
            cJSON_AddNumberToObject(bp_out, "id", id);
            cJSON_AddBoolToObject(bp_out, "verified", 1);
            cJSON_AddNumberToObject(bp_out, "line", line);
            cJSON_AddItemToArray(bps_out, bp_out);
        }
    }
    cJSON_AddItemToObject(body, "breakpoints", bps_out);
    dap_send_response(dbg, req_seq, "setBreakpoints", body);
}

static void handle_set_function_breakpoints(Debugger *dbg, int req_seq, cJSON *args) {
    /* Clear existing function breakpoints */
    for (size_t i = 0; i < dbg->bp_count;) {
        if (dbg->breakpoints[i].type == BP_FUNCTION) {
            free(dbg->breakpoints[i].func_name);
            free(dbg->breakpoints[i].condition);
            dbg->breakpoints[i] = dbg->breakpoints[--dbg->bp_count];
        } else {
            i++;
        }
    }

    cJSON *body = cJSON_CreateObject();
    cJSON *bps_out = cJSON_CreateArray();
    cJSON *bps_in = cJSON_GetObjectItem(args, "breakpoints");
    if (bps_in) {
        cJSON *bp_item;
        cJSON_ArrayForEach(bp_item, bps_in) {
            cJSON *name_json = cJSON_GetObjectItem(bp_item, "name");
            if (!name_json || !name_json->valuestring) continue;
            cJSON *cond_json = cJSON_GetObjectItem(bp_item, "condition");
            const char *cond = cond_json ? cond_json->valuestring : NULL;

            int id = debugger_add_breakpoint_func(dbg, name_json->valuestring, cond);

            cJSON *bp_out = cJSON_CreateObject();
            cJSON_AddNumberToObject(bp_out, "id", id);
            cJSON_AddBoolToObject(bp_out, "verified", 1);
            cJSON_AddItemToArray(bps_out, bp_out);
        }
    }
    cJSON_AddItemToObject(body, "breakpoints", bps_out);
    dap_send_response(dbg, req_seq, "setFunctionBreakpoints", body);
}

static void handle_evaluate(Debugger *dbg, void *vm, int req_seq, cJSON *args) {
    cJSON *expr_json = cJSON_GetObjectItem(args, "expression");
    if (!expr_json || !expr_json->valuestring) {
        dap_send_error(dbg, req_seq, "evaluate", "missing expression");
        return;
    }

    char *result = NULL, *error = NULL;
    if (debugger_eval_expr(dbg, vm, expr_json->valuestring, &result, &error)) {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "result", result);
        cJSON_AddNumberToObject(body, "variablesReference", 0);
        dap_send_response(dbg, req_seq, "evaluate", body);
        free(result);
    } else {
        dap_send_error(dbg, req_seq, "evaluate", error);
        free(error);
    }
}

/* ── DAP handshake ── */

bool dap_handshake(Debugger *dbg, const char *source_path) {
    (void)source_path;

    /* Process messages until we get configurationDone */
    while (true) {
        cJSON *msg = dap_read_message(dbg->dap_in);
        if (!msg) return false;

        cJSON *type = cJSON_GetObjectItem(msg, "type");
        if (!type || !type->valuestring || strcmp(type->valuestring, "request") != 0) {
            cJSON_Delete(msg);
            continue;
        }

        cJSON *cmd = cJSON_GetObjectItem(msg, "command");
        cJSON *seq = cJSON_GetObjectItem(msg, "seq");
        int req_seq = seq ? seq->valueint : 0;
        const char *command = cmd ? cmd->valuestring : "";
        cJSON *args = cJSON_GetObjectItem(msg, "arguments");

        if (strcmp(command, "initialize") == 0) {
            /* Send capabilities */
            cJSON *body = cJSON_CreateObject();
            cJSON_AddBoolToObject(body, "supportsConfigurationDoneRequest", 1);
            cJSON_AddBoolToObject(body, "supportsFunctionBreakpoints", 1);
            cJSON_AddBoolToObject(body, "supportsConditionalBreakpoints", 1);
            cJSON_AddBoolToObject(body, "supportsEvaluateForHovers", 1);
            cJSON_AddBoolToObject(body, "supportsStepBack", 0);
            cJSON_AddBoolToObject(body, "supportsSetVariable", 0);
            cJSON_AddBoolToObject(body, "supportsSteppingGranularity", 0);
            dap_send_response(dbg, req_seq, "initialize", body);

            /* Send initialized event */
            dap_send_event(dbg, "initialized", NULL);
            dbg->dap_initialized = true;

        } else if (strcmp(command, "launch") == 0) {
            /* Check for stopOnEntry */
            bool stop_on_entry = false;
            if (args) {
                cJSON *soe = cJSON_GetObjectItem(args, "stopOnEntry");
                if (soe && cJSON_IsTrue(soe)) stop_on_entry = true;
            }
            if (stop_on_entry) {
                dbg->step_mode = true;
            } else {
                dbg->running = true;
            }
            dbg->dap_launched = true;
            dap_send_response(dbg, req_seq, "launch", NULL);

        } else if (strcmp(command, "setBreakpoints") == 0) {
            handle_set_breakpoints(dbg, req_seq, args);

        } else if (strcmp(command, "setFunctionBreakpoints") == 0) {
            handle_set_function_breakpoints(dbg, req_seq, args);

        } else if (strcmp(command, "setExceptionBreakpoints") == 0) {
            /* Acknowledge but we don't support exception breakpoints */
            dap_send_response(dbg, req_seq, "setExceptionBreakpoints", NULL);

        } else if (strcmp(command, "configurationDone") == 0) {
            dap_send_response(dbg, req_seq, "configurationDone", NULL);
            /* Set up output capture */
            dbg->print_callback = dap_print_callback;
            dbg->print_userdata = dbg;
            cJSON_Delete(msg);
            return true;

        } else if (strcmp(command, "threads") == 0) {
            handle_threads(dbg, req_seq);

        } else if (strcmp(command, "disconnect") == 0) {
            dap_send_response(dbg, req_seq, "disconnect", NULL);
            cJSON_Delete(msg);
            return false;

        } else {
            dap_send_error(dbg, req_seq, command, "not supported during initialization");
        }

        cJSON_Delete(msg);
    }
}

/* ── DAP stopped message loop ── */

bool dap_debugger_check(Debugger *dbg, void *vm_ptr, void *frame_ptr, size_t frame_count, int line,
                        const char *stop_reason) {
    StackVM *vm = (StackVM *)vm_ptr;
    StackCallFrame *frame = (StackCallFrame *)frame_ptr;
    (void)line;

    /* Clear variable references from previous stop */
    varrefs_clear();

    /* Send stopped event */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "reason", stop_reason);
    cJSON_AddNumberToObject(body, "threadId", 1);
    cJSON_AddBoolToObject(body, "allThreadsStopped", 1);
    dap_send_event(dbg, "stopped", body);

    /* Enter DAP message loop */
    while (true) {
        cJSON *msg = dap_read_message(dbg->dap_in);
        if (!msg) return false; /* EOF */

        cJSON *type = cJSON_GetObjectItem(msg, "type");
        if (!type || !type->valuestring || strcmp(type->valuestring, "request") != 0) {
            cJSON_Delete(msg);
            continue;
        }

        cJSON *cmd = cJSON_GetObjectItem(msg, "command");
        cJSON *seq = cJSON_GetObjectItem(msg, "seq");
        int req_seq = seq ? seq->valueint : 0;
        const char *command = cmd ? cmd->valuestring : "";
        cJSON *args = cJSON_GetObjectItem(msg, "arguments");

        if (strcmp(command, "continue") == 0) {
            dbg->running = true;
            cJSON *resp_body = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp_body, "allThreadsContinued", 1);
            dap_send_response(dbg, req_seq, "continue", resp_body);
            cJSON_Delete(msg);
            return true;

        } else if (strcmp(command, "next") == 0) {
            dbg->next_mode = true;
            dbg->next_depth = (int)frame_count;
            dap_send_response(dbg, req_seq, "next", NULL);
            cJSON_Delete(msg);
            return true;

        } else if (strcmp(command, "stepIn") == 0) {
            dbg->step_mode = true;
            dap_send_response(dbg, req_seq, "stepIn", NULL);
            cJSON_Delete(msg);
            return true;

        } else if (strcmp(command, "stepOut") == 0) {
            dbg->step_out_mode = true;
            dbg->step_out_depth = (int)frame_count;
            dap_send_response(dbg, req_seq, "stepOut", NULL);
            cJSON_Delete(msg);
            return true;

        } else if (strcmp(command, "threads") == 0) {
            handle_threads(dbg, req_seq);

        } else if (strcmp(command, "stackTrace") == 0) {
            handle_stack_trace(dbg, vm, req_seq);

        } else if (strcmp(command, "scopes") == 0) {
            int frame_id = 0;
            if (args) {
                cJSON *fid = cJSON_GetObjectItem(args, "frameId");
                if (fid) frame_id = fid->valueint;
            }
            handle_scopes(dbg, req_seq, frame_id);

        } else if (strcmp(command, "variables") == 0) {
            int var_ref = 0;
            if (args) {
                cJSON *vr = cJSON_GetObjectItem(args, "variablesReference");
                if (vr) var_ref = vr->valueint;
            }
            handle_variables(dbg, vm, frame, req_seq, var_ref);

        } else if (strcmp(command, "setBreakpoints") == 0) {
            handle_set_breakpoints(dbg, req_seq, args);

        } else if (strcmp(command, "setFunctionBreakpoints") == 0) {
            handle_set_function_breakpoints(dbg, req_seq, args);

        } else if (strcmp(command, "setExceptionBreakpoints") == 0) {
            dap_send_response(dbg, req_seq, "setExceptionBreakpoints", NULL);

        } else if (strcmp(command, "evaluate") == 0) {
            handle_evaluate(dbg, vm_ptr, req_seq, args);

        } else if (strcmp(command, "disconnect") == 0) {
            dap_send_response(dbg, req_seq, "disconnect", NULL);
            cJSON_Delete(msg);
            return false;

        } else {
            dap_send_error(dbg, req_seq, command, "not supported");
        }

        cJSON_Delete(msg);
    }
}

/* ── DAP termination ── */

void dap_send_terminated(Debugger *dbg) {
    if (!dbg || dbg->mode != DBG_MODE_DAP) return;
    dap_send_event(dbg, "terminated", NULL);
}

void dap_wait_disconnect(Debugger *dbg) {
    if (!dbg || dbg->mode != DBG_MODE_DAP) return;

    /* Wait for disconnect request */
    while (true) {
        cJSON *msg = dap_read_message(dbg->dap_in);
        if (!msg) return; /* EOF */

        cJSON *cmd = cJSON_GetObjectItem(msg, "command");
        cJSON *seq = cJSON_GetObjectItem(msg, "seq");
        int req_seq = seq ? seq->valueint : 0;

        if (cmd && cmd->valuestring && strcmp(cmd->valuestring, "disconnect") == 0) {
            dap_send_response(dbg, req_seq, "disconnect", NULL);
            cJSON_Delete(msg);
            return;
        }
        cJSON_Delete(msg);
    }
}
