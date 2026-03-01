#include "debugger.h"
#include "stackvm.h"
#include "stackcompiler.h"
#include "chunk.h"
#include "value.h"
#include "env.h"
#include "stackopcode.h"
#include "lexer.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Lifecycle ── */

static void debugger_init_common(Debugger *dbg) {
    dbg->bp_cap = 8;
    dbg->breakpoints = malloc(dbg->bp_cap * sizeof(Breakpoint));
    if (!dbg->breakpoints) {
        free(dbg);
        return;
    }
    dbg->bp_count = 0;
    dbg->next_bp_id = 1;
    dbg->step_mode = true;
    dbg->next_mode = false;
    dbg->next_depth = 0;
    dbg->running = false;
    dbg->step_out_mode = false;
    dbg->step_out_depth = 0;
    dbg->last_line = -1;
    dbg->last_frame_count = 0;
    dbg->watches = NULL;
    dbg->watch_count = 0;
    dbg->watch_cap = 0;
    dbg->next_watch_id = 1;
    dbg->source_lines = NULL;
    dbg->source_line_count = 0;
    dbg->source_path = NULL;
    dbg->mode = DBG_MODE_CLI;
    dbg->dap_in = NULL;
    dbg->dap_out = NULL;
    dbg->dap_seq = 1;
    dbg->dap_initialized = false;
    dbg->dap_launched = false;
    dbg->stop_reason = NULL;
    dbg->print_callback = NULL;
    dbg->print_userdata = NULL;
}

Debugger *debugger_new(void) {
    Debugger *dbg = calloc(1, sizeof(Debugger));
    if (!dbg) return NULL;
    debugger_init_common(dbg);
    return dbg;
}

Debugger *debugger_new_dap(FILE *in, FILE *out) {
    Debugger *dbg = calloc(1, sizeof(Debugger));
    if (!dbg) return NULL;
    debugger_init_common(dbg);
    dbg->mode = DBG_MODE_DAP;
    dbg->dap_in = in;
    dbg->dap_out = out;
    /* DAP starts in running mode, not step mode — waits for configurationDone */
    dbg->step_mode = false;
    return dbg;
}

void debugger_free(Debugger *dbg) {
    if (!dbg) return;
    for (size_t i = 0; i < dbg->bp_count; i++) {
        free(dbg->breakpoints[i].func_name);
        free(dbg->breakpoints[i].condition);
    }
    free(dbg->breakpoints);
    for (size_t i = 0; i < dbg->watch_count; i++) { free(dbg->watches[i].expr); }
    free(dbg->watches);
    if (dbg->source_lines) {
        for (size_t i = 0; i < dbg->source_line_count; i++) free(dbg->source_lines[i]);
        free(dbg->source_lines);
    }
    free(dbg->source_path);
    free(dbg->stop_reason);
    free(dbg);
}

/* ── Breakpoints ── */

int debugger_add_breakpoint_line(Debugger *dbg, int line, const char *condition) {
    /* Check for duplicate line breakpoint */
    for (size_t i = 0; i < dbg->bp_count; i++) {
        if (dbg->breakpoints[i].type == BP_LINE && dbg->breakpoints[i].line == line) return dbg->breakpoints[i].id;
    }
    if (dbg->bp_count >= dbg->bp_cap) {
        dbg->bp_cap *= 2;
        dbg->breakpoints = realloc(dbg->breakpoints, dbg->bp_cap * sizeof(Breakpoint));
        if (!dbg->breakpoints) return -1;
    }
    Breakpoint *bp = &dbg->breakpoints[dbg->bp_count++];
    bp->id = dbg->next_bp_id++;
    bp->type = BP_LINE;
    bp->enabled = true;
    bp->line = line;
    bp->func_name = NULL;
    bp->condition = condition ? strdup(condition) : NULL;
    bp->hit_count = 0;
    return bp->id;
}

