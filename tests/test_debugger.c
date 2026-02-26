#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "debugger.h"

/* Import test macros from test_main.c */
extern void register_test(const char *name, void (*fn)(void));
extern int test_current_failed;

#define ASSERT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            test_current_failed = 1;                                           \
            return;                                                            \
        }                                                                      \
    } while (0)

#define ASSERT_EQ_INT(a, b)                                                               \
    do {                                                                                  \
        long long _a = (long long)(a), _b = (long long)(b);                               \
        if (_a != _b) {                                                                   \
            fprintf(stderr, "  FAIL: %s:%d: %lld != %lld\n", __FILE__, __LINE__, _a, _b); \
            test_current_failed = 1;                                                      \
            return;                                                                       \
        }                                                                                 \
    } while (0)

#define TEST(name)                                                    \
    static void name(void);                                           \
    static void name##_register(void) __attribute__((constructor));   \
    static void name##_register(void) { register_test(#name, name); } \
    static void name(void)

/* ── Debugger lifecycle ── */

TEST(test_dbg_new_free) {
    Debugger *dbg = debugger_new();
    ASSERT(dbg != NULL);
    ASSERT(dbg->step_mode == true);
    ASSERT(dbg->running == false);
    ASSERT(dbg->next_mode == false);
    ASSERT_EQ_INT(dbg->bp_count, 0);
    ASSERT_EQ_INT(dbg->last_line, -1);
    debugger_free(dbg);
}

/* ── Breakpoint management ── */

TEST(test_dbg_add_breakpoint) {
    Debugger *dbg = debugger_new();
    debugger_add_breakpoint(dbg, 10);
    ASSERT_EQ_INT(dbg->bp_count, 1);
    ASSERT(debugger_has_breakpoint(dbg, 10));
    ASSERT(!debugger_has_breakpoint(dbg, 11));
    debugger_free(dbg);
}

TEST(test_dbg_add_duplicate_breakpoint) {
    Debugger *dbg = debugger_new();
    debugger_add_breakpoint(dbg, 10);
    debugger_add_breakpoint(dbg, 10); /* duplicate */
    ASSERT_EQ_INT(dbg->bp_count, 1);  /* should not add twice */
    debugger_free(dbg);
}

TEST(test_dbg_add_multiple_breakpoints) {
    Debugger *dbg = debugger_new();
    debugger_add_breakpoint(dbg, 5);
    debugger_add_breakpoint(dbg, 10);
    debugger_add_breakpoint(dbg, 15);
    ASSERT_EQ_INT(dbg->bp_count, 3);
    ASSERT(debugger_has_breakpoint(dbg, 5));
    ASSERT(debugger_has_breakpoint(dbg, 10));
    ASSERT(debugger_has_breakpoint(dbg, 15));
    ASSERT(!debugger_has_breakpoint(dbg, 7));
    debugger_free(dbg);
}

TEST(test_dbg_remove_breakpoint) {
    Debugger *dbg = debugger_new();
    debugger_add_breakpoint(dbg, 10);
    debugger_add_breakpoint(dbg, 20);
    ASSERT_EQ_INT(dbg->bp_count, 2);

    debugger_remove_breakpoint(dbg, 10);
    ASSERT_EQ_INT(dbg->bp_count, 1);
    ASSERT(!debugger_has_breakpoint(dbg, 10));
    ASSERT(debugger_has_breakpoint(dbg, 20));

    debugger_free(dbg);
}

TEST(test_dbg_remove_nonexistent) {
    Debugger *dbg = debugger_new();
    debugger_add_breakpoint(dbg, 10);
    debugger_remove_breakpoint(dbg, 99); /* not present */
    ASSERT_EQ_INT(dbg->bp_count, 1);     /* unchanged */
    debugger_free(dbg);
}

TEST(test_dbg_breakpoint_grow) {
    /* Test that the breakpoints array grows beyond initial capacity */
    Debugger *dbg = debugger_new();
    for (int i = 1; i <= 20; i++) { debugger_add_breakpoint(dbg, i); }
    ASSERT_EQ_INT(dbg->bp_count, 20);
    for (int i = 1; i <= 20; i++) { ASSERT(debugger_has_breakpoint(dbg, i)); }
    debugger_free(dbg);
}

/* ── Source loading ── */

TEST(test_dbg_load_source_nonexistent) {
    Debugger *dbg = debugger_new();
    ASSERT(!debugger_load_source(dbg, "/tmp/nonexistent_debugger_test_file.lat"));
    ASSERT_EQ_INT(dbg->source_line_count, 0);
    debugger_free(dbg);
}

TEST(test_dbg_load_source_real) {
    /* Write a temporary file and load it */
    const char *tmppath = "/tmp/debugger_test_source.lat";
    FILE *f = fopen(tmppath, "w");
    ASSERT(f != NULL);
    fprintf(f, "let x = 1\n");
    fprintf(f, "let y = 2\n");
    fprintf(f, "print(x + y)\n");
    fclose(f);

    Debugger *dbg = debugger_new();
    ASSERT(debugger_load_source(dbg, tmppath));
    ASSERT_EQ_INT(dbg->source_line_count, 3);
    ASSERT(strcmp(dbg->source_lines[0], "let x = 1") == 0);
    ASSERT(strcmp(dbg->source_lines[1], "let y = 2") == 0);
    ASSERT(strcmp(dbg->source_lines[2], "print(x + y)") == 0);

    debugger_free(dbg);
    remove(tmppath);
}

/* ── Mode toggling ── */

TEST(test_dbg_mode_defaults) {
    Debugger *dbg = debugger_new();
    /* Default: step mode on, others off */
    ASSERT(dbg->step_mode == true);
    ASSERT(dbg->next_mode == false);
    ASSERT(dbg->running == false);
    debugger_free(dbg);
}
