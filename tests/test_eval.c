#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lattice.h"
#include "lexer.h"
#include "parser.h"
#include "phase_check.h"
#include "eval.h"

/* Import test macros from test_main.c */
extern void register_test(const char *name, void (*fn)(void));
extern int test_current_failed;

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        test_current_failed = 1; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_INT(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL: %s:%d: %lld != %lld\n", __FILE__, __LINE__, _a, _b); \
        test_current_failed = 1; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) != 0) { \
        fprintf(stderr, "  FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a, _b); \
        test_current_failed = 1; \
        return; \
    } \
} while(0)

#define TEST(name) \
    static void name(void); \
    static void name##_register(void) __attribute__((constructor)); \
    static void name##_register(void) { register_test(#name, name); } \
    static void name(void)

/* ── Helper: run a Lattice source string through the full pipeline ── */

/*
 * run_source_ok: lex -> parse -> phase_check (if strict) -> eval
 * Returns 0 on success, non-zero on failure.
 * If err_out is non-NULL, stores a heap-allocated error string on failure.
 */
static bool gc_stress = false; /* toggled by gc-stress tests */

static int run_source_ok(const char *source, char **err_out) {
    if (err_out) *err_out = NULL;

    /* Lex */
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        if (err_out) *err_out = lex_err;
        else free(lex_err);
        lat_vec_free(&tokens);
        return 1;
    }

    /* Parse */
    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    if (parse_err) {
        if (err_out) *err_out = parse_err;
        else free(parse_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 1;
    }

    /* Phase check (strict mode only) */
    if (prog.mode == MODE_STRICT) {
        LatVec errors = phase_check(&prog);
        if (errors.len > 0) {
            if (err_out) {
                char **first = lat_vec_get(&errors, 0);
                *err_out = *first;
                /* Free remaining error strings */
                for (size_t i = 1; i < errors.len; i++) {
                    char **msg = lat_vec_get(&errors, i);
                    free(*msg);
                }
            } else {
                for (size_t i = 0; i < errors.len; i++) {
                    char **msg = lat_vec_get(&errors, i);
                    free(*msg);
                }
            }
            lat_vec_free(&errors);
            program_free(&prog);
            for (size_t i = 0; i < tokens.len; i++)
                token_free(lat_vec_get(&tokens, i));
            lat_vec_free(&tokens);
            return 1;
        }
        lat_vec_free(&errors);
    }

    /* Evaluate */
    Evaluator *ev = evaluator_new();
    if (gc_stress) evaluator_set_gc_stress(ev, true);
    char *eval_err = evaluator_run(ev, &prog);
    int result = 0;
    if (eval_err) {
        if (err_out) *err_out = eval_err;
        else free(eval_err);
        result = 1;
    }

    evaluator_free(ev);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++)
        token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    return result;
}

/* Convenience: assert source runs without error */
#define ASSERT_RUNS(src) do { \
    char *_err = NULL; \
    int _rc = run_source_ok(src, &_err); \
    if (_rc != 0) { \
        fprintf(stderr, "  FAIL: %s:%d: source failed: %s\n", \
                __FILE__, __LINE__, _err ? _err : "(unknown)"); \
        free(_err); \
        test_current_failed = 1; \
        return; \
    } \
} while(0)

/* Convenience: assert source fails with an error */
#define ASSERT_FAILS(src) do { \
    char *_err = NULL; \
    int _rc = run_source_ok(src, &_err); \
    free(_err); \
    if (_rc == 0) { \
        fprintf(stderr, "  FAIL: %s:%d: expected failure but source succeeded\n", \
                __FILE__, __LINE__); \
        test_current_failed = 1; \
        return; \
    } \
} while(0)

/* ── Test: Hello World ── */

TEST(eval_hello_world) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    print(\"Hello, World!\")\n"
        "}\n"
    );
}

/* ── Test: Basic Arithmetic ── */

TEST(eval_basic_arithmetic) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    print(1 + 2)\n"
        "}\n"
    );
}

TEST(eval_arithmetic_compound) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let x = 10 + 20\n"
        "    let y = x * 2\n"
        "    let z = y - 5\n"
        "    print(z)\n"
        "}\n"
    );
}

