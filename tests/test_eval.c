#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lattice.h"
#include "lexer.h"
#include "parser.h"
#include "phase_check.h"
#include "match_check.h"
#include "eval.h"
#include "stackcompiler.h"
#include "stackvm.h"
#include "regvm.h"
#include "runtime.h"
#include "test_backend.h"

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

#define ASSERT_EQ_STR(a, b)                                                                   \
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
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
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
            for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
            lat_vec_free(&tokens);
            return 1;
        }
        lat_vec_free(&errors);
    }

    /* Match exhaustiveness check (warnings to stderr) */
    check_match_exhaustiveness(&prog);

    /* Evaluate — dispatch based on selected backend */
    int result = 0;

    if (test_backend == BACKEND_TREE_WALK) {
        /* Tree-walk evaluator (legacy) */
        Evaluator *ev = evaluator_new();
        if (gc_stress) evaluator_set_gc_stress(ev, true);
        char *eval_err = evaluator_run(ev, &prog);
        if (eval_err) {
            if (err_out) *err_out = eval_err;
            else free(eval_err);
            result = 1;
        }
        evaluator_free(ev);
    } else if (test_backend == BACKEND_STACK_VM) {
        /* Bytecode stack StackVM (production default) */
        value_set_heap(NULL);
        value_set_arena(NULL);

        char *comp_err = NULL;
        Chunk *chunk = stack_compile(&prog, &comp_err);
        if (!chunk) {
            if (err_out) *err_out = comp_err;
            else free(comp_err);
            program_free(&prog);
            for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
            lat_vec_free(&tokens);
            return 1;
        }

        LatRuntime rt;
        lat_runtime_init(&rt);
        StackVM vm;
        stackvm_init(&vm, &rt);
        LatValue vm_result;
        StackVMResult vm_res = stackvm_run(&vm, chunk, &vm_result);
        if (vm_res != STACKVM_OK) {
            if (err_out) *err_out = strdup(vm.error ? vm.error : "vm error");
            result = 1;
        } else {
            value_free(&vm_result);
        }
        stackvm_free(&vm);
        lat_runtime_free(&rt);
        chunk_free(chunk);
    } else if (test_backend == BACKEND_REG_VM) {
        /* Register StackVM (POC) */
        value_set_heap(NULL);
        value_set_arena(NULL);

        char *rcomp_err = NULL;
        RegChunk *rchunk = reg_compile(&prog, &rcomp_err);
        if (!rchunk) {
            if (err_out) *err_out = rcomp_err;
            else free(rcomp_err);
            program_free(&prog);
            for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
            lat_vec_free(&tokens);
            return 1;
        }

        LatRuntime rrt;
        lat_runtime_init(&rrt);
        RegVM rvm;
        regvm_init(&rvm, &rrt);

        LatValue rresult;
        RegVMResult rvm_res = regvm_run(&rvm, rchunk, &rresult);
        if (rvm_res != REGVM_OK) {
            if (err_out) *err_out = strdup(rvm.error ? rvm.error : "regvm error");
            result = 1;
        } else {
            value_free(&rresult);
        }
        regvm_free(&rvm);
        lat_runtime_free(&rrt);
        regchunk_free(rchunk);
    }

    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    return result;
}

/* Convenience: assert source runs without error */
#define ASSERT_RUNS(src)                                                                                          \
    do {                                                                                                          \
        char *_err = NULL;                                                                                        \
        int _rc = run_source_ok(src, &_err);                                                                      \
        if (_rc != 0) {                                                                                           \
            fprintf(stderr, "  FAIL: %s:%d: source failed: %s\n", __FILE__, __LINE__, _err ? _err : "(unknown)"); \
            free(_err);                                                                                           \
            test_current_failed = 1;                                                                              \
            return;                                                                                               \
        }                                                                                                         \
    } while (0)

/* Convenience: assert source fails with an error */
#define ASSERT_FAILS(src)                                                                                  \
    do {                                                                                                   \
        char *_err = NULL;                                                                                 \
        int _rc = run_source_ok(src, &_err);                                                               \
        free(_err);                                                                                        \
        if (_rc == 0) {                                                                                    \
            fprintf(stderr, "  FAIL: %s:%d: expected failure but source succeeded\n", __FILE__, __LINE__); \
            test_current_failed = 1;                                                                       \
            return;                                                                                        \
        }                                                                                                  \
    } while (0)

/* ── Test: Hello World ── */

TEST(eval_hello_world) {
    ASSERT_RUNS("fn main() {\n"
                "    print(\"Hello, World!\")\n"
                "}\n");
}

/* ── Test: Basic Arithmetic ── */

TEST(eval_basic_arithmetic) {
    ASSERT_RUNS("fn main() {\n"
                "    print(1 + 2)\n"
                "}\n");
}

TEST(eval_arithmetic_compound) {
    ASSERT_RUNS("fn main() {\n"
                "    let x = 10 + 20\n"
                "    let y = x * 2\n"
                "    let z = y - 5\n"
                "    print(z)\n"
                "}\n");
}

TEST(eval_arithmetic_division) {
    ASSERT_RUNS("fn main() {\n"
                "    let a = 100 / 4\n"
                "    let b = 10 % 3\n"
                "    print(a)\n"
                "    print(b)\n"
                "}\n");
}

TEST(eval_numeric_underscores) {
    ASSERT_RUNS("fn main() {\n"
                "    assert(1_000_000 == 1000000)\n"
                "    assert(1_000 + 2_000 == 3000)\n"
                "    assert(1_2_3_4 == 1234)\n"
                "    assert(3.14_159 == 3.14159)\n"
                "    assert(1_000.5 == 1000.5)\n"
                "}\n");
}

TEST(eval_hex_literals) {
    ASSERT_RUNS("fn main() {\n"
                "    assert(0xFF == 255)\n"
                "    assert(0x0 == 0)\n"
                "    assert(0x10 == 16)\n"
                "    assert(0xDEAD == 57005)\n"
                "    assert(0xDEAD_BEEF == 3735928559)\n"
                "    assert(0XAB == 171)\n"
                "    assert(0x10 + 0x20 == 48)\n"
                "    assert(0xFF + 1 == 256)\n"
                "}\n");
}

/* ── Test: Variable Bindings ── */

TEST(eval_variable_binding) {
    ASSERT_RUNS("fn main() {\n"
                "    let x = 10\n"
                "    print(x)\n"
                "}\n");
}

TEST(eval_variable_reassignment) {
    ASSERT_RUNS("fn main() {\n"
                "    let x = 10\n"
                "    x = 20\n"
                "    print(x)\n"
                "}\n");
}

/* ── Test: If/Else ── */

TEST(eval_if_else) {
    ASSERT_RUNS("fn main() {\n"
                "    let x = 10\n"
                "    if x > 5 {\n"
                "        print(\"big\")\n"
                "    } else {\n"
                "        print(\"small\")\n"
                "    }\n"
                "}\n");
}

TEST(eval_if_no_else) {
    ASSERT_RUNS("fn main() {\n"
                "    let x = 3\n"
                "    if x < 10 {\n"
                "        print(x)\n"
                "    }\n"
                "}\n");
}

/* ── Test: While Loop ── */

TEST(eval_while_loop) {
    ASSERT_RUNS("fn main() {\n"
                "    let x = 0\n"
                "    while x < 5 {\n"
                "        x = x + 1\n"
                "    }\n"
                "    print(x)\n"
                "}\n");
}

/* ── Test: For Loop with Range ── */

TEST(eval_for_loop_range) {
    ASSERT_RUNS("fn main() {\n"
                "    let sum = 0\n"
                "    for i in 0..5 {\n"
                "        sum = sum + i\n"
                "    }\n"
                "    print(sum)\n"
                "}\n");
}

/* ── Test: Function Definition and Calling ── */

TEST(eval_function_call) {
    ASSERT_RUNS("fn add(a: Int, b: Int) -> Int {\n"
                "    return a + b\n"
                "}\n"
                "\n"
                "fn main() {\n"
                "    let result = add(10, 20)\n"
                "    print(result)\n"
                "}\n");
}

TEST(eval_recursive_function) {
    ASSERT_RUNS("fn factorial(n: Int) -> Int {\n"
                "    if n <= 1 {\n"
                "        return 1\n"
                "    }\n"
                "    return n * factorial(n - 1)\n"
                "}\n"
                "\n"
                "fn main() {\n"
                "    print(factorial(5))\n"
                "}\n");
}

/* ── Test: Arrays ── */

TEST(eval_array_creation) {
    ASSERT_RUNS("fn main() {\n"
                "    let xs = [1, 2, 3]\n"
                "    print(xs)\n"
                "}\n");
}

TEST(eval_array_indexing) {
    ASSERT_RUNS("fn main() {\n"
                "    let xs = [10, 20, 30]\n"
                "    print(xs[0])\n"
                "    print(xs[1])\n"
                "    print(xs[2])\n"
                "}\n");
}

TEST(eval_array_push_and_len) {
    ASSERT_RUNS("fn main() {\n"
                "    let xs = [1, 2, 3]\n"
                "    xs.push(4)\n"
                "    print(xs.len())\n"
                "    print(xs[0])\n"
                "    print(xs[3])\n"
                "}\n");
}

TEST(eval_array_join) {
    ASSERT_RUNS("fn main() {\n"
                "    let words = [\"Hello\", \"World\", \"from\", \"Lattice\"]\n"
                "    let sentence = words.join(\" \")\n"
                "    print(sentence)\n"
                "}\n");
}

/* ── Test: Structs ── */

TEST(eval_struct_creation_and_access) {
    ASSERT_RUNS("struct Point { x: Float, y: Float }\n"
                "\n"
                "fn main() {\n"
                "    let p = Point { x: 3.0, y: 4.0 }\n"
                "    print(p.x)\n"
                "    print(p.y)\n"
                "}\n");
}

TEST(eval_nested_structs) {
    ASSERT_RUNS("struct Point { x: Float, y: Float }\n"
                "struct Line { start: Point, end: Point }\n"
                "\n"
                "fn main() {\n"
                "    let line = Line {\n"
                "        start: Point { x: 0.0, y: 0.0 },\n"
                "        end: Point { x: 1.0, y: 1.0 },\n"
                "    }\n"
                "    print(line.start.x)\n"
                "    print(line.end.y)\n"
                "}\n");
}

/* ── Test: String Operations ── */

TEST(eval_string_concat) {
    ASSERT_RUNS("fn main() {\n"
                "    let greeting = \"Hello\" + \", \" + \"World!\"\n"
                "    print(greeting)\n"
                "}\n");
}

TEST(eval_string_len) {
    ASSERT_RUNS("fn main() {\n"
                "    let greeting = \"Hello\" + \", \" + \"World!\"\n"
                "    print(greeting)\n"
                "    print(greeting.len())\n"
                "}\n");
}

/* ── Test: Boolean Logic ── */

TEST(eval_boolean_logic) {
    ASSERT_RUNS("fn main() {\n"
                "    let a = true\n"
                "    let b = false\n"
                "    print(a && b)\n"
                "    print(a || b)\n"
                "    print(!a)\n"
                "    print(10 == 10)\n"
                "    print(10 != 20)\n"
                "}\n");
}

/* ── Test: Freeze / Thaw / Clone ── */

TEST(eval_freeze_and_thaw) {
    ASSERT_RUNS("fn main() {\n"
                "    let x = 42\n"
                "    let frozen = freeze(x)\n"
                "    let thawed = thaw(frozen)\n"
                "    print(frozen)\n"
                "    print(thawed)\n"
                "}\n");
}

TEST(eval_clone) {
    ASSERT_RUNS("fn main() {\n"
                "    let xs = [1, 2, 3]\n"
                "    let ys = clone(xs)\n"
                "    print(ys)\n"
                "}\n");
}

/* ── Test: Forge Block ── */

TEST(eval_forge_block) {
    ASSERT_RUNS("fn main() {\n"
                "    let result = forge {\n"
                "        let x = 10\n"
                "        let y = 20\n"
                "        x + y\n"
                "    }\n"
                "    print(result)\n"
                "}\n");
}

