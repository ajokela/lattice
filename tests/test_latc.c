#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lattice.h"
#include "lexer.h"
#include "parser.h"
#include "stackcompiler.h"
#include "stackvm.h"
#include "regvm.h"
#include "latc.h"
#include "runtime.h"
#include "value.h"

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

/* ── Helper: generate a unique temp file path, write nothing ── */
static char *make_temp_path(const char *suffix) {
    static int counter = 0;
    char *path = malloc(256);
    snprintf(path, 256, "/tmp/test_latc_%d_%d%s", (int)getpid(), counter++, suffix);
    return path;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Stack VM round-trip helper:
 *   source -> lex -> parse -> stack_compile -> chunk_save -> chunk_load
 *          -> stackvm_run -> verify no error -> cleanup
 * Returns 0 on success, non-zero on failure (sets *err_out).
 * ══════════════════════════════════════════════════════════════════════════ */
static int stack_roundtrip(const char *source, char **err_out) {
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

    /* Compile */
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

    /* Save to temp file */
    char *path = make_temp_path(".latc");
    int save_rc = chunk_save(chunk, path);
    chunk_free(chunk);
    if (save_rc != 0) {
        if (err_out) *err_out = strdup("chunk_save failed");
        free(path);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 1;
    }

    /* Load from temp file */
    char *load_err = NULL;
    Chunk *loaded = chunk_load(path, &load_err);
    unlink(path);
    free(path);
    if (!loaded) {
        if (err_out) *err_out = load_err;
        else free(load_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 1;
    }

    /* Run the deserialized chunk */
    LatRuntime rt;
    lat_runtime_init(&rt);
    StackVM vm;
    stackvm_init(&vm, &rt);
    LatValue result;
    StackVMResult vm_res = stackvm_run(&vm, loaded, &result);
    int rc = 0;
    if (vm_res != STACKVM_OK) {
        if (err_out) *err_out = strdup(vm.error ? vm.error : "vm error");
        rc = 1;
    } else {
        value_free(&result);
    }
    stackvm_free(&vm);
    lat_runtime_free(&rt);
    chunk_free(loaded);

    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    return rc;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Stack VM in-memory round-trip helper (serialize/deserialize without file):
 *   source -> lex -> parse -> stack_compile -> chunk_serialize
 *          -> chunk_deserialize -> stackvm_run -> verify -> cleanup
 * ══════════════════════════════════════════════════════════════════════════ */
static int stack_roundtrip_mem(const char *source, char **err_out) {
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

    /* Compile */
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

    /* Serialize to memory */
    size_t data_len;
    uint8_t *data = chunk_serialize(chunk, &data_len);
    chunk_free(chunk);
    if (!data) {
        if (err_out) *err_out = strdup("chunk_serialize returned NULL");
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 1;
    }

    /* Deserialize from memory */
    char *deser_err = NULL;
    Chunk *loaded = chunk_deserialize(data, data_len, &deser_err);
    free(data);
    if (!loaded) {
        if (err_out) *err_out = deser_err;
        else free(deser_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 1;
    }

    /* Run the deserialized chunk */
    LatRuntime rt;
    lat_runtime_init(&rt);
    StackVM vm;
    stackvm_init(&vm, &rt);
    LatValue result;
    StackVMResult vm_res = stackvm_run(&vm, loaded, &result);
    int rc = 0;
    if (vm_res != STACKVM_OK) {
        if (err_out) *err_out = strdup(vm.error ? vm.error : "vm error");
        rc = 1;
    } else {
        value_free(&result);
    }
    stackvm_free(&vm);
    lat_runtime_free(&rt);
    chunk_free(loaded);

    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    return rc;
}

/* ══════════════════════════════════════════════════════════════════════════
 * RegVM round-trip helper:
 *   source -> lex -> parse -> reg_compile -> regchunk_save -> regchunk_load
 *          -> regvm_run -> verify no error -> cleanup
 * ══════════════════════════════════════════════════════════════════════════ */
static int reg_roundtrip(const char *source, char **err_out) {
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

    /* Compile */
    value_set_heap(NULL);
    value_set_arena(NULL);

    char *comp_err = NULL;
    RegChunk *rchunk = reg_compile(&prog, &comp_err);
    if (!rchunk) {
        if (err_out) *err_out = comp_err;
        else free(comp_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 1;
    }

    /* Save to temp file */
    char *path = make_temp_path(".rlatc");
    int save_rc = regchunk_save(rchunk, path);
    regchunk_free(rchunk);
    if (save_rc != 0) {
        if (err_out) *err_out = strdup("regchunk_save failed");
        free(path);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 1;
    }

    /* Load from temp file */
    char *load_err = NULL;
    RegChunk *loaded = regchunk_load(path, &load_err);
    unlink(path);
    free(path);
    if (!loaded) {
        if (err_out) *err_out = load_err;
        else free(load_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 1;
    }

    /* Run the deserialized chunk */
    LatRuntime rt;
    lat_runtime_init(&rt);
    RegVM rvm;
    regvm_init(&rvm, &rt);
    LatValue result;
    RegVMResult rvm_res = regvm_run(&rvm, loaded, &result);
    int rc = 0;
    if (rvm_res != REGVM_OK) {
        if (err_out) *err_out = strdup(rvm.error ? rvm.error : "regvm error");
        rc = 1;
    } else {
        value_free(&result);
    }
    regvm_free(&rvm);
    lat_runtime_free(&rt);
    regchunk_free(loaded);

    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    return rc;
}

/* ══════════════════════════════════════════════════════════════════════════
 * RegVM in-memory round-trip helper:
 *   source -> lex -> parse -> reg_compile -> regchunk_serialize
 *          -> regchunk_deserialize -> regvm_run -> verify -> cleanup
 * ══════════════════════════════════════════════════════════════════════════ */
static int reg_roundtrip_mem(const char *source, char **err_out) {
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

    /* Compile */
    value_set_heap(NULL);
    value_set_arena(NULL);

    char *comp_err = NULL;
    RegChunk *rchunk = reg_compile(&prog, &comp_err);
    if (!rchunk) {
        if (err_out) *err_out = comp_err;
        else free(comp_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 1;
    }

    /* Serialize to memory */
    size_t data_len;
    uint8_t *data = regchunk_serialize(rchunk, &data_len);
    regchunk_free(rchunk);
    if (!data) {
        if (err_out) *err_out = strdup("regchunk_serialize returned NULL");
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 1;
    }

    /* Deserialize from memory */
    char *deser_err = NULL;
    RegChunk *loaded = regchunk_deserialize(data, data_len, &deser_err);
    free(data);
    if (!loaded) {
        if (err_out) *err_out = deser_err;
        else free(deser_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 1;
    }

    /* Run the deserialized chunk */
    LatRuntime rt;
    lat_runtime_init(&rt);
    RegVM rvm;
    regvm_init(&rvm, &rt);
    LatValue result;
    RegVMResult rvm_res = regvm_run(&rvm, loaded, &result);
    int rc = 0;
    if (rvm_res != REGVM_OK) {
        if (err_out) *err_out = strdup(rvm.error ? rvm.error : "regvm error");
        rc = 1;
    } else {
        value_free(&result);
    }
    regvm_free(&rvm);
    lat_runtime_free(&rt);
    regchunk_free(loaded);

    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    return rc;
}

/* Convenience macros for round-trip assertions */
#define ASSERT_STACK_ROUNDTRIP(src)                                                            \
    do {                                                                                       \
        char *_err = NULL;                                                                     \
        int _rc = stack_roundtrip(src, &_err);                                                 \
        if (_rc != 0) {                                                                        \
            fprintf(stderr, "  FAIL: %s:%d: stack roundtrip failed: %s\n", __FILE__, __LINE__, \
                    _err ? _err : "(unknown)");                                                \
            free(_err);                                                                        \
            test_current_failed = 1;                                                           \
            return;                                                                            \
        }                                                                                      \
    } while (0)

#define ASSERT_STACK_ROUNDTRIP_MEM(src)                                                            \
    do {                                                                                           \
        char *_err = NULL;                                                                         \
        int _rc = stack_roundtrip_mem(src, &_err);                                                 \
        if (_rc != 0) {                                                                            \
            fprintf(stderr, "  FAIL: %s:%d: stack mem roundtrip failed: %s\n", __FILE__, __LINE__, \
                    _err ? _err : "(unknown)");                                                    \
            free(_err);                                                                            \
            test_current_failed = 1;                                                               \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_REG_ROUNDTRIP(src)                                                            \
    do {                                                                                     \
        char *_err = NULL;                                                                   \
        int _rc = reg_roundtrip(src, &_err);                                                 \
        if (_rc != 0) {                                                                      \
            fprintf(stderr, "  FAIL: %s:%d: reg roundtrip failed: %s\n", __FILE__, __LINE__, \
                    _err ? _err : "(unknown)");                                              \
            free(_err);                                                                      \
            test_current_failed = 1;                                                         \
            return;                                                                          \
        }                                                                                    \
    } while (0)

#define ASSERT_REG_ROUNDTRIP_MEM(src)                                                            \
    do {                                                                                         \
        char *_err = NULL;                                                                       \
        int _rc = reg_roundtrip_mem(src, &_err);                                                 \
        if (_rc != 0) {                                                                          \
            fprintf(stderr, "  FAIL: %s:%d: reg mem roundtrip failed: %s\n", __FILE__, __LINE__, \
                    _err ? _err : "(unknown)");                                                  \
            free(_err);                                                                          \
            test_current_failed = 1;                                                             \
            return;                                                                              \
        }                                                                                        \
    } while (0)

/* ══════════════════════════════════════════════════════════════════════════
 * Stack VM Round-Trip Tests
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(latc_stack_basic_arithmetic) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    print(1 + 2)\n"
                           "    print(10 - 3)\n"
                           "    print(4 * 5)\n"
                           "    print(15 / 3)\n"
                           "    print(17 % 5)\n"
                           "}\n");
}

TEST(latc_stack_float_arithmetic) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    print(3.14 + 2.86)\n"
                           "    print(1.5 * 2.0)\n"
                           "}\n");
}

TEST(latc_stack_string_ops) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    let s = \"hello\"\n"
                           "    print(s)\n"
                           "    print(s + \" world\")\n"
                           "    print(s.len())\n"
                           "}\n");
}

TEST(latc_stack_string_interpolation) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    let name = \"Lattice\"\n"
                           "    let ver = 3\n"
                           "    print(\"Hello ${name} v${ver}\")\n"
                           "}\n");
}

TEST(latc_stack_boolean_ops) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    print(true && false)\n"
                           "    print(true || false)\n"
                           "    print(!true)\n"
                           "    print(1 == 1)\n"
                           "    print(1 != 2)\n"
                           "}\n");
}

TEST(latc_stack_variables_and_assignment) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    let x = 10\n"
                           "    let y = 20\n"
                           "    print(x + y)\n"
                           "    let z = x * y\n"
                           "    print(z)\n"
                           "}\n");
}

TEST(latc_stack_if_else) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    let x = 42\n"
                           "    if x > 10 {\n"
                           "        print(\"big\")\n"
                           "    } else {\n"
                           "        print(\"small\")\n"
                           "    }\n"
                           "}\n");
}

TEST(latc_stack_while_loop) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    let i = 0\n"
                           "    while i < 5 {\n"
                           "        print(i)\n"
                           "        i = i + 1\n"
                           "    }\n"
                           "}\n");
}

TEST(latc_stack_for_loop) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    for i in 0..5 {\n"
                           "        print(i)\n"
                           "    }\n"
                           "}\n");
}

TEST(latc_stack_functions) {
    ASSERT_STACK_ROUNDTRIP("fn add(a: Int, b: Int) -> Int {\n"
                           "    return a + b\n"
                           "}\n"
                           "fn main() {\n"
                           "    print(add(3, 4))\n"
                           "}\n");
}

TEST(latc_stack_closures) {
    ASSERT_STACK_ROUNDTRIP("fn make_adder(n: Int) {\n"
                           "    return |x| { n + x }\n"
                           "}\n"
                           "fn main() {\n"
                           "    let add5 = make_adder(5)\n"
                           "    print(add5(10))\n"
                           "}\n");
}

TEST(latc_stack_recursion) {
    ASSERT_STACK_ROUNDTRIP("fn fib(n: Int) -> Int {\n"
                           "    if n <= 1 { return n }\n"
                           "    return fib(n - 1) + fib(n - 2)\n"
                           "}\n"
                           "fn main() {\n"
                           "    print(fib(10))\n"
                           "}\n");
}

TEST(latc_stack_arrays) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    let arr = [1, 2, 3, 4, 5]\n"
                           "    print(arr.len())\n"
                           "    print(arr[0])\n"
                           "    print(arr[4])\n"
                           "    arr.push(6)\n"
                           "    print(arr.len())\n"
                           "}\n");
}

TEST(latc_stack_structs) {
    ASSERT_STACK_ROUNDTRIP("struct Point { x: Int, y: Int }\n"
                           "fn main() {\n"
                           "    let p = Point { x: 10, y: 20 }\n"
                           "    print(p.x)\n"
                           "    print(p.y)\n"
                           "}\n");
}

TEST(latc_stack_enums) {
    ASSERT_STACK_ROUNDTRIP("enum Color { Red, Green, Blue }\n"
                           "fn main() {\n"
                           "    let c = Color::Red\n"
                           "    match c.variant_name() {\n"
                           "        \"Red\" => print(\"red\"),\n"
                           "        \"Green\" => print(\"green\"),\n"
                           "        \"Blue\" => print(\"blue\")\n"
                           "    }\n"
                           "}\n");
}

TEST(latc_stack_match_expression) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    let x = 2\n"
                           "    match x {\n"
                           "        1 => print(\"one\"),\n"
                           "        2 => print(\"two\"),\n"
                           "        _ => print(\"other\")\n"
                           "    }\n"
                           "}\n");
}

TEST(latc_stack_multiple_functions) {
    ASSERT_STACK_ROUNDTRIP("fn square(x: Int) -> Int { return x * x }\n"
                           "fn cube(x: Int) -> Int { return x * x * x }\n"
                           "fn max_val(a: Int, b: Int) -> Int {\n"
                           "    if a > b { return a }\n"
                           "    return b\n"
                           "}\n"
                           "fn main() {\n"
                           "    print(square(5))\n"
                           "    print(cube(3))\n"
                           "    print(max_val(10, 20))\n"
                           "}\n");
}

TEST(latc_stack_nil_and_unit) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    let x = nil\n"
                           "    print(x)\n"
                           "}\n");
}

TEST(latc_stack_break_continue) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    let sum = 0\n"
                           "    for i in 0..10 {\n"
                           "        if i == 5 { break }\n"
                           "        if i % 2 == 0 { continue }\n"
                           "        sum = sum + i\n"
                           "    }\n"
                           "    print(sum)\n"
                           "}\n");
}

/* ── Stack VM: in-memory round-trip (serialize/deserialize, no file I/O) ── */

TEST(latc_stack_mem_basic) {
    ASSERT_STACK_ROUNDTRIP_MEM("fn main() {\n"
                               "    print(42)\n"
                               "}\n");
}

TEST(latc_stack_mem_closures) {
    ASSERT_STACK_ROUNDTRIP_MEM("fn make_counter() {\n"
                               "    flux count = 0\n"
                               "    return |_| {\n"
                               "        count = count + 1\n"
                               "        count\n"
                               "    }\n"
                               "}\n"
                               "fn main() {\n"
                               "    let c = make_counter()\n"
                               "    print(c(0))\n"
                               "    print(c(0))\n"
                               "    print(c(0))\n"
                               "}\n");
}

TEST(latc_stack_mem_strings_and_arrays) {
    ASSERT_STACK_ROUNDTRIP_MEM("fn main() {\n"
                               "    let names = [\"alice\", \"bob\", \"charlie\"]\n"
                               "    for name in names {\n"
                               "        print(name)\n"
                               "    }\n"
                               "}\n");
}

/* ── Stack VM: many constants (exercises wide constant opcodes) ── */

TEST(latc_stack_many_constants) {
    /* Build source with many distinct string constants to test constant pool
     * handling. If the compiler emits > 256 constants, this exercises
     * OP_CONSTANT_16 and wide global opcodes. */
    char source[16384];
    int off = 0;
    off += snprintf(source + off, sizeof(source) - (size_t)off, "fn main() {\n");
    for (int i = 0; i < 300; i++) {
        off += snprintf(source + off, sizeof(source) - (size_t)off, "    let v%d = \"str_%d\"\n", i, i);
    }
    off += snprintf(source + off, sizeof(source) - (size_t)off,
                    "    print(v0)\n"
                    "    print(v299)\n"
                    "}\n");
    ASSERT_STACK_ROUNDTRIP(source);
}

/* ══════════════════════════════════════════════════════════════════════════
 * RegVM Round-Trip Tests
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(latc_reg_basic_arithmetic) {
    ASSERT_REG_ROUNDTRIP("fn main() {\n"
                         "    print(1 + 2)\n"
                         "    print(10 - 3)\n"
                         "    print(4 * 5)\n"
                         "    print(15 / 3)\n"
                         "}\n");
}

TEST(latc_reg_float_arithmetic) {
    ASSERT_REG_ROUNDTRIP("fn main() {\n"
                         "    print(3.14 + 2.86)\n"
                         "    print(1.5 * 2.0)\n"
                         "}\n");
}

TEST(latc_reg_string_ops) {
    ASSERT_REG_ROUNDTRIP("fn main() {\n"
                         "    let s = \"hello\"\n"
                         "    print(s)\n"
                         "    print(s + \" world\")\n"
                         "}\n");
}

TEST(latc_reg_boolean_ops) {
    ASSERT_REG_ROUNDTRIP("fn main() {\n"
                         "    print(true && false)\n"
                         "    print(true || false)\n"
                         "    print(!true)\n"
                         "}\n");
}

TEST(latc_reg_variables) {
    ASSERT_REG_ROUNDTRIP("fn main() {\n"
                         "    let x = 10\n"
                         "    let y = 20\n"
                         "    print(x + y)\n"
                         "}\n");
}

TEST(latc_reg_if_else) {
    ASSERT_REG_ROUNDTRIP("fn main() {\n"
                         "    let x = 42\n"
                         "    if x > 10 {\n"
                         "        print(\"big\")\n"
                         "    } else {\n"
                         "        print(\"small\")\n"
                         "    }\n"
                         "}\n");
}

TEST(latc_reg_while_loop) {
    ASSERT_REG_ROUNDTRIP("fn main() {\n"
                         "    let i = 0\n"
                         "    while i < 5 {\n"
                         "        print(i)\n"
                         "        i = i + 1\n"
                         "    }\n"
                         "}\n");
}

TEST(latc_reg_for_loop) {
    ASSERT_REG_ROUNDTRIP("fn main() {\n"
                         "    for i in 0..5 {\n"
                         "        print(i)\n"
                         "    }\n"
                         "}\n");
}

TEST(latc_reg_functions) {
    ASSERT_REG_ROUNDTRIP("fn add(a: Int, b: Int) -> Int {\n"
                         "    return a + b\n"
                         "}\n"
                         "fn main() {\n"
                         "    print(add(3, 4))\n"
                         "}\n");
}

TEST(latc_reg_closures) {
    ASSERT_REG_ROUNDTRIP("fn make_adder(n: Int) {\n"
                         "    return |x| { n + x }\n"
                         "}\n"
                         "fn main() {\n"
                         "    let add5 = make_adder(5)\n"
                         "    print(add5(10))\n"
                         "}\n");
}

TEST(latc_reg_recursion) {
    ASSERT_REG_ROUNDTRIP("fn fib(n: Int) -> Int {\n"
                         "    if n <= 1 { return n }\n"
                         "    return fib(n - 1) + fib(n - 2)\n"
                         "}\n"
                         "fn main() {\n"
                         "    print(fib(10))\n"
                         "}\n");
}

TEST(latc_reg_arrays) {
    ASSERT_REG_ROUNDTRIP("fn main() {\n"
                         "    let arr = [1, 2, 3, 4, 5]\n"
                         "    print(arr.len())\n"
                         "    print(arr[0])\n"
                         "}\n");
}

/* NOTE: RegVM struct/enum round-trip tests are omitted because the RegVM
 * compiler stores struct field metadata as VAL_ARRAY constants in the constant
 * pool, and the .rlatc serialization format does not yet support VAL_ARRAY
 * constants (they are written as TAG_NIL fallback). This causes struct field
 * count mismatches and enum metadata loss after deserialization.
 * The stack VM tests above cover struct/enum serialization because the stack
 * compiler uses a different metadata mechanism. */

TEST(latc_reg_multiple_functions) {
    ASSERT_REG_ROUNDTRIP("fn square(x: Int) -> Int { return x * x }\n"
                         "fn cube(x: Int) -> Int { return x * x * x }\n"
                         "fn main() {\n"
                         "    print(square(5))\n"
                         "    print(cube(3))\n"
                         "}\n");
}

/* ── RegVM: in-memory round-trip ── */

TEST(latc_reg_mem_basic) {
    ASSERT_REG_ROUNDTRIP_MEM("fn main() {\n"
                             "    print(42)\n"
                             "}\n");
}

TEST(latc_reg_mem_closures) {
    ASSERT_REG_ROUNDTRIP_MEM("fn make_counter() {\n"
                             "    flux count = 0\n"
                             "    return |_| {\n"
                             "        count = count + 1\n"
                             "        count\n"
                             "    }\n"
                             "}\n"
                             "fn main() {\n"
                             "    let c = make_counter()\n"
                             "    print(c(0))\n"
                             "    print(c(0))\n"
                             "}\n");
}

/* ── RegVM: many constants ── */

TEST(latc_reg_many_constants) {
    /* RegVM has a 256-register limit per frame, so we use fewer variables
     * but still generate many distinct constants to test the constant pool. */
    char source[16384];
    int off = 0;
    off += snprintf(source + off, sizeof(source) - (size_t)off, "fn main() {\n");
    for (int i = 0; i < 100; i++) {
        off += snprintf(source + off, sizeof(source) - (size_t)off, "    let v%d = \"str_%d\"\n", i, i);
    }
    off += snprintf(source + off, sizeof(source) - (size_t)off,
                    "    print(v0)\n"
                    "    print(v99)\n"
                    "}\n");
    ASSERT_REG_ROUNDTRIP(source);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Error Cases: deserialization of invalid data
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(latc_stack_load_nonexistent_file) {
    char *err = NULL;
    Chunk *c = chunk_load("/tmp/nonexistent_test_file_42.latc", &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

TEST(latc_stack_load_empty_file) {
    char *path = make_temp_path(".latc");
    /* Create empty file */
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    fclose(f);

    char *err = NULL;
    Chunk *c = chunk_load(path, &err);
    unlink(path);
    free(path);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

TEST(latc_stack_load_garbage_bytes) {
    char *path = make_temp_path(".latc");
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    /* Write random garbage that does not match LATC magic */
    const char garbage[] = "THIS_IS_NOT_VALID_BYTECODE_DATA_1234567890";
    fwrite(garbage, 1, sizeof(garbage) - 1, f);
    fclose(f);

    char *err = NULL;
    Chunk *c = chunk_load(path, &err);
    unlink(path);
    free(path);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

TEST(latc_stack_deserialize_truncated) {
    /* Provide just the magic + version but no chunk data */
    uint8_t data[] = {'L', 'A', 'T', 'C', 0x01, 0x00, 0x00, 0x00};
    char *err = NULL;
    Chunk *c = chunk_deserialize(data, sizeof(data), &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

TEST(latc_stack_deserialize_bad_magic) {
    uint8_t data[] = {'N', 'O', 'P', 'E', 0x01, 0x00, 0x00, 0x00};
    char *err = NULL;
    Chunk *c = chunk_deserialize(data, sizeof(data), &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

TEST(latc_stack_deserialize_bad_version) {
    /* Valid magic but wrong version */
    uint8_t data[] = {'L', 'A', 'T', 'C', 0xFF, 0x00, 0x00, 0x00};
    char *err = NULL;
    Chunk *c = chunk_deserialize(data, sizeof(data), &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

TEST(latc_reg_load_nonexistent_file) {
    char *err = NULL;
    RegChunk *c = regchunk_load("/tmp/nonexistent_test_file_42.rlatc", &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

TEST(latc_reg_load_empty_file) {
    char *path = make_temp_path(".rlatc");
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    fclose(f);

    char *err = NULL;
    RegChunk *c = regchunk_load(path, &err);
    unlink(path);
    free(path);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

TEST(latc_reg_load_garbage_bytes) {
    char *path = make_temp_path(".rlatc");
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    const char garbage[] = "THIS_IS_NOT_VALID_BYTECODE_DATA_1234567890";
    fwrite(garbage, 1, sizeof(garbage) - 1, f);
    fclose(f);

    char *err = NULL;
    RegChunk *c = regchunk_load(path, &err);
    unlink(path);
    free(path);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

TEST(latc_reg_deserialize_truncated) {
    /* RLATC magic + version, but no chunk data */
    uint8_t data[] = {'R', 'L', 'A', 'T', 0x02, 0x00, 0x00, 0x00};
    char *err = NULL;
    RegChunk *c = regchunk_deserialize(data, sizeof(data), &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

TEST(latc_reg_deserialize_bad_magic) {
    uint8_t data[] = {'N', 'O', 'P', 'E', 0x02, 0x00, 0x00, 0x00};
    char *err = NULL;
    RegChunk *c = regchunk_deserialize(data, sizeof(data), &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

TEST(latc_reg_deserialize_bad_version) {
    uint8_t data[] = {'R', 'L', 'A', 'T', 0xFF, 0x00, 0x00, 0x00};
    char *err = NULL;
    RegChunk *c = regchunk_deserialize(data, sizeof(data), &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Serialization data integrity tests (verify bytes survive round-trip)
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(latc_stack_serialize_deserialize_preserves_code) {
    /* Compile a program, serialize, deserialize, check code_len matches */
    const char *source = "fn main() {\n"
                         "    let x = 10\n"
                         "    let y = 20\n"
                         "    print(x + y)\n"
                         "}\n";

    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    ASSERT(lex_err == NULL);

    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    ASSERT(parse_err == NULL);

    value_set_heap(NULL);
    value_set_arena(NULL);

    char *comp_err = NULL;
    Chunk *original = stack_compile(&prog, &comp_err);
    ASSERT(original != NULL);

    size_t data_len;
    uint8_t *data = chunk_serialize(original, &data_len);
    ASSERT(data != NULL);
    ASSERT(data_len > 8); /* At least header size */

    char *deser_err = NULL;
    Chunk *restored = chunk_deserialize(data, data_len, &deser_err);
    ASSERT(restored != NULL);
    ASSERT(deser_err == NULL);

    /* Verify code length matches */
    ASSERT_EQ_INT(original->code_len, restored->code_len);

    /* Verify bytecode matches byte-for-byte */
    ASSERT(memcmp(original->code, restored->code, original->code_len) == 0);

    /* Verify constant count matches */
    ASSERT_EQ_INT(original->const_len, restored->const_len);

    free(data);
    chunk_free(original);
    chunk_free(restored);

    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
}

TEST(latc_reg_serialize_deserialize_preserves_code) {
    const char *source = "fn main() {\n"
                         "    let x = 10\n"
                         "    let y = 20\n"
                         "    print(x + y)\n"
                         "}\n";

    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    ASSERT(lex_err == NULL);

    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    ASSERT(parse_err == NULL);

    value_set_heap(NULL);
    value_set_arena(NULL);

    char *comp_err = NULL;
    RegChunk *original = reg_compile(&prog, &comp_err);
    ASSERT(original != NULL);

    size_t data_len;
    uint8_t *data = regchunk_serialize(original, &data_len);
    ASSERT(data != NULL);
    ASSERT(data_len > 8);

    char *deser_err = NULL;
    RegChunk *restored = regchunk_deserialize(data, data_len, &deser_err);
    ASSERT(restored != NULL);
    ASSERT(deser_err == NULL);

    /* Verify instruction count matches */
    ASSERT_EQ_INT(original->code_len, restored->code_len);

    /* Verify instructions match */
    for (size_t i = 0; i < original->code_len; i++) { ASSERT_EQ_INT(original->code[i], restored->code[i]); }

    /* Verify constant count matches */
    ASSERT_EQ_INT(original->const_len, restored->const_len);

    /* Verify max_reg matches */
    ASSERT_EQ_INT(original->max_reg, restored->max_reg);

    free(data);
    regchunk_free(original);
    regchunk_free(restored);

    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Deterministic serialization: compiling the same source twice must
 * produce byte-identical .latc output.
 * ══════════════════════════════════════════════════════════════════════════ */

static uint8_t *compile_and_serialize(const char *source, size_t *out_len) {
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        free(lex_err);
        lat_vec_free(&tokens);
        return NULL;
    }

    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    if (parse_err) {
        free(parse_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return NULL;
    }

    value_set_heap(NULL);
    value_set_arena(NULL);

    char *comp_err = NULL;
    Chunk *chunk = stack_compile(&prog, &comp_err);
    if (!chunk) {
        free(comp_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return NULL;
    }

    uint8_t *data = chunk_serialize(chunk, out_len);
    chunk_free(chunk);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    return data;
}

TEST(latc_stack_deterministic_serialization) {
    const char *source = "fn fib(n: Int) -> Int {\n"
                         "    if n <= 1 { return n }\n"
                         "    return fib(n - 1) + fib(n - 2)\n"
                         "}\n"
                         "fn main() {\n"
                         "    let x = \"hello\"\n"
                         "    let y = 3.14\n"
                         "    let z = true\n"
                         "    print(fib(10))\n"
                         "    print(x)\n"
                         "    print(y)\n"
                         "    print(z)\n"
                         "}\n";

    size_t len1, len2;
    uint8_t *data1 = compile_and_serialize(source, &len1);
    ASSERT(data1 != NULL);

    uint8_t *data2 = compile_and_serialize(source, &len2);
    ASSERT(data2 != NULL);

    /* Same length */
    ASSERT_EQ_INT((long long)len1, (long long)len2);

    /* Byte-identical */
    ASSERT(memcmp(data1, data2, len1) == 0);

    free(data1);
    free(data2);
}

TEST(latc_stack_deterministic_with_closures) {
    const char *source = "fn make_adder(n: Int) {\n"
                         "    return |x| { n + x }\n"
                         "}\n"
                         "fn make_counter() {\n"
                         "    flux count = 0\n"
                         "    return |_| { count = count + 1; count }\n"
                         "}\n"
                         "fn main() {\n"
                         "    let add5 = make_adder(5)\n"
                         "    print(add5(10))\n"
                         "    let c = make_counter()\n"
                         "    print(c(0))\n"
                         "}\n";

    size_t len1, len2;
    uint8_t *data1 = compile_and_serialize(source, &len1);
    ASSERT(data1 != NULL);

    uint8_t *data2 = compile_and_serialize(source, &len2);
    ASSERT(data2 != NULL);

    ASSERT_EQ_INT((long long)len1, (long long)len2);
    ASSERT(memcmp(data1, data2, len1) == 0);

    free(data1);
    free(data2);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Constant value preservation: verify each constant type survives
 * serialization round-trip with exact values.
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(latc_stack_preserves_int_constants) {
    const char *source = "fn main() {\n"
                         "    let a = 0\n"
                         "    let b = 42\n"
                         "    let c = -1\n"
                         "    let d = 2147483647\n"
                         "    let e = -2147483648\n"
                         "    print(a)\n"
                         "    print(b)\n"
                         "    print(c)\n"
                         "    print(d)\n"
                         "    print(e)\n"
                         "}\n";

    ASSERT_STACK_ROUNDTRIP(source);
    ASSERT_STACK_ROUNDTRIP_MEM(source);
}

TEST(latc_stack_preserves_float_constants) {
    const char *source = "fn main() {\n"
                         "    let a = 3.14159\n"
                         "    let b = 0.0\n"
                         "    let c = -1.5\n"
                         "    let d = 1000000.001\n"
                         "    print(a)\n"
                         "    print(b)\n"
                         "    print(c)\n"
                         "    print(d)\n"
                         "}\n";

    ASSERT_STACK_ROUNDTRIP(source);
    ASSERT_STACK_ROUNDTRIP_MEM(source);
}

TEST(latc_stack_preserves_string_constants) {
    const char *source = "fn main() {\n"
                         "    let a = \"hello world\"\n"
                         "    let b = \"\"\n"
                         "    let c = \"special chars: !@#$%^&*()\"\n"
                         "    let d = \"newline: \\n tab: \\t\"\n"
                         "    print(a)\n"
                         "    print(b)\n"
                         "    print(c)\n"
                         "    print(d)\n"
                         "}\n";

    ASSERT_STACK_ROUNDTRIP(source);
    ASSERT_STACK_ROUNDTRIP_MEM(source);
}

TEST(latc_stack_preserves_bool_nil_unit) {
    const char *source = "fn main() {\n"
                         "    let a = true\n"
                         "    let b = false\n"
                         "    let c = nil\n"
                         "    print(a)\n"
                         "    print(b)\n"
                         "    print(c)\n"
                         "}\n";

    ASSERT_STACK_ROUNDTRIP(source);
    ASSERT_STACK_ROUNDTRIP_MEM(source);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Comprehensive feature round-trip tests
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(latc_stack_nested_closures) {
    ASSERT_STACK_ROUNDTRIP("fn make_pair(a: Int, b: Int) {\n"
                           "    return |selector| {\n"
                           "        if selector == 0 { return a }\n"
                           "        return b\n"
                           "    }\n"
                           "}\n"
                           "fn main() {\n"
                           "    let p = make_pair(10, 20)\n"
                           "    print(p(0))\n"
                           "    print(p(1))\n"
                           "}\n");
}

TEST(latc_stack_deep_nesting) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    if true {\n"
                           "        if true {\n"
                           "            if true {\n"
                           "                if true {\n"
                           "                    print(42)\n"
                           "                }\n"
                           "            }\n"
                           "        }\n"
                           "    }\n"
                           "}\n");
}

TEST(latc_stack_complex_control_flow) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    let sum = 0\n"
                           "    for i in 0..10 {\n"
                           "        if i % 3 == 0 { continue }\n"
                           "        if i > 7 { break }\n"
                           "        for j in 0..3 {\n"
                           "            if j == i { print(i) }\n"
                           "        }\n"
                           "    }\n"
                           "}\n");
}

TEST(latc_stack_string_interpolation_complex) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    let name = \"world\"\n"
                           "    let num = 42\n"
                           "    let msg = \"Hello ${name}, the answer is ${num}\"\n"
                           "    print(msg)\n"
                           "}\n");
}

TEST(latc_stack_map_operations) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    flux m = Map::new()\n"
                           "    m[\"a\"] = 1\n"
                           "    m[\"b\"] = 2\n"
                           "    print(m[\"a\"])\n"
                           "    print(m[\"b\"])\n"
                           "    m[\"a\"] = 10\n"
                           "    print(m[\"a\"])\n"
                           "}\n");
}

TEST(latc_stack_array_operations) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    flux arr = []\n"
                           "    arr.push(10)\n"
                           "    arr.push(20)\n"
                           "    arr.push(30)\n"
                           "    print(arr.len())\n"
                           "    for v in arr {\n"
                           "        print(v)\n"
                           "    }\n"
                           "    arr[1] = 99\n"
                           "    print(arr[1])\n"
                           "}\n");
}

TEST(latc_stack_try_catch_roundtrip) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    try {\n"
                           "        let x = 1 / 0\n"
                           "        print(\"FAIL\")\n"
                           "    } catch e {\n"
                           "        print(\"caught\")\n"
                           "    }\n"
                           "    try {\n"
                           "        print(42)\n"
                           "    } catch e {\n"
                           "        print(\"FAIL\")\n"
                           "    }\n"
                           "}\n");
}

TEST(latc_stack_mutual_recursion) {
    ASSERT_STACK_ROUNDTRIP("fn is_even(n: Int) -> Bool {\n"
                           "    if n == 0 { return true }\n"
                           "    return is_odd(n - 1)\n"
                           "}\n"
                           "fn is_odd(n: Int) -> Bool {\n"
                           "    if n == 0 { return false }\n"
                           "    return is_even(n - 1)\n"
                           "}\n"
                           "fn main() {\n"
                           "    print(is_even(10))\n"
                           "    print(is_odd(7))\n"
                           "}\n");
}

TEST(latc_stack_higher_order_functions) {
    ASSERT_STACK_ROUNDTRIP("fn apply(f: any, x: Int) -> Int {\n"
                           "    return f(x)\n"
                           "}\n"
                           "fn main() {\n"
                           "    let double = |x| { x * 2 }\n"
                           "    let inc = |x| { x + 1 }\n"
                           "    print(apply(double, 5))\n"
                           "    print(apply(inc, 10))\n"
                           "}\n");
}

TEST(latc_stack_compound_assignment) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    flux x = 10\n"
                           "    x += 5\n"
                           "    print(x)\n"
                           "    x -= 3\n"
                           "    print(x)\n"
                           "    x *= 2\n"
                           "    print(x)\n"
                           "    x /= 4\n"
                           "    print(x)\n"
                           "    x %= 3\n"
                           "    print(x)\n"
                           "}\n");
}

TEST(latc_stack_nested_arrays) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    let matrix = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]\n"
                           "    print(matrix[0][0])\n"
                           "    print(matrix[1][1])\n"
                           "    print(matrix[2][2])\n"
                           "}\n");
}

TEST(latc_stack_struct_operations) {
    ASSERT_STACK_ROUNDTRIP("struct Point { x: Int, y: Int }\n"
                           "fn main() {\n"
                           "    let p = Point { x: 10, y: 20 }\n"
                           "    print(p.x)\n"
                           "    print(p.y)\n"
                           "    print(p.x + p.y)\n"
                           "}\n");
}

TEST(latc_stack_enum_operations) {
    ASSERT_STACK_ROUNDTRIP("enum Dir { Up, Down, Left, Right }\n"
                           "fn main() {\n"
                           "    let d = Dir::Up\n"
                           "    print(d)\n"
                           "    let d2 = Dir::Right\n"
                           "    print(d2)\n"
                           "}\n");
}

TEST(latc_stack_enum_with_payload) {
    ASSERT_STACK_ROUNDTRIP("enum Shape { Circle(any), Rect(any, any) }\n"
                           "fn main() {\n"
                           "    let c = Shape::Circle(5)\n"
                           "    print(c)\n"
                           "    let r = Shape::Rect(10, 20)\n"
                           "    print(r)\n"
                           "}\n");
}

TEST(latc_stack_match_with_guards) {
    ASSERT_STACK_ROUNDTRIP("fn classify(n: Int) {\n"
                           "    match n {\n"
                           "        x if x < 0 => print(\"negative\"),\n"
                           "        0 => print(\"zero\"),\n"
                           "        x if x > 100 => print(\"large\"),\n"
                           "        _ => print(\"positive\")\n"
                           "    }\n"
                           "}\n"
                           "fn main() {\n"
                           "    classify(-5)\n"
                           "    classify(0)\n"
                           "    classify(50)\n"
                           "    classify(200)\n"
                           "}\n");
}

TEST(latc_stack_for_in_range) {
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    flux sum = 0\n"
                           "    for i in 1..101 {\n"
                           "        sum = sum + i\n"
                           "    }\n"
                           "    print(sum)\n"
                           "}\n");
}

TEST(latc_stack_closure_counter) {
    ASSERT_STACK_ROUNDTRIP("fn make_counter() {\n"
                           "    flux n = 0\n"
                           "    return |_| {\n"
                           "        n = n + 1\n"
                           "        n\n"
                           "    }\n"
                           "}\n"
                           "fn main() {\n"
                           "    let c = make_counter()\n"
                           "    print(c(0))\n"
                           "    print(c(0))\n"
                           "    print(c(0))\n"
                           "}\n");
}

/* ── RegVM additional round-trip feature tests ── */

TEST(latc_reg_nested_closures) {
    ASSERT_REG_ROUNDTRIP("fn make_adder(n: Int) {\n"
                         "    return |x| { n + x }\n"
                         "}\n"
                         "fn main() {\n"
                         "    let add5 = make_adder(5)\n"
                         "    print(add5(10))\n"
                         "    print(add5(20))\n"
                         "}\n");
}

TEST(latc_reg_compound_assignment) {
    ASSERT_REG_ROUNDTRIP("fn main() {\n"
                         "    flux x = 10\n"
                         "    x += 5\n"
                         "    print(x)\n"
                         "    x -= 3\n"
                         "    print(x)\n"
                         "    x *= 2\n"
                         "    print(x)\n"
                         "}\n");
}

TEST(latc_reg_string_interpolation) {
    ASSERT_REG_ROUNDTRIP("fn main() {\n"
                         "    let name = \"world\"\n"
                         "    print(\"hello ${name}\")\n"
                         "}\n");
}

TEST(latc_reg_try_catch) {
    ASSERT_REG_ROUNDTRIP("fn main() {\n"
                         "    try {\n"
                         "        let x = 1 / 0\n"
                         "        print(\"FAIL\")\n"
                         "    } catch e {\n"
                         "        print(\"caught\")\n"
                         "    }\n"
                         "}\n");
}

TEST(latc_reg_higher_order) {
    ASSERT_REG_ROUNDTRIP("fn apply(f: any, x: Int) {\n"
                         "    return f(x)\n"
                         "}\n"
                         "fn main() {\n"
                         "    let double = |x| { x * 2 }\n"
                         "    print(apply(double, 5))\n"
                         "}\n");
}

TEST(latc_reg_map_operations) {
    ASSERT_REG_ROUNDTRIP("fn main() {\n"
                         "    flux m = Map::new()\n"
                         "    m[\"key\"] = 42\n"
                         "    print(m[\"key\"])\n"
                         "}\n");
}

TEST(latc_reg_nested_loops) {
    ASSERT_REG_ROUNDTRIP("fn main() {\n"
                         "    for i in 0..3 {\n"
                         "        for j in 0..3 {\n"
                         "            if i == j {\n"
                         "                print(i)\n"
                         "            }\n"
                         "        }\n"
                         "    }\n"
                         "}\n");
}

/* ── RegVM in-memory additional tests ── */

TEST(latc_reg_mem_strings) {
    ASSERT_REG_ROUNDTRIP_MEM("fn main() {\n"
                             "    let s = \"hello\"\n"
                             "    print(s)\n"
                             "    print(s + \" world\")\n"
                             "}\n");
}

TEST(latc_reg_mem_arrays) {
    ASSERT_REG_ROUNDTRIP_MEM("fn main() {\n"
                             "    let arr = [1, 2, 3]\n"
                             "    for v in arr {\n"
                             "        print(v)\n"
                             "    }\n"
                             "}\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Constant value detail verification: check individual constant types
 * and values survive serialization exactly.
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(latc_stack_constant_types_preserved) {
    /* Compile a source with int, float, string, bool, nil constants.
     * Serialize and deserialize, then verify each constant type matches. */
    const char *source = "fn main() {\n"
                         "    let i = 42\n"
                         "    let f = 3.14\n"
                         "    let s = \"hello\"\n"
                         "    let b = true\n"
                         "    let n = nil\n"
                         "    print(i)\n"
                         "    print(f)\n"
                         "    print(s)\n"
                         "    print(b)\n"
                         "    print(n)\n"
                         "}\n";

    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    ASSERT(lex_err == NULL);

    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    ASSERT(parse_err == NULL);

    value_set_heap(NULL);
    value_set_arena(NULL);

    char *comp_err = NULL;
    Chunk *original = stack_compile(&prog, &comp_err);
    ASSERT(original != NULL);

    size_t data_len;
    uint8_t *data = chunk_serialize(original, &data_len);
    ASSERT(data != NULL);

    char *deser_err = NULL;
    Chunk *restored = chunk_deserialize(data, data_len, &deser_err);
    ASSERT(restored != NULL);

    /* Verify constant pool matches exactly */
    ASSERT_EQ_INT(original->const_len, restored->const_len);

    for (size_t i = 0; i < original->const_len; i++) {
        const LatValue *orig_v = &original->constants[i];
        const LatValue *rest_v = &restored->constants[i];
        ASSERT_EQ_INT(orig_v->type, rest_v->type);

        switch (orig_v->type) {
            case VAL_INT: ASSERT_EQ_INT(orig_v->as.int_val, rest_v->as.int_val); break;
            case VAL_FLOAT: ASSERT(orig_v->as.float_val == rest_v->as.float_val); break;
            case VAL_BOOL: ASSERT(orig_v->as.bool_val == rest_v->as.bool_val); break;
            case VAL_STR: ASSERT_EQ_STR(orig_v->as.str_val, rest_v->as.str_val); break;
            case VAL_NIL:
            case VAL_UNIT:
                /* Just type match is enough */
                break;
            case VAL_CLOSURE:
                /* Sub-chunk: verify param count and variadic match */
                ASSERT_EQ_INT((long long)orig_v->as.closure.param_count, (long long)rest_v->as.closure.param_count);
                ASSERT(orig_v->as.closure.has_variadic == rest_v->as.closure.has_variadic);
                break;
            default: break;
        }
    }

    free(data);
    chunk_free(original);
    chunk_free(restored);

    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Local name preservation: verify debug info survives round-trip.
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(latc_stack_local_names_preserved) {
    const char *source = "fn main() {\n"
                         "    let alpha = 1\n"
                         "    let beta = 2\n"
                         "    let gamma = 3\n"
                         "    print(alpha + beta + gamma)\n"
                         "}\n";

    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    ASSERT(lex_err == NULL);

    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    ASSERT(parse_err == NULL);

    value_set_heap(NULL);
    value_set_arena(NULL);

    char *comp_err = NULL;
    Chunk *original = stack_compile(&prog, &comp_err);
    ASSERT(original != NULL);

    size_t data_len;
    uint8_t *data = chunk_serialize(original, &data_len);
    ASSERT(data != NULL);

    char *deser_err = NULL;
    Chunk *restored = chunk_deserialize(data, data_len, &deser_err);
    ASSERT(restored != NULL);

    /* Verify local_name_cap matches */
    ASSERT_EQ_INT(original->local_name_cap, restored->local_name_cap);

    /* Verify each local name */
    for (size_t i = 0; i < original->local_name_cap; i++) {
        if (original->local_names && original->local_names[i]) {
            ASSERT(restored->local_names != NULL);
            ASSERT(restored->local_names[i] != NULL);
            ASSERT_EQ_STR(original->local_names[i], restored->local_names[i]);
        } else {
            /* Both should be absent or NULL */
            if (restored->local_names) ASSERT(restored->local_names[i] == NULL);
        }
    }

    free(data);
    chunk_free(original);
    chunk_free(restored);

    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Header validation tests: verify correct magic and format version
 * in serialized output.
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(latc_stack_header_format) {
    const char *source = "fn main() { print(1) }\n";

    size_t data_len;
    uint8_t *data = compile_and_serialize(source, &data_len);
    ASSERT(data != NULL);
    ASSERT(data_len >= 8);

    /* Verify magic bytes */
    ASSERT(data[0] == 'L');
    ASSERT(data[1] == 'A');
    ASSERT(data[2] == 'T');
    ASSERT(data[3] == 'C');

    /* Verify format version (LE u16) */
    uint16_t version = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
    ASSERT_EQ_INT(version, LATC_FORMAT);

    /* Reserved field should be zero */
    uint16_t reserved = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
    ASSERT_EQ_INT(reserved, 0);

    free(data);
}

TEST(latc_reg_header_format) {
    const char *source = "fn main() { print(1) }\n";

    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    ASSERT(lex_err == NULL);

    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    ASSERT(parse_err == NULL);

    value_set_heap(NULL);
    value_set_arena(NULL);

    char *comp_err = NULL;
    RegChunk *rchunk = reg_compile(&prog, &comp_err);
    ASSERT(rchunk != NULL);

    size_t data_len;
    uint8_t *data = regchunk_serialize(rchunk, &data_len);
    ASSERT(data != NULL);
    ASSERT(data_len >= 8);

    /* Verify magic bytes */
    ASSERT(data[0] == 'R');
    ASSERT(data[1] == 'L');
    ASSERT(data[2] == 'A');
    ASSERT(data[3] == 'T');

    /* Verify format version */
    uint16_t version = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
    ASSERT_EQ_INT(version, RLATC_FORMAT);

    free(data);
    regchunk_free(rchunk);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Truncated data edge cases: ensure graceful failure for various
 * truncation points in the serialized data.
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(latc_stack_truncated_at_various_points) {
    /* Compile a simple program to get valid serialized data */
    const char *source = "fn main() { print(42) }\n";
    size_t full_len;
    uint8_t *full_data = compile_and_serialize(source, &full_len);
    ASSERT(full_data != NULL);
    ASSERT(full_len > 20); /* Should be reasonably sized */

    /* Try deserializing with various truncation points */
    size_t test_points[] = {0, 4, 6, 8, 9, 10, 12, full_len / 2, full_len - 1};
    int num_points = (int)(sizeof(test_points) / sizeof(test_points[0]));

    for (int i = 0; i < num_points; i++) {
        size_t trunc_len = test_points[i];
        if (trunc_len >= full_len) continue; /* Skip if not actually truncated */
        char *err = NULL;
        Chunk *c = chunk_deserialize(full_data, trunc_len, &err);
        /* Should fail (c == NULL) or be incomplete, never crash */
        if (c) chunk_free(c);
        free(err);
    }

    free(full_data);
}

TEST(latc_stack_zero_length_data) {
    char *err = NULL;
    Chunk *c = chunk_deserialize(NULL, 0, &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

TEST(latc_reg_zero_length_data) {
    char *err = NULL;
    RegChunk *c = regchunk_deserialize(NULL, 0, &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

/* ══════════════════════════════════════════════════════════════════════════
 * File I/O round-trip with multiple programs: save/load multiple .latc
 * files to verify file I/O doesn't interfere.
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(latc_stack_multiple_file_roundtrips) {
    const char *sources[] = {
        "fn main() { print(1) }\n",
        "fn main() { print(\"hello\") }\n",
        ("fn main() {\n"
         "    for i in 0..3 { print(i) }\n"
         "}\n"),
        ("fn add(a: Int, b: Int) -> Int { return a + b }\n"
         "fn main() { print(add(3, 4)) }\n"),
    };
    int num = 4;

    for (int i = 0; i < num; i++) {
        char *err = NULL;
        int rc = stack_roundtrip(sources[i], &err);
        if (rc != 0) {
            fprintf(stderr, "  FAIL: source %d: %s\n", i, err ? err : "(unknown)");
            free(err);
            test_current_failed = 1;
            return;
        }
    }
}

TEST(latc_reg_multiple_file_roundtrips) {
    const char *sources[] = {
        "fn main() { print(1) }\n",
        "fn main() { print(\"hello\") }\n",
        ("fn main() {\n"
         "    for i in 0..3 { print(i) }\n"
         "}\n"),
        ("fn add(a: Int, b: Int) -> Int { return a + b }\n"
         "fn main() { print(add(3, 4)) }\n"),
    };
    int num = 4;

    for (int i = 0; i < num; i++) {
        char *err = NULL;
        int rc = reg_roundtrip(sources[i], &err);
        if (rc != 0) {
            fprintf(stderr, "  FAIL: source %d: %s\n", i, err ? err : "(unknown)");
            free(err);
            test_current_failed = 1;
            return;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Sub-chunk (closure) serialization: verify nested function chunks
 * survive round-trip correctly.
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(latc_stack_subchunk_closure_roundtrip) {
    /* Multiple closures with different signatures */
    ASSERT_STACK_ROUNDTRIP("fn main() {\n"
                           "    let a = |x| { x + 1 }\n"
                           "    let b = |x, y| { x * y }\n"
                           "    let c = |x| {\n"
                           "        let inner = |y| { x + y }\n"
                           "        return inner\n"
                           "    }\n"
                           "    print(a(5))\n"
                           "    print(b(3, 4))\n"
                           "    let add10 = c(10)\n"
                           "    print(add10(5))\n"
                           "}\n");
}

TEST(latc_stack_subchunk_preserves_param_count) {
    /* Verify closure param counts survive serialization */
    const char *source = "fn main() {\n"
                         "    let f1 = |x| { x }\n"
                         "    let f2 = |x, y| { x + y }\n"
                         "    let f3 = |x, y, z| { x + y + z }\n"
                         "    print(f1(1))\n"
                         "    print(f2(1, 2))\n"
                         "    print(f3(1, 2, 3))\n"
                         "}\n";

    ASSERT_STACK_ROUNDTRIP(source);
    ASSERT_STACK_ROUNDTRIP_MEM(source);
}
