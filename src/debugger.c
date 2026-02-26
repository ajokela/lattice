#include "debugger.h"
#include "stackvm.h"
#include "chunk.h"
#include "value.h"
#include "env.h"
#include "stackopcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Lifecycle ── */

Debugger *debugger_new(void) {
    Debugger *dbg = calloc(1, sizeof(Debugger));
    if (!dbg) return NULL;
    dbg->bp_cap = 8;
    dbg->breakpoints_line = malloc(dbg->bp_cap * sizeof(int));
    if (!dbg->breakpoints_line) {
        free(dbg);
        return NULL;
    }
    dbg->bp_count = 0;
    dbg->step_mode = true; /* Start in step mode (break at first instruction) */
    dbg->next_mode = false;
    dbg->next_depth = 0;
    dbg->running = false;
    dbg->last_line = -1;
    dbg->source_lines = NULL;
    dbg->source_line_count = 0;
    dbg->source_path = NULL;
    return dbg;
}

void debugger_free(Debugger *dbg) {
    if (!dbg) return;
    free(dbg->breakpoints_line);
    if (dbg->source_lines) {
        for (size_t i = 0; i < dbg->source_line_count; i++) free(dbg->source_lines[i]);
        free(dbg->source_lines);
    }
    free(dbg->source_path);
    free(dbg);
}

/* ── Breakpoints ── */

void debugger_add_breakpoint(Debugger *dbg, int line) {
    /* Check if already set */
    for (size_t i = 0; i < dbg->bp_count; i++) {
        if (dbg->breakpoints_line[i] == line) return;
    }
    if (dbg->bp_count >= dbg->bp_cap) {
        dbg->bp_cap *= 2;
        dbg->breakpoints_line = realloc(dbg->breakpoints_line, dbg->bp_cap * sizeof(int));
        if (!dbg->breakpoints_line) return;
    }
    dbg->breakpoints_line[dbg->bp_count++] = line;
}

void debugger_remove_breakpoint(Debugger *dbg, int line) {
    for (size_t i = 0; i < dbg->bp_count; i++) {
        if (dbg->breakpoints_line[i] == line) {
            dbg->breakpoints_line[i] = dbg->breakpoints_line[--dbg->bp_count];
            return;
        }
    }
}

bool debugger_has_breakpoint(Debugger *dbg, int line) {
    for (size_t i = 0; i < dbg->bp_count; i++) {
        if (dbg->breakpoints_line[i] == line) return true;
    }
    return false;
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

bool debugger_check(Debugger *dbg, void *vm_ptr, void *frame_ptr, size_t frame_count) {
    StackVM *vm = (StackVM *)vm_ptr;
    StackCallFrame *frame = (StackCallFrame *)frame_ptr;
    int line = frame_current_line(frame);
    if (line == 0) return true; /* No line info — skip */

    bool line_changed = (line != dbg->last_line);

    /* Determine if we should pause */
    bool should_pause = false;

    if (dbg->step_mode) {
        /* Pause on every new line */
        if (line_changed) should_pause = true;
    } else if (dbg->next_mode) {
        /* Pause when line changes AND we're at the same or shallower call depth */
        if (line_changed && (int)frame_count <= dbg->next_depth) should_pause = true;
    } else if (dbg->running) {
        /* Only pause on breakpoints when line changes */
        if (debugger_has_breakpoint(dbg, line) && line_changed) should_pause = true;
    }

    /* Always track the current line so that revisiting a breakpoint line
     * (e.g., in a loop) will be detected as a line change. */
    dbg->last_line = line;

    if (!should_pause) return true;

    dbg->step_mode = false;
    dbg->next_mode = false;
    dbg->running = false;

    /* Show current location */
    const char *fname = frame->chunk->name;
    fprintf(stderr, "\n");
    if (debugger_has_breakpoint(dbg, line)) fprintf(stderr, "Breakpoint at line %d", line);
    else fprintf(stderr, "Stopped at line %d", line);
    if (fname && fname[0]) fprintf(stderr, " in %s()", fname);
    fprintf(stderr, "\n");

    /* Show the current source line */
    print_source_context(dbg, line, 0);
    fprintf(stderr, "\n");

    /* Enter debug REPL */
    for (;;) {
        char *input = debug_readline("(dbg) ");
        if (!input) {
            /* EOF — treat as quit */
            fprintf(stderr, "Exiting debugger.\n");
            return false;
        }

        /* Skip leading whitespace */
        char *cmd = input;
        while (*cmd == ' ' || *cmd == '\t') cmd++;

        /* Empty line: repeat nothing (just prompt again) */
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
        } else if (strcmp(cmd, "c") == 0 || strcmp(cmd, "continue") == 0) {
            dbg->running = true;
            /* Keep last_line so we don't re-trigger on the same line.
             * Breakpoint will fire again naturally when we leave and re-enter. */
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
            int bp_line = atoi(arg);
            if (bp_line > 0) {
                debugger_add_breakpoint(dbg, bp_line);
                fprintf(stderr, "Breakpoint set at line %d\n", bp_line);
            } else {
                fprintf(stderr, "Usage: b <line> / break <line>\n");
            }
        } else if (strncmp(cmd, "d ", 2) == 0 || strncmp(cmd, "delete ", 7) == 0) {
            const char *arg = (cmd[0] == 'd' && cmd[1] == ' ') ? cmd + 2 : cmd + 7;
            int bp_line = atoi(arg);
            if (bp_line > 0) {
                debugger_remove_breakpoint(dbg, bp_line);
                fprintf(stderr, "Breakpoint removed at line %d\n", bp_line);
            } else {
                fprintf(stderr, "Usage: d <line> / delete <line>\n");
            }
        } else if (strncmp(cmd, "p ", 2) == 0 || strncmp(cmd, "print ", 6) == 0) {
            const char *arg = (cmd[0] == 'p' && cmd[1] == ' ') ? cmd + 2 : cmd + 6;
            /* Skip leading whitespace in arg */
            while (*arg == ' ' || *arg == '\t') arg++;
            if (*arg) print_variable(vm, frame, arg);
            else fprintf(stderr, "Usage: p <variable>\n");
        } else if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0) {
            fprintf(stderr, "Exiting debugger.\n");
            free(input);
            return false;
        } else if (strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0) {
            fprintf(stderr, "Debugger commands:\n");
            fprintf(stderr, "  s, step       Step into (execute one line, enter function calls)\n");
            fprintf(stderr, "  n, next       Step over (execute one line, skip function calls)\n");
            fprintf(stderr, "  c, continue   Continue until next breakpoint\n");
            fprintf(stderr, "  l, locals     Show local variables\n");
            fprintf(stderr, "  bt, stack     Show call stack / backtrace\n");
            fprintf(stderr, "  list          Show source context around current line\n");
            fprintf(stderr, "  b <line>      Set breakpoint at line\n");
            fprintf(stderr, "  d <line>      Delete breakpoint at line\n");
            fprintf(stderr, "  p <var>       Print variable value\n");
            fprintf(stderr, "  q, quit       Exit debugger\n");
            fprintf(stderr, "  h, help       Show this help\n");
        } else {
            fprintf(stderr, "Unknown command: '%s'. Type 'help' for available commands.\n", cmd);
        }

        free(input);
    }
}
