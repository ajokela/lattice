#ifndef DEBUGGER_H
#define DEBUGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Forward declarations -- StackVM types are typedefs, not struct tags,
 * so we just declare the function signatures with void* and cast in the .c file. */

/* ── Breakpoint types ── */

typedef enum { BP_LINE, BP_FUNCTION } BreakpointType;

typedef struct {
    int id; /* Auto-increment ID for DAP */
    BreakpointType type;
    bool enabled;
    int line;        /* For BP_LINE */
    char *func_name; /* For BP_FUNCTION (heap-allocated) */
    char *condition; /* Expression string, NULL = unconditional */
    int hit_count;
} Breakpoint;

/* ── Watch expressions ── */

typedef struct {
    int id;
    char *expr;
} WatchExpr;

/* ── Debugger mode ── */

typedef enum { DBG_MODE_CLI, DBG_MODE_DAP } DebuggerMode;

/* ── Debugger state ── */

typedef struct Debugger {
    Breakpoint *breakpoints; /* Structured breakpoint array */
    size_t bp_count;
    size_t bp_cap;
    int next_bp_id; /* Auto-increment for breakpoint IDs */

    bool step_mode; /* Single-step (step into) */
    bool next_mode; /* Step-over (skip into calls) */
    int next_depth; /* Call depth when 'next' was issued */
    bool running;   /* Continue until breakpoint */

    bool step_out_mode; /* Step-out (run until return to caller) */
    int step_out_depth; /* Call depth when 'out' was issued */

    int last_line;           /* Last line we paused on (avoid re-pausing same line) */
    size_t last_frame_count; /* Last frame count (for function entry detection) */

    /* Watch expressions */
    WatchExpr *watches;
    size_t watch_count;
    size_t watch_cap;
    int next_watch_id;

    /* Source file for 'list' command */
    char **source_lines; /* Array of source lines (heap-allocated) */
    size_t source_line_count;
    char *source_path; /* Path to the source file */

    /* DAP mode fields */
    DebuggerMode mode;
    FILE *dap_in;         /* DAP input stream (stdin in DAP mode) */
    FILE *dap_out;        /* DAP output stream (stdout in DAP mode) */
    int dap_seq;          /* Outgoing sequence counter */
    bool dap_initialized; /* Has received 'initialize' request */
    bool dap_launched;    /* Has received 'launch' request */
    char *stop_reason;    /* Reason for stop (step/breakpoint/entry/pause) */

    /* Output capture callback for DAP mode */
    void (*print_callback)(const char *text, void *userdata);
    void *print_userdata;
} Debugger;

/* ── Lifecycle ── */

Debugger *debugger_new(void);
Debugger *debugger_new_dap(FILE *in, FILE *out);
void debugger_free(Debugger *dbg);

/* ── Breakpoints ── */

/* Legacy wrapper: adds a line breakpoint with no condition. */
void debugger_add_breakpoint(Debugger *dbg, int line);
void debugger_remove_breakpoint(Debugger *dbg, int line);
bool debugger_has_breakpoint(Debugger *dbg, int line);

/* Structured breakpoint API (returns breakpoint ID, or -1 on failure). */
int debugger_add_breakpoint_line(Debugger *dbg, int line, const char *condition);
int debugger_add_breakpoint_func(Debugger *dbg, const char *name, const char *condition);
bool debugger_remove_breakpoint_by_id(Debugger *dbg, int id);

/* ── Watch expressions ── */

int debugger_add_watch(Debugger *dbg, const char *expr);
bool debugger_remove_watch(Debugger *dbg, int id);

/* ── Expression evaluation ── */

/* Evaluate an expression string in the context of the current VM state.
 * On success, returns true and sets *result_repr to a heap-allocated string.
 * On failure, returns false and sets *error to a heap-allocated message. */
bool debugger_eval_expr(Debugger *dbg, void *vm, const char *expr, char **result_repr, char **error);

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