int debugger_add_breakpoint_func(Debugger *dbg, const char *name, const char *condition) {
    /* Check for duplicate function breakpoint */
    for (size_t i = 0; i < dbg->bp_count; i++) {
        if (dbg->breakpoints[i].type == BP_FUNCTION && dbg->breakpoints[i].func_name &&
            strcmp(dbg->breakpoints[i].func_name, name) == 0)
            return dbg->breakpoints[i].id;
    }
    if (dbg->bp_count >= dbg->bp_cap) {
        dbg->bp_cap *= 2;
        dbg->breakpoints = realloc(dbg->breakpoints, dbg->bp_cap * sizeof(Breakpoint));
        if (!dbg->breakpoints) return -1;
    }
    Breakpoint *bp = &dbg->breakpoints[dbg->bp_count++];
    bp->id = dbg->next_bp_id++;
    bp->type = BP_FUNCTION;
    bp->enabled = true;
    bp->line = 0;
    bp->func_name = strdup(name);
    bp->condition = condition ? strdup(condition) : NULL;
    bp->hit_count = 0;
    return bp->id;
}

/* Legacy wrapper */
void debugger_add_breakpoint(Debugger *dbg, int line) { debugger_add_breakpoint_line(dbg, line, NULL); }

void debugger_remove_breakpoint(Debugger *dbg, int line) {
    for (size_t i = 0; i < dbg->bp_count; i++) {
        if (dbg->breakpoints[i].type == BP_LINE && dbg->breakpoints[i].line == line) {
            free(dbg->breakpoints[i].func_name);
            free(dbg->breakpoints[i].condition);
            dbg->breakpoints[i] = dbg->breakpoints[--dbg->bp_count];
            return;
        }
    }
}

bool debugger_remove_breakpoint_by_id(Debugger *dbg, int id) {
    for (size_t i = 0; i < dbg->bp_count; i++) {
        if (dbg->breakpoints[i].id == id) {
            free(dbg->breakpoints[i].func_name);
            free(dbg->breakpoints[i].condition);
            dbg->breakpoints[i] = dbg->breakpoints[--dbg->bp_count];
            return true;
        }
    }
    return false;
}

bool debugger_has_breakpoint(Debugger *dbg, int line) {
    for (size_t i = 0; i < dbg->bp_count; i++) {
        if (dbg->breakpoints[i].type == BP_LINE && dbg->breakpoints[i].enabled && dbg->breakpoints[i].line == line)
            return true;
    }
    return false;
}

/* ── Watch expressions ── */

int debugger_add_watch(Debugger *dbg, const char *expr) {
    if (dbg->watch_count >= dbg->watch_cap) {
        dbg->watch_cap = dbg->watch_cap ? dbg->watch_cap * 2 : 4;
        dbg->watches = realloc(dbg->watches, dbg->watch_cap * sizeof(WatchExpr));
        if (!dbg->watches) return -1;
    }
    WatchExpr *w = &dbg->watches[dbg->watch_count++];
    w->id = dbg->next_watch_id++;
    w->expr = strdup(expr);
    return w->id;
}

bool debugger_remove_watch(Debugger *dbg, int id) {
    for (size_t i = 0; i < dbg->watch_count; i++) {
        if (dbg->watches[i].id == id) {
            free(dbg->watches[i].expr);
            dbg->watches[i] = dbg->watches[--dbg->watch_count];
            return true;
        }
    }
    return false;
}

/* ── Expression evaluation ── */

bool debugger_eval_expr(Debugger *dbg, void *vm_ptr, const char *expr, char **result_repr, char **error) {
    (void)dbg;
    StackVM *vm = (StackVM *)vm_ptr;

    /* Lex the expression */
    char *lex_err = NULL;
    Lexer lex = lexer_new(expr);
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        *error = lex_err;
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return false;
    }

    /* Parse */
    char *parse_err = NULL;
    Parser parser = parser_new(&tokens);
    Program prog = parser_parse(&parser, &parse_err);
    if (parse_err) {
        *error = parse_err;
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return false;
    }

    /* Compile as REPL expression (keeps result on stack) */
    char *comp_err = NULL;
    Chunk *chunk = stack_compile_repl(&prog, &comp_err);
    if (!chunk) {
        *error = comp_err;
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return false;
    }

    /* Save VM state */
    size_t saved_handler_count = vm->handler_count;
    size_t saved_defer_count = vm->defer_count;

    /* Execute on the live VM */
    LatValue result;
    StackVMResult res = stackvm_run(vm, chunk, &result);

    /* Restore handler/defer counts */
    vm->handler_count = saved_handler_count;
    vm->defer_count = saved_defer_count;

    if (res != STACKVM_OK) {
        *error = vm->error ? strdup(vm->error) : strdup("evaluation error");
        /* Clear VM error so it doesn't persist */
        free(vm->error);
        vm->error = NULL;
        chunk_free(chunk);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return false;
    }

    *result_repr = value_repr(&result);
    value_free(&result);
    chunk_free(chunk);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    return true;
}