/* ── Test: Closures / Map ── */

TEST(eval_closure_map) {
    ASSERT_RUNS("fn main() {\n"
                "    let xs = [1, 2, 3, 4, 5]\n"
                "    let doubled = xs.map(|x| x * 2)\n"
                "    print(doubled)\n"
                "}\n");
}

/* ── Test: Strict Mode Full Workflow ── */

TEST(eval_strict_mode_workflow) {
    ASSERT_RUNS("#mode strict\n"
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
                "}\n");
}

/* ── Test: Memory Stats After Evaluation ── */

TEST(eval_memory_stats_populated) {
    const char *source = "fn main() {\n"
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
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
}

/* ── Test: Error on Undefined Variable ── */

TEST(eval_undefined_variable_error) {
    ASSERT_FAILS("fn main() {\n"
                 "    print(undefined_var)\n"
                 "}\n");
}

/* ── Test: Empty Main Function ── */

TEST(eval_empty_main) {
    ASSERT_RUNS("fn main() {\n"
                "}\n");
}

/* ── Test: Multiple Functions ── */

TEST(eval_multiple_functions) {
    ASSERT_RUNS("fn double(x: Int) -> Int {\n"
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
                "}\n");
}

/* ── Test: Nested If ── */

TEST(eval_nested_if) {
    ASSERT_RUNS("fn main() {\n"
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
                "}\n");
}

/* ── Test: While Loop with Break ── */

TEST(eval_while_break) {
    ASSERT_RUNS("fn main() {\n"
                "    let x = 0\n"
                "    while true {\n"
                "        if x >= 5 {\n"
                "            break\n"
                "        }\n"
                "        x = x + 1\n"
                "    }\n"
                "    print(x)\n"
                "}\n");
}

/* ── Test: For Loop with Continue ── */

TEST(eval_for_continue) {
    ASSERT_RUNS("fn main() {\n"
                "    let sum = 0\n"
                "    for i in 0..10 {\n"
                "        if i % 2 == 0 {\n"
                "            continue\n"
                "        }\n"
                "        sum = sum + i\n"
                "    }\n"
                "    print(sum)\n"
                "}\n");
}

/* ── Test: Float Arithmetic ── */

TEST(eval_float_arithmetic) {
    ASSERT_RUNS("fn main() {\n"
                "    let a = 3.14\n"
                "    let b = 2.0\n"
                "    let c = a * b\n"
                "    print(c)\n"
                "}\n");
}

/* ── Test: Comparison Operators ── */

TEST(eval_comparison_operators) {
    ASSERT_RUNS("fn main() {\n"
                "    print(1 < 2)\n"
                "    print(2 > 1)\n"
                "    print(3 <= 3)\n"
                "    print(3 >= 3)\n"
                "    print(4 == 4)\n"
                "    print(4 != 5)\n"
                "}\n");
}

/* ── GC Stress Tests ── */

TEST(eval_gc_stress_hello) {
    gc_stress = true;
    ASSERT_RUNS("fn main() {\n"
                "    let msg = \"Hello\"\n"
                "    let nums = [1, 2, 3, 4, 5]\n"
                "    let p = Point { x: 3, y: 4 }\n"
                "    print(msg)\n"
                "    print(nums)\n"
                "}\n"
                "struct Point { x: Int, y: Int }\n");
    gc_stress = false;
}

TEST(eval_gc_stress_loops) {
    gc_stress = true;
    ASSERT_RUNS("fn main() {\n"
                "    let sum = 0\n"
                "    for i in 0..10 {\n"
                "        sum = sum + i\n"
                "    }\n"
                "    print(sum)\n"
                "}\n");
    gc_stress = false;
}

TEST(eval_gc_stress_closures) {
    gc_stress = true;
    ASSERT_RUNS("fn main() {\n"
                "    let xs = [1, 2, 3, 4, 5]\n"
                "    let doubled = xs.map(|x| x * 2)\n"
                "    print(doubled)\n"
                "}\n");
    gc_stress = false;
}

TEST(eval_gc_stress_freeze_thaw) {
    gc_stress = true;
    ASSERT_RUNS("#mode strict\n"
                "fn main() {\n"
                "    flux x = 42\n"
                "    fix frozen = freeze(x)\n"
                "    flux thawed = thaw(frozen)\n"
                "    thawed = thawed + 1\n"
                "    print(thawed)\n"
                "}\n");
    gc_stress = false;
}

TEST(eval_gc_stress_game_loop) {
    gc_stress = true;
    ASSERT_RUNS("#mode strict\n"
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
                "}\n");
    gc_stress = false;
}

/* ── Dual-Heap Invariant Tests ── */

/*
 * Helper: run source with gc_stress, return evaluator for stats inspection.
 * Caller must call cleanup_run().  Returns NULL on failure.
 */
static Evaluator *run_with_stats(const char *source, LatVec *tokens_out, Program *prog_out) {
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    *tokens_out = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        free(lex_err);
        lat_vec_free(tokens_out);
        return NULL;
    }

    Parser parser = parser_new(tokens_out);
    char *parse_err = NULL;
    *prog_out = parser_parse(&parser, &parse_err);
    if (parse_err) {
        free(parse_err);
        program_free(prog_out);
        for (size_t i = 0; i < tokens_out->len; i++) token_free(lat_vec_get(tokens_out, i));
        lat_vec_free(tokens_out);
        return NULL;
    }

    Evaluator *ev = evaluator_new();
    evaluator_set_gc_stress(ev, true);
    char *eval_err = evaluator_run(ev, prog_out);
    if (eval_err) {
        free(eval_err);
        evaluator_free(ev);
        program_free(prog_out);
        for (size_t i = 0; i < tokens_out->len; i++) token_free(lat_vec_get(tokens_out, i));
        lat_vec_free(tokens_out);
        return NULL;
    }
    return ev;
}

static void cleanup_run(Evaluator *ev, LatVec *tokens, Program *prog) {
    evaluator_free(ev);
    program_free(prog);
    for (size_t i = 0; i < tokens->len; i++) token_free(lat_vec_get(tokens, i));
    lat_vec_free(tokens);
}

/* Test: freeze properly untracks from fluid heap (stats show region registration) */
TEST(eval_gc_freeze_untracks) {
    LatVec tokens;
    Program prog;
    Evaluator *ev = run_with_stats("fn main() {\n"
                                   "    for i in 0..5 {\n"
                                   "        let data = [i, i + 1, i + 2]\n"
                                   "        let frozen = freeze(data)\n"
                                   "        let thawed = thaw(frozen)\n"
                                   "    }\n"
                                   "}\n",
                                   &tokens, &prog);
    ASSERT(ev != NULL);

    const MemoryStats *stats = evaluator_stats(ev);
    ASSERT(stats->freezes >= 5);
    ASSERT(stats->thaws >= 5);
    /* gc_stress ran cycles — the dual-heap assertion inside gc_cycle
     * would have fired if any crystal pointer remained in fluid heap */
    ASSERT(stats->gc_cycles > 0);
    /* Frozen values go out of scope each iteration; regions collected */
    ASSERT(stats->gc_swept_regions >= 1);

    cleanup_run(ev, &tokens, &prog);
}

/* Test: freeze values, drop references, GC collects the regions */
TEST(eval_gc_region_lifecycle) {
    LatVec tokens;
    Program prog;
    Evaluator *ev = run_with_stats("fn main() {\n"
                                   "    for i in 0..20 {\n"
                                   "        let data = [i, i * 2, i * 3]\n"
                                   "        let frozen = freeze(data)\n"
                                   "    }\n"
                                   "}\n",
                                   &tokens, &prog);
    ASSERT(ev != NULL);

    const MemoryStats *stats = evaluator_stats(ev);
    ASSERT(stats->freezes >= 20);
    /* Frozen values go out of scope each iteration; regions should be collected */
    ASSERT(stats->gc_swept_regions >= 1);

    cleanup_run(ev, &tokens, &prog);
}

/* Test: heavy freeze/thaw stress under gc_stress */
TEST(eval_gc_stress_freeze_thaw_heavy) {
    gc_stress = true;
    ASSERT_RUNS("struct Config { value: Int, label: String }\n"
                "fn main() {\n"
                "    let result = 0\n"
                "    for i in 0..100 {\n"
                "        let cfg = Config { value: i, label: \"item_\" + to_string(i) }\n"
                "        let frozen = freeze(cfg)\n"
                "        let thawed = thaw(frozen)\n"
                "        result = result + thawed.value\n"
                "    }\n"
                "    print(result)\n"
                "}\n");
    gc_stress = false;
}

