#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "debugger.h"
#include "dap.h"
#include "lexer.h"
#include "parser.h"
#include "stackcompiler.h"
#include "stackvm.h"
#include "runtime.h"
#include "value.h"
#include "test_backend.h"
#include "../vendor/cJSON.h"

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

#define ASSERT_STR_EQ(a, b)                                                                   \
    do {                                                                                      \
        const char *_a = (a), *_b = (b);                                                      \
        if (strcmp(_a, _b) != 0) {                                                            \
            fprintf(stderr, "  FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a, _b); \
            test_current_failed = 1;                                                          \
            return;                                                                           \
        }                                                                                     \
    } while (0)

#define TEST(name)                                                    \
    static void name(void);                                           \
    static void name##_register(void) __attribute__((constructor));   \
    static void name##_register(void) { register_test(#name, name); } \
    static void name(void)

/* ── Helper: compile source and set up a StackVM for debugger testing ── */

typedef struct {
    StackVM vm;
    LatRuntime rt;
    Chunk *chunk;
    LatVec tokens;
    Program prog;
    bool ok;
} DebugTestVM;

static void dbg_vm_init(DebugTestVM *t, const char *source) {
    memset(t, 0, sizeof(*t));
    t->ok = false;

    value_set_heap(NULL);
    value_set_arena(NULL);

    char *lex_err = NULL;
    Lexer lex = lexer_new(source);
    t->tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        free(lex_err);
        return;
    }

    char *parse_err = NULL;
    Parser parser = parser_new(&t->tokens);
    t->prog = parser_parse(&parser, &parse_err);
    if (parse_err) {
        free(parse_err);
        program_free(&t->prog);
        for (size_t i = 0; i < t->tokens.len; i++) token_free(lat_vec_get(&t->tokens, i));
        lat_vec_free(&t->tokens);
        return;
    }

    char *comp_err = NULL;
    t->chunk = stack_compile(&t->prog, &comp_err);
    if (!t->chunk) {
        free(comp_err);
        program_free(&t->prog);
        for (size_t i = 0; i < t->tokens.len; i++) token_free(lat_vec_get(&t->tokens, i));
        lat_vec_free(&t->tokens);
        return;
    }

    lat_runtime_init(&t->rt);
    stackvm_init(&t->vm, &t->rt);
    t->ok = true;
}

static void dbg_vm_free(DebugTestVM *t) {
    if (!t->ok) return;
    stackvm_free(&t->vm);
    lat_runtime_free(&t->rt);
    chunk_free(t->chunk);
    program_free(&t->prog);
    for (size_t i = 0; i < t->tokens.len; i++) token_free(lat_vec_get(&t->tokens, i));
    lat_vec_free(&t->tokens);
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 1: Data structure unit tests (existing + expanded)
 * ══════════════════════════════════════════════════════════════════════ */

/* ── Debugger lifecycle ── */

TEST(test_dbg_new_free) {
    Debugger *dbg = debugger_new();
    ASSERT(dbg != NULL);
    ASSERT(dbg->step_mode == true);
    ASSERT(dbg->running == false);
    ASSERT(dbg->next_mode == false);
    ASSERT(dbg->step_out_mode == false);
    ASSERT_EQ_INT(dbg->bp_count, 0);
    ASSERT_EQ_INT(dbg->last_line, -1);
    ASSERT_EQ_INT(dbg->next_bp_id, 1);
    ASSERT(dbg->mode == DBG_MODE_CLI);
    ASSERT(dbg->print_callback == NULL);
    ASSERT(dbg->watches == NULL);
    ASSERT_EQ_INT(dbg->watch_count, 0);
    debugger_free(dbg);
}

/* ── Breakpoint management ── */

TEST(test_dbg_add_breakpoint) {
    Debugger *dbg = debugger_new();
    debugger_add_breakpoint(dbg, 10);
    ASSERT_EQ_INT(dbg->bp_count, 1);
    ASSERT(debugger_has_breakpoint(dbg, 10));
    ASSERT(!debugger_has_breakpoint(dbg, 11));
    ASSERT(dbg->breakpoints[0].type == BP_LINE);
    ASSERT_EQ_INT(dbg->breakpoints[0].line, 10);
    ASSERT(dbg->breakpoints[0].enabled == true);
    ASSERT(dbg->breakpoints[0].condition == NULL);
    debugger_free(dbg);
}

TEST(test_dbg_add_duplicate_breakpoint) {
    Debugger *dbg = debugger_new();
    debugger_add_breakpoint(dbg, 10);
    debugger_add_breakpoint(dbg, 10);
    ASSERT_EQ_INT(dbg->bp_count, 1);
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
    debugger_remove_breakpoint(dbg, 99);
    ASSERT_EQ_INT(dbg->bp_count, 1);
    debugger_free(dbg);
}

TEST(test_dbg_breakpoint_grow) {
    Debugger *dbg = debugger_new();
    for (int i = 1; i <= 20; i++) { debugger_add_breakpoint(dbg, i); }
    ASSERT_EQ_INT(dbg->bp_count, 20);
    for (int i = 1; i <= 20; i++) { ASSERT(debugger_has_breakpoint(dbg, i)); }
    debugger_free(dbg);
}

/* ── Structured breakpoint API ── */

TEST(test_dbg_breakpoint_line_with_id) {
    Debugger *dbg = debugger_new();
    int id1 = debugger_add_breakpoint_line(dbg, 10, NULL);
    int id2 = debugger_add_breakpoint_line(dbg, 20, "x > 5");
    ASSERT(id1 > 0);
    ASSERT(id2 > 0);
    ASSERT(id1 != id2);
    ASSERT_EQ_INT(dbg->bp_count, 2);
    ASSERT(dbg->breakpoints[0].condition == NULL);
    ASSERT(dbg->breakpoints[1].condition != NULL);
    ASSERT(strcmp(dbg->breakpoints[1].condition, "x > 5") == 0);
    debugger_free(dbg);
}

TEST(test_dbg_breakpoint_func) {
    Debugger *dbg = debugger_new();
    int id = debugger_add_breakpoint_func(dbg, "my_func", NULL);
    ASSERT(id > 0);
    ASSERT_EQ_INT(dbg->bp_count, 1);
    ASSERT(dbg->breakpoints[0].type == BP_FUNCTION);
    ASSERT(strcmp(dbg->breakpoints[0].func_name, "my_func") == 0);
    debugger_free(dbg);
}

TEST(test_dbg_breakpoint_func_duplicate) {
    Debugger *dbg = debugger_new();
    int id1 = debugger_add_breakpoint_func(dbg, "foo", NULL);
    int id2 = debugger_add_breakpoint_func(dbg, "foo", NULL);
    ASSERT(id1 == id2);
    ASSERT_EQ_INT(dbg->bp_count, 1);
    debugger_free(dbg);
}

TEST(test_dbg_remove_by_id) {
    Debugger *dbg = debugger_new();
    int id1 = debugger_add_breakpoint_line(dbg, 10, NULL);
    int id2 = debugger_add_breakpoint_line(dbg, 20, NULL);
    ASSERT_EQ_INT(dbg->bp_count, 2);

    ASSERT(debugger_remove_breakpoint_by_id(dbg, id1));
    ASSERT_EQ_INT(dbg->bp_count, 1);
    ASSERT(!debugger_has_breakpoint(dbg, 10));
    ASSERT(debugger_has_breakpoint(dbg, 20));

    ASSERT(!debugger_remove_breakpoint_by_id(dbg, 999));
    ASSERT_EQ_INT(dbg->bp_count, 1);

    ASSERT(debugger_remove_breakpoint_by_id(dbg, id2));
    ASSERT_EQ_INT(dbg->bp_count, 0);

    debugger_free(dbg);
}

TEST(test_dbg_breakpoint_hit_count) {
    Debugger *dbg = debugger_new();
    debugger_add_breakpoint_line(dbg, 10, NULL);
    ASSERT_EQ_INT(dbg->breakpoints[0].hit_count, 0);
    debugger_free(dbg);
}

/* ── Disabled breakpoints ── */

TEST(test_dbg_disabled_breakpoint_not_found) {
    Debugger *dbg = debugger_new();
    debugger_add_breakpoint_line(dbg, 10, NULL);
    dbg->breakpoints[0].enabled = false;
    /* debugger_has_breakpoint checks enabled flag */
    ASSERT(!debugger_has_breakpoint(dbg, 10));
    debugger_free(dbg);
}

/* ── Mixed breakpoint types ── */

TEST(test_dbg_mixed_bp_types) {
    Debugger *dbg = debugger_new();
    int id1 = debugger_add_breakpoint_line(dbg, 10, NULL);
    int id2 = debugger_add_breakpoint_func(dbg, "foo", NULL);
    int id3 = debugger_add_breakpoint_line(dbg, 20, "x > 0");
    ASSERT_EQ_INT(dbg->bp_count, 3);

    /* Remove by line should only remove line breakpoints */
    debugger_remove_breakpoint(dbg, 10);
    ASSERT_EQ_INT(dbg->bp_count, 2);
    /* Function bp should survive */
    bool found_func = false;
    for (size_t i = 0; i < dbg->bp_count; i++) {
        if (dbg->breakpoints[i].type == BP_FUNCTION) found_func = true;
    }
    ASSERT(found_func);

    /* Remove by id should work for function bp */
    ASSERT(debugger_remove_breakpoint_by_id(dbg, id2));
    ASSERT_EQ_INT(dbg->bp_count, 1);

    /* Remaining is the conditional line bp */
    ASSERT(dbg->breakpoints[0].id == id3);
    ASSERT(dbg->breakpoints[0].condition != NULL);

    /* Suppress unused variable warning */
    (void)id1;
    debugger_free(dbg);
}

/* ── Auto-incrementing IDs ── */

TEST(test_dbg_bp_ids_increment) {
    Debugger *dbg = debugger_new();
    int id1 = debugger_add_breakpoint_line(dbg, 1, NULL);
    int id2 = debugger_add_breakpoint_line(dbg, 2, NULL);
    int id3 = debugger_add_breakpoint_func(dbg, "bar", NULL);
    ASSERT_EQ_INT(id1, 1);
    ASSERT_EQ_INT(id2, 2);
    ASSERT_EQ_INT(id3, 3);

    /* Remove id2 and add another — ID should keep incrementing */
    debugger_remove_breakpoint_by_id(dbg, id2);
    int id4 = debugger_add_breakpoint_line(dbg, 5, NULL);
    ASSERT_EQ_INT(id4, 4);

    debugger_free(dbg);
}

/* ── Breakpoint condition memory management ── */

TEST(test_dbg_bp_condition_freed) {
    Debugger *dbg = debugger_new();
    debugger_add_breakpoint_line(dbg, 10, "a + b > 100");
    debugger_add_breakpoint_func(dbg, "foo", "x == 42");
    ASSERT_EQ_INT(dbg->bp_count, 2);
    /* Verify conditions were copied */
    ASSERT(strcmp(dbg->breakpoints[0].condition, "a + b > 100") == 0);
    ASSERT(strcmp(dbg->breakpoints[1].condition, "x == 42") == 0);
    /* Free should clean up conditions and func_names without leaks */
    debugger_free(dbg);
}

/* ── Watch expressions ── */

TEST(test_dbg_watch_add_remove) {
    Debugger *dbg = debugger_new();
    int id1 = debugger_add_watch(dbg, "x + 1");
    int id2 = debugger_add_watch(dbg, "y * 2");
    ASSERT(id1 > 0);
    ASSERT(id2 > 0);
    ASSERT(id1 != id2);
    ASSERT_EQ_INT(dbg->watch_count, 2);
    ASSERT(strcmp(dbg->watches[0].expr, "x + 1") == 0);
    ASSERT(strcmp(dbg->watches[1].expr, "y * 2") == 0);

    ASSERT(debugger_remove_watch(dbg, id1));
    ASSERT_EQ_INT(dbg->watch_count, 1);

    ASSERT(!debugger_remove_watch(dbg, 999));
    ASSERT_EQ_INT(dbg->watch_count, 1);

    debugger_free(dbg);
}

TEST(test_dbg_watch_grow) {
    Debugger *dbg = debugger_new();
    /* Add enough watches to force growth (initial cap is 0, grows to 4, 8, 16...) */
    for (int i = 0; i < 10; i++) {
        char expr[32];
        snprintf(expr, sizeof(expr), "w_%d", i);
        int id = debugger_add_watch(dbg, expr);
        ASSERT(id > 0);
    }
    ASSERT_EQ_INT(dbg->watch_count, 10);
    /* Verify all are present */
    for (int i = 0; i < 10; i++) {
        char expected[32];
        snprintf(expected, sizeof(expected), "w_%d", i);
        ASSERT(strcmp(dbg->watches[i].expr, expected) == 0);
    }
    debugger_free(dbg);
}

TEST(test_dbg_watch_ids_increment) {
    Debugger *dbg = debugger_new();
    int id1 = debugger_add_watch(dbg, "a");
    int id2 = debugger_add_watch(dbg, "b");
    ASSERT_EQ_INT(id1, 1);
    ASSERT_EQ_INT(id2, 2);
    debugger_remove_watch(dbg, id1);
    int id3 = debugger_add_watch(dbg, "c");
    ASSERT_EQ_INT(id3, 3); /* IDs never reuse */
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
    ASSERT(dbg->step_mode == true);
    ASSERT(dbg->next_mode == false);
    ASSERT(dbg->step_out_mode == false);
    ASSERT(dbg->running == false);
    debugger_free(dbg);
}

TEST(test_dbg_step_out_state) {
    Debugger *dbg = debugger_new();
    dbg->step_out_mode = true;
    dbg->step_out_depth = 3;
    ASSERT(dbg->step_out_mode == true);
    ASSERT_EQ_INT(dbg->step_out_depth, 3);
    debugger_free(dbg);
}

/* ── DAP mode constructor ── */

TEST(test_dbg_dap_constructor) {
    Debugger *dbg = debugger_new_dap(stdin, stdout);
    ASSERT(dbg != NULL);
    ASSERT(dbg->mode == DBG_MODE_DAP);
    ASSERT(dbg->dap_in == stdin);
    ASSERT(dbg->dap_out == stdout);
    ASSERT(dbg->step_mode == false);
    ASSERT(dbg->dap_seq == 1);
    ASSERT(dbg->dap_initialized == false);
    ASSERT(dbg->dap_launched == false);
    debugger_free(dbg);
}

/* ══════════════════════════════════════════════════════════════════════
 * SECTION 2: Integration tests (require live VM)
 * These only run on the stack-vm backend.
 * ══════════════════════════════════════════════════════════════════════ */

/* ── Expression evaluation ── */

TEST(test_dbg_eval_simple_arithmetic) {
    if (test_backend != BACKEND_STACK_VM) return;

    DebugTestVM t;
    dbg_vm_init(&t, "fn main() { let x = 10 }");
    ASSERT(t.ok);

    /* Run the program first so globals are set up */
    LatValue result;
    stackvm_run(&t.vm, t.chunk, &result);
    value_free(&result);

    Debugger *dbg = debugger_new();
    char *repr = NULL, *error = NULL;
    ASSERT(debugger_eval_expr(dbg, &t.vm, "1 + 2 * 3", &repr, &error));
    ASSERT(repr != NULL);
    ASSERT_STR_EQ(repr, "7");
    free(repr);

    debugger_free(dbg);
    dbg_vm_free(&t);
}

TEST(test_dbg_eval_string_concat) {
    if (test_backend != BACKEND_STACK_VM) return;

    DebugTestVM t;
    dbg_vm_init(&t, "fn main() { let x = 1 }");
    ASSERT(t.ok);

    LatValue result;
    stackvm_run(&t.vm, t.chunk, &result);
    value_free(&result);

    Debugger *dbg = debugger_new();
    char *repr = NULL, *error = NULL;
    ASSERT(debugger_eval_expr(dbg, &t.vm, "\"hello\" + \" world\"", &repr, &error));
    ASSERT(repr != NULL);
    ASSERT_STR_EQ(repr, "\"hello world\"");
    free(repr);

    debugger_free(dbg);
    dbg_vm_free(&t);
}

TEST(test_dbg_eval_boolean) {
    if (test_backend != BACKEND_STACK_VM) return;

    DebugTestVM t;
    dbg_vm_init(&t, "fn main() { let x = 1 }");
    ASSERT(t.ok);

    LatValue result;
    stackvm_run(&t.vm, t.chunk, &result);
    value_free(&result);

    Debugger *dbg = debugger_new();
    char *repr = NULL, *error = NULL;
    ASSERT(debugger_eval_expr(dbg, &t.vm, "10 > 5", &repr, &error));
    ASSERT(repr != NULL);
    ASSERT_STR_EQ(repr, "true");
    free(repr);

    debugger_free(dbg);
    dbg_vm_free(&t);
}

TEST(test_dbg_eval_syntax_error) {
    if (test_backend != BACKEND_STACK_VM) return;

    DebugTestVM t;
    dbg_vm_init(&t, "fn main() { let x = 1 }");
    ASSERT(t.ok);

    LatValue result;
    stackvm_run(&t.vm, t.chunk, &result);
    value_free(&result);

    Debugger *dbg = debugger_new();
    char *repr = NULL, *error = NULL;
    ASSERT(!debugger_eval_expr(dbg, &t.vm, "1 +", &repr, &error));
    ASSERT(error != NULL);
    free(error);

    debugger_free(dbg);
    dbg_vm_free(&t);
}

TEST(test_dbg_eval_array_literal) {
    if (test_backend != BACKEND_STACK_VM) return;

    DebugTestVM t;
    dbg_vm_init(&t, "fn main() { let x = 1 }");
    ASSERT(t.ok);

    LatValue result;
    stackvm_run(&t.vm, t.chunk, &result);
    value_free(&result);

    Debugger *dbg = debugger_new();
    char *repr = NULL, *error = NULL;
    ASSERT(debugger_eval_expr(dbg, &t.vm, "[1, 2, 3]", &repr, &error));
    ASSERT(repr != NULL);
    ASSERT_STR_EQ(repr, "[1, 2, 3]");
    free(repr);

    debugger_free(dbg);
    dbg_vm_free(&t);
}

/* ── Output redirection callback ── */

static char captured_output[4096];
static size_t captured_len;

static void test_print_callback(const char *text, void *userdata) {
    (void)userdata;
    size_t len = strlen(text);
    if (captured_len + len < sizeof(captured_output)) {
        memcpy(captured_output + captured_len, text, len);
        captured_len += len;
        captured_output[captured_len] = '\0';
    }
}

TEST(test_dbg_print_callback_redirect) {
    if (test_backend != BACKEND_STACK_VM) return;

    DebugTestVM t;
    dbg_vm_init(&t, "fn main() { print(42) }");
    ASSERT(t.ok);

    Debugger *dbg = debugger_new();
    dbg->print_callback = test_print_callback;
    dbg->print_userdata = NULL;
    /* Don't step — just run through */
    dbg->step_mode = false;
    dbg->running = true;
    t.vm.debugger = dbg;

    captured_output[0] = '\0';
    captured_len = 0;

    /* Suppress stdout so test runner doesn't see it */
    fflush(stdout);
    int saved_stdout = dup(STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, STDOUT_FILENO);
    close(devnull);

    LatValue result;
    stackvm_run(&t.vm, t.chunk, &result);
    value_free(&result);

    /* Restore stdout */
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);

    /* Verify the callback received the output */
    ASSERT(captured_len > 0);
    ASSERT_STR_EQ(captured_output, "42\n");

    debugger_free(dbg);
    dbg_vm_free(&t);
}

TEST(test_dbg_print_callback_multi_arg) {
    if (test_backend != BACKEND_STACK_VM) return;

    DebugTestVM t;
    dbg_vm_init(&t, "fn main() { print(\"a\", \"b\", \"c\") }");
    ASSERT(t.ok);

    Debugger *dbg = debugger_new();
    dbg->print_callback = test_print_callback;
    dbg->print_userdata = NULL;
    dbg->step_mode = false;
    dbg->running = true;
    t.vm.debugger = dbg;

    captured_output[0] = '\0';
    captured_len = 0;

    fflush(stdout);
    int saved_stdout = dup(STDOUT_FILENO);
    int devnull = open("/dev/null", 1);
    dup2(devnull, STDOUT_FILENO);
    close(devnull);

    LatValue result;
    stackvm_run(&t.vm, t.chunk, &result);
    value_free(&result);

    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);

    ASSERT_STR_EQ(captured_output, "a b c\n");

    debugger_free(dbg);
    dbg_vm_free(&t);
}

/* ── DAP message framing ── */

TEST(test_dap_message_roundtrip) {
    /* Write a DAP message to a temp file, read it back */
    const char *tmppath = "/tmp/dap_msg_test.bin";
    FILE *out = fopen(tmppath, "w+");
    ASSERT(out != NULL);

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "request");
    cJSON_AddStringToObject(msg, "command", "initialize");
    cJSON_AddNumberToObject(msg, "seq", 1);
    dap_write_message(msg, out);
    cJSON_Delete(msg);

    /* Rewind and read back */
    rewind(out);
    cJSON *read_msg = dap_read_message(out);
    fclose(out);
    remove(tmppath);

    ASSERT(read_msg != NULL);
    cJSON *type = cJSON_GetObjectItem(read_msg, "type");
    ASSERT(type != NULL);
    ASSERT_STR_EQ(type->valuestring, "request");
    cJSON *cmd = cJSON_GetObjectItem(read_msg, "command");
    ASSERT(cmd != NULL);
    ASSERT_STR_EQ(cmd->valuestring, "initialize");
    cJSON *seq = cJSON_GetObjectItem(read_msg, "seq");
    ASSERT(seq != NULL);
    ASSERT_EQ_INT(seq->valueint, 1);
    cJSON_Delete(read_msg);
}