TEST(eval_arithmetic_division) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let a = 100 / 4\n"
        "    let b = 10 % 3\n"
        "    print(a)\n"
        "    print(b)\n"
        "}\n"
    );
}

/* ── Test: Variable Bindings ── */

TEST(eval_variable_binding) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let x = 10\n"
        "    print(x)\n"
        "}\n"
    );
}

TEST(eval_variable_reassignment) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let x = 10\n"
        "    x = 20\n"
        "    print(x)\n"
        "}\n"
    );
}

/* ── Test: If/Else ── */

TEST(eval_if_else) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let x = 10\n"
        "    if x > 5 {\n"
        "        print(\"big\")\n"
        "    } else {\n"
        "        print(\"small\")\n"
        "    }\n"
        "}\n"
    );
}

TEST(eval_if_no_else) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let x = 3\n"
        "    if x < 10 {\n"
        "        print(x)\n"
        "    }\n"
        "}\n"
    );
}

/* ── Test: While Loop ── */

TEST(eval_while_loop) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let x = 0\n"
        "    while x < 5 {\n"
        "        x = x + 1\n"
        "    }\n"
        "    print(x)\n"
        "}\n"
    );
}

/* ── Test: For Loop with Range ── */

TEST(eval_for_loop_range) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let sum = 0\n"
        "    for i in 0..5 {\n"
        "        sum = sum + i\n"
        "    }\n"
        "    print(sum)\n"
        "}\n"
    );
}

/* ── Test: Function Definition and Calling ── */

TEST(eval_function_call) {
    ASSERT_RUNS(
        "fn add(a: Int, b: Int) -> Int {\n"
        "    return a + b\n"
        "}\n"
        "\n"
        "fn main() {\n"
        "    let result = add(10, 20)\n"
        "    print(result)\n"
        "}\n"
    );
}

TEST(eval_recursive_function) {
    ASSERT_RUNS(
        "fn factorial(n: Int) -> Int {\n"
        "    if n <= 1 {\n"
        "        return 1\n"
        "    }\n"
        "    return n * factorial(n - 1)\n"
        "}\n"
        "\n"
        "fn main() {\n"
        "    print(factorial(5))\n"
        "}\n"
    );
}

/* ── Test: Arrays ── */

TEST(eval_array_creation) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let xs = [1, 2, 3]\n"
        "    print(xs)\n"
        "}\n"
    );
}

TEST(eval_array_indexing) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let xs = [10, 20, 30]\n"
        "    print(xs[0])\n"
        "    print(xs[1])\n"
        "    print(xs[2])\n"
        "}\n"
    );
}

TEST(eval_array_push_and_len) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let xs = [1, 2, 3]\n"
        "    xs.push(4)\n"
        "    print(xs.len())\n"
        "    print(xs[0])\n"
        "    print(xs[3])\n"
        "}\n"
    );
}

TEST(eval_array_join) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let words = [\"Hello\", \"World\", \"from\", \"Lattice\"]\n"
        "    let sentence = words.join(\" \")\n"
        "    print(sentence)\n"
        "}\n"
    );
}

/* ── Test: Structs ── */

TEST(eval_struct_creation_and_access) {
    ASSERT_RUNS(
        "struct Point { x: Float, y: Float }\n"
        "\n"
        "fn main() {\n"
        "    let p = Point { x: 3.0, y: 4.0 }\n"
        "    print(p.x)\n"
        "    print(p.y)\n"
        "}\n"
    );
}

TEST(eval_nested_structs) {
    ASSERT_RUNS(
        "struct Point { x: Float, y: Float }\n"
        "struct Line { start: Point, end: Point }\n"
        "\n"
        "fn main() {\n"
        "    let line = Line {\n"
        "        start: Point { x: 0.0, y: 0.0 },\n"
        "        end: Point { x: 1.0, y: 1.0 },\n"
        "    }\n"
        "    print(line.start.x)\n"
        "    print(line.end.y)\n"
        "}\n"
    );
}

/* ── Test: String Operations ── */

TEST(eval_string_concat) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let greeting = \"Hello\" + \", \" + \"World!\"\n"
        "    print(greeting)\n"
        "}\n"
    );
}

TEST(eval_string_len) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let greeting = \"Hello\" + \", \" + \"World!\"\n"
        "    print(greeting)\n"
        "    print(greeting.len())\n"
        "}\n"
    );
}