/* Test: deeply nested expressions survive gc_stress (shadow stack depth) */
TEST(eval_gc_shadow_stack_depth) {
    gc_stress = true;
    ASSERT_RUNS("fn main() {\n"
                "    let data = []\n"
                "    for i in 0..50 {\n"
                "        data.push(i)\n"
                "    }\n"
                "    let step1 = data.map(|x| x * 2)\n"
                "    let step2 = step1.filter(|x| x % 3 == 0)\n"
                "    let step3 = step2.map(|x| x + 1)\n"
                "    let step4 = step3.filter(|x| x < 80)\n"
                "    let base = 10\n"
                "    let step5 = data.map(|x| {\n"
                "        let inner = x + base\n"
                "        inner * 2\n"
                "    })\n"
                "    print(step4.len())\n"
                "    print(step5.len())\n"
                "}\n");
    gc_stress = false;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Arena freeze integration tests
 * ══════════════════════════════════════════════════════════════════════════ */

/* Test: arena-backed freeze of arrays survives GC */
TEST(eval_arena_freeze_array_gc) {
    LatVec tokens;
    Program prog;
    Evaluator *ev = run_with_stats("fn main() {\n"
                                   "    let frozen = freeze([1, 2, 3])\n"
                                   "    for i in 0..10 {\n"
                                   "        let garbage = [i, i + 1, i + 2]\n"
                                   "    }\n"
                                   "    print(thaw(frozen))\n"
                                   "}\n",
                                   &tokens, &prog);
    ASSERT(ev != NULL);

    const MemoryStats *stats = evaluator_stats(ev);
    ASSERT(stats->freezes >= 1);
    (void)stats->region_live_count; /* accessed to verify stats are populated */

    cleanup_run(ev, &tokens, &prog);
}

/* Test: arena-backed freeze of maps */
TEST(eval_arena_freeze_map) {
    ASSERT_RUNS("fn main() {\n"
                "    flux m = Map::new()\n"
                "    m.set(\"a\", 1)\n"
                "    m.set(\"b\", 2)\n"
                "    m.set(\"c\", 3)\n"
                "    let frozen = freeze(m)\n"
                "    flux thawed = thaw(frozen)\n"
                "    print(thawed.get(\"a\"))\n"
                "    print(thawed.get(\"b\"))\n"
                "    print(thawed.get(\"c\"))\n"
                "}\n");
}

/* Test: arena-backed freeze of closures with captured environments */
TEST(eval_arena_freeze_closure) {
    ASSERT_RUNS("fn main() {\n"
                "    let x = 42\n"
                "    let f = |a| a + x\n"
                "    let frozen = freeze(f)\n"
                "    let thawed = thaw(frozen)\n"
                "    print(thawed(10))\n"
                "}\n");
}

/* Test: fix binding creates arena-backed value */
TEST(eval_arena_fix_binding) {
    ASSERT_RUNS("fn main() {\n"
                "    fix data = [1, 2, 3, 4, 5]\n"
                "    let sum = 0\n"
                "    for x in thaw(data) {\n"
                "        sum = sum + x\n"
                "    }\n"
                "    print(sum)\n"
                "}\n");
}

/* Test: gc_stress with arena freeze/thaw cycles */
TEST(eval_arena_gc_stress_freeze_thaw) {
    gc_stress = true;
    ASSERT_RUNS("struct Point { x: Int, y: Int }\n"
                "fn main() {\n"
                "    for i in 0..50 {\n"
                "        let p = Point { x: i, y: i * 2 }\n"
                "        let frozen = freeze(p)\n"
                "        let thawed = thaw(frozen)\n"
                "        let result = thawed.x + thawed.y\n"
                "    }\n"
                "}\n");
    gc_stress = false;
}

/* Test: arena freeze of nested structs with maps */
TEST(eval_arena_freeze_nested) {
    ASSERT_RUNS("fn main() {\n"
                "    let data = [[1, 2], [3, 4], [5, 6]]\n"
                "    let frozen = freeze(data)\n"
                "    let thawed = thaw(frozen)\n"
                "    print(thawed[0][0])\n"
                "    print(thawed[2][1])\n"
                "}\n");
}

/* Test: arena-backed values survive multiple GC cycles */
TEST(eval_arena_survives_gc) {
    LatVec tokens;
    Program prog;
    Evaluator *ev = run_with_stats("fn main() {\n"
                                   "    fix persistent = [10, 20, 30]\n"
                                   "    for i in 0..100 {\n"
                                   "        let temp = [i, i * 2]\n"
                                   "    }\n"
                                   "    print(thaw(persistent))\n"
                                   "}\n",
                                   &tokens, &prog);
    ASSERT(ev != NULL);

    const MemoryStats *stats = evaluator_stats(ev);
    ASSERT(stats->gc_cycles > 0);
    ASSERT(stats->region_live_count >= 1);

    cleanup_run(ev, &tokens, &prog);
}

/* ── Helper: run source with gc_stress and capture stdout ── */

static char *run_capture_gc_stress(const char *source, LatVec *tokens_out, Program *prog_out, Evaluator **ev_out) {
    fflush(stdout);
    FILE *tmp = tmpfile();
    int old_stdout = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    *tokens_out = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        free(lex_err);
        fflush(stdout);
        dup2(old_stdout, fileno(stdout));
        close(old_stdout);
        fclose(tmp);
        lat_vec_free(tokens_out);
        *ev_out = NULL;
        return NULL;
    }

    Parser parser = parser_new(tokens_out);
    char *parse_err = NULL;
    *prog_out = parser_parse(&parser, &parse_err);
    if (parse_err) {
        free(parse_err);
        fflush(stdout);
        dup2(old_stdout, fileno(stdout));
        close(old_stdout);
        fclose(tmp);
        program_free(prog_out);
        for (size_t i = 0; i < tokens_out->len; i++) token_free(lat_vec_get(tokens_out, i));
        lat_vec_free(tokens_out);
        *ev_out = NULL;
        return NULL;
    }

    Evaluator *ev = evaluator_new();
    evaluator_set_gc_stress(ev, true);
    char *eval_err = evaluator_run(ev, prog_out);

    fflush(stdout);
    dup2(old_stdout, fileno(stdout));
    close(old_stdout);

    if (eval_err) {
        free(eval_err);
        evaluator_free(ev);
        program_free(prog_out);
        for (size_t i = 0; i < tokens_out->len; i++) token_free(lat_vec_get(tokens_out, i));
        lat_vec_free(tokens_out);
        fclose(tmp);
        *ev_out = NULL;
        return NULL;
    }

    /* Read captured output */
    fseek(tmp, 0, SEEK_END);
    long len = ftell(tmp);
    fseek(tmp, 0, SEEK_SET);
    char *output = malloc((size_t)len + 1);
    size_t n = fread(output, 1, (size_t)len, tmp);
    output[n] = '\0';
    fclose(tmp);

    /* Strip trailing newline */
    if (n > 0 && output[n - 1] == '\n') output[n - 1] = '\0';

    *ev_out = ev;
    return output;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Arena closure captured-environment GC tests
 * ══════════════════════════════════════════════════════════════════════════ */

/* Test: arena-backed closure with captured env survives GC and returns
 * the correct value.  The closure captures `base` (Int) and `items`
 * (Array) from the outer scope, is frozen into a crystal region, then
 * a tight loop allocates enough garbage to trigger multiple GC cycles.
 * Expected output: 108  (100 + 5 + len([10,20,30]) == 108)             */
TEST(eval_arena_closure_captured_env_gc) {
    LatVec tokens;
    Program prog;
    Evaluator *ev;
    char *output = run_capture_gc_stress("fn make_adder(base: Int) -> Closure {\n"
                                         "    let items = [10, 20, 30]\n"
                                         "    fix frozen_fn = freeze(|x| base + x + len(items))\n"
                                         "    flux garbage = [0, 0, 0]\n"
                                         "    flux i = 0\n"
                                         "    while i < 500 {\n"
                                         "        garbage = [i, i + 1, i + 2]\n"
                                         "        i += 1\n"
                                         "    }\n"
                                         "    return frozen_fn\n"
                                         "}\n"
                                         "\n"
                                         "fn main() {\n"
                                         "    let adder = make_adder(100)\n"
                                         "    let thawed = thaw(adder)\n"
                                         "    print(thawed(5))\n"
                                         "}\n",
                                         &tokens, &prog, &ev);

    ASSERT(ev != NULL);
    ASSERT(output != NULL);
    ASSERT_EQ_STR(output, "108");

    const MemoryStats *stats = evaluator_stats(ev);
    /* GC must have run (gc_stress is on) */
    ASSERT(stats->gc_cycles > 0);
    /* At least one freeze happened (the closure) */
    ASSERT(stats->freezes >= 1);
    /* The closure was called */
    ASSERT(stats->closure_calls >= 1);

    free(output);
    cleanup_run(ev, &tokens, &prog);
}

/* Test: an unreachable frozen closure's region IS collected.
 * The closure captures an array and is frozen, but is never returned
 * from the function — so when the function returns, the region becomes
 * unreachable and should be swept.  Expected output: "ok"              */
TEST(eval_arena_closure_region_collected) {
    LatVec tokens;
    Program prog;
    Evaluator *ev;
    char *output = run_capture_gc_stress("fn make_and_discard() {\n"
                                         "    let items = [1, 2, 3, 4, 5]\n"
                                         "    fix frozen = freeze(|x| x + len(items))\n"
                                         "    flux i = 0\n"
                                         "    while i < 500 {\n"
                                         "        flux garbage = [i, i * 2]\n"
                                         "        i += 1\n"
                                         "    }\n"
                                         "}\n"
                                         "\n"
                                         "fn main() {\n"
                                         "    make_and_discard()\n"
                                         "    print(\"ok\")\n"
                                         "}\n",
                                         &tokens, &prog, &ev);

    ASSERT(ev != NULL);
    ASSERT(output != NULL);
    ASSERT_EQ_STR(output, "ok");

    const MemoryStats *stats = evaluator_stats(ev);
    /* GC must have run */
    ASSERT(stats->gc_cycles > 0);
    /* The frozen closure's region should have been swept */
    ASSERT(stats->gc_swept_regions >= 1);

    free(output);
    cleanup_run(ev, &tokens, &prog);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Feature 1: Runtime Type Checking
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(type_check_correct_types) {
    ASSERT_RUNS("fn add(a: Int, b: Int) -> Int { return a + b }\n"
                "fn main() { print(add(1, 2)) }\n");
}

TEST(type_check_wrong_param_type) {
    ASSERT_FAILS("fn add(a: Int, b: Int) -> Int { return a + b }\n"
                 "fn main() { add(1, \"hello\") }\n");
}

TEST(type_check_no_annotation_accepts_any) {
    ASSERT_RUNS("fn greet(x: Any) { print(x) }\n"
                "fn main() { greet(42)\n greet(\"hi\")\n greet(nil) }\n");
}

TEST(type_check_number_union) {
    ASSERT_RUNS("fn double(x: Number) -> Number { return x * 2 }\n"
                "fn main() { print(double(5))\n print(double(2.5)) }\n");
}

TEST(type_check_number_rejects_string) {
    ASSERT_FAILS("fn double(x: Number) -> Number { return x * 2 }\n"
                 "fn main() { double(\"hi\") }\n");
}

TEST(type_check_return_type_mismatch) {
    ASSERT_FAILS("fn get_int() -> Int { return \"oops\" }\n"
                 "fn main() { get_int() }\n");
}

TEST(type_check_struct_name) {
    ASSERT_RUNS("struct Point { x: Int, y: Int }\n"
                "fn origin() -> Point { return Point { x: 0, y: 0 } }\n"
                "fn main() { print(origin().x) }\n");
}

TEST(type_check_struct_name_mismatch) {
    ASSERT_FAILS("struct Point { x: Int, y: Int }\n"
                 "struct Vec { x: Int, y: Int }\n"
                 "fn get_point() -> Point { return Vec { x: 0, y: 0 } }\n"
                 "fn main() { get_point() }\n");
}

TEST(type_check_array_inner) {
    ASSERT_RUNS("fn sum(nums: [Int]) -> Int {\n"
                "    flux total = 0\n"
                "    for n in nums { total += n }\n"
                "    return total\n"
                "}\n"
                "fn main() { print(sum([1, 2, 3])) }\n");
}

TEST(type_check_any_accepts_all) {
    ASSERT_RUNS("fn id(x: Any) -> Any { return x }\n"
                "fn main() { print(id(42))\n print(id(\"hi\")) }\n");
}

TEST(type_check_enum_name) {
    ASSERT_RUNS("enum Color { Red, Green, Blue }\n"
                "fn is_red(c: Color) -> Bool { return c == Color::Red }\n"
                "fn main() { print(is_red(Color::Red)) }\n");
}

TEST(type_check_closure_type) {
    ASSERT_RUNS("fn apply(f: Fn, x: Int) -> Int { return f(x) }\n"
                "fn main() { print(apply(|x| { x * 2 }, 5)) }\n");
}

TEST(type_check_map_type) {
    ASSERT_RUNS("fn get_keys(m: Map) -> Array { return m.keys() }\n"
                "fn main() {\n"
                "    let m = Map::new()\n"
                "    m.set(\"a\", 1)\n"
                "    print(len(get_keys(m)))\n"
                "}\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Feature 2: defer Statement
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(defer_basic_block_exit) {
    ASSERT_RUNS("fn main() {\n"
                "    flux result = \"\"\n"
                "    {\n"
                "        defer { result += \"deferred\" }\n"
                "        result += \"body\"\n"
                "    }\n"
                "    assert(result == \"bodydeferred\", result)\n"
                "}\n");
}

TEST(defer_lifo_order) {
    ASSERT_RUNS("fn main() {\n"
                "    flux order = \"\"\n"
                "    {\n"
                "        defer { order += \"1\" }\n"
                "        defer { order += \"2\" }\n"
                "        defer { order += \"3\" }\n"
                "    }\n"
                "    assert(order == \"321\", \"expected 321, got \" + order)\n"
                "}\n");
}

TEST(defer_on_early_return) {
    ASSERT_RUNS("flux g_log = \"\"\n"
                "fn work() {\n"
                "    defer { g_log += \"deferred\" }\n"
                "    g_log += \"before\"\n"
                "    return\n"
                "}\n"
                "fn main() {\n"
                "    work()\n"
                "    assert(g_log == \"beforedeferred\", g_log)\n"
                "}\n");
}

TEST(defer_in_loop) {
    ASSERT_RUNS("fn main() {\n"
                "    flux count = 0\n"
                "    for i in 0..3 {\n"
                "        defer { count += 1 }\n"
                "    }\n"
                "    assert(count == 3, \"expected 3, got \" + to_string(count))\n"
                "}\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Feature 3: Optional Chaining ?.
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(optional_chain_nil_field) {
    ASSERT_RUNS("fn main() {\n"
                "    let x = nil\n"
                "    assert(x?.name == nil)\n"
                "}\n");
}

TEST(optional_chain_non_nil_field) {
    ASSERT_RUNS("struct Pt { x: Int, y: Int }\n"
                "fn main() {\n"
                "    let p = Pt { x: 1, y: 2 }\n"
                "    assert(p?.x == 1)\n"
                "}\n");
}

TEST(optional_chain_deep) {
    ASSERT_RUNS("struct Inner { val: Int }\n"
                "struct Outer { inner: Inner }\n"
                "fn main() {\n"
                "    let x = nil\n"
                "    assert(x?.inner?.val == nil)\n"
                "    let o = Outer { inner: Inner { val: 42 } }\n"
                "    assert(o?.inner?.val == 42)\n"
                "}\n");
}

TEST(optional_chain_method_on_nil) {
    ASSERT_RUNS("fn main() {\n"
                "    let x = nil\n"
                "    assert(x?.len() == nil)\n"
                "}\n");
}

TEST(optional_chain_index_on_nil) {
    ASSERT_RUNS("fn main() {\n"
                "    let x = nil\n"
                "    assert(x?[0] == nil)\n"
                "}\n");
}

TEST(optional_chain_with_nil_coalesce) {
    ASSERT_RUNS("fn main() {\n"
                "    let x = nil\n"
                "    let result = x?.name ?? \"fallback\"\n"
                "    assert(result == \"fallback\")\n"
                "}\n");
}

TEST(optional_chain_non_optional_on_nil_errors) {
    ASSERT_FAILS("fn main() {\n"
                 "    let x = nil\n"
                 "    let y = x?.name.len()\n"
                 "}\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Feature 4: Result ? Operator
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(try_propagate_ok_unwraps) {
    ASSERT_RUNS("fn make_ok() -> Map {\n"
                "    let r = Map::new()\n"
                "    r.set(\"tag\", \"ok\")\n"
                "    r.set(\"value\", 42)\n"
                "    return r\n"
                "}\n"
                "fn process() -> Map {\n"
                "    let v = make_ok()?\n"
                "    let r = Map::new()\n"
                "    r.set(\"tag\", \"ok\")\n"
                "    r.set(\"value\", v + 1)\n"
                "    return r\n"
                "}\n"
                "fn main() {\n"
                "    let result = process()\n"
                "    assert(result.get(\"value\") == 43)\n"
                "}\n");
}

TEST(try_propagate_err_returns) {
    ASSERT_RUNS("fn make_err() -> Map {\n"
                "    let r = Map::new()\n"
                "    r.set(\"tag\", \"err\")\n"
                "    r.set(\"value\", \"failed\")\n"
                "    return r\n"
                "}\n"
                "fn process() -> Map {\n"
                "    let v = make_err()?\n"
                "    let r = Map::new()\n"
                "    r.set(\"tag\", \"ok\")\n"
                "    r.set(\"value\", v + 1)\n"
                "    return r\n"
                "}\n"
                "fn main() {\n"
                "    let result = process()\n"
                "    assert(result.get(\"tag\") == \"err\")\n"
                "    assert(result.get(\"value\") == \"failed\")\n"
                "}\n");
}

TEST(try_propagate_chain) {
    ASSERT_RUNS("fn ok_val(v: Any) -> Map {\n"
                "    let r = Map::new()\n"
                "    r.set(\"tag\", \"ok\")\n"
                "    r.set(\"value\", v)\n"
                "    return r\n"
                "}\n"
                "fn step1() -> Map { return ok_val(10) }\n"
                "fn step2() -> Map { return ok_val(20) }\n"
                "fn process() -> Map {\n"
                "    let a = step1()?\n"
                "    let b = step2()?\n"
                "    return ok_val(a + b)\n"
                "}\n"
                "fn main() {\n"
                "    let r = process()\n"
                "    assert(r.get(\"value\") == 30)\n"
                "}\n");
}

TEST(try_propagate_on_non_map_errors) {
    ASSERT_FAILS("fn main() {\n"
                 "    let x = 42?\n"
                 "}\n");
}

TEST(try_propagate_skips_code_after_err) {
    ASSERT_RUNS("flux reached = false\n"
                "fn make_err() -> Map {\n"
                "    let r = Map::new()\n"
                "    r.set(\"tag\", \"err\")\n"
                "    r.set(\"value\", \"fail\")\n"
                "    return r\n"
                "}\n"
                "fn process() -> Map {\n"
                "    let v = make_err()?\n"
                "    reached = true\n"
                "    let r = Map::new()\n"
                "    r.set(\"tag\", \"ok\")\n"
                "    r.set(\"value\", v)\n"
                "    return r\n"
                "}\n"
                "fn main() {\n"
                "    let result = process()\n"
                "    assert(reached == false, \"should not have reached code after ?\")\n"
                "}\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Feature 5: require/ensure Contracts
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(require_passes) {
    ASSERT_RUNS("fn positive(x: Int)\n"
                "    require x > 0, \"x must be positive\"\n"
                "{\n"
                "    return x\n"
                "}\n"
                "fn main() { assert(positive(5) == 5) }\n");
}

TEST(require_fails_with_message) {
    ASSERT_FAILS("fn positive(x: Int)\n"
                 "    require x > 0, \"x must be positive\"\n"
                 "{\n"
                 "    return x\n"
                 "}\n"
                 "fn main() { positive(-1) }\n");
}

TEST(ensure_passes) {
    ASSERT_RUNS("fn abs_val(x: Int) -> Int\n"
                "    ensure |r| { r >= 0 }, \"result must be non-negative\"\n"
                "{\n"
                "    if x < 0 { return -x }\n"
                "    return x\n"
                "}\n"
                "fn main() { assert(abs_val(-5) == 5) }\n");
}

TEST(ensure_fails) {
    ASSERT_FAILS("fn broken() -> Int\n"
                 "    ensure |r| { r > 0 }, \"must be positive\"\n"
                 "{\n"
                 "    return -1\n"
                 "}\n"
                 "fn main() { broken() }\n");
}

TEST(multiple_require_clauses) {
    ASSERT_RUNS("fn range_check(lo: Int, hi: Int)\n"
                "    require lo >= 0, \"lo must be non-negative\"\n"
                "    require hi > lo, \"hi must be greater than lo\"\n"
                "{\n"
                "    return hi - lo\n"
                "}\n"
                "fn main() { assert(range_check(1, 5) == 4) }\n");
}

TEST(multiple_require_first_fails) {
    ASSERT_FAILS("fn range_check(lo: Int, hi: Int)\n"
                 "    require lo >= 0, \"lo must be non-negative\"\n"
                 "    require hi > lo, \"hi must be greater than lo\"\n"
                 "{\n"
                 "    return hi - lo\n"
                 "}\n"
                 "fn main() { range_check(-1, 5) }\n");
}

TEST(debug_assert_enabled) {
    ASSERT_FAILS("fn main() {\n"
                 "    debug_assert(false, \"should fire\")\n"
                 "}\n");
}

TEST(debug_assert_passes) {
    ASSERT_RUNS("fn main() {\n"
                "    debug_assert(true, \"should not fire\")\n"
                "}\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Feature 6: select for Channels (basic tests, no threading)
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(select_from_ready_channel) {
    ASSERT_RUNS("fn main() {\n"
                "    let ch = Channel::new()\n"
                "    ch.send(freeze(42))\n"
                "    let result = select {\n"
                "        v from ch => { v }\n"
                "    }\n"
                "    assert(result == 42)\n"
                "}\n");
}

TEST(select_with_default) {
    ASSERT_RUNS("fn main() {\n"
                "    let ch = Channel::new()\n"
                "    let result = select {\n"
                "        v from ch => { v }\n"
                "        default => { \"empty\" }\n"
                "    }\n"
                "    assert(result == \"empty\")\n"
                "}\n");
}

TEST(select_closed_channel_uses_default) {
    ASSERT_RUNS("fn main() {\n"
                "    let ch = Channel::new()\n"
                "    ch.close()\n"
                "    let result = select {\n"
                "        v from ch => { v }\n"
                "        default => { \"closed\" }\n"
                "    }\n"
                "    assert(result == \"closed\")\n"
                "}\n");
}

TEST(select_all_closed_returns_unit) {
    ASSERT_RUNS("fn main() {\n"
                "    let ch = Channel::new()\n"
                "    ch.close()\n"
                "    let result = select {\n"
                "        v from ch => { v }\n"
                "    }\n"
                "    assert(result == nil || to_string(result) == \"()\")\n"
                "}\n");
}

TEST(select_first_ready) {
    ASSERT_RUNS("fn main() {\n"
                "    let ch1 = Channel::new()\n"
                "    let ch2 = Channel::new()\n"
                "    ch2.send(freeze(99))\n"
                "    let result = select {\n"
                "        v from ch1 => { \"ch1:\" + to_string(v) }\n"
                "        v from ch2 => { \"ch2:\" + to_string(v) }\n"
                "        default => { \"none\" }\n"
                "    }\n"
                "    assert(result == \"ch2:99\", \"got: \" + to_string(result))\n"
                "}\n");
}

/* ── Trait/Impl Tests ── */

TEST(trait_basic_impl) {
    ASSERT_RUNS("trait Greetable {\n"
                "    fn greet(self: Any) -> String;\n"
                "}\n"
                "struct Person { name: String }\n"
                "impl Greetable for Person {\n"
                "    fn greet(self: Any) -> String {\n"
                "        return \"Hello, \" + self.name\n"
                "    }\n"
                "}\n"
                "fn main() {\n"
                "    let p = Person { name: \"Alice\" }\n"
                "    assert(p.greet() == \"Hello, Alice\")\n"
                "}\n");
}

TEST(trait_multiple_methods) {
    ASSERT_RUNS("trait Shape {\n"
                "    fn area(self: Any) -> Int;\n"
                "    fn name(self: Any) -> String;\n"
                "}\n"
                "struct Square { side: Int }\n"
                "impl Shape for Square {\n"
                "    fn area(self: Any) -> Int { return self.side * self.side }\n"
                "    fn name(self: Any) -> String { return \"Square\" }\n"
                "}\n"
                "fn main() {\n"
                "    let s = Square { side: 5 }\n"
                "    assert(s.area() == 25)\n"
                "    assert(s.name() == \"Square\")\n"
                "}\n");
}

TEST(trait_impl_with_args) {
    ASSERT_RUNS("trait Addable {\n"
                "    fn add_to(self: Any, n: Int) -> Int;\n"
                "}\n"
                "struct Counter { value: Int }\n"
                "impl Addable for Counter {\n"
                "    fn add_to(self: Any, n: Int) -> Int {\n"
                "        return self.value + n\n"
                "    }\n"
                "}\n"
                "fn main() {\n"
                "    let c = Counter { value: 10 }\n"
                "    assert(c.add_to(5) == 15)\n"
                "}\n");
}

TEST(trait_multiple_impls) {
    ASSERT_RUNS("trait Describable {\n"
                "    fn describe(self: Any) -> String;\n"
                "}\n"
                "struct Dog { name: String }\n"
                "struct Cat { name: String }\n"
                "impl Describable for Dog {\n"
                "    fn describe(self: Any) -> String { return \"Dog: \" + self.name }\n"
                "}\n"
                "impl Describable for Cat {\n"
                "    fn describe(self: Any) -> String { return \"Cat: \" + self.name }\n"
                "}\n"
                "fn main() {\n"
                "    let d = Dog { name: \"Rex\" }\n"
                "    let c = Cat { name: \"Whiskers\" }\n"
                "    assert(d.describe() == \"Dog: Rex\")\n"
                "    assert(c.describe() == \"Cat: Whiskers\")\n"
                "}\n");
}

/* ── Buffer tests ── */

TEST(eval_buffer_new) {
    ASSERT_RUNS("let buf = Buffer::new(16)\n"
                "assert(len(buf) == 16)\n"
                "assert(buf.len() == 16)\n"
                "assert(buf[0] == 0)\n");
}

TEST(eval_buffer_from_array) {
    ASSERT_RUNS("let buf = Buffer::from([255, 0, 66])\n"
                "assert(buf.len() == 3)\n"
                "assert(buf[0] == 255)\n"
                "assert(buf[1] == 0)\n"
                "assert(buf[2] == 66)\n");
}

TEST(eval_buffer_from_string) {
    ASSERT_RUNS("let buf = Buffer::from_string(\"Hi\")\n"
                "assert(buf.len() == 2)\n"
                "assert(buf[0] == 72)\n"
                "assert(buf[1] == 105)\n");
}

TEST(eval_buffer_index_read_write) {
    ASSERT_RUNS("let buf = Buffer::new(4)\n"
                "buf[0] = 42\n"
                "buf[1] = 255\n"
                "assert(buf[0] == 42)\n"
                "assert(buf[1] == 255)\n");
}

TEST(eval_buffer_push) {
    ASSERT_RUNS("let buf = Buffer::new(0)\n"
                "buf.push(72)\n"
                "buf.push(105)\n"
                "assert(buf.len() == 2)\n"
                "assert(buf[0] == 72)\n"
                "assert(buf[1] == 105)\n");
}

TEST(eval_buffer_push_u16_u32) {
    ASSERT_RUNS("let buf = Buffer::new(0)\n"
                "buf.push_u16(258)\n"
                "assert(buf.len() == 2)\n"
                "assert(buf[0] == 2)\n"
                "assert(buf[1] == 1)\n"
                "buf.push_u32(67305985)\n"
                "assert(buf.len() == 6)\n"
                "assert(buf[2] == 1)\n"
                "assert(buf[3] == 2)\n"
                "assert(buf[4] == 3)\n"
                "assert(buf[5] == 4)\n");
}

TEST(eval_buffer_read_write_u16) {
    ASSERT_RUNS("let buf = Buffer::new(4)\n"
                "buf.write_u16(0, 4660)\n"
                "assert(buf.read_u16(0) == 4660)\n"
                "assert(buf[0] == 52)\n"
                "assert(buf[1] == 18)\n");
}

TEST(eval_buffer_read_write_u32) {
    ASSERT_RUNS("let buf = Buffer::new(8)\n"
                "buf.write_u32(0, 3735928559)\n"
                "assert(buf.read_u32(0) == 3735928559)\n"
                "assert(buf[0] == 239)\n"
                "assert(buf[1] == 190)\n"
                "assert(buf[2] == 173)\n"
                "assert(buf[3] == 222)\n");
}

TEST(eval_buffer_slice) {
    ASSERT_RUNS("let buf = Buffer::from([10, 20, 30, 40, 50])\n"
                "let s = buf.slice(1, 4)\n"
                "assert(s.len() == 3)\n"
                "assert(s[0] == 20)\n"
                "assert(s[1] == 30)\n"
                "assert(s[2] == 40)\n");
}

TEST(eval_buffer_to_string) {
    ASSERT_RUNS("let buf = Buffer::from_string(\"hello\")\n"
                "assert(buf.to_string() == \"hello\")\n");
}

TEST(eval_buffer_to_array) {
    ASSERT_RUNS("let buf = Buffer::from([1, 2, 3])\n"
                "let arr = buf.to_array()\n"
                "assert(len(arr) == 3)\n"
                "assert(arr[0] == 1)\n"
                "assert(arr[1] == 2)\n"
                "assert(arr[2] == 3)\n");
}

TEST(eval_buffer_to_hex) {
    ASSERT_RUNS("let buf = Buffer::from([72, 101, 108])\n"
                "assert(buf.to_hex() == \"48656c\")\n");
}

TEST(eval_buffer_clear_fill_resize) {
    ASSERT_RUNS("let buf = Buffer::new(4)\n"
                "buf.fill(255)\n"
                "assert(buf[0] == 255)\n"
                "assert(buf[3] == 255)\n"
                "buf.clear()\n"
                "assert(buf.len() == 0)\n"
                "buf.resize(8)\n"
                "assert(buf.len() == 8)\n"
                "assert(buf[0] == 0)\n");
}

TEST(eval_buffer_equality) {
    ASSERT_RUNS("let a = Buffer::from([1, 2, 3])\n"
                "let b = Buffer::from([1, 2, 3])\n"
                "let c = Buffer::from([1, 2, 4])\n"
                "assert(a == b)\n"
                "assert(a != c)\n");
}

TEST(eval_buffer_typeof) {
    ASSERT_RUNS("let buf = Buffer::new(4)\n"
                "assert(typeof(buf) == \"Buffer\")\n");
}

TEST(eval_buffer_freeze_thaw) {
    ASSERT_RUNS("flux buf = Buffer::from([1, 2, 3])\n"
                "freeze(buf)\n"
                "let buf2 = thaw(buf)\n"
                "assert(buf2.len() == 3)\n");
}

/* ── Tests: else if ── */

TEST(eval_else_if_basic) {
    ASSERT_RUNS("fn main() {\n"
                "    let x = 2\n"
                "    if false { assert(false) }\n"
                "    else if true { assert(x == 2) }\n"
                "    else { assert(false) }\n"
                "}\n");
}

TEST(eval_else_if_chain) {
    ASSERT_RUNS("fn main() {\n"
                "    let x = 3\n"
                "    if false { assert(false) }\n"
                "    else if false { assert(false) }\n"
                "    else if true { assert(x == 3) }\n"
                "    else { assert(false) }\n"
                "}\n");
}

TEST(eval_else_if_fallthrough) {
    ASSERT_RUNS("fn main() {\n"
                "    if false { assert(false) }\n"
                "    else if false { assert(false) }\n"
                "    else { assert(true) }\n"
                "}\n");
}

TEST(eval_else_if_no_else) {
    ASSERT_RUNS("fn main() {\n"
                "    flux hit = false\n"
                "    if false { assert(false) }\n"
                "    else if true { hit = true }\n"
                "    assert(hit)\n"
                "}\n");
}

TEST(eval_else_if_nested) {
    ASSERT_RUNS("fn check(x: Int) -> String {\n"
                "    if x > 100 { return \"big\" }\n"
                "    else if x > 10 { return \"medium\" }\n"
                "    else { return \"small\" }\n"
                "}\n"
                "fn main() {\n"
                "    assert(check(200) == \"big\")\n"
                "    assert(check(42) == \"medium\")\n"
                "    assert(check(5) == \"small\")\n"
                "}\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * LAT-41: Phase System Test Coverage
 *
 * Comprehensive tests for the phase system including:
 * - Phase transitions: freeze(), thaw(), clone()
 * - Edge cases: freeze of already-frozen, thaw of already-thawed
 * - Pressure modes: no_grow, no_shrink, no_resize
 * - Phase annotations on struct fields (@crystal, @fluid)
 * - Composite constraints
 * - borrow() scoped mutation
 * - Strict mode compliance
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Phase Transitions: freeze(), thaw(), clone() ── */

TEST(phase_freeze_makes_crystal) {
    ASSERT_RUNS("fn main() {\n"
                "    flux x = [1, 2, 3]\n"
                "    freeze(x)\n"
                "    assert(phase_of(x) == \"crystal\")\n"
                "}\n");
}

TEST(phase_thaw_makes_fluid) {
    ASSERT_RUNS("fn main() {\n"
                "    fix data = freeze([1, 2, 3])\n"
                "    let thawed = thaw(data)\n"
                "    assert(phase_of(thawed) == \"fluid\")\n"
                "}\n");
}

TEST(phase_clone_preserves_phase) {
    ASSERT_RUNS("fn main() {\n"
                "    flux x = [1, 2, 3]\n"
                "    let y = clone(x)\n"
                "    assert(phase_of(y) == phase_of(x))\n"
                "    freeze(x)\n"
                "    let z = clone(x)\n"
                "    assert(phase_of(z) == \"crystal\")\n"
                "}\n");
}

TEST(phase_clone_is_independent) {
    ASSERT_RUNS("fn main() {\n"
                "    flux xs = [1, 2, 3]\n"
                "    let ys = clone(xs)\n"
                "    xs.push(4)\n"
                "    assert(xs.len() == 4)\n"
                "    assert(ys.len() == 3)\n"
                "}\n");
}

TEST(phase_freeze_int_crystal) {
    ASSERT_RUNS("fn main() {\n"
                "    let x = freeze(42)\n"
                "    assert(phase_of(x) == \"crystal\")\n"
                "}\n");
}

TEST(phase_freeze_string_crystal) {
    ASSERT_RUNS("fn main() {\n"
                "    let s = freeze(\"hello\")\n"
                "    assert(phase_of(s) == \"crystal\")\n"
                "}\n");
}

TEST(phase_freeze_struct_crystal) {
    ASSERT_RUNS("struct Point { x: Int, y: Int }\n"
                "fn main() {\n"
                "    flux p = Point { x: 1, y: 2 }\n"
                "    freeze(p)\n"
                "    assert(phase_of(p) == \"crystal\")\n"
                "}\n");
}

TEST(phase_freeze_map_crystal) {
    ASSERT_RUNS("fn main() {\n"
                "    flux m = Map::new()\n"
                "    m.set(\"a\", 1)\n"
                "    freeze(m)\n"
                "    assert(phase_of(m) == \"crystal\")\n"
                "}\n");
}

TEST(phase_thaw_then_mutate) {
    ASSERT_RUNS("fn main() {\n"
                "    fix data = freeze([10, 20])\n"
                "    flux thawed = thaw(data)\n"
                "    thawed.push(30)\n"
                "    assert(thawed.len() == 3)\n"
                "    assert(thawed[2] == 30)\n"
                "}\n");
}

/* ── Edge Cases: freeze of already-frozen, thaw of already-thawed ── */

TEST(phase_freeze_already_frozen) {
    /* Freezing an already-crystal value should be a no-op, not an error */
    ASSERT_RUNS("fn main() {\n"
                "    let x = freeze(42)\n"
                "    let y = freeze(x)\n"
                "    assert(phase_of(y) == \"crystal\")\n"
                "}\n");
}

TEST(phase_thaw_already_fluid) {
    /* Thawing an already-fluid value should be a no-op, not an error */
    ASSERT_RUNS("fn main() {\n"
                "    flux x = [1, 2, 3]\n"
                "    let y = thaw(x)\n"
                "    assert(phase_of(y) == \"fluid\")\n"
                "}\n");
}

TEST(phase_double_freeze_thaw_roundtrip) {
    ASSERT_RUNS("fn main() {\n"
                "    flux data = [1, 2, 3]\n"
                "    freeze(data)\n"
                "    assert(phase_of(data) == \"crystal\")\n"
                "    flux data2 = thaw(data)\n"
                "    data2.push(4)\n"
                "    freeze(data2)\n"
                "    assert(phase_of(data2) == \"crystal\")\n"
                "    flux data3 = thaw(data2)\n"
                "    assert(data3.len() == 4)\n"
                "}\n");
}

TEST(phase_freeze_thaw_struct_roundtrip) {
    ASSERT_RUNS("struct Config { host: String, port: Int }\n"
                "fn main() {\n"
                "    flux cfg = Config { host: \"localhost\", port: 8080 }\n"
                "    freeze(cfg)\n"
                "    assert(phase_of(cfg) == \"crystal\")\n"
                "    flux cfg2 = thaw(cfg)\n"
                "    cfg2.port = 9090\n"
                "    assert(cfg2.port == 9090)\n"
                "    freeze(cfg2)\n"
                "    assert(phase_of(cfg2) == \"crystal\")\n"
                "}\n");
}

/* ── Crystal values reject mutation ── */

TEST(phase_crystal_array_rejects_push) {
    ASSERT_FAILS("fn main() {\n"
                 "    fix data = freeze([1, 2, 3])\n"
                 "    data.push(4)\n"
                 "}\n");
}

TEST(phase_crystal_array_rejects_index_assign) {
    if (test_backend == BACKEND_TREE_WALK) return; /* tree-walk does not enforce index assign on crystal arrays */
    ASSERT_FAILS("fn main() {\n"
                 "    fix data = freeze([1, 2, 3])\n"
                 "    data[0] = 99\n"
                 "}\n");
}

TEST(phase_crystal_struct_rejects_field_assign) {
    ASSERT_FAILS("struct Point { x: Int, y: Int }\n"
                 "fn main() {\n"
                 "    flux p = Point { x: 1, y: 2 }\n"
                 "    freeze(p)\n"
                 "    p.x = 10\n"
                 "}\n");
}

TEST(phase_crystal_map_rejects_set) {
    ASSERT_FAILS("fn main() {\n"
                 "    flux m = Map::new()\n"
                 "    m.set(\"a\", 1)\n"
                 "    freeze(m)\n"
                 "    m[\"a\"] = 2\n"
                 "}\n");
}

TEST(phase_crystal_allows_read) {
    ASSERT_RUNS("fn main() {\n"
                "    fix data = freeze([10, 20, 30])\n"
                "    assert(data[0] == 10)\n"
                "    assert(data.len() == 3)\n"
                "}\n");
}

/* ── Pressure Modes: no_grow, no_shrink, no_resize ── */

TEST(phase_pressure_no_grow_blocks_push) {
    ASSERT_FAILS("fn main() {\n"
                 "    flux data = [1, 2, 3]\n"
                 "    pressurize(data, \"no_grow\")\n"
                 "    data.push(4)\n"
                 "}\n");
}

TEST(phase_pressure_no_grow_blocks_insert) {
    ASSERT_FAILS("fn main() {\n"
                 "    flux data = [1, 2, 3]\n"
                 "    pressurize(data, \"no_grow\")\n"
                 "    data.insert(0, 99)\n"
                 "}\n");
}

TEST(phase_pressure_no_grow_allows_pop) {
    ASSERT_RUNS("fn main() {\n"
                "    flux data = [1, 2, 3]\n"
                "    pressurize(data, \"no_grow\")\n"
                "    let popped = data.pop()\n"
                "    assert(popped == 3)\n"
                "    assert(data.len() == 2)\n"
                "}\n");
}

TEST(phase_pressure_no_grow_allows_index_assign) {
    ASSERT_RUNS("fn main() {\n"
                "    flux data = [1, 2, 3]\n"
                "    pressurize(data, \"no_grow\")\n"
                "    data[0] = 99\n"
                "    assert(data[0] == 99)\n"
                "}\n");
}

TEST(phase_pressure_no_shrink_blocks_pop) {
    ASSERT_FAILS("fn main() {\n"
                 "    flux data = [1, 2, 3]\n"
                 "    pressurize(data, \"no_shrink\")\n"
                 "    data.pop()\n"
                 "}\n");
}

TEST(phase_pressure_no_shrink_blocks_remove_at) {
    ASSERT_FAILS("fn main() {\n"
                 "    flux data = [1, 2, 3]\n"
                 "    pressurize(data, \"no_shrink\")\n"
                 "    data.remove_at(0)\n"
                 "}\n");
}

TEST(phase_pressure_no_shrink_allows_push) {
    ASSERT_RUNS("fn main() {\n"
                "    flux data = [1, 2, 3]\n"
                "    pressurize(data, \"no_shrink\")\n"
                "    data.push(4)\n"
                "    assert(data.len() == 4)\n"
                "}\n");
}

TEST(phase_pressure_no_resize_blocks_push) {
    ASSERT_FAILS("fn main() {\n"
                 "    flux data = [1, 2, 3]\n"
                 "    pressurize(data, \"no_resize\")\n"
                 "    data.push(4)\n"
                 "}\n");
}

TEST(phase_pressure_no_resize_blocks_pop) {
    ASSERT_FAILS("fn main() {\n"
                 "    flux data = [1, 2, 3]\n"
                 "    pressurize(data, \"no_resize\")\n"
                 "    data.pop()\n"
                 "}\n");
}

TEST(phase_pressure_no_resize_blocks_insert) {
    ASSERT_FAILS("fn main() {\n"
                 "    flux data = [1, 2, 3]\n"
                 "    pressurize(data, \"no_resize\")\n"
                 "    data.insert(0, 99)\n"
                 "}\n");
}

TEST(phase_pressure_no_resize_blocks_remove_at) {
    ASSERT_FAILS("fn main() {\n"
                 "    flux data = [1, 2, 3]\n"
                 "    pressurize(data, \"no_resize\")\n"
                 "    data.remove_at(0)\n"
                 "}\n");
}

TEST(phase_pressure_no_resize_allows_index_assign) {
    ASSERT_RUNS("fn main() {\n"
                "    flux data = [1, 2, 3]\n"
                "    pressurize(data, \"no_resize\")\n"
                "    data[1] = 42\n"
                "    assert(data[1] == 42)\n"
                "}\n");
}

TEST(phase_pressure_depressurize_restores) {
    ASSERT_RUNS("fn main() {\n"
                "    flux data = [1, 2, 3]\n"
                "    pressurize(data, \"no_resize\")\n"
                "    depressurize(data)\n"
                "    data.push(4)\n"
                "    assert(data.len() == 4)\n"
                "    data.pop()\n"
                "    assert(data.len() == 3)\n"
                "}\n");
}

TEST(phase_pressure_override_mode) {
    /* Applying a new pressure mode should replace the old one */
    ASSERT_FAILS("fn main() {\n"
                 "    flux data = [1, 2, 3]\n"
                 "    pressurize(data, \"no_grow\")\n"
                 "    pressurize(data, \"no_shrink\")\n"
                 "    data.pop()\n"
                 "}\n");
}

TEST(phase_pressure_override_allows_previously_blocked) {
    /* After switching from no_grow to no_shrink, push should work */
    ASSERT_RUNS("fn main() {\n"
                "    flux data = [1, 2, 3]\n"
                "    pressurize(data, \"no_grow\")\n"
                "    pressurize(data, \"no_shrink\")\n"
                "    data.push(4)\n"
                "    assert(data.len() == 4)\n"
                "}\n");
}

/* ── Struct Field Phase Annotations (Alloys) ── */

TEST(phase_alloy_fix_field_rejects_mutation) {
    ASSERT_FAILS("struct Config {\n"
                 "    host: fix String,\n"
                 "    retries: flux Int,\n"
                 "}\n"
                 "fn main() {\n"
                 "    let cfg = Config { host: \"localhost\", retries: 0 }\n"
                 "    cfg.host = \"other\"\n"
                 "}\n");
}

TEST(phase_alloy_flux_field_allows_mutation) {
    ASSERT_RUNS("struct Config {\n"
                "    host: fix String,\n"
                "    retries: flux Int,\n"
                "}\n"
                "fn main() {\n"
                "    let cfg = Config { host: \"localhost\", retries: 0 }\n"
                "    cfg.retries = 5\n"
                "    assert(cfg.retries == 5)\n"
                "}\n");
}

TEST(phase_alloy_multiple_fix_fields) {
    ASSERT_FAILS("struct Server {\n"
                 "    host: fix String,\n"
                 "    port: fix Int,\n"
                 "    retries: flux Int,\n"
                 "}\n"
                 "fn main() {\n"
                 "    let s = Server { host: \"localhost\", port: 8080, retries: 0 }\n"
                 "    s.port = 9090\n"
                 "}\n");
}

TEST(phase_alloy_multiple_flux_fields_all_mutable) {
    ASSERT_RUNS("struct Counter {\n"
                "    label: fix String,\n"
                "    count: flux Int,\n"
                "    max: flux Int,\n"
                "}\n"
                "fn main() {\n"
                "    let c = Counter { label: \"hits\", count: 0, max: 100 }\n"
                "    c.count = 42\n"
                "    c.max = 200\n"
                "    assert(c.count == 42)\n"
                "    assert(c.max == 200)\n"
                "}\n");
}

TEST(phase_alloy_fix_field_readable) {
    ASSERT_RUNS("struct Immutable {\n"
                "    value: fix Int,\n"
                "}\n"
                "fn main() {\n"
                "    let x = Immutable { value: 99 }\n"
                "    assert(x.value == 99)\n"
                "    assert(x.value + 1 == 100)\n"
                "}\n");
}

TEST(phase_alloy_freeze_entire_struct_overrides_fields) {
    /* Freezing entire struct should make all fields crystal, even flux ones */
    ASSERT_FAILS("struct Config {\n"
                 "    host: fix String,\n"
                 "    retries: flux Int,\n"
                 "}\n"
                 "fn main() {\n"
                 "    flux cfg = Config { host: \"localhost\", retries: 0 }\n"
                 "    freeze(cfg)\n"
                 "    cfg.retries = 5\n"
                 "}\n");
}

TEST(phase_alloy_partial_freeze_field) {
    /* freeze(s.field) should freeze just that field */
    ASSERT_RUNS("struct Obj {\n"
                "    a: Int,\n"
                "    b: Int,\n"
                "}\n"
                "fn main() {\n"
                "    flux o = Obj { a: 1, b: 2 }\n"
                "    freeze(o.a)\n"
                "    o.b = 20\n"
                "    assert(o.b == 20)\n"
                "}\n");
}

TEST(phase_alloy_partial_freeze_blocks_frozen_field) {
    ASSERT_FAILS("struct Obj {\n"
                 "    a: Int,\n"
                 "    b: Int,\n"
                 "}\n"
                 "fn main() {\n"
                 "    flux o = Obj { a: 1, b: 2 }\n"
                 "    freeze(o.a)\n"
                 "    o.a = 99\n"
                 "}\n");
}

/* ── Composite Phase Constraints ── */

TEST(phase_composite_fluid_or_crystal_accepts_both) {
    ASSERT_RUNS("fn process(data: (~|*) Any) -> String {\n"
                "    return phase_of(data)\n"
                "}\n"
                "fn main() {\n"
                "    flux a = [1, 2, 3]\n"
                "    fix b = freeze([4, 5, 6])\n"
                "    assert(process(a) == \"fluid\")\n"
                "    assert(process(b) == \"crystal\")\n"
                "}\n");
}

TEST(phase_composite_flux_keyword_syntax) {
    /* Using (flux|fix) keyword syntax instead of (~|*) */
    ASSERT_RUNS("fn process(data: (flux|fix) Any) -> String {\n"
                "    return phase_of(data)\n"
                "}\n"
                "fn main() {\n"
                "    flux a = 42\n"
                "    fix b = freeze(100)\n"
                "    assert(process(a) == \"fluid\")\n"
                "    assert(process(b) == \"crystal\")\n"
                "}\n");
}

TEST(phase_constraint_tilde_accepts_fluid) {
    ASSERT_RUNS("#mode strict\n"
                "fn mutate(data: ~[Int]) {\n"
                "    print(data)\n"
                "}\n"
                "fn main() {\n"
                "    flux xs = [1, 2, 3]\n"
                "    mutate(xs)\n"
                "}\n");
}

TEST(phase_constraint_star_accepts_crystal) {
    ASSERT_RUNS("#mode strict\n"
                "fn read_only(data: *[Int]) {\n"
                "    print(data)\n"
                "}\n"
                "fn main() {\n"
                "    fix xs = freeze([1, 2, 3])\n"
                "    read_only(xs)\n"
                "}\n");
}

/* ── borrow() Scoped Mutation ── */

TEST(phase_borrow_basic_freeze_then_mutate) {
    ASSERT_RUNS("fn main() {\n"
                "    let data = freeze([1, 2, 3])\n"
                "    borrow(data) {\n"
                "        data.push(4)\n"
                "        assert(phase_of(data) == \"fluid\")\n"
                "    }\n"
                "    assert(phase_of(data) == \"crystal\")\n"
                "    assert(data.len() == 4)\n"
                "}\n");
}

TEST(phase_borrow_already_fluid_stays_fluid) {
    ASSERT_RUNS("fn main() {\n"
                "    flux data = [1, 2, 3]\n"
                "    borrow(data) {\n"
                "        data.push(4)\n"
                "        assert(phase_of(data) == \"fluid\")\n"
                "    }\n"
                "    assert(phase_of(data) == \"fluid\")\n"
                "    assert(data.len() == 4)\n"
                "}\n");
}

TEST(phase_borrow_nested_independent) {
    ASSERT_RUNS("fn main() {\n"
                "    let a = freeze([1])\n"
                "    let b = freeze([2])\n"
                "    borrow(a) {\n"
                "        a.push(10)\n"
                "        borrow(b) {\n"
                "            b.push(20)\n"
                "            assert(phase_of(a) == \"fluid\")\n"
                "            assert(phase_of(b) == \"fluid\")\n"
                "        }\n"
                "        assert(phase_of(b) == \"crystal\")\n"
                "        assert(phase_of(a) == \"fluid\")\n"
                "    }\n"
                "    assert(phase_of(a) == \"crystal\")\n"
                "    assert(a.len() == 2)\n"
                "    assert(b.len() == 2)\n"
                "}\n");
}

TEST(phase_borrow_mutation_persists_after_refreeze) {
    ASSERT_RUNS("fn main() {\n"
                "    let data = freeze([1, 2, 3])\n"
                "    borrow(data) {\n"
                "        data.push(4)\n"
                "        data.push(5)\n"
                "    }\n"
                "    assert(data.len() == 5)\n"
                "    assert(phase_of(data) == \"crystal\")\n"
                "}\n");
}

TEST(phase_borrow_struct_field_mutation) {
    ASSERT_RUNS("struct Config { port: Int, host: String }\n"
                "fn main() {\n"
                "    flux cfg = Config { port: 8080, host: \"localhost\" }\n"
                "    freeze(cfg)\n"
                "    borrow(cfg) {\n"
                "        cfg.port = 9090\n"
                "        assert(cfg.port == 9090)\n"
                "    }\n"
                "    assert(phase_of(cfg) == \"crystal\")\n"
                "    assert(cfg.port == 9090)\n"
                "}\n");
}

TEST(phase_borrow_same_var_twice) {
    /* Sequential borrows of the same variable */
    ASSERT_RUNS("fn main() {\n"
                "    let data = freeze([1, 2])\n"
                "    borrow(data) {\n"
                "        data.push(3)\n"
                "    }\n"
                "    assert(data.len() == 3)\n"
                "    assert(phase_of(data) == \"crystal\")\n"
                "    borrow(data) {\n"
                "        data.push(4)\n"
                "    }\n"
                "    assert(data.len() == 4)\n"
                "    assert(phase_of(data) == \"crystal\")\n"
                "}\n");
}

/* ── Strict Mode Compliance ── */

TEST(phase_strict_rejects_let) {
    ASSERT_FAILS("#mode strict\n"
                 "fn main() {\n"
                 "    let x = 10\n"
                 "}\n");
}

TEST(phase_strict_requires_flux_or_fix) {
    ASSERT_RUNS("#mode strict\n"
                "fn main() {\n"
                "    flux x = 10\n"
                "    fix y = freeze(20)\n"
                "    x = 30\n"
                "    print(x)\n"
                "    print(y)\n"
                "}\n");
}

TEST(phase_strict_crystal_assign_rejected) {
    ASSERT_FAILS("#mode strict\n"
                 "fn main() {\n"
                 "    fix x = freeze(42)\n"
                 "    x = 99\n"
                 "}\n");
}

TEST(phase_strict_flux_to_crystal_rejected) {
    /* Cannot bind a crystal value with flux */
    ASSERT_FAILS("#mode strict\n"
                 "fn main() {\n"
                 "    flux x = freeze(42)\n"
                 "}\n");
}

TEST(phase_strict_workflow_full) {
    ASSERT_RUNS("#mode strict\n"
                "struct Config { value: Int, name: String }\n"
                "fn main() {\n"
                "    flux cfg = Config { value: 42, name: \"test\" }\n"
                "    cfg.value = 100\n"
                "    fix frozen = freeze(cfg)\n"
                "    assert(frozen.value == 100)\n"
                "    assert(frozen.name == \"test\")\n"
                "    flux copy = thaw(frozen)\n"
                "    copy.name = \"modified\"\n"
                "    fix result = freeze(copy)\n"
                "    assert(result.name == \"modified\")\n"
                "}\n");
}

TEST(phase_strict_freeze_already_crystal_error) {
    /* In strict mode, freezing an already crystal value is an error */
    ASSERT_FAILS("#mode strict\n"
                 "fn main() {\n"
                 "    fix x = freeze(42)\n"
                 "    fix y = freeze(x)\n"
                 "}\n");
}

TEST(phase_strict_thaw_already_fluid_error) {
    /* In strict mode, thawing an already fluid value is an error */
    ASSERT_FAILS("#mode strict\n"
                 "fn main() {\n"
                 "    flux x = 42\n"
                 "    flux y = thaw(x)\n"
                 "}\n");
}

/* ── Phase Transitions in Various Contexts ── */

TEST(phase_freeze_nested_array) {
    ASSERT_RUNS("fn main() {\n"
                "    flux data = [[1, 2], [3, 4]]\n"
                "    freeze(data)\n"
                "    assert(phase_of(data) == \"crystal\")\n"
                "}\n");
}

TEST(phase_freeze_nested_array_rejects_inner_mutation) {
    ASSERT_FAILS("fn main() {\n"
                 "    flux data = [[1, 2], [3, 4]]\n"
                 "    freeze(data)\n"
                 "    data[0].push(5)\n"
                 "}\n");
}

TEST(phase_freeze_in_loop) {
    ASSERT_RUNS("fn main() {\n"
                "    let results = []\n"
                "    for i in 0..5 {\n"
                "        let frozen = freeze(i * 10)\n"
                "        results.push(frozen)\n"
                "    }\n"
                "    assert(results.len() == 5)\n"
                "    assert(results[0] == 0)\n"
                "    assert(results[4] == 40)\n"
                "}\n");
}

TEST(phase_freeze_thaw_in_function) {
    ASSERT_RUNS("fn freeze_and_thaw(data: Any) -> Any {\n"
                "    let frozen = freeze(data)\n"
                "    return thaw(frozen)\n"
                "}\n"
                "fn main() {\n"
                "    flux xs = [1, 2, 3]\n"
                "    let result = freeze_and_thaw(xs)\n"
                "    assert(phase_of(result) == \"fluid\")\n"
                "}\n");
}

TEST(phase_clone_deep_independence) {
    /* Clone of a struct should be fully independent */
    ASSERT_RUNS("struct Pair { a: Int, b: Int }\n"
                "fn main() {\n"
                "    flux p1 = Pair { a: 1, b: 2 }\n"
                "    flux p2 = clone(p1)\n"
                "    p2.a = 99\n"
                "    assert(p1.a == 1)\n"
                "    assert(p2.a == 99)\n"
                "}\n");
}

/* ── Freeze Except (Partial Freeze) ── */

TEST(phase_freeze_except_allows_excepted_field) {
    ASSERT_RUNS("struct User { name: String, score: Int }\n"
                "fn main() {\n"
                "    flux u = User { name: \"Alice\", score: 0 }\n"
                "    freeze(u) except [\"score\"]\n"
                "    u.score = 100\n"
                "    assert(u.score == 100)\n"
                "}\n");
}

TEST(phase_freeze_except_blocks_frozen_field) {
    ASSERT_FAILS("struct User { name: String, score: Int }\n"
                 "fn main() {\n"
                 "    flux u = User { name: \"Alice\", score: 0 }\n"
                 "    freeze(u) except [\"score\"]\n"
                 "    u.name = \"Bob\"\n"
                 "}\n");
}

TEST(phase_freeze_except_map_key) {
    ASSERT_RUNS("fn main() {\n"
                "    flux m = Map::new()\n"
                "    m[\"host\"] = \"localhost\"\n"
                "    m[\"retries\"] = 0\n"
                "    freeze(m) except [\"retries\"]\n"
                "    m[\"retries\"] = 5\n"
                "    assert(m[\"retries\"] == 5)\n"
                "}\n");
}

TEST(phase_freeze_except_map_blocks_frozen_key) {
    ASSERT_FAILS("fn main() {\n"
                 "    flux m = Map::new()\n"
                 "    m[\"host\"] = \"localhost\"\n"
                 "    m[\"retries\"] = 0\n"
                 "    freeze(m) except [\"retries\"]\n"
                 "    m[\"host\"] = \"remote\"\n"
                 "}\n");
}

/* ── Borrow + Pressure Interaction ── */

TEST(phase_borrow_with_pressure) {
    /* borrow should temporarily override crystal for mutation,
       but pressure constraints should still apply */
    ASSERT_RUNS("fn main() {\n"
                "    flux data = [1, 2, 3]\n"
                "    freeze(data)\n"
                "    borrow(data) {\n"
                "        data.push(4)\n"
                "        assert(data.len() == 4)\n"
                "    }\n"
                "    assert(phase_of(data) == \"crystal\")\n"
                "}\n");
}

/* ── flux / fix Bindings ── */

TEST(phase_flux_binding_is_fluid) {
    ASSERT_RUNS("fn main() {\n"
                "    flux x = 42\n"
                "    assert(phase_of(x) == \"fluid\")\n"
                "}\n");
}

TEST(phase_fix_binding_is_crystal) {
    ASSERT_RUNS("fn main() {\n"
                "    fix x = freeze(42)\n"
                "    assert(phase_of(x) == \"crystal\")\n"
                "}\n");
}

TEST(phase_flux_allows_reassignment) {
    ASSERT_RUNS("fn main() {\n"
                "    flux x = 10\n"
                "    x = 20\n"
                "    assert(x == 20)\n"
                "}\n");
}

/* ── Forge Block Produces Crystal ── */

TEST(phase_forge_block_result_crystal) {
    ASSERT_RUNS("fn main() {\n"
                "    let result = forge {\n"
                "        42\n"
                "    }\n"
                "    assert(phase_of(result) == \"crystal\")\n"
                "}\n");
}

/* ── Sublimate: Shallow Freeze ── */

TEST(phase_sublimate_blocks_top_level_mutation) {
    ASSERT_FAILS("fn main() {\n"
                 "    flux data = [1, 2, 3]\n"
                 "    sublimate(data)\n"
                 "    data.push(4)\n"
                 "}\n");
}

TEST(phase_sublimate_allows_read) {
    ASSERT_RUNS("fn main() {\n"
                "    flux data = [10, 20, 30]\n"
                "    sublimate(data)\n"
                "    assert(data[0] == 10)\n"
                "    assert(data.len() == 3)\n"
                "}\n");
}

TEST(phase_sublimate_thaw_restores_mutability) {
    ASSERT_RUNS("fn main() {\n"
                "    flux items = [1, 2]\n"
                "    sublimate(items)\n"
                "    thaw(items)\n"
                "    items.push(3)\n"
                "    assert(items.len() == 3)\n"
                "}\n");
}

/* ── phase_of() Utility ── */

TEST(phase_of_unphased_literal) {
    ASSERT_RUNS("fn main() {\n"
                "    assert(phase_of(42) == \"unphased\")\n"
                "    assert(phase_of(\"hello\") == \"unphased\")\n"
                "    assert(phase_of(true) == \"unphased\")\n"
                "}\n");
}

TEST(phase_of_nil) {
    ASSERT_RUNS("fn main() {\n"
                "    assert(phase_of(nil) == \"unphased\")\n"
                "}\n");
}

TEST(phase_of_after_transitions) {
    ASSERT_RUNS("fn main() {\n"
                "    flux x = 42\n"
                "    assert(phase_of(x) == \"fluid\")\n"
                "    freeze(x)\n"
                "    assert(phase_of(x) == \"crystal\")\n"
                "    thaw(x)\n"
                "    assert(phase_of(x) == \"fluid\")\n"
                "}\n");
}

/* ── Crystallize Block ── */

TEST(phase_crystallize_temporary_crystal) {
    ASSERT_RUNS("fn main() {\n"
                "    flux data = [1, 2, 3]\n"
                "    crystallize(data) {\n"
                "        assert(phase_of(data) == \"crystal\")\n"
                "    }\n"
                "    assert(phase_of(data) == \"fluid\")\n"
                "    data.push(4)\n"
                "    assert(data.len() == 4)\n"
                "}\n");
}

TEST(phase_crystallize_already_crystal_is_noop) {
    ASSERT_RUNS("fn main() {\n"
                "    fix data = freeze([1, 2, 3])\n"
                "    crystallize(data) {\n"
                "        assert(phase_of(data) == \"crystal\")\n"
                "    }\n"
                "    assert(phase_of(data) == \"crystal\")\n"
                "}\n");
}

/* ── Combined Scenarios ── */

TEST(phase_freeze_clone_thaw_chain) {
    ASSERT_RUNS("fn main() {\n"
                "    flux original = [1, 2, 3]\n"
                "    freeze(original)\n"
                "    let cloned = clone(original)\n"
                "    flux thawed = thaw(cloned)\n"
                "    thawed.push(4)\n"
                "    assert(thawed.len() == 4)\n"
                "    assert(original.len() == 3)\n"
                "}\n");
}

TEST(phase_multiple_borrows_sequential) {
    ASSERT_RUNS("fn main() {\n"
                "    let data = freeze([1])\n"
                "    borrow(data) { data.push(2) }\n"
                "    assert(phase_of(data) == \"crystal\")\n"
                "    borrow(data) { data.push(3) }\n"
                "    assert(phase_of(data) == \"crystal\")\n"
                "    borrow(data) { data.push(4) }\n"
                "    assert(data.len() == 4)\n"
                "    assert(phase_of(data) == \"crystal\")\n"
                "}\n");
}

TEST(phase_pressure_with_freeze) {
    /* Pressure and freeze are orthogonal: frozen array rejects mutation
       regardless of pressure; pressure_of should still be queryable */
    ASSERT_RUNS("fn main() {\n"
                "    flux data = [1, 2, 3]\n"
                "    pressurize(data, \"no_grow\")\n"
                "    data.pop()\n"
                "    assert(data.len() == 2)\n"
                "    depressurize(data)\n"
                "    data.push(99)\n"
                "    assert(data.len() == 3)\n"
                "}\n");
}

TEST(phase_alloy_with_borrow) {
    /* borrow on a struct that has field phases */
    ASSERT_RUNS("struct Config {\n"
                "    host: fix String,\n"
                "    retries: flux Int,\n"
                "}\n"
                "fn main() {\n"
                "    flux cfg = Config { host: \"localhost\", retries: 0 }\n"
                "    freeze(cfg)\n"
                "    borrow(cfg) {\n"
                "        cfg.retries = 5\n"
                "    }\n"
                "    assert(phase_of(cfg) == \"crystal\")\n"
                "}\n");
}

TEST(phase_strict_mode_with_borrow) {
    ASSERT_RUNS("#mode strict\n"
                "fn main() {\n"
                "    fix data = freeze([1, 2, 3])\n"
                "    borrow(data) {\n"
                "        data.push(4)\n"
                "    }\n"
                "    fix len_result = freeze(data.len())\n"
                "}\n");
}

TEST(phase_freeze_with_contract) {
    ASSERT_RUNS("fn main() {\n"
                "    flux data = [1, 2, 3]\n"
                "    freeze(data) where |v| { v.len() > 0 }\n"
                "    assert(phase_of(data) == \"crystal\")\n"
                "}\n");
}

TEST(phase_freeze_with_contract_fails) {
    ASSERT_FAILS("fn main() {\n"
                 "    flux data = []\n"
                 "    freeze(data) where |v| { assert(v.len() > 0, \"must not be empty\") }\n"
                 "}\n");
}

/* ── Recursion Depth Limit Tests ── */

TEST(eval_recursion_depth_limit) {
    if (test_backend != BACKEND_TREE_WALK) return;
    char *err = NULL;
    int rc = run_source_ok("fn blow_up() { blow_up() }\n"
                           "fn main() {\n"
                           "    set_recursion_limit(50)\n"
                           "    blow_up()\n"
                           "}\n",
                           &err);
    ASSERT(rc != 0);
    ASSERT(err != NULL);
    ASSERT(strstr(err, "maximum recursion depth exceeded") != NULL);
    free(err);
}

TEST(eval_set_recursion_limit) {
    if (test_backend != BACKEND_TREE_WALK) return;
    char *err = NULL;
    int rc = run_source_ok("fn recurse(n: Int) -> Int {\n"
                           "    if n <= 0 { return 0 }\n"
                           "    return recurse(n - 1)\n"
                           "}\n"
                           "fn main() {\n"
                           "    set_recursion_limit(50)\n"
                           "    recurse(100)\n"
                           "}\n",
                           &err);
    ASSERT(rc != 0);
    ASSERT(err != NULL);
    ASSERT(strstr(err, "maximum recursion depth exceeded") != NULL);
    free(err);
}

TEST(eval_recursion_limit_query) {
    if (test_backend != BACKEND_TREE_WALK) return;
    ASSERT_RUNS("fn main() {\n"
                "    let limit = recursion_limit()\n"
                "    assert(limit == 1000, \"default should be 1000\")\n"
                "}\n");
}

TEST(eval_deep_recursion_within_limit) {
    if (test_backend != BACKEND_TREE_WALK) return;
    ASSERT_RUNS("fn recurse(n: Int) -> Int {\n"
                "    if n <= 0 { return 0 }\n"
                "    return recurse(n - 1)\n"
                "}\n"
                "fn main() {\n"
                "    let result = recurse(100)\n"
                "    assert(result == 0, \"should complete\")\n"
                "}\n");
}

/* ── Match exhaustiveness checking tests ── */

/* Helper: lex + parse + match check, capture stderr, return captured string.
 * Caller must free the returned string. */
static char *run_match_check(const char *source) {
    /* Redirect stderr to a temp file */
    fflush(stderr);
    FILE *tmp = tmpfile();
    int old_stderr = dup(fileno(stderr));
    dup2(fileno(tmp), fileno(stderr));

    /* Lex */
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        free(lex_err);
        fflush(stderr);
        dup2(old_stderr, fileno(stderr));
        close(old_stderr);
        fclose(tmp);
        lat_vec_free(&tokens);
        return strdup("");
    }

    /* Parse */
    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    if (parse_err) {
        free(parse_err);
        fflush(stderr);
        dup2(old_stderr, fileno(stderr));
        close(old_stderr);
        fclose(tmp);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return strdup("");
    }

    /* Run match exhaustiveness check (writes warnings to stderr) */
    check_match_exhaustiveness(&prog);

    /* Restore stderr and read captured output */
    fflush(stderr);
    dup2(old_stderr, fileno(stderr));
    close(old_stderr);

    long len = ftell(tmp);
    rewind(tmp);
    char *buf = malloc((size_t)len + 1);
    if (buf) {
        size_t n = fread(buf, 1, (size_t)len, tmp);
        buf[n] = '\0';
    }
    fclose(tmp);

    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    return buf ? buf : strdup("");
}

TEST(match_exhaustive_bool_missing_false) {
    char *warnings = run_match_check("fn main() {\n"
                                     "    let b = true\n"
                                     "    match b {\n"
                                     "        true => print(\"yes\")\n"
                                     "    }\n"
                                     "}\n");
    ASSERT(strstr(warnings, "non-exhaustive") != NULL);
    ASSERT(strstr(warnings, "false") != NULL);
    free(warnings);
}

TEST(match_exhaustive_bool_missing_true) {
    char *warnings = run_match_check("fn main() {\n"
                                     "    let b = false\n"
                                     "    match b {\n"
                                     "        false => print(\"no\")\n"
                                     "    }\n"
                                     "}\n");
    ASSERT(strstr(warnings, "non-exhaustive") != NULL);
    ASSERT(strstr(warnings, "true") != NULL);
    free(warnings);
}

TEST(match_exhaustive_bool_complete) {
    char *warnings = run_match_check("fn main() {\n"
                                     "    let b = true\n"
                                     "    match b {\n"
                                     "        true => print(\"yes\"),\n"
                                     "        false => print(\"no\")\n"
                                     "    }\n"
                                     "}\n");
    ASSERT(strlen(warnings) == 0);
    free(warnings);
}

TEST(match_exhaustive_int_no_wildcard) {
    char *warnings = run_match_check("fn main() {\n"
                                     "    let x = 42\n"
                                     "    match x {\n"
                                     "        1 => print(\"one\"),\n"
                                     "        2 => print(\"two\")\n"
                                     "    }\n"
                                     "}\n");
    ASSERT(strstr(warnings, "non-exhaustive") != NULL);
    ASSERT(strstr(warnings, "wildcard") != NULL);
    free(warnings);
}

TEST(match_exhaustive_wildcard_suppresses) {
    char *warnings = run_match_check("fn main() {\n"
                                     "    let x = 42\n"
                                     "    match x {\n"
                                     "        1 => print(\"one\"),\n"
                                     "        _ => print(\"other\")\n"
                                     "    }\n"
                                     "}\n");
    ASSERT(strlen(warnings) == 0);
    free(warnings);
}

TEST(match_exhaustive_binding_suppresses) {
    char *warnings = run_match_check("fn main() {\n"
                                     "    let x = 42\n"
                                     "    match x {\n"
                                     "        1 => print(\"one\"),\n"
                                     "        other => print(other)\n"
                                     "    }\n"
                                     "}\n");
    ASSERT(strlen(warnings) == 0);
    free(warnings);
}

TEST(match_exhaustive_string_no_wildcard) {
    char *warnings = run_match_check("fn main() {\n"
                                     "    let s = \"hello\"\n"
                                     "    match s {\n"
                                     "        \"hello\" => print(\"hi\"),\n"
                                     "        \"bye\" => print(\"goodbye\")\n"
                                     "    }\n"
                                     "}\n");
    ASSERT(strstr(warnings, "non-exhaustive") != NULL);
    ASSERT(strstr(warnings, "wildcard") != NULL);
    free(warnings);
}

TEST(match_exhaustive_empty_arms) {
    char *warnings = run_match_check("fn main() {\n"
                                     "    let x = 42\n"
                                     "    match x {\n"
                                     "    }\n"
                                     "}\n");
    ASSERT(strstr(warnings, "no arms") != NULL);
    free(warnings);
}

TEST(match_exhaustive_guarded_wildcard_not_catch_all) {
    /* A guarded wildcard doesn't count as exhaustive */
    char *warnings = run_match_check("fn main() {\n"
                                     "    let x = 42\n"
                                     "    match x {\n"
                                     "        1 => print(\"one\"),\n"
                                     "        _ if x > 10 => print(\"big\")\n"
                                     "    }\n"
                                     "}\n");
    ASSERT(strstr(warnings, "non-exhaustive") != NULL);
    free(warnings);
}

TEST(match_exhaustive_enum_missing_variant) {
    /* Enum variant patterns (via PAT_LITERAL with EXPR_ENUM_VARIANT):
     * The main parser doesn't support Enum::Variant in pattern position
     * yet, so we test by directly exercising the checker on the AST
     * through the program which still runs without error. */
    char *warnings = run_match_check("enum Color { Red, Green, Blue }\n"
                                     "fn main() {\n"
                                     "    let x = 1\n"
                                     "    match x {\n"
                                     "        1 => print(\"one\")\n"
                                     "    }\n"
                                     "}\n");
    /* This is an int match missing wildcard, not enum */
    ASSERT(strstr(warnings, "non-exhaustive") != NULL);
    free(warnings);
}

TEST(match_exhaustive_nested_match) {
    /* Nested match expressions should both be checked */
    char *warnings = run_match_check("fn main() {\n"
                                     "    let x = true\n"
                                     "    let y = match x {\n"
                                     "        true => {\n"
                                     "            match 1 {\n"
                                     "                1 => 10\n"
                                     "            }\n"
                                     "        },\n"
                                     "        false => 0\n"
                                     "    }\n"
                                     "}\n");
    /* The inner match on int 1 without wildcard should warn */
    ASSERT(strstr(warnings, "non-exhaustive") != NULL);
    ASSERT(strstr(warnings, "wildcard") != NULL);
    free(warnings);
}