TEST(test_dap_message_multiple) {
    /* Write multiple messages, read them in order */
    const char *tmppath = "/tmp/dap_multi_test.bin";
    FILE *out = fopen(tmppath, "w+");
    ASSERT(out != NULL);

    for (int i = 1; i <= 3; i++) {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddNumberToObject(msg, "seq", i);
        cJSON_AddStringToObject(msg, "type", "request");
        dap_write_message(msg, out);
        cJSON_Delete(msg);
    }

    rewind(out);
    for (int i = 1; i <= 3; i++) {
        cJSON *msg = dap_read_message(out);
        ASSERT(msg != NULL);
        cJSON *seq = cJSON_GetObjectItem(msg, "seq");
        ASSERT_EQ_INT(seq->valueint, i);
        cJSON_Delete(msg);
    }

    /* EOF should return NULL */
    cJSON *eof_msg = dap_read_message(out);
    ASSERT(eof_msg == NULL);

    fclose(out);
    remove(tmppath);
}

/* ── DAP response/event helpers ── */

TEST(test_dap_send_response) {
    const char *tmppath = "/tmp/dap_resp_test.bin";
    FILE *out = fopen(tmppath, "w+");
    ASSERT(out != NULL);

    Debugger *dbg = debugger_new_dap(NULL, out);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "supportsConfigurationDoneRequest", 1);
    dap_send_response(dbg, 1, "initialize", body);

    rewind(out);
    cJSON *msg = dap_read_message(out);
    fclose(out);
    remove(tmppath);

    ASSERT(msg != NULL);
    cJSON *type = cJSON_GetObjectItem(msg, "type");
    ASSERT_STR_EQ(type->valuestring, "response");
    cJSON *success = cJSON_GetObjectItem(msg, "success");
    ASSERT(cJSON_IsTrue(success));
    cJSON *cmd = cJSON_GetObjectItem(msg, "command");
    ASSERT_STR_EQ(cmd->valuestring, "initialize");
    cJSON *req_seq = cJSON_GetObjectItem(msg, "request_seq");
    ASSERT_EQ_INT(req_seq->valueint, 1);
    cJSON *resp_body = cJSON_GetObjectItem(msg, "body");
    ASSERT(resp_body != NULL);
    cJSON *cap = cJSON_GetObjectItem(resp_body, "supportsConfigurationDoneRequest");
    ASSERT(cJSON_IsTrue(cap));
    cJSON_Delete(msg);

    debugger_free(dbg);
}