/* ── Source loading ── */

bool debugger_load_source(Debugger *dbg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    dbg->source_path = strdup(path);

    /* Read all lines */
    size_t cap = 64;
    dbg->source_lines = malloc(cap * sizeof(char *));
    if (!dbg->source_lines) {
        fclose(f);
        return false;
    }
    dbg->source_line_count = 0;

    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) {
        if (dbg->source_line_count >= cap) {
            cap *= 2;
            dbg->source_lines = realloc(dbg->source_lines, cap * sizeof(char *));
            if (!dbg->source_lines) {
                fclose(f);
                return false;
            }
        }
        /* Strip trailing newline */
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
        dbg->source_lines[dbg->source_line_count++] = strdup(buf);
    }

    fclose(f);
    return true;
}

/* ── Helper: get current source line number from a frame ── */

static int frame_current_line(StackCallFrame *frame) {
    if (!frame->chunk || !frame->chunk->lines || frame->chunk->lines_len == 0) return 0;
    size_t offset = (size_t)(frame->ip - frame->chunk->code);
    if (offset > 0) offset--; /* ip already advanced past the opcode */
    if (offset >= frame->chunk->lines_len) offset = frame->chunk->lines_len - 1;
    return frame->chunk->lines[offset];
}

/* ── Helper: print source context around a line ── */

static void print_source_context(Debugger *dbg, int line, int context) {
    if (!dbg->source_lines || dbg->source_line_count == 0) {
        fprintf(stderr, "(source not available)\n");
        return;
    }
    int start = line - context;
    if (start < 1) start = 1;
    int end = line + context;
    if (end > (int)dbg->source_line_count) end = (int)dbg->source_line_count;

    for (int i = start; i <= end; i++) {
        const char *marker = (i == line) ? "-->" : "   ";
        fprintf(stderr, "%s %4d | %s\n", marker, i, dbg->source_lines[i - 1]);
    }
}

/* ── Helper: show local variables ── */

static void print_locals(StackVM *vm, StackCallFrame *frame) {
    Chunk *chunk = frame->chunk;
    if (!chunk || !chunk->local_names) {
        fprintf(stderr, "(no local variable info)\n");
        return;
    }
    bool found_any = false;
    /* How many slots are used by this frame? */
    LatValue *frame_top = vm->stack_top;
    size_t slot_count = (size_t)(frame_top - frame->slots);
    for (size_t i = 0; i < chunk->local_name_cap && i < slot_count; i++) {
        if (chunk->local_names[i] && chunk->local_names[i][0] != '\0') {
            char *repr = value_repr(&frame->slots[i]);
            fprintf(stderr, "  %s = %s\n", chunk->local_names[i], repr);
            free(repr);
            found_any = true;
        }
    }
    if (!found_any) fprintf(stderr, "(no named locals in current scope)\n");
}

/* ── Helper: print backtrace ── */

static void print_backtrace(StackVM *vm, size_t frame_count) {
    fprintf(stderr, "Backtrace:\n");
    for (size_t i = 0; i < frame_count; i++) {
        StackCallFrame *f = &vm->frames[i];
        if (!f->chunk) continue;
        size_t offset = (size_t)(f->ip - f->chunk->code);
        if (offset > 0) offset--;
        int line = 0;
        if (f->chunk->lines && offset < f->chunk->lines_len) line = f->chunk->lines[offset];
        const char *name = f->chunk->name;
        if (i == frame_count - 1)
            fprintf(stderr, "  #%zu [line %d] in %s  <-- current\n", i, line, (name && name[0]) ? name : "<script>");
        else fprintf(stderr, "  #%zu [line %d] in %s\n", i, line, (name && name[0]) ? name : "<script>");
    }
}

/* ── Helper: print a variable by name ── */

