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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
    for (size_t i = 0; i < tokens.len; i++)
        token_free(lat_vec_get(&tokens, i));
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
    for (size_t i = 0; i < tokens.len; i++)
        token_free(lat_vec_get(&tokens, i));
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
    for (size_t i = 0; i < tokens.len; i++)
        token_free(lat_vec_get(&tokens, i));
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
    for (size_t i = 0; i < tokens.len; i++)
        token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    return rc;
}

/* Convenience macros for round-trip assertions */
#define ASSERT_STACK_ROUNDTRIP(src) do { \
    char *_err = NULL; \
    int _rc = stack_roundtrip(src, &_err); \
    if (_rc != 0) { \
        fprintf(stderr, "  FAIL: %s:%d: stack roundtrip failed: %s\n", \
                __FILE__, __LINE__, _err ? _err : "(unknown)"); \
        free(_err); \
        test_current_failed = 1; \
        return; \
    } \
} while(0)

#define ASSERT_STACK_ROUNDTRIP_MEM(src) do { \
    char *_err = NULL; \
    int _rc = stack_roundtrip_mem(src, &_err); \
    if (_rc != 0) { \
        fprintf(stderr, "  FAIL: %s:%d: stack mem roundtrip failed: %s\n", \
                __FILE__, __LINE__, _err ? _err : "(unknown)"); \
        free(_err); \
        test_current_failed = 1; \
        return; \
    } \
} while(0)

#define ASSERT_REG_ROUNDTRIP(src) do { \
    char *_err = NULL; \
    int _rc = reg_roundtrip(src, &_err); \
    if (_rc != 0) { \
        fprintf(stderr, "  FAIL: %s:%d: reg roundtrip failed: %s\n", \
                __FILE__, __LINE__, _err ? _err : "(unknown)"); \
        free(_err); \
        test_current_failed = 1; \
        return; \
    } \
} while(0)

#define ASSERT_REG_ROUNDTRIP_MEM(src) do { \
    char *_err = NULL; \
    int _rc = reg_roundtrip_mem(src, &_err); \
    if (_rc != 0) { \
        fprintf(stderr, "  FAIL: %s:%d: reg mem roundtrip failed: %s\n", \
                __FILE__, __LINE__, _err ? _err : "(unknown)"); \
        free(_err); \
        test_current_failed = 1; \
        return; \
    } \
} while(0)

/* ══════════════════════════════════════════════════════════════════════════
 * Stack VM Round-Trip Tests
 * ══════════════════════════════════════════════════════════════════════════ */

TEST(latc_stack_basic_arithmetic) {
    ASSERT_STACK_ROUNDTRIP(
        "fn main() {\n"
        "    print(1 + 2)\n"
        "    print(10 - 3)\n"
        "    print(4 * 5)\n"
        "    print(15 / 3)\n"
        "    print(17 % 5)\n"
        "}\n"
    );
}

TEST(latc_stack_float_arithmetic) {
    ASSERT_STACK_ROUNDTRIP(
        "fn main() {\n"
        "    print(3.14 + 2.86)\n"
        "    print(1.5 * 2.0)\n"
        "}\n"
    );
}

TEST(latc_stack_string_ops) {
    ASSERT_STACK_ROUNDTRIP(
        "fn main() {\n"
        "    let s = \"hello\"\n"
        "    print(s)\n"
        "    print(s + \" world\")\n"
        "    print(s.len())\n"
        "}\n"
    );
}

TEST(latc_stack_string_interpolation) {
    ASSERT_STACK_ROUNDTRIP(
        "fn main() {\n"
        "    let name = \"Lattice\"\n"
        "    let ver = 3\n"
        "    print(\"Hello ${name} v${ver}\")\n"
        "}\n"
    );
}

TEST(latc_stack_boolean_ops) {
    ASSERT_STACK_ROUNDTRIP(
        "fn main() {\n"
        "    print(true && false)\n"
        "    print(true || false)\n"
        "    print(!true)\n"
        "    print(1 == 1)\n"
        "    print(1 != 2)\n"
        "}\n"
    );
}

TEST(latc_stack_variables_and_assignment) {
    ASSERT_STACK_ROUNDTRIP(
        "fn main() {\n"
        "    let x = 10\n"
        "    let y = 20\n"
        "    print(x + y)\n"
        "    let z = x * y\n"
        "    print(z)\n"
        "}\n"
    );
}