TEST(test_dap_send_event) {
    const char *tmppath = "/tmp/dap_event_test.bin";
    FILE *out = fopen(tmppath, "w+");
    ASSERT(out != NULL);

    Debugger *dbg = debugger_new_dap(NULL, out);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "reason", "breakpoint");
    cJSON_AddNumberToObject(body, "threadId", 1);
    dap_send_event(dbg, "stopped", body);

    rewind(out);
    cJSON *msg = dap_read_message(out);
    fclose(out);
    remove(tmppath);

    ASSERT(msg != NULL);
    cJSON *type = cJSON_GetObjectItem(msg, "type");
    ASSERT_STR_EQ(type->valuestring, "event");
    cJSON *event = cJSON_GetObjectItem(msg, "event");
    ASSERT_STR_EQ(event->valuestring, "stopped");
    cJSON *evt_body = cJSON_GetObjectItem(msg, "body");
    ASSERT(evt_body != NULL);
    cJSON *reason = cJSON_GetObjectItem(evt_body, "reason");
    ASSERT_STR_EQ(reason->valuestring, "breakpoint");
    cJSON_Delete(msg);

    debugger_free(dbg);
}

TEST(test_dap_send_error) {
    const char *tmppath = "/tmp/dap_err_test.bin";
    FILE *out = fopen(tmppath, "w+");
    ASSERT(out != NULL);

    Debugger *dbg = debugger_new_dap(NULL, out);
    dap_send_error(dbg, 5, "evaluate", "syntax error in expression");

    rewind(out);
    cJSON *msg = dap_read_message(out);
    fclose(out);
    remove(tmppath);

    ASSERT(msg != NULL);
    cJSON *type = cJSON_GetObjectItem(msg, "type");
    ASSERT_STR_EQ(type->valuestring, "response");
    cJSON *success = cJSON_GetObjectItem(msg, "success");
    ASSERT(cJSON_IsFalse(success));
    cJSON *errmsg = cJSON_GetObjectItem(msg, "message");
    ASSERT_STR_EQ(errmsg->valuestring, "syntax error in expression");
    cJSON_Delete(msg);

    debugger_free(dbg);
}