static void print_variable(StackVM *vm, StackCallFrame *frame, const char *name) {
    Chunk *chunk = frame->chunk;
    /* Try local variables first */
    if (chunk && chunk->local_names) {
        LatValue *frame_top = vm->stack_top;
        size_t slot_count = (size_t)(frame_top - frame->slots);
        for (size_t i = 0; i < chunk->local_name_cap && i < slot_count; i++) {
            if (chunk->local_names[i] && strcmp(chunk->local_names[i], name) == 0) {
                char *repr = value_repr(&frame->slots[i]);
                fprintf(stderr, "%s = %s\n", name, repr);
                free(repr);
                return;
            }
        }
    }
    /* Try global variables */
    LatValue gval;
    if (env_get(vm->env, name, &gval)) {
        char *repr = value_repr(&gval);
        fprintf(stderr, "%s = %s\n", name, repr);
        free(repr);
        value_free(&gval);
        return;
    }
    fprintf(stderr, "variable '%s' not found\n", name);
}

/* ── Helper: evaluate and display watch expressions ── */

static void display_watches(Debugger *dbg, void *vm) {
    if (dbg->watch_count == 0) return;
    fprintf(stderr, "Watch expressions:\n");
    for (size_t i = 0; i < dbg->watch_count; i++) {
        char *result = NULL, *error = NULL;
        fprintf(stderr, "  [%d] %s = ", dbg->watches[i].id, dbg->watches[i].expr);
        if (debugger_eval_expr(dbg, vm, dbg->watches[i].expr, &result, &error)) {
            fprintf(stderr, "%s\n", result);
            free(result);
        } else {
            fprintf(stderr, "<error: %s>\n", error);
            free(error);
        }
    }
}

/* ── Helper: check if a breakpoint condition is truthy ── */

static bool check_bp_condition(Debugger *dbg, void *vm, Breakpoint *bp) {
    if (!bp->condition) return true; /* unconditional */
    char *result = NULL, *error = NULL;
    if (!debugger_eval_expr(dbg, vm, bp->condition, &result, &error)) {
        fprintf(stderr, "Warning: breakpoint %d condition error: %s\n", bp->id, error);
        free(error);
        return true; /* break on error to let user fix condition */
    }
    /* Check if result is truthy: not "false", not "nil", not "0", not empty string */
    bool truthy = true;
    if (strcmp(result, "false") == 0 || strcmp(result, "nil") == 0 || strcmp(result, "0") == 0 ||
        strcmp(result, "\"\"") == 0) {
        truthy = false;
    }
    free(result);
    return truthy;
}

/* ── Debug REPL ── */

/* Read a line from stdin for the debug prompt. Returns heap-allocated string or NULL on EOF. */
static char *debug_readline(const char *prompt) {
    fprintf(stderr, "%s", prompt);
    fflush(stderr);
    char *buf = malloc(1024);
    if (!buf) return NULL;
    if (!fgets(buf, 1024, stdin)) {
        free(buf);
        return NULL;
    }
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    return buf;
}

/* Forward declaration for DAP check (not available in WASM builds) */
#ifndef __EMSCRIPTEN__
bool dap_debugger_check(Debugger *dbg, void *vm, void *frame, size_t frame_count, int line, const char *stop_reason);
#endif