/* ── Test: Boolean Logic ── */

TEST(eval_boolean_logic) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let a = true\n"
        "    let b = false\n"
        "    print(a && b)\n"
        "    print(a || b)\n"
        "    print(!a)\n"
        "    print(10 == 10)\n"
        "    print(10 != 20)\n"
        "}\n"
    );
}

/* ── Test: Freeze / Thaw / Clone ── */

TEST(eval_freeze_and_thaw) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let x = 42\n"
        "    let frozen = freeze(x)\n"
        "    let thawed = thaw(frozen)\n"
        "    print(frozen)\n"
        "    print(thawed)\n"
        "}\n"
    );
}

TEST(eval_clone) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let xs = [1, 2, 3]\n"
        "    let ys = clone(xs)\n"
        "    print(ys)\n"
        "}\n"
    );
}

/* ── Test: Forge Block ── */

TEST(eval_forge_block) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let result = forge {\n"
        "        let x = 10\n"
        "        let y = 20\n"
        "        x + y\n"
        "    }\n"
        "    print(result)\n"
        "}\n"
    );
}

/* ── Test: Closures / Map ── */

TEST(eval_closure_map) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let xs = [1, 2, 3, 4, 5]\n"
        "    let doubled = xs.map(|x| x * 2)\n"
        "    print(doubled)\n"
        "}\n"
    );
}

/* ── Test: Strict Mode Full Workflow ── */

TEST(eval_strict_mode_workflow) {
    ASSERT_RUNS(
        "#mode strict\n"
        "struct Config { value: Int, name: String }\n"
        "\n"
        "fn main() {\n"
        "    flux cfg = Config { value: 42, name: \"test\" }\n"
        "    cfg.value = 100\n"
        "    fix frozen = freeze(cfg)\n"
        "    print(frozen.value)\n"
        "    print(frozen.name)\n"
        "\n"
        "    flux copy = thaw(frozen)\n"
        "    copy.name = \"modified\"\n"
        "    fix result = freeze(copy)\n"
        "    print(result.name)\n"
        "}\n"
    );
}

/* ── Test: Memory Stats After Evaluation ── */

TEST(eval_memory_stats_populated) {
    const char *source =
        "fn main() {\n"
        "    let x = 42\n"
        "    let frozen = freeze(x)\n"
        "    let thawed = thaw(frozen)\n"
        "    print(thawed)\n"
        "}\n";

    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    ASSERT(lex_err == NULL);

    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    ASSERT(parse_err == NULL);

    Evaluator *ev = evaluator_new();
    char *eval_err = evaluator_run(ev, &prog);
    ASSERT(eval_err == NULL);

    const MemoryStats *stats = evaluator_stats(ev);
    ASSERT(stats != NULL);
    /* freeze(x) should register at least 1 freeze */
    ASSERT(stats->freezes >= 1);
    /* thaw(frozen) should register at least 1 thaw */
    ASSERT(stats->thaws >= 1);
    /* At least some bindings were created (x, frozen, thawed) */
    ASSERT(stats->bindings_created >= 3);
    /* At least one fn call (main) */
    ASSERT(stats->fn_calls >= 1);

    evaluator_free(ev);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++)
        token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
}

/* ── Test: Error on Undefined Variable ── */

TEST(eval_undefined_variable_error) {
    ASSERT_FAILS(
        "fn main() {\n"
        "    print(undefined_var)\n"
        "}\n"
    );
}

/* ── Test: Empty Main Function ── */

TEST(eval_empty_main) {
    ASSERT_RUNS(
        "fn main() {\n"
        "}\n"
    );
}

/* ── Test: Multiple Functions ── */

TEST(eval_multiple_functions) {
    ASSERT_RUNS(
        "fn double(x: Int) -> Int {\n"
        "    return x * 2\n"
        "}\n"
        "\n"
        "fn triple(x: Int) -> Int {\n"
        "    return x * 3\n"
        "}\n"
        "\n"
        "fn main() {\n"
        "    print(double(5))\n"
        "    print(triple(5))\n"
        "}\n"
    );
}

/* ── Test: Nested If ── */