/* ── DAP seq counter increments ── */

TEST(test_dap_seq_counter) {
    const char *tmppath = "/tmp/dap_seq_test.bin";
    FILE *out = fopen(tmppath, "w+");
    ASSERT(out != NULL);

    Debugger *dbg = debugger_new_dap(NULL, out);
    ASSERT_EQ_INT(dbg->dap_seq, 1);
    dap_send_event(dbg, "initialized", NULL);
    ASSERT_EQ_INT(dbg->dap_seq, 2);
    dap_send_response(dbg, 1, "initialize", NULL);
    ASSERT_EQ_INT(dbg->dap_seq, 3);
    dap_send_error(dbg, 2, "foo", "err");
    ASSERT_EQ_INT(dbg->dap_seq, 4);

    fclose(out);
    remove(tmppath);
    debugger_free(dbg);
}

/* ── DAP read returns NULL on empty/invalid input ── */

TEST(test_dap_read_eof) {
    const char *tmppath = "/tmp/dap_eof_test.bin";
    FILE *f = fopen(tmppath, "w+");
    ASSERT(f != NULL);
    /* Empty file */
    rewind(f);
    cJSON *msg = dap_read_message(f);
    ASSERT(msg == NULL);
    fclose(f);
    remove(tmppath);
}

TEST(test_dap_read_bad_content_length) {
    const char *tmppath = "/tmp/dap_bad_cl_test.bin";
    FILE *f = fopen(tmppath, "w+");
    ASSERT(f != NULL);
    /* Content-Length of 100 but only 5 bytes of body */
    fprintf(f, "Content-Length: 100\r\n\r\nhello");
    rewind(f);
    cJSON *msg = dap_read_message(f);
    ASSERT(msg == NULL); /* Short read should fail */
    fclose(f);
    remove(tmppath);
}