bool debugger_check(Debugger *dbg, void *vm_ptr, void *frame_ptr, size_t frame_count) {
    StackVM *vm = (StackVM *)vm_ptr;
    StackCallFrame *frame = (StackCallFrame *)frame_ptr;
    int line = frame_current_line(frame);
    if (line == 0) return true; /* No line info — skip */

    bool line_changed = (line != dbg->last_line);

    /* Determine if we should pause */
    bool should_pause = false;
    const char *pause_reason = "step";
    Breakpoint *hit_bp = NULL;

    if (dbg->step_mode) {
        if (line_changed) should_pause = true;
        pause_reason = "step";
    } else if (dbg->next_mode) {
        if (line_changed && (int)frame_count <= dbg->next_depth) should_pause = true;
        pause_reason = "step";
    } else if (dbg->step_out_mode) {
        if ((int)frame_count < dbg->step_out_depth && line_changed) {
            should_pause = true;
            pause_reason = "step";
        }
    }

    /* Check line breakpoints (only on line change) when running */
    if (!should_pause && dbg->running && line_changed) {
        for (size_t i = 0; i < dbg->bp_count; i++) {
            Breakpoint *bp = &dbg->breakpoints[i];
            if (!bp->enabled) continue;
            if (bp->type == BP_LINE && bp->line == line) {
                if (check_bp_condition(dbg, vm_ptr, bp)) {
                    bp->hit_count++;
                    should_pause = true;
                    hit_bp = bp;
                    pause_reason = "breakpoint";
                    break;
                }
            }
        }
    }

    /* Check function-name breakpoints on function entry */
    if (!should_pause && frame_count > dbg->last_frame_count) {
        const char *fname = frame->chunk ? frame->chunk->name : NULL;
        if (fname && fname[0]) {
            for (size_t i = 0; i < dbg->bp_count; i++) {
                Breakpoint *bp = &dbg->breakpoints[i];
                if (!bp->enabled || bp->type != BP_FUNCTION) continue;
                if (strcmp(bp->func_name, fname) == 0) {
                    if (check_bp_condition(dbg, vm_ptr, bp)) {
                        bp->hit_count++;
                        should_pause = true;
                        hit_bp = bp;
                        pause_reason = "function breakpoint";
                        break;
                    }
                }
            }
        }
    }

    /* Also check line breakpoints in step/next mode (still bump hit_count) */
    if (should_pause && !hit_bp && line_changed) {
        for (size_t i = 0; i < dbg->bp_count; i++) {
            Breakpoint *bp = &dbg->breakpoints[i];
            if (!bp->enabled) continue;
            if (bp->type == BP_LINE && bp->line == line) {
                bp->hit_count++;
                hit_bp = bp;
                pause_reason = "breakpoint";
                break;
            }
        }
    }

    dbg->last_line = line;
    dbg->last_frame_count = frame_count;

    if (!should_pause) return true;

    dbg->step_mode = false;
    dbg->next_mode = false;
    dbg->step_out_mode = false;
    dbg->running = false;

    /* DAP mode: delegate to DAP handler (not available in WASM builds) */
#ifndef __EMSCRIPTEN__
    if (dbg->mode == DBG_MODE_DAP) {
        return dap_debugger_check(dbg, vm_ptr, frame_ptr, frame_count, line, pause_reason);
    }
#else
    (void)pause_reason;
#endif

    /* Show current location */
    const char *fname = frame->chunk->name;
    fprintf(stderr, "\n");
    if (hit_bp) {
        if (hit_bp->type == BP_FUNCTION)
            fprintf(stderr, "Breakpoint %d at %s(), line %d", hit_bp->id, hit_bp->func_name, line);
        else fprintf(stderr, "Breakpoint %d at line %d", hit_bp->id, line);
    } else {
        fprintf(stderr, "Stopped at line %d", line);
    }
    if (fname && fname[0]) fprintf(stderr, " in %s()", fname);
    fprintf(stderr, "\n");

    /* Show the current source line */
    print_source_context(dbg, line, 0);
    fprintf(stderr, "\n");

    /* Display watches */
    display_watches(dbg, vm_ptr);

    /* Enter debug REPL */
    for (;;) {
        char *input = debug_readline("(dbg) ");
        if (!input) {
            fprintf(stderr, "Exiting debugger.\n");
            return false;
        }

        /* Skip leading whitespace */
        char *cmd = input;
        while (*cmd == ' ' || *cmd == '\t') cmd++;

        if (*cmd == '\0') {
            free(input);
            continue;
        }

        if (strcmp(cmd, "s") == 0 || strcmp(cmd, "step") == 0) {
            dbg->step_mode = true;
            free(input);
            return true;
        } else if (strcmp(cmd, "n") == 0 || strcmp(cmd, "next") == 0) {
            dbg->next_mode = true;
            dbg->next_depth = (int)frame_count;
            free(input);
            return true;
        } else if (strcmp(cmd, "o") == 0 || strcmp(cmd, "out") == 0 || strcmp(cmd, "finish") == 0) {
            dbg->step_out_mode = true;
            dbg->step_out_depth = (int)frame_count;
            free(input);
            return true;
        } else if (strcmp(cmd, "c") == 0 || strcmp(cmd, "continue") == 0) {
            dbg->running = true;
            free(input);
            return true;
        } else if (strcmp(cmd, "l") == 0 || strcmp(cmd, "locals") == 0) {
            print_locals(vm, frame);
        } else if (strcmp(cmd, "bt") == 0 || strcmp(cmd, "stack") == 0 || strcmp(cmd, "backtrace") == 0) {
            print_backtrace(vm, frame_count);
        } else if (strcmp(cmd, "list") == 0) {
            print_source_context(dbg, line, 5);
        } else if (strncmp(cmd, "b ", 2) == 0 || strncmp(cmd, "break ", 6) == 0) {
            const char *arg = (cmd[0] == 'b' && cmd[1] == ' ') ? cmd + 2 : cmd + 6;
            while (*arg == ' ') arg++;

            /* Check for "break <line> if <expr>" or "break <funcname>" */
            const char *if_pos = strstr(arg, " if ");
            const char *condition = if_pos ? if_pos + 4 : NULL;

            /* Extract the target (line or function name) */
            char target[256];
            if (if_pos) {
                size_t tlen = (size_t)(if_pos - arg);
                if (tlen >= sizeof(target)) tlen = sizeof(target) - 1;
                memcpy(target, arg, tlen);
                target[tlen] = '\0';
            } else {
                snprintf(target, sizeof(target), "%s", arg);
            }

            /* Determine if target is a number or function name */
            bool is_number = true;
            for (const char *p = target; *p; p++) {
                if (!isdigit((unsigned char)*p)) {
                    is_number = false;
                    break;
                }
            }

            if (is_number && target[0]) {
                int bp_line = atoi(target);
                if (bp_line > 0) {
                    int id = debugger_add_breakpoint_line(dbg, bp_line, condition);
                    fprintf(stderr, "Breakpoint %d set at line %d", id, bp_line);
                    if (condition) fprintf(stderr, " if %s", condition);
                    fprintf(stderr, "\n");
                } else {
                    fprintf(stderr, "Invalid line number: %s\n", target);
                }
            } else if (target[0]) {
                int id = debugger_add_breakpoint_func(dbg, target, condition);
                fprintf(stderr, "Breakpoint %d set on function %s()", id, target);
                if (condition) fprintf(stderr, " if %s", condition);
                fprintf(stderr, "\n");
            } else {
                fprintf(stderr, "Usage: b <line|func> [if <condition>]\n");
            }
        } else if (strncmp(cmd, "d ", 2) == 0 || strncmp(cmd, "delete ", 7) == 0) {
            const char *arg = (cmd[0] == 'd' && cmd[1] == ' ') ? cmd + 2 : cmd + 7;
            int id = atoi(arg);
            if (id > 0) {
                if (debugger_remove_breakpoint_by_id(dbg, id)) fprintf(stderr, "Breakpoint %d removed\n", id);
                else fprintf(stderr, "No breakpoint with ID %d\n", id);
            } else {
                fprintf(stderr, "Usage: d <breakpoint-id> / delete <breakpoint-id>\n");
            }
        } else if (strncmp(cmd, "p ", 2) == 0 || strncmp(cmd, "print ", 6) == 0) {
            const char *arg = (cmd[0] == 'p' && cmd[1] == ' ') ? cmd + 2 : cmd + 6;
            while (*arg == ' ' || *arg == '\t') arg++;
            if (*arg) print_variable(vm, frame, arg);
            else fprintf(stderr, "Usage: p <variable>\n");
        } else if (strncmp(cmd, "eval ", 5) == 0 || strncmp(cmd, "e ", 2) == 0) {
            const char *expr = (cmd[0] == 'e' && cmd[1] == ' ') ? cmd + 2 : cmd + 5;
            while (*expr == ' ') expr++;
            if (*expr) {
                char *result = NULL, *error = NULL;
                if (debugger_eval_expr(dbg, vm_ptr, expr, &result, &error)) {
                    fprintf(stderr, "=> %s\n", result);
                    free(result);
                } else {
                    fprintf(stderr, "Error: %s\n", error);
                    free(error);
                }
            } else {
                fprintf(stderr, "Usage: eval <expression>\n");
            }
        } else if (strncmp(cmd, "watch ", 6) == 0 || strncmp(cmd, "w ", 2) == 0) {
            const char *expr;
            if (cmd[0] == 'w' && cmd[1] == ' ') expr = cmd + 2;
            else expr = cmd + 6;
            while (*expr == ' ') expr++;
            if (*expr) {
                int id = debugger_add_watch(dbg, expr);
                fprintf(stderr, "Watch %d: %s\n", id, expr);
            } else {
                fprintf(stderr, "Usage: watch <expression>\n");
            }
        } else if (strncmp(cmd, "unwatch ", 8) == 0) {
            int id = atoi(cmd + 8);
            if (id > 0) {
                if (debugger_remove_watch(dbg, id)) fprintf(stderr, "Watch %d removed\n", id);
                else fprintf(stderr, "No watch with ID %d\n", id);
            } else {
                fprintf(stderr, "Usage: unwatch <watch-id>\n");
            }
        } else if (strncmp(cmd, "info ", 5) == 0 || strncmp(cmd, "i ", 2) == 0) {
            const char *what = (cmd[0] == 'i' && cmd[1] == ' ') ? cmd + 2 : cmd + 5;
            while (*what == ' ') what++;
            if (strcmp(what, "breakpoints") == 0 || strcmp(what, "b") == 0 || strcmp(what, "break") == 0) {
                if (dbg->bp_count == 0) {
                    fprintf(stderr, "No breakpoints set.\n");
                } else {
                    fprintf(stderr, "Num  Type      Enb  What\n");
                    for (size_t i = 0; i < dbg->bp_count; i++) {
                        Breakpoint *bp = &dbg->breakpoints[i];
                        fprintf(stderr, "%-4d %-9s %-4s ", bp->id, bp->type == BP_LINE ? "line" : "function",
                                bp->enabled ? "y" : "n");
                        if (bp->type == BP_LINE) fprintf(stderr, "line %d", bp->line);
                        else fprintf(stderr, "%s()", bp->func_name);
                        if (bp->condition) fprintf(stderr, "  if %s", bp->condition);
                        fprintf(stderr, "  (hits: %d)\n", bp->hit_count);
                    }
                }
            } else if (strcmp(what, "watches") == 0 || strcmp(what, "w") == 0 || strcmp(what, "watch") == 0) {
                if (dbg->watch_count == 0) {
                    fprintf(stderr, "No watch expressions set.\n");
                } else {
                    for (size_t i = 0; i < dbg->watch_count; i++) {
                        char *result = NULL, *error = NULL;
                        fprintf(stderr, "  [%d] %s = ", dbg->watches[i].id, dbg->watches[i].expr);
                        if (debugger_eval_expr(dbg, vm_ptr, dbg->watches[i].expr, &result, &error)) {
                            fprintf(stderr, "%s\n", result);
                            free(result);
                        } else {
                            fprintf(stderr, "<error: %s>\n", error);
                            free(error);
                        }
                    }
                }
            } else {
                fprintf(stderr, "info breakpoints / info watches\n");
            }
        } else if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0) {
            fprintf(stderr, "Exiting debugger.\n");
            free(input);
            return false;
        } else if (strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0) {
            fprintf(stderr, "Debugger commands:\n");
            fprintf(stderr, "  s, step           Step into (execute one line, enter calls)\n");
            fprintf(stderr, "  n, next           Step over (execute one line, skip calls)\n");
            fprintf(stderr, "  o, out, finish    Step out (run until current function returns)\n");
            fprintf(stderr, "  c, continue       Continue until next breakpoint\n");
            fprintf(stderr, "  l, locals         Show local variables\n");
            fprintf(stderr, "  bt, stack         Show call stack / backtrace\n");
            fprintf(stderr, "  list              Show source context around current line\n");
            fprintf(stderr, "  b <line|func>     Set breakpoint at line or function\n");
            fprintf(stderr, "  b <N> if <expr>   Set conditional breakpoint\n");
            fprintf(stderr, "  d <id>            Delete breakpoint by ID\n");
            fprintf(stderr, "  p <var>           Print variable value\n");
            fprintf(stderr, "  eval <expr>       Evaluate expression\n");
            fprintf(stderr, "  watch <expr>      Add watch expression\n");
            fprintf(stderr, "  unwatch <id>      Remove watch expression\n");
            fprintf(stderr, "  info b            List breakpoints\n");
            fprintf(stderr, "  info w            List watch expressions\n");
            fprintf(stderr, "  q, quit           Exit debugger\n");
            fprintf(stderr, "  h, help           Show this help\n");
        } else {
            fprintf(stderr, "Unknown command: '%s'. Type 'help' for available commands.\n", cmd);
        }

        free(input);
    }
}