TEST(latc_stack_if_else) {
    ASSERT_STACK_ROUNDTRIP(
        "fn main() {\n"
        "    let x = 42\n"
        "    if x > 10 {\n"
        "        print(\"big\")\n"
        "    } else {\n"
        "        print(\"small\")\n"
        "    }\n"
        "}\n"
    );
}

TEST(latc_stack_while_loop) {
    ASSERT_STACK_ROUNDTRIP(
        "fn main() {\n"
        "    let i = 0\n"
        "    while i < 5 {\n"
        "        print(i)\n"
        "        i = i + 1\n"
        "    }\n"
        "}\n"
    );
}

TEST(latc_stack_for_loop) {
    ASSERT_STACK_ROUNDTRIP(
        "fn main() {\n"
        "    for i in 0..5 {\n"
        "        print(i)\n"
        "    }\n"
        "}\n"
    );
}

TEST(latc_stack_functions) {
    ASSERT_STACK_ROUNDTRIP(
        "fn add(a: Int, b: Int) -> Int {\n"
        "    return a + b\n"
        "}\n"
        "fn main() {\n"
        "    print(add(3, 4))\n"
        "}\n"
    );
}

TEST(latc_stack_closures) {
    ASSERT_STACK_ROUNDTRIP(
        "fn make_adder(n: Int) {\n"
        "    return |x| { n + x }\n"
        "}\n"
        "fn main() {\n"
        "    let add5 = make_adder(5)\n"
        "    print(add5(10))\n"
        "}\n"
    );
}

TEST(latc_stack_recursion) {
    ASSERT_STACK_ROUNDTRIP(
        "fn fib(n: Int) -> Int {\n"
        "    if n <= 1 { return n }\n"
        "    return fib(n - 1) + fib(n - 2)\n"
        "}\n"
        "fn main() {\n"
        "    print(fib(10))\n"
        "}\n"
    );
}

TEST(latc_stack_arrays) {
    ASSERT_STACK_ROUNDTRIP(
        "fn main() {\n"
        "    let arr = [1, 2, 3, 4, 5]\n"
        "    print(arr.len())\n"
        "    print(arr[0])\n"
        "    print(arr[4])\n"
        "    arr.push(6)\n"
        "    print(arr.len())\n"
        "}\n"
    );
}

TEST(latc_stack_structs) {
    ASSERT_STACK_ROUNDTRIP(
        "struct Point { x: Int, y: Int }\n"
        "fn main() {\n"
        "    let p = Point { x: 10, y: 20 }\n"
        "    print(p.x)\n"
        "    print(p.y)\n"
        "}\n"
    );
}

TEST(latc_stack_enums) {
    ASSERT_STACK_ROUNDTRIP(
        "enum Color { Red, Green, Blue }\n"
        "fn main() {\n"
        "    let c = Color::Red\n"
        "    match c.variant_name() {\n"
        "        \"Red\" => print(\"red\"),\n"
        "        \"Green\" => print(\"green\"),\n"
        "        \"Blue\" => print(\"blue\")\n"
        "    }\n"
        "}\n"
    );
}

TEST(latc_stack_match_expression) {
    ASSERT_STACK_ROUNDTRIP(
        "fn main() {\n"
        "    let x = 2\n"
        "    match x {\n"
        "        1 => print(\"one\"),\n"
        "        2 => print(\"two\"),\n"
        "        _ => print(\"other\")\n"
        "    }\n"
        "}\n"
    );
}

TEST(latc_stack_multiple_functions) {
    ASSERT_STACK_ROUNDTRIP(
        "fn square(x: Int) -> Int { return x * x }\n"
        "fn cube(x: Int) -> Int { return x * x * x }\n"
        "fn max_val(a: Int, b: Int) -> Int {\n"
        "    if a > b { return a }\n"
        "    return b\n"
        "}\n"
        "fn main() {\n"
        "    print(square(5))\n"
        "    print(cube(3))\n"
        "    print(max_val(10, 20))\n"
        "}\n"
    );
}

TEST(latc_stack_nil_and_unit) {
    ASSERT_STACK_ROUNDTRIP(
        "fn main() {\n"
        "    let x = nil\n"
        "    print(x)\n"
        "}\n"
    );
}

TEST(latc_stack_break_continue) {
    ASSERT_STACK_ROUNDTRIP(
        "fn main() {\n"
        "    let sum = 0\n"
        "    for i in 0..10 {\n"
        "        if i == 5 { break }\n"
        "        if i % 2 == 0 { continue }\n"
        "        sum = sum + i\n"
        "    }\n"
        "    print(sum)\n"
        "}\n"
    );
}

