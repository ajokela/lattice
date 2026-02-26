#ifndef DEBUGGER_H
#define DEBUGGER_H

#include <stdbool.h>
#include <stddef.h>

/* Forward declarations -- StackVM types are typedefs, not struct tags,
 * so we just declare the function signatures with void* and cast in the .c file. */

/* ── Debugger state ── */

typedef struct Debugger {
    int *breakpoints_line; /* Line numbers */
    size_t bp_count;
    size_t bp_cap;

    bool step_mode; /* Single-step (step into) */
    bool next_mode; /* Step-over (skip into calls) */
    int next_depth; /* Call depth when 'next' was issued */
    bool running;   /* Continue until breakpoint */

    int last_line; /* Last line we paused on (avoid re-pausing same line) */

    /* Source file for 'list' command */
    char **source_lines; /* Array of source lines (heap-allocated) */
    size_t source_line_count;
    char *source_path; /* Path to the source file */
} Debugger;

/* ── Lifecycle ── */

Debugger *debugger_new(void);
void debugger_free(Debugger *dbg);

/* ── Breakpoints ── */

void debugger_add_breakpoint(Debugger *dbg, int line);
void debugger_remove_breakpoint(Debugger *dbg, int line);
bool debugger_has_breakpoint(Debugger *dbg, int line);

/* ── Source loading ── */

/* Load source file into debugger for 'list' command display. */
bool debugger_load_source(Debugger *dbg, const char *path);

/* ── Debug REPL ── */

/* Called before each instruction. Returns true if execution should continue,
 * false if the user chose to quit.
 * vm and frame are StackVM* and StackCallFrame* respectively (void* to avoid
 * circular includes since StackVM contains a Debugger* pointer). */
bool debugger_check(Debugger *dbg, void *vm, void *frame, size_t frame_count);

#endif /* DEBUGGER_H */