TEST(eval_nested_if) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let x = 15\n"
        "    if x > 10 {\n"
        "        if x > 20 {\n"
        "            print(\"very big\")\n"
        "        } else {\n"
        "            print(\"medium\")\n"
        "        }\n"
        "    } else {\n"
        "        print(\"small\")\n"
        "    }\n"
        "}\n"
    );
}

/* ── Test: While Loop with Break ── */

TEST(eval_while_break) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let x = 0\n"
        "    while true {\n"
        "        if x >= 5 {\n"
        "            break\n"
        "        }\n"
        "        x = x + 1\n"
        "    }\n"
        "    print(x)\n"
        "}\n"
    );
}

/* ── Test: For Loop with Continue ── */

TEST(eval_for_continue) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let sum = 0\n"
        "    for i in 0..10 {\n"
        "        if i % 2 == 0 {\n"
        "            continue\n"
        "        }\n"
        "        sum = sum + i\n"
        "    }\n"
        "    print(sum)\n"
        "}\n"
    );
}

/* ── Test: Float Arithmetic ── */

TEST(eval_float_arithmetic) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    let a = 3.14\n"
        "    let b = 2.0\n"
        "    let c = a * b\n"
        "    print(c)\n"
        "}\n"
    );
}

/* ── Test: Comparison Operators ── */

TEST(eval_comparison_operators) {
    ASSERT_RUNS(
        "fn main() {\n"
        "    print(1 < 2)\n"
        "    print(2 > 1)\n"
        "    print(3 <= 3)\n"
        "    print(3 >= 3)\n"
        "    print(4 == 4)\n"
        "    print(4 != 5)\n"
        "}\n"
    );
}

/* ── GC Stress Tests ── */

TEST(eval_gc_stress_hello) {
    gc_stress = true;
    ASSERT_RUNS(
        "fn main() {\n"
        "    let msg = \"Hello\"\n"
        "    let nums = [1, 2, 3, 4, 5]\n"
        "    let p = Point { x: 3, y: 4 }\n"
        "    print(msg)\n"
        "    print(nums)\n"
        "}\n"
        "struct Point { x: Int, y: Int }\n"
    );
    gc_stress = false;
}

TEST(eval_gc_stress_loops) {
    gc_stress = true;
    ASSERT_RUNS(
        "fn main() {\n"
        "    let sum = 0\n"
        "    for i in 0..10 {\n"
        "        sum = sum + i\n"
        "    }\n"
        "    print(sum)\n"
        "}\n"
    );
    gc_stress = false;
}

TEST(eval_gc_stress_closures) {
    gc_stress = true;
    ASSERT_RUNS(
        "fn main() {\n"
        "    let xs = [1, 2, 3, 4, 5]\n"
        "    let doubled = xs.map(|x| x * 2)\n"
        "    print(doubled)\n"
        "}\n"
    );
    gc_stress = false;
}

TEST(eval_gc_stress_freeze_thaw) {
    gc_stress = true;
    ASSERT_RUNS(
        "#mode strict\n"
        "fn main() {\n"
        "    flux x = 42\n"
        "    fix frozen = freeze(x)\n"
        "    flux thawed = thaw(frozen)\n"
        "    thawed = thawed + 1\n"
        "    print(thawed)\n"
        "}\n"
    );
    gc_stress = false;
}

TEST(eval_gc_stress_game_loop) {
    gc_stress = true;
    ASSERT_RUNS(
        "#mode strict\n"
        "struct Entity { x: Float, y: Float, name: String }\n"
        "struct World { entities: [Entity], tick: Int }\n"
        "fn update_physics(world: ~World) {\n"
        "    for i in 0..world.entities.len() {\n"
        "        world.entities[i].x = world.entities[i].x + 1.0\n"
        "        world.entities[i].y = world.entities[i].y + 0.5\n"
        "    }\n"
        "    world.tick = world.tick + 1\n"
        "}\n"
        "fn main() {\n"
        "    flux world = World {\n"
        "        entities: [\n"
        "            Entity { x: 0.0, y: 0.0, name: \"Player\" },\n"
        "        ],\n"
        "        tick: 0,\n"
        "    }\n"
        "    update_physics(world)\n"
        "    fix frame = freeze(clone(world))\n"
        "    print(frame.tick)\n"
        "}\n"
    );
    gc_stress = false;
}