/* ── Stack VM: in-memory round-trip (serialize/deserialize, no file I/O) ── */

TEST(latc_stack_mem_basic) {
    ASSERT_STACK_ROUNDTRIP_MEM(
        "fn main() {\n"
        "    print(42)\n"
        "}\n"
    );
}

TEST(latc_stack_mem_closures) {
    ASSERT_STACK_ROUNDTRIP_MEM(
        "fn make_counter() {\n"
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
        "}\n"
    );
}

TEST(latc_stack_mem_strings_and_arrays) {
    ASSERT_STACK_ROUNDTRIP_MEM(
        "fn main() {\n"
        "    let names = [\"alice\", \"bob\", \"charlie\"]\n"
        "    for name in names {\n"
        "        print(name)\n"
        "    }\n"
        "}\n"
    );
}

/* ── Stack VM: many constants (exercises wide constant opcodes) ── */

TEST(latc_stack_many_constants) {
    /* Build source with many distinct string constants to test constant pool
     * handling. If the compiler emits > 256 constants, this exercises
     * OP_CONSTANT_16 and wide global opcodes. */
    char source[16384];
    int off = 0;
    off += snprintf(source + off, sizeof(source) - (size_t)off,
                    "fn main() {\n");
    for (int i = 0; i < 300; i++) {
        off += snprintf(source + off, sizeof(source) - (size_t)off,
                        "    let v%d = \"str_%d\"\n", i, i);
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
    ASSERT_REG_ROUNDTRIP(
        "fn main() {\n"
        "    print(1 + 2)\n"
        "    print(10 - 3)\n"
        "    print(4 * 5)\n"
        "    print(15 / 3)\n"
        "}\n"
    );
}

TEST(latc_reg_float_arithmetic) {
    ASSERT_REG_ROUNDTRIP(
        "fn main() {\n"
        "    print(3.14 + 2.86)\n"
        "    print(1.5 * 2.0)\n"
        "}\n"
    );
}

TEST(latc_reg_string_ops) {
    ASSERT_REG_ROUNDTRIP(
        "fn main() {\n"
        "    let s = \"hello\"\n"
        "    print(s)\n"
        "    print(s + \" world\")\n"
        "}\n"
    );
}

TEST(latc_reg_boolean_ops) {
    ASSERT_REG_ROUNDTRIP(
        "fn main() {\n"
        "    print(true && false)\n"
        "    print(true || false)\n"
        "    print(!true)\n"
        "}\n"
    );
}

TEST(latc_reg_variables) {
    ASSERT_REG_ROUNDTRIP(
        "fn main() {\n"
        "    let x = 10\n"
        "    let y = 20\n"
        "    print(x + y)\n"
        "}\n"
    );
}

TEST(latc_reg_if_else) {
    ASSERT_REG_ROUNDTRIP(
        "fn main() {\n"
        "    let x = 42\n"
        "    if x > 10 {\n"
        "        print(\"big\")\n"
        "    } else {\n"
        "        print(\"small\")\n"
        "    }\n"
        "}\n"
    );
}

TEST(latc_reg_while_loop) {
    ASSERT_REG_ROUNDTRIP(
        "fn main() {\n"
        "    let i = 0\n"
        "    while i < 5 {\n"
        "        print(i)\n"
        "        i = i + 1\n"
        "    }\n"
        "}\n"
    );
}

TEST(latc_reg_for_loop) {
    ASSERT_REG_ROUNDTRIP(
        "fn main() {\n"
        "    for i in 0..5 {\n"
        "        print(i)\n"
        "    }\n"
        "}\n"
    );
}

TEST(latc_reg_functions) {
    ASSERT_REG_ROUNDTRIP(
        "fn add(a: Int, b: Int) -> Int {\n"
        "    return a + b\n"
        "}\n"
        "fn main() {\n"
        "    print(add(3, 4))\n"
        "}\n"
    );
}

TEST(latc_reg_closures) {
    ASSERT_REG_ROUNDTRIP(
        "fn make_adder(n: Int) {\n"
        "    return |x| { n + x }\n"
        "}\n"
        "fn main() {\n"
        "    let add5 = make_adder(5)\n"
        "    print(add5(10))\n"
        "}\n"
    );
}

TEST(latc_reg_recursion) {
    ASSERT_REG_ROUNDTRIP(
        "fn fib(n: Int) -> Int {\n"
        "    if n <= 1 { return n }\n"
        "    return fib(n - 1) + fib(n - 2)\n"
        "}\n"
        "fn main() {\n"
        "    print(fib(10))\n"
        "}\n"
    );
}

TEST(latc_reg_arrays) {
    ASSERT_REG_ROUNDTRIP(
        "fn main() {\n"
        "    let arr = [1, 2, 3, 4, 5]\n"
        "    print(arr.len())\n"
        "    print(arr[0])\n"
        "}\n"
    );
}

/* NOTE: RegVM struct/enum round-trip tests are omitted because the RegVM
 * compiler stores struct field metadata as VAL_ARRAY constants in the constant
 * pool, and the .rlatc serialization format does not yet support VAL_ARRAY
 * constants (they are written as TAG_NIL fallback). This causes struct field
 * count mismatches and enum metadata loss after deserialization.
 * The stack VM tests above cover struct/enum serialization because the stack
 * compiler uses a different metadata mechanism. */

TEST(latc_reg_multiple_functions) {
    ASSERT_REG_ROUNDTRIP(
        "fn square(x: Int) -> Int { return x * x }\n"
        "fn cube(x: Int) -> Int { return x * x * x }\n"
        "fn main() {\n"
        "    print(square(5))\n"
        "    print(cube(3))\n"
        "}\n"
    );
}

/* ── RegVM: in-memory round-trip ── */

TEST(latc_reg_mem_basic) {
    ASSERT_REG_ROUNDTRIP_MEM(
        "fn main() {\n"
        "    print(42)\n"
        "}\n"
    );
}

TEST(latc_reg_mem_closures) {
    ASSERT_REG_ROUNDTRIP_MEM(
        "fn make_counter() {\n"
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
        "}\n"
    );
}

/* ── RegVM: many constants ── */

TEST(latc_reg_many_constants) {
    /* RegVM has a 256-register limit per frame, so we use fewer variables
     * but still generate many distinct constants to test the constant pool. */
    char source[16384];
    int off = 0;
    off += snprintf(source + off, sizeof(source) - (size_t)off,
                    "fn main() {\n");
    for (int i = 0; i < 100; i++) {
        off += snprintf(source + off, sizeof(source) - (size_t)off,
                        "    let v%d = \"str_%d\"\n", i, i);
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
    uint8_t data[] = { 'L', 'A', 'T', 'C', 0x01, 0x00, 0x00, 0x00 };
    char *err = NULL;
    Chunk *c = chunk_deserialize(data, sizeof(data), &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

TEST(latc_stack_deserialize_bad_magic) {
    uint8_t data[] = { 'N', 'O', 'P', 'E', 0x01, 0x00, 0x00, 0x00 };
    char *err = NULL;
    Chunk *c = chunk_deserialize(data, sizeof(data), &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

TEST(latc_stack_deserialize_bad_version) {
    /* Valid magic but wrong version */
    uint8_t data[] = { 'L', 'A', 'T', 'C', 0xFF, 0x00, 0x00, 0x00 };
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
    uint8_t data[] = { 'R', 'L', 'A', 'T', 0x02, 0x00, 0x00, 0x00 };
    char *err = NULL;
    RegChunk *c = regchunk_deserialize(data, sizeof(data), &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

TEST(latc_reg_deserialize_bad_magic) {
    uint8_t data[] = { 'N', 'O', 'P', 'E', 0x02, 0x00, 0x00, 0x00 };
    char *err = NULL;
    RegChunk *c = regchunk_deserialize(data, sizeof(data), &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

TEST(latc_reg_deserialize_bad_version) {
    uint8_t data[] = { 'R', 'L', 'A', 'T', 0xFF, 0x00, 0x00, 0x00 };
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
    const char *source =
        "fn main() {\n"
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
    for (size_t i = 0; i < tokens.len; i++)
        token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
}

TEST(latc_reg_serialize_deserialize_preserves_code) {
    const char *source =
        "fn main() {\n"
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
    for (size_t i = 0; i < original->code_len; i++) {
        ASSERT_EQ_INT(original->code[i], restored->code[i]);
    }

    /* Verify constant count matches */
    ASSERT_EQ_INT(original->const_len, restored->const_len);

    /* Verify max_reg matches */
    ASSERT_EQ_INT(original->max_reg, restored->max_reg);

    free(data);
    regchunk_free(original);
    regchunk_free(restored);

    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++)
        token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
}
