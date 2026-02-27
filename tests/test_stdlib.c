#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "lattice.h"
#include "lexer.h"
#include "parser.h"
#include "eval.h"
#include "builtins.h"
#include "net.h"
#include "tls.h"
#include "http.h"
#include "json.h"
#include "math_ops.h"
#include "env_ops.h"
#include "time_ops.h"
#include "fs_ops.h"
#include "stackcompiler.h"
#include "stackvm.h"
#include "latc.h"
#include "regvm.h"
#include "runtime.h"
#include "test_backend.h"

/* Import test harness from test_main.c */
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

#define ASSERT_STR_EQ(a, b)                                                                                    \
    do {                                                                                                       \
        const char *_a = (a), *_b = (b);                                                                       \
        if (strcmp(_a, _b) != 0) {                                                                             \
            fprintf(stderr, "  ASSERT_STR_EQ FAILED: \"%s\" != \"%s\" (%s:%d)\n", _a, _b, __FILE__, __LINE__); \
            test_current_failed = 1;                                                                           \
            return;                                                                                            \
        }                                                                                                      \
    } while (0)

/* ── Helper: run Lattice source and capture stdout ── */

static char *run_capture(const char *source) {
    /* Redirect stdout to a temp file */
    fflush(stdout);
    FILE *tmp = tmpfile();
    int old_stdout = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    /* Run the source */
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        free(lex_err);
        fflush(stdout);
        dup2(old_stdout, fileno(stdout));
        close(old_stdout);
        fclose(tmp);
        return strdup("LEX_ERROR");
    }

    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    if (parse_err) {
        size_t pe_len = strlen("PARSE_ERROR:") + strlen(parse_err) + 1;
        char *pe_out = malloc(pe_len);
        snprintf(pe_out, pe_len, "PARSE_ERROR:%s", parse_err);
        free(parse_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        fflush(stdout);
        dup2(old_stdout, fileno(stdout));
        close(old_stdout);
        fclose(tmp);
        return pe_out;
    }

    /* Run via selected backend */
    char *eval_err_str = NULL;

    if (test_backend == BACKEND_TREE_WALK) {
        Evaluator *ev = evaluator_new();
        eval_err_str = evaluator_run(ev, &prog);
        fflush(stdout);
        dup2(old_stdout, fileno(stdout));
        close(old_stdout);
        evaluator_free(ev);
    } else if (test_backend == BACKEND_STACK_VM) {
        value_set_heap(NULL);
        value_set_arena(NULL);
        char *comp_err = NULL;
        Chunk *chunk = stack_compile(&prog, &comp_err);
        if (!chunk) {
            fflush(stdout);
            dup2(old_stdout, fileno(stdout));
            close(old_stdout);
            fclose(tmp);
            program_free(&prog);
            for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
            lat_vec_free(&tokens);
            size_t err_len = strlen(comp_err) + 12;
            char *out = malloc(err_len);
            snprintf(out, err_len, "EVAL_ERROR:%s", comp_err);
            free(comp_err);
            return out;
        }
        LatRuntime rt;
        lat_runtime_init(&rt);
        StackVM vm;
        stackvm_init(&vm, &rt);
        LatValue result;
        StackVMResult vm_res = stackvm_run(&vm, chunk, &result);
        if (vm_res != STACKVM_OK) {
            eval_err_str = strdup(vm.error ? vm.error : "vm error");
        } else {
            value_free(&result);
        }
        fflush(stdout);
        dup2(old_stdout, fileno(stdout));
        close(old_stdout);
        stackvm_free(&vm);
        lat_runtime_free(&rt);
        chunk_free(chunk);
    } else if (test_backend == BACKEND_REG_VM) {
        value_set_heap(NULL);
        value_set_arena(NULL);
        char *rcomp_err = NULL;
        RegChunk *rchunk = reg_compile(&prog, &rcomp_err);
        if (!rchunk) {
            fflush(stdout);
            dup2(old_stdout, fileno(stdout));
            close(old_stdout);
            fclose(tmp);
            program_free(&prog);
            for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
            lat_vec_free(&tokens);
            size_t err_len = strlen(rcomp_err) + 12;
            char *out = malloc(err_len);
            snprintf(out, err_len, "EVAL_ERROR:%s", rcomp_err);
            free(rcomp_err);
            return out;
        }
        LatRuntime rrt;
        lat_runtime_init(&rrt);
        RegVM rvm;
        regvm_init(&rvm, &rrt);
        LatValue rresult;
        RegVMResult rvm_res = regvm_run(&rvm, rchunk, &rresult);
        if (rvm_res != REGVM_OK) {
            eval_err_str = strdup(rvm.error ? rvm.error : "regvm error");
        } else {
            value_free(&rresult);
        }
        fflush(stdout);
        dup2(old_stdout, fileno(stdout));
        close(old_stdout);
        regvm_free(&rvm);
        lat_runtime_free(&rrt);
        regchunk_free(rchunk);
    } else {
        fflush(stdout);
        dup2(old_stdout, fileno(stdout));
        close(old_stdout);
    }

    /* Read captured output */
    fseek(tmp, 0, SEEK_END);
    long len = ftell(tmp);
    fseek(tmp, 0, SEEK_SET);
    char *output = malloc((size_t)len + 1);
    size_t n = fread(output, 1, (size_t)len, tmp);
    output[n] = '\0';
    fclose(tmp);

    /* Strip trailing newline for comparison */
    if (n > 0 && output[n - 1] == '\n') output[n - 1] = '\0';

    if (eval_err_str) {
        free(output);
        size_t err_len = strlen(eval_err_str) + 12;
        output = malloc(err_len);
        snprintf(output, err_len, "EVAL_ERROR:%s", eval_err_str);
        free(eval_err_str);
    }

    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);

    return output;
}

/* Helper macro for testing Lattice source -> expected output */
#define ASSERT_OUTPUT(source, expected)                                                                              \
    do {                                                                                                             \
        char *out = run_capture(source);                                                                             \
        if (strcmp(out, expected) != 0) {                                                                            \
            fprintf(stderr,                                                                                          \
                    "  ASSERT_OUTPUT FAILED:\n    source:   %s\n    expected: %s\n    actual:   %s\n    at %s:%d\n", \
                    source, expected, out, __FILE__, __LINE__);                                                      \
            free(out);                                                                                               \
            test_current_failed = 1;                                                                                 \
            return;                                                                                                  \
        }                                                                                                            \
        free(out);                                                                                                   \
    } while (0)

/* Helper macro: assert output starts with a prefix (useful for error checks) */
#define ASSERT_OUTPUT_STARTS_WITH(source, prefix)                                                                     \
    do {                                                                                                              \
        char *out = run_capture(source);                                                                              \
        if (strncmp(out, prefix, strlen(prefix)) != 0) {                                                              \
            fprintf(stderr,                                                                                           \
                    "  ASSERT_OUTPUT_STARTS_WITH FAILED:\n    source:   %s\n    prefix:   %s\n    actual:   %s\n    " \
                    "at %s:%d\n",                                                                                     \
                    source, prefix, out, __FILE__, __LINE__);                                                         \
            free(out);                                                                                                \
            test_current_failed = 1;                                                                                  \
            return;                                                                                                   \
        }                                                                                                             \
        free(out);                                                                                                    \
    } while (0)

/* ======================================================================
 * String Methods
 * ====================================================================== */

/* 1. test_str_len - "hello".len() -> 5 */
static void test_str_len(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".len())\n"
                  "}\n",
                  "5");
    /* Empty string */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"\".len())\n"
                  "}\n",
                  "0");
}

/* 2. test_str_contains - "hello world".contains("world") -> true */
static void test_str_contains(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello world\".contains(\"world\"))\n"
                  "}\n",
                  "true");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".contains(\"xyz\"))\n"
                  "}\n",
                  "false");
    /* Empty substring always contained */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".contains(\"\"))\n"
                  "}\n",
                  "true");
}

/* 3. test_str_starts_with - "hello".starts_with("he") -> true */
static void test_str_starts_with(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".starts_with(\"he\"))\n"
                  "}\n",
                  "true");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".starts_with(\"lo\"))\n"
                  "}\n",
                  "false");
}

/* 4. test_str_ends_with - "hello".ends_with("lo") -> true */
static void test_str_ends_with(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".ends_with(\"lo\"))\n"
                  "}\n",
                  "true");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".ends_with(\"he\"))\n"
                  "}\n",
                  "false");
}

/* 5. test_str_trim - "  hello  ".trim() -> "hello" */
static void test_str_trim(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"  hello  \".trim())\n"
                  "}\n",
                  "hello");
    /* Trim with no whitespace is a no-op */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".trim())\n"
                  "}\n",
                  "hello");
}

/* 6. test_str_to_upper - "hello".to_upper() -> "HELLO" */
static void test_str_to_upper(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".to_upper())\n"
                  "}\n",
                  "HELLO");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"Hello World\".to_upper())\n"
                  "}\n",
                  "HELLO WORLD");
}

/* 7. test_str_to_lower - "HELLO".to_lower() -> "hello" */
static void test_str_to_lower(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"HELLO\".to_lower())\n"
                  "}\n",
                  "hello");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"Hello World\".to_lower())\n"
                  "}\n",
                  "hello world");
}

/* 8. test_str_replace - "hello world".replace("world", "lattice") -> "hello lattice" */
static void test_str_replace(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello world\".replace(\"world\", \"lattice\"))\n"
                  "}\n",
                  "hello lattice");
    /* Replace all occurrences */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"aabaa\".replace(\"a\", \"\"))\n"
                  "}\n",
                  "b");
}

/* 9. test_str_split - "a,b,c".split(",") -> array with 3 elements */
static void test_str_split(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let parts = \"a,b,c\".split(\",\")\n"
                  "    print(parts.len())\n"
                  "    print(parts[0])\n"
                  "    print(parts[1])\n"
                  "    print(parts[2])\n"
                  "}\n",
                  "3\na\nb\nc");
}

/* 10. test_str_index_of - "hello".index_of("ll") -> 2, not found -> -1 */
static void test_str_index_of(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".index_of(\"ll\"))\n"
                  "}\n",
                  "2");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".index_of(\"xyz\"))\n"
                  "}\n",
                  "-1");
}

/* 11. test_str_substring - "hello".substring(1, 4) -> "ell" */
static void test_str_substring(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".substring(1, 4))\n"
                  "}\n",
                  "ell");
    /* Full string */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".substring(0, 5))\n"
                  "}\n",
                  "hello");
}

/* 12. test_str_chars - "abc".chars() returns array of single-char strings */
static void test_str_chars(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let cs = \"abc\".chars()\n"
                  "    print(cs.len())\n"
                  "    print(cs[0])\n"
                  "    print(cs[1])\n"
                  "    print(cs[2])\n"
                  "}\n",
                  "3\na\nb\nc");
}

/* 13. test_str_reverse - "hello".reverse() -> "olleh" */
static void test_str_reverse(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".reverse())\n"
                  "}\n",
                  "olleh");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"\".reverse())\n"
                  "}\n",
                  "");
}

/* 14. test_str_repeat - "ab".repeat(3) -> "ababab" */
static void test_str_repeat(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"ab\".repeat(3))\n"
                  "}\n",
                  "ababab");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"x\".repeat(0))\n"
                  "}\n",
                  "");
}

/* ======================================================================
 * String Indexing and Concatenation (already working -- verification)
 * ====================================================================== */

/* 15. test_str_index - "hello"[0] -> "h", "hello"[4] -> "o" */
static void test_str_index(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\"[0])\n"
                  "}\n",
                  "h");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\"[4])\n"
                  "}\n",
                  "o");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\"[2])\n"
                  "}\n",
                  "l");
}

/* 16. test_str_concat - "hello" + " " + "world" -> "hello world" */
static void test_str_concat(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\" + \" \" + \"world\")\n"
                  "}\n",
                  "hello world");
    /* Concat with empty */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"\" + \"abc\")\n"
                  "}\n",
                  "abc");
}

/* 17. test_str_range_slice - "hello"[1..4] -> "ell" */
static void test_str_range_slice(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\"[1..4])\n"
                  "}\n",
                  "ell");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\"[0..5])\n"
                  "}\n",
                  "hello");
}

/* ======================================================================
 * Built-in Functions
 * ====================================================================== */

/* 18. test_typeof - typeof(42) -> "Int", typeof("hi") -> "String", etc. */
static void test_typeof(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(typeof(42))\n"
                  "}\n",
                  "Int");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(typeof(\"hi\"))\n"
                  "}\n",
                  "String");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(typeof(true))\n"
                  "}\n",
                  "Bool");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(typeof(3.14))\n"
                  "}\n",
                  "Float");
}

/* 19. test_phase_of - phase_of(42) -> "unphased", phase_of(freeze(42)) -> "crystal" */
static void test_phase_of(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(phase_of(42))\n"
                  "}\n",
                  "unphased");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(phase_of(freeze(42)))\n"
                  "}\n",
                  "crystal");
}

/* 20. test_to_string - to_string(42) -> "42", to_string(true) -> "true" */
static void test_to_string(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(to_string(42))\n"
                  "}\n",
                  "42");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(to_string(true))\n"
                  "}\n",
                  "true");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(to_string(3.14))\n"
                  "}\n",
                  "3.14");
}

/* 21. test_ord - ord("A") -> 65, ord("a") -> 97 */
static void test_ord(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(ord(\"A\"))\n"
                  "}\n",
                  "65");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(ord(\"a\"))\n"
                  "}\n",
                  "97");
}

/* 22. test_chr - chr(65) -> "A", chr(97) -> "a" */
static void test_chr(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(chr(65))\n"
                  "}\n",
                  "A");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(chr(97))\n"
                  "}\n",
                  "a");
}

/* ======================================================================
 * Try/Catch
 * ====================================================================== */

/* 23. test_try_catch_no_error - try block succeeds, returns try value */
static void test_try_catch_no_error(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let x = try {\n"
                  "        42\n"
                  "    } catch e {\n"
                  "        0\n"
                  "    }\n"
                  "    print(x)\n"
                  "}\n",
                  "42");
}

/* 24. test_try_catch_div_zero - catches division by zero */
static void test_try_catch_div_zero(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let result = try {\n"
                  "        let x = 1 / 0\n"
                  "        x\n"
                  "    } catch e {\n"
                  "        e.message\n"
                  "    }\n"
                  "    print(result)\n"
                  "}\n",
                  "division by zero");
}

/* 25. test_try_catch_undefined_var - catches undefined variable */
static void test_try_catch_undefined_var(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let result = try {\n"
                  "        undefined_var\n"
                  "    } catch e {\n"
                  "        \"caught\"\n"
                  "    }\n"
                  "    print(result)\n"
                  "}\n",
                  "caught");
}

/* 26. test_try_catch_nested - nested try/catch */
static void test_try_catch_nested(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let result = try {\n"
                  "        let inner = try {\n"
                  "            1 / 0\n"
                  "        } catch e {\n"
                  "            \"inner caught\"\n"
                  "        }\n"
                  "        inner\n"
                  "    } catch e {\n"
                  "        \"outer caught\"\n"
                  "    }\n"
                  "    print(result)\n"
                  "}\n",
                  "inner caught");
    /* Outer catch handles error from inner block */
    ASSERT_OUTPUT("fn main() {\n"
                  "    let result = try {\n"
                  "        try {\n"
                  "            42\n"
                  "        } catch e {\n"
                  "            e\n"
                  "        }\n"
                  "        let x = 1 / 0\n"
                  "        x\n"
                  "    } catch e {\n"
                  "        \"outer: \" + e.message\n"
                  "    }\n"
                  "    print(result)\n"
                  "}\n",
                  "outer: division by zero");
}

/* ======================================================================
 * Lattice Eval and Tokenize Built-in Functions
 * ====================================================================== */

/* 27. test_eval_simple - the Lattice eval() builtin evaluates "1 + 2" -> 3 */
static void test_eval_simple(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let result = lat_eval(\"1 + 2\")\n"
                  "    print(result)\n"
                  "}\n",
                  "3");
}

/* 28. test_eval_string - the Lattice eval() builtin evaluates a string literal */
static void test_eval_string(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let result = lat_eval(\"\\\"hello\\\"\")\n"
                  "    print(result)\n"
                  "}\n",
                  "hello");
}

/* 29. test_tokenize - the Lattice tokenize() builtin returns token array */
static void test_tokenize(void) {
    /* tokenize should return an array; verify it has elements */
    ASSERT_OUTPUT("fn main() {\n"
                  "    let tokens = tokenize(\"let x = 42\")\n"
                  "    print(tokens.len() > 0)\n"
                  "}\n",
                  "true");
}

/* ======================================================================
 * Read/Write File
 * ====================================================================== */

/* 30. test_write_and_read_file - write a temp file and read it back */
static void test_write_and_read_file(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    write_file(\"/tmp/lattice_test_stdlib.txt\", \"hello from lattice\")\n"
                  "    let content = read_file(\"/tmp/lattice_test_stdlib.txt\")\n"
                  "    print(content)\n"
                  "}\n",
                  "hello from lattice");
    /* Clean up the temp file */
    (void)remove("/tmp/lattice_test_stdlib.txt");
}

/* ======================================================================
 * Escape Sequences
 * ====================================================================== */

/* 31. test_escape_hex - \x1b produces ESC character (code 27) */
static void test_escape_hex(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(ord(\"\\x1b\"))\n"
                  "}\n",
                  "27");
    /* \x41 = 'A' */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"\\x41\")\n"
                  "}\n",
                  "A");
}

/* 32. test_escape_carriage_return - \r produces carriage return */
static void test_escape_carriage_return(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(ord(\"\\r\"))\n"
                  "}\n",
                  "13");
}

/* 33. test_escape_null_byte - \0 produces null byte */
static void test_escape_null_byte(void) {
    /* Null byte in a string - len should be 0 because C string stops at \0 */
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = \"\\0hello\"\n"
                  "    print(s.len())\n"
                  "}\n",
                  "0");
}

/* 34. test_escape_hex_error - invalid hex escape reports error */
static void test_escape_hex_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    print(\"\\xZZ\")\n"
                              "}\n",
                              "LEX_ERROR");
}

/* ======================================================================
 * Compound Assignment
 * ====================================================================== */

/* 35. test_compound_add_int - x += 5 on int */
static void test_compound_add_int(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 10\n"
                  "    x += 5\n"
                  "    print(x)\n"
                  "}\n",
                  "15");
}

/* 36. test_compound_add_string - s += " world" on string */
static void test_compound_add_string(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux s = \"hello\"\n"
                  "    s += \" world\"\n"
                  "    print(s)\n"
                  "}\n",
                  "hello world");
}

/* 37. test_compound_sub_mul_div_mod - various compound operators */
static void test_compound_sub_mul_div_mod(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 20\n"
                  "    x -= 5\n"
                  "    print(x)\n"
                  "    x *= 3\n"
                  "    print(x)\n"
                  "    x /= 5\n"
                  "    print(x)\n"
                  "    x %= 2\n"
                  "    print(x)\n"
                  "}\n",
                  "15\n45\n9\n1");
}

/* 38. test_compound_field - compound assignment on struct field */
static void test_compound_field(void) {
    ASSERT_OUTPUT("struct Counter { val: Int }\n"
                  "fn main() {\n"
                  "    flux c = Counter { val: 10 }\n"
                  "    c.val += 5\n"
                  "    print(c.val)\n"
                  "}\n",
                  "15");
}

/* 39. test_compound_index - compound assignment on array index */
static void test_compound_index(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux arr = [1, 2, 3]\n"
                  "    arr[1] += 10\n"
                  "    print(arr[1])\n"
                  "}\n",
                  "12");
}

/* ======================================================================
 * Array Methods
 * ====================================================================== */

/* 40. test_array_filter - [1,2,3,4,5].filter(|x| x > 3) -> [4, 5] */
static void test_array_filter(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let arr = [1, 2, 3, 4, 5]\n"
                  "    let filtered = arr.filter(|x| x > 3)\n"
                  "    print(filtered)\n"
                  "}\n",
                  "[4, 5]");
}

/* 41. test_array_for_each - iterate and print each */
static void test_array_for_each(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let arr = [10, 20, 30]\n"
                  "    arr.for_each(|x| print(x))\n"
                  "}\n",
                  "10\n20\n30");
}

/* 42. test_array_find - find first element matching predicate */
static void test_array_find(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let arr = [1, 2, 3, 4, 5]\n"
                  "    let found = arr.find(|x| x > 3)\n"
                  "    print(found)\n"
                  "}\n",
                  "4");
    /* Not found returns unit */
    ASSERT_OUTPUT("fn main() {\n"
                  "    let arr = [1, 2, 3]\n"
                  "    let found = arr.find(|x| x > 10)\n"
                  "    print(found)\n"
                  "}\n",
                  "()");
}

/* 43. test_array_contains - check if array contains a value */
static void test_array_contains(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let arr = [1, 2, 3]\n"
                  "    print(arr.contains(2))\n"
                  "    print(arr.contains(5))\n"
                  "}\n",
                  "true\nfalse");
}

/* 44. test_array_reverse - reverse an array */
static void test_array_reverse(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let arr = [1, 2, 3]\n"
                  "    print(arr.reverse())\n"
                  "}\n",
                  "[3, 2, 1]");
}

/* 45. test_array_enumerate - enumerate returns [index, value] pairs */
static void test_array_enumerate(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let arr = [\"a\", \"b\", \"c\"]\n"
                  "    let pairs = arr.enumerate()\n"
                  "    for pair in pairs {\n"
                  "        print(pair[0], pair[1])\n"
                  "    }\n"
                  "}\n",
                  "0 a\n1 b\n2 c");
}

/* ======================================================================
 * Parsing & Utility Built-ins
 * ====================================================================== */

/* 46. test_parse_int - parse_int("42") -> 42 */
static void test_parse_int(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(parse_int(\"42\"))\n"
                  "}\n",
                  "42");
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(parse_int(\"-7\"))\n"
                  "}\n",
                  "-7");
}

/* 47. test_parse_float - parse_float("3.14") -> 3.14 */
static void test_parse_float(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(parse_float(\"3.14\"))\n"
                  "}\n",
                  "3.14");
}

/* 48. test_len - generic len() function */
static void test_len(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(len(\"hello\"))\n"
                  "    print(len([1, 2, 3]))\n"
                  "}\n",
                  "5\n3");
}

/* 49. test_print_raw - print without newline */
static void test_print_raw(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print_raw(\"hello\")\n"
                  "    print_raw(\" world\")\n"
                  "    print(\"!\")\n"
                  "}\n",
                  "hello world!");
}

/* 50. test_eprint - print to stderr (won't show in captured stdout) */
static void test_eprint(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    eprint(\"error message\")\n"
                  "    print(\"ok\")\n"
                  "}\n",
                  "ok");
}

/* ======================================================================
 * HashMap
 * ====================================================================== */

/* 51. test_map_new - create an empty map */
static void test_map_new(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let m = Map::new()\n"
                  "    print(typeof(m))\n"
                  "}\n",
                  "Map");
}

/* 52. test_map_set_get - set and get values */
static void test_map_set_get(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m.set(\"key\", 42)\n"
                  "    print(m.get(\"key\"))\n"
                  "}\n",
                  "42");
    /* Get nonexistent key returns nil */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    print(m.get(\"nope\"))\n"
                  "}\n",
                  "nil");
}

/* 53. test_map_has - check key existence */
static void test_map_has(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m.set(\"x\", 1)\n"
                  "    print(m.has(\"x\"))\n"
                  "    print(m.has(\"y\"))\n"
                  "}\n",
                  "true\nfalse");
}

/* 54. test_map_remove - remove a key */
static void test_map_remove(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m.set(\"a\", 1)\n"
                  "    m.set(\"b\", 2)\n"
                  "    m.remove(\"a\")\n"
                  "    print(m.has(\"a\"))\n"
                  "    print(m.get(\"b\"))\n"
                  "}\n",
                  "false\n2");
}

/* 55. test_map_keys_values - get keys and values arrays */
static void test_map_keys_values(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m.set(\"a\", 1)\n"
                  "    let ks = m.keys()\n"
                  "    let vs = m.values()\n"
                  "    print(ks.len())\n"
                  "    print(vs[0])\n"
                  "}\n",
                  "1\n1");
}

/* 56. test_map_len - map length */
static void test_map_len(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    print(m.len())\n"
                  "    m.set(\"x\", 1)\n"
                  "    m.set(\"y\", 2)\n"
                  "    print(m.len())\n"
                  "}\n",
                  "0\n2");
}

/* 57. test_map_index_read_write - map["key"] read and write */
static void test_map_index_read_write(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"x\"] = 42\n"
                  "    print(m[\"x\"])\n"
                  "    m[\"x\"] = 99\n"
                  "    print(m[\"x\"])\n"
                  "}\n",
                  "42\n99");
}

/* 58. test_map_for_in - iterate map keys */
static void test_map_for_in(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m.set(\"hello\", 1)\n"
                  "    flux count = 0\n"
                  "    for key in m {\n"
                  "        count += 1\n"
                  "    }\n"
                  "    print(count)\n"
                  "}\n",
                  "1");
}

/* 59. test_map_display - map display format */
static void test_map_display(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m.set(\"x\", 1)\n"
                  "    let s = to_string(m)\n"
                  "    // Should contain \"x\": 1\n"
                  "    print(s.contains(\"x\"))\n"
                  "}\n",
                  "true");
}

/* 60. test_map_freeze_thaw - freeze and thaw a map */
static void test_map_freeze_thaw(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m.set(\"a\", 1)\n"
                  "    fix frozen = freeze(m)\n"
                  "    print(phase_of(frozen))\n"
                  "    flux thawed = thaw(frozen)\n"
                  "    print(phase_of(thawed))\n"
                  "    thawed.set(\"b\", 2)\n"
                  "    print(thawed.len())\n"
                  "}\n",
                  "crystal\nfluid\n2");
}

/* 61. test_map_len_builtin - len() built-in works on maps */
static void test_map_len_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m.set(\"a\", 1)\n"
                  "    m.set(\"b\", 2)\n"
                  "    print(len(m))\n"
                  "}\n",
                  "2");
}

/* 62. test_callable_struct_field - closure in struct field called as method */
static void test_callable_struct_field(void) {
    ASSERT_OUTPUT("struct Greeter { name: String, greet: Fn }\n"
                  "fn main() {\n"
                  "    let g = Greeter { name: \"World\", greet: |self| print(\"Hello, \" + self.name) }\n"
                  "    g.greet()\n"
                  "}\n",
                  "Hello, World");
}

/* 63. test_callable_struct_field_with_args - closure field with extra args */
static void test_callable_struct_field_with_args(void) {
    ASSERT_OUTPUT("struct Calc { value: Int, add: Fn }\n"
                  "fn main() {\n"
                  "    let c = Calc { value: 10, add: |self, n| print(self.value + n) }\n"
                  "    c.add(5)\n"
                  "}\n",
                  "15");
}

/* 64. test_callable_struct_field_returns - closure field returns a value */
static void test_callable_struct_field_returns(void) {
    ASSERT_OUTPUT("struct Counter { val: Int, next: Fn }\n"
                  "fn main() {\n"
                  "    let c = Counter { val: 42, next: |self| self.val + 1 }\n"
                  "    print(c.next())\n"
                  "}\n",
                  "43");
}

/* 65. test_callable_struct_non_closure_field - regular field access still works */
static void test_callable_struct_non_closure_field(void) {
    ASSERT_OUTPUT("struct Point { x: Int, y: Int }\n"
                  "fn main() {\n"
                  "    let p = Point { x: 3, y: 4 }\n"
                  "    print(p.x + p.y)\n"
                  "}\n",
                  "7");
}

/* ======================================================================
 * Block Closures and Block Expressions
 * ====================================================================== */

/* 66. test_block_closure_basic - |x| { let y = x + 1; y } */
static void test_block_closure_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let f = |x| { let y = x + 1; y }\n"
                  "    print(f(5))\n"
                  "}\n",
                  "6");
}

/* 67. test_block_closure_multi_stmt - multiple statements, last is return value */
static void test_block_closure_multi_stmt(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let f = |x| {\n"
                  "        let a = x * 2\n"
                  "        let b = a + 3\n"
                  "        b\n"
                  "    }\n"
                  "    print(f(10))\n"
                  "}\n",
                  "23");
}

/* 68. test_block_closure_in_map - arr.map(|x| { let sq = x * x; sq }) */
static void test_block_closure_in_map(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let arr = [1, 2, 3]\n"
                  "    let result = arr.map(|x| { let sq = x * x; sq })\n"
                  "    print(result)\n"
                  "}\n",
                  "[1, 4, 9]");
}

/* 69. test_block_expr_standalone - let x = { let a = 1; a + 2 } */
static void test_block_expr_standalone(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let x = { let a = 1; a + 2 }\n"
                  "    print(x)\n"
                  "}\n",
                  "3");
}

/* 70. test_callable_field_block_body - struct field closure with block body */
static void test_callable_field_block_body(void) {
    ASSERT_OUTPUT("struct Doubler { factor: Int, compute: Fn }\n"
                  "fn main() {\n"
                  "    let d = Doubler { factor: 3, compute: |self, x| {\n"
                  "        let result = self.factor * x\n"
                  "        result\n"
                  "    }}\n"
                  "    print(d.compute(7))\n"
                  "}\n",
                  "21");
}

/* ======================================================================
 * is_complete Builtin
 * ====================================================================== */

/* 71. test_is_complete_true - complete expression returns true */
static void test_is_complete_true(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(is_complete(\"print(1)\"))\n"
                  "}\n",
                  "true");
}

/* 72. test_is_complete_unclosed_brace - "fn main() {" returns false */
static void test_is_complete_unclosed_brace(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(is_complete(\"fn main() {\"))\n"
                  "}\n",
                  "false");
}

/* 73. test_is_complete_unclosed_paren - "print(" returns false */
static void test_is_complete_unclosed_paren(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(is_complete(\"print(\"))\n"
                  "}\n",
                  "false");
}

/* 74. test_is_complete_balanced - complete but invalid code returns true */
static void test_is_complete_balanced(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(is_complete(\"let x = }\"))\n"
                  "}\n",
                  "true");
}

/* ======================================================================
 * lat_eval Persistence (REPL support)
 * ====================================================================== */

/* 75. test_lat_eval_var_persistence - variables persist across lat_eval calls */
static void test_lat_eval_var_persistence(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    lat_eval(\"let x = 42\")\n"
                  "    let result = lat_eval(\"x + 10\")\n"
                  "    print(result)\n"
                  "}\n",
                  "52");
}

/* 76. test_lat_eval_fn_persistence - functions persist across lat_eval calls */
static void test_lat_eval_fn_persistence(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    lat_eval(\"fn add(a: Int, b: Int) -> Int { return a + b }\")\n"
                  "    let result = lat_eval(\"add(3, 4)\")\n"
                  "    print(result)\n"
                  "}\n",
                  "7");
}

/* 77. test_lat_eval_struct_persistence - structs persist across lat_eval calls */
static void test_lat_eval_struct_persistence(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    lat_eval(\"struct Point { x: Int, y: Int }\")\n"
                  "    lat_eval(\"let p = Point { x: 3, y: 4 }\")\n"
                  "    let result = lat_eval(\"p.x + p.y\")\n"
                  "    print(result)\n"
                  "}\n",
                  "7");
}

/* 78. test_lat_eval_mutable_var - mutable variables can be updated across calls */
static void test_lat_eval_mutable_var(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    lat_eval(\"flux counter = 0\")\n"
                  "    lat_eval(\"counter += 1\")\n"
                  "    lat_eval(\"counter += 1\")\n"
                  "    let result = lat_eval(\"counter\")\n"
                  "    print(result)\n"
                  "}\n",
                  "2");
}

/* 79. test_lat_eval_version - version() returns a string */
static void test_lat_eval_version(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(version())\n"
                  "}\n",
                  "0.3.27");
}

/* ======================================================================
 * TCP Networking
 * ====================================================================== */

/* 80. test_tcp_listen_close - create a listening socket and close it */
static void test_tcp_listen_close(void) {
    char *err = NULL;
    int fd = net_tcp_listen("127.0.0.1", 0, &err);
    ASSERT(fd >= 0);
    ASSERT(err == NULL);
    net_tcp_close(fd);
}

/* 81. test_tcp_connect_write_read - full loopback send/receive */
static void test_tcp_connect_write_read(void) {
    char *err = NULL;

    /* Listen on a random port — use port 0 and read back the assigned port */
    int server = net_tcp_listen("127.0.0.1", 0, &err);
    ASSERT(server >= 0);
    ASSERT(err == NULL);

    /* Get the port the OS assigned */
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    getsockname(server, (struct sockaddr *)&addr, &alen);
    int port = ntohs(addr.sin_port);

    pid_t pid = fork();
    ASSERT(pid >= 0);

    if (pid == 0) {
        /* Child: connect and send */
        usleep(10000); /* 10ms — let parent call accept */
        char *cerr = NULL;
        int cfd = net_tcp_connect("127.0.0.1", port, &cerr);
        if (cfd < 0) _exit(1);
        net_tcp_write(cfd, "hello", 5, &cerr);
        net_tcp_close(cfd);
        _exit(0);
    }

    /* Parent: accept and read */
    int client = net_tcp_accept(server, &err);
    ASSERT(client >= 0);

    char *data = net_tcp_read(client, &err);
    ASSERT(data != NULL);
    ASSERT_STR_EQ(data, "hello");
    free(data);

    net_tcp_close(client);
    net_tcp_close(server);

    int status;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* 82. test_tcp_peer_addr - verify peer address string format */
static void test_tcp_peer_addr(void) {
    char *err = NULL;

    int server = net_tcp_listen("127.0.0.1", 0, &err);
    ASSERT(server >= 0);

    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    getsockname(server, (struct sockaddr *)&addr, &alen);
    int port = ntohs(addr.sin_port);

    pid_t pid = fork();
    ASSERT(pid >= 0);

    if (pid == 0) {
        usleep(10000);
        char *cerr = NULL;
        int cfd = net_tcp_connect("127.0.0.1", port, &cerr);
        if (cfd < 0) _exit(1);
        usleep(50000);
        net_tcp_close(cfd);
        _exit(0);
    }

    int client = net_tcp_accept(server, &err);
    ASSERT(client >= 0);

    char *peer = net_tcp_peer_addr(client, &err);
    ASSERT(peer != NULL);
    /* Should start with 127.0.0.1: */
    ASSERT(strncmp(peer, "127.0.0.1:", 10) == 0);
    free(peer);

    net_tcp_close(client);
    net_tcp_close(server);

    int status;
    waitpid(pid, &status, 0);
}

/* 83. test_tcp_set_timeout - set timeout on a socket */
static void test_tcp_set_timeout(void) {
    char *err = NULL;
    int fd = net_tcp_listen("127.0.0.1", 0, &err);
    ASSERT(fd >= 0);

    bool ok = net_tcp_set_timeout(fd, 1, &err);
    ASSERT(ok);
    ASSERT(err == NULL);

    net_tcp_close(fd);
}

/* 84. test_tcp_invalid_fd - operations on non-tracked fd produce errors */
static void test_tcp_invalid_fd(void) {
    char *err = NULL;
    int result = net_tcp_accept(999, &err);
    ASSERT(result == -1);
    ASSERT(err != NULL);
    free(err);

    err = NULL;
    char *data = net_tcp_read(999, &err);
    ASSERT(data == NULL);
    ASSERT(err != NULL);
    free(err);
}

/* 85. test_tcp_lattice_integration - test TCP builtins from Lattice code */
static void test_tcp_lattice_integration(void) {
    /* Test that tcp_listen and tcp_close work from Lattice */
    ASSERT_OUTPUT("fn main() {\n"
                  "    let server = tcp_listen(\"127.0.0.1\", 0)\n"
                  "    print(server >= 0)\n"
                  "    tcp_close(server)\n"
                  "    print(\"done\")\n"
                  "}\n",
                  "true\ndone");
}

/* 86. test_tcp_error_handling - bad args produce eval errors */
static void test_tcp_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    tcp_listen(123, 80)\n"
                              "}\n",
                              "EVAL_ERROR:");
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    tcp_read(\"bad\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* ======================================================================
 * require()
 * ====================================================================== */

/* 87. test_require_basic - require a file and call its function */
static void test_require_basic(void) {
    /* Write a library file */
    builtin_write_file("/tmp/lattice_test_lib.lat", "fn helper() -> Int { return 42 }\n");

    ASSERT_OUTPUT("fn main() {\n"
                  "    require(\"/tmp/lattice_test_lib\")\n"
                  "    print(helper())\n"
                  "}\n",
                  "42");
    (void)remove("/tmp/lattice_test_lib.lat");
}

/* 88. test_require_with_extension - require with .lat extension works */
static void test_require_with_extension(void) {
    builtin_write_file("/tmp/lattice_test_lib2.lat", "fn helper2() -> Int { return 99 }\n");

    ASSERT_OUTPUT("fn main() {\n"
                  "    require(\"/tmp/lattice_test_lib2.lat\")\n"
                  "    print(helper2())\n"
                  "}\n",
                  "99");
    (void)remove("/tmp/lattice_test_lib2.lat");
}

/* 89. test_require_dedup - requiring same file twice is a no-op */
static void test_require_dedup(void) {
    builtin_write_file("/tmp/lattice_test_dedup.lat", "fn dedup_fn() -> Int { return 7 }\n");

    ASSERT_OUTPUT("fn main() {\n"
                  "    require(\"/tmp/lattice_test_dedup\")\n"
                  "    require(\"/tmp/lattice_test_dedup\")\n"
                  "    require(\"/tmp/lattice_test_dedup.lat\")\n"
                  "    print(dedup_fn())\n"
                  "}\n",
                  "7");
    (void)remove("/tmp/lattice_test_dedup.lat");
}

/* 90. test_require_structs - require a file that defines structs */
static void test_require_structs(void) {
    builtin_write_file("/tmp/lattice_test_structs.lat", "struct Pair { a: Int, b: Int }\n"
                                                        "fn make_pair(x: Int, y: Int) -> Pair {\n"
                                                        "    return Pair { a: x, b: y }\n"
                                                        "}\n");

    ASSERT_OUTPUT("fn main() {\n"
                  "    require(\"/tmp/lattice_test_structs\")\n"
                  "    let p = make_pair(3, 4)\n"
                  "    print(p.a + p.b)\n"
                  "}\n",
                  "7");
    (void)remove("/tmp/lattice_test_structs.lat");
}

/* 91. test_require_missing - require a nonexistent file produces error */
static void test_require_missing(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    require(\"/tmp/lattice_no_such_file_xyz\")\n"
                              "}\n",
                              "EVAL_ERROR:require: cannot find");
}

/* 92. test_require_nested - transitive require */
static void test_require_nested(void) {
    builtin_write_file("/tmp/lattice_test_base.lat", "fn base_fn() -> Int { return 10 }\n");
    builtin_write_file("/tmp/lattice_test_mid.lat", "require(\"/tmp/lattice_test_base\")\n"
                                                    "fn mid_fn() -> Int { return base_fn() + 5 }\n");

    ASSERT_OUTPUT("fn main() {\n"
                  "    require(\"/tmp/lattice_test_mid\")\n"
                  "    print(mid_fn())\n"
                  "}\n",
                  "15");
    (void)remove("/tmp/lattice_test_base.lat");
    (void)remove("/tmp/lattice_test_mid.lat");
}

/* ======================================================================
 * TLS Networking
 * ====================================================================== */

/* 93. test_tls_available - tls_available matches build config */
static void test_tls_available(void) {
#ifdef LATTICE_HAS_TLS
    ASSERT(net_tls_available() == true);
#else
    ASSERT(net_tls_available() == false);
#endif
}

/* 94. test_tls_connect_read - connect to httpbin.org:443 and read response */
#ifdef LATTICE_HAS_TLS
static void test_tls_connect_read(void) {
    char *err = NULL;
    int fd = net_tls_connect("httpbin.org", 443, &err);
    ASSERT(fd >= 0);
    ASSERT(err == NULL);

    const char *req = "GET /get HTTP/1.1\r\nHost: httpbin.org\r\nConnection: close\r\n\r\n";
    bool ok = net_tls_write(fd, req, strlen(req), &err);
    ASSERT(ok);
    ASSERT(err == NULL);

    char *data = net_tls_read(fd, &err);
    ASSERT(data != NULL);
    ASSERT(strncmp(data, "HTTP/1.1", 8) == 0);
    free(data);

    net_tls_close(fd);
}
#endif

/* 95. test_tls_invalid_fd - operations on bad fd return errors */
static void test_tls_invalid_fd(void) {
    char *err = NULL;
    char *data = net_tls_read(999, &err);
    ASSERT(data == NULL);
    ASSERT(err != NULL);
    free(err);

    err = NULL;
    bool ok = net_tls_write(999, "hi", 2, &err);
    ASSERT(!ok);
    ASSERT(err != NULL);
    free(err);
}

/* 96. test_tls_lattice_integration - tls_available() from Lattice code */
static void test_tls_lattice_integration(void) {
#ifdef LATTICE_HAS_TLS
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(tls_available())\n"
                  "}\n",
                  "true");
#else
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(tls_available())\n"
                  "}\n",
                  "false");
#endif
}

/* 97. test_tls_error_handling - bad arg types produce eval errors */
static void test_tls_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    tls_connect(123, 443)\n"
                              "}\n",
                              "EVAL_ERROR:");
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    tls_read(\"bad\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* ======================================================================
 * JSON Tests
 * ====================================================================== */

static void test_json_parse_object(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let obj = json_parse(\"{\\\"name\\\": \\\"Alice\\\", \\\"age\\\": 30}\")\n"
                  "    print(obj[\"name\"])\n"
                  "    print(to_string(obj[\"age\"]))\n"
                  "}\n",
                  "Alice\n30");
}

static void test_json_parse_array(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let arr = json_parse(\"[1, 2, 3]\")\n"
                  "    print(to_string(len(arr)))\n"
                  "    print(to_string(arr[0]))\n"
                  "    print(to_string(arr[2]))\n"
                  "}\n",
                  "3\n1\n3");
}

static void test_json_parse_nested(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let data = json_parse(\"{\\\"items\\\": [1, 2], \\\"ok\\\": true}\")\n"
                  "    print(to_string(data[\"ok\"]))\n"
                  "    print(to_string(len(data[\"items\"])))\n"
                  "}\n",
                  "true\n2");
}

static void test_json_parse_primitives(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(to_string(json_parse(\"42\")))\n"
                  "    print(to_string(json_parse(\"3.14\")))\n"
                  "    print(to_string(json_parse(\"true\")))\n"
                  "    print(to_string(json_parse(\"false\")))\n"
                  "    print(to_string(json_parse(\"null\")))\n"
                  "}\n",
                  "42\n3.14\ntrue\nfalse\nnil");
}

static void test_json_stringify_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(json_stringify(42))\n"
                  "    print(json_stringify(\"hello\"))\n"
                  "    print(json_stringify(true))\n"
                  "    print(json_stringify(false))\n"
                  "}\n",
                  "42\n\"hello\"\ntrue\nfalse");
}

static void test_json_stringify_array(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(json_stringify([1, 2, 3]))\n"
                  "}\n",
                  "[1,2,3]");
}

static void test_json_roundtrip(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let json = \"{\\\"a\\\": 1}\"\n"
                  "    let obj = json_parse(json)\n"
                  "    let back = json_stringify(obj)\n"
                  "    let obj2 = json_parse(back)\n"
                  "    print(to_string(obj2[\"a\"]))\n"
                  "}\n",
                  "1");
}

static void test_json_parse_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    json_parse(\"{bad json}\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_json_stringify_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    json_stringify(123, 456)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* ======================================================================
 * Math Tests
 * ====================================================================== */

static void test_math_abs(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(to_string(abs(-5)))\n"
                  "    print(to_string(abs(5)))\n"
                  "    print(to_string(abs(-3.14)))\n"
                  "}\n",
                  "5\n5\n3.14");
}

static void test_math_floor_ceil_round(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(to_string(floor(3.7)))\n"
                  "    print(to_string(ceil(3.2)))\n"
                  "    print(to_string(round(3.5)))\n"
                  "    print(to_string(round(3.4)))\n"
                  "}\n",
                  "3\n4\n4\n3");
}

static void test_math_sqrt(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(to_string(sqrt(9)))\n"
                  "    print(to_string(sqrt(4)))\n"
                  "}\n",
                  "3\n2");
}

static void test_math_sqrt_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    sqrt(-1)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_math_pow(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(to_string(pow(2, 10)))\n"
                  "    print(to_string(pow(3, 0)))\n"
                  "}\n",
                  "1024\n1");
}

static void test_math_min_max(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(to_string(min(3, 7)))\n"
                  "    print(to_string(max(3, 7)))\n"
                  "    print(to_string(min(1.5, 2.5)))\n"
                  "    print(to_string(max(1.5, 2.5)))\n"
                  "}\n",
                  "3\n7\n1.5\n2.5");
}

static void test_math_random(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = random()\n"
                  "    if r >= 0.0 {\n"
                  "        if r < 1.0 {\n"
                  "            print(\"ok\")\n"
                  "        }\n"
                  "    }\n"
                  "}\n",
                  "ok");
}

static void test_math_random_int(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = random_int(1, 10)\n"
                  "    if r >= 1 {\n"
                  "        if r <= 10 {\n"
                  "            print(\"ok\")\n"
                  "        }\n"
                  "    }\n"
                  "}\n",
                  "ok");
}

/* ======================================================================
 * Environment Variable Tests
 * ====================================================================== */

static void test_env_get(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let path = env(\"PATH\")\n"
                  "    if len(path) > 0 {\n"
                  "        print(\"ok\")\n"
                  "    }\n"
                  "}\n",
                  "ok");
}

static void test_env_get_missing(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let val = env(\"LATTICE_NONEXISTENT_VAR_12345\")\n"
                  "    print(to_string(val))\n"
                  "}\n",
                  "()");
}

static void test_env_set_get(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    env_set(\"LATTICE_TEST_VAR\", \"hello\")\n"
                  "    print(env(\"LATTICE_TEST_VAR\"))\n"
                  "}\n",
                  "hello");
}

static void test_env_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    env(123)\n"
                              "}\n",
                              "EVAL_ERROR:");
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    env_set(123, \"val\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* ======================================================================
 * Time Tests
 * ====================================================================== */

static void test_time_now(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let t = time()\n"
                  "    if t > 0 {\n"
                  "        print(\"ok\")\n"
                  "    }\n"
                  "}\n",
                  "ok");
}

static void test_time_sleep(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let before = time()\n"
                  "    sleep(50)\n"
                  "    let after = time()\n"
                  "    if after - before >= 40 {\n"
                  "        print(\"ok\")\n"
                  "    }\n"
                  "}\n",
                  "ok");
}

static void test_time_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    sleep(\"bad\")\n"
                              "}\n",
                              "EVAL_ERROR:");
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    time(1)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* ======================================================================
 * Filesystem Operations
 * ====================================================================== */

/* test_file_exists - file_exists returns true for existing file, false otherwise */
static void test_file_exists(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    write_file(\"/tmp/lattice_test_exists.txt\", \"hi\")\n"
                  "    print(file_exists(\"/tmp/lattice_test_exists.txt\"))\n"
                  "    print(file_exists(\"/tmp/lattice_test_no_such_file_xyz.txt\"))\n"
                  "}\n",
                  "true\nfalse");
    (void)remove("/tmp/lattice_test_exists.txt");
}

/* test_delete_file - delete_file removes an existing file */
static void test_delete_file(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    write_file(\"/tmp/lattice_test_del.txt\", \"bye\")\n"
                  "    print(file_exists(\"/tmp/lattice_test_del.txt\"))\n"
                  "    delete_file(\"/tmp/lattice_test_del.txt\")\n"
                  "    print(file_exists(\"/tmp/lattice_test_del.txt\"))\n"
                  "}\n",
                  "true\nfalse");
}

/* test_delete_file_error - deleting nonexistent file produces error */
static void test_delete_file_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    delete_file(\"/tmp/lattice_test_no_such_file_xyz.txt\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* test_list_dir - list_dir returns array of filenames */
static void test_list_dir(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    write_file(\"/tmp/lattice_test_listdir_a.txt\", \"a\")\n"
                  "    write_file(\"/tmp/lattice_test_listdir_b.txt\", \"b\")\n"
                  "    let entries = list_dir(\"/tmp\")\n"
                  "    // entries should be an array with at least 2 elements\n"
                  "    print(typeof(entries))\n"
                  "    let found_a = entries.contains(\"lattice_test_listdir_a.txt\")\n"
                  "    let found_b = entries.contains(\"lattice_test_listdir_b.txt\")\n"
                  "    print(found_a)\n"
                  "    print(found_b)\n"
                  "}\n",
                  "Array\ntrue\ntrue");
    (void)remove("/tmp/lattice_test_listdir_a.txt");
    (void)remove("/tmp/lattice_test_listdir_b.txt");
}

/* test_list_dir_error - listing nonexistent directory produces error */
static void test_list_dir_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    list_dir(\"/tmp/lattice_no_such_dir_xyz\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* test_append_file - append_file adds data to existing file */
static void test_append_file(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    write_file(\"/tmp/lattice_test_append.txt\", \"hello\")\n"
                  "    append_file(\"/tmp/lattice_test_append.txt\", \" world\")\n"
                  "    let content = read_file(\"/tmp/lattice_test_append.txt\")\n"
                  "    print(content)\n"
                  "}\n",
                  "hello world");
    (void)remove("/tmp/lattice_test_append.txt");
}

/* test_append_file_creates - append_file creates file if it doesn't exist */
static void test_append_file_creates(void) {
    (void)remove("/tmp/lattice_test_append_new.txt");
    ASSERT_OUTPUT("fn main() {\n"
                  "    append_file(\"/tmp/lattice_test_append_new.txt\", \"new content\")\n"
                  "    let content = read_file(\"/tmp/lattice_test_append_new.txt\")\n"
                  "    print(content)\n"
                  "}\n",
                  "new content");
    (void)remove("/tmp/lattice_test_append_new.txt");
}

/* test_fs_error_handling - bad arg types produce eval errors */
static void test_fs_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    file_exists(123)\n"
                              "}\n",
                              "EVAL_ERROR:");
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    delete_file(123)\n"
                              "}\n",
                              "EVAL_ERROR:");
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    list_dir(123)\n"
                              "}\n",
                              "EVAL_ERROR:");
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    append_file(123, \"data\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* ======================================================================
 * Regex Builtins
 * ====================================================================== */

/* regex_match: pattern matches */
static void test_regex_match_true(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(regex_match(\"[0-9]+\", \"abc123\"))\n"
                  "}\n",
                  "true");
}

/* regex_match: pattern does not match */
static void test_regex_match_false(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(regex_match(\"^[0-9]+$\", \"abc\"))\n"
                  "}\n",
                  "false");
}

/* regex_match: full string anchor */
static void test_regex_match_anchored(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(regex_match(\"^hello$\", \"hello\"))\n"
                  "}\n",
                  "true");
}

/* regex_find_all: multiple matches */
static void test_regex_find_all_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let matches = regex_find_all(\"[0-9]+\", \"a1b22c333\")\n"
                  "    print(matches)\n"
                  "}\n",
                  "[1, 22, 333]");
}

/* regex_find_all: no matches returns empty array */
static void test_regex_find_all_no_match(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let matches = regex_find_all(\"[0-9]+\", \"abc\")\n"
                  "    print(len(matches))\n"
                  "}\n",
                  "0");
}

/* regex_find_all: word matches */
static void test_regex_find_all_words(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let matches = regex_find_all(\"[a-z]+\", \"foo123bar456baz\")\n"
                  "    print(matches)\n"
                  "}\n",
                  "[foo, bar, baz]");
}

/* regex_replace: basic replacement */
static void test_regex_replace_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(regex_replace(\"[0-9]+\", \"a1b2\", \"X\"))\n"
                  "}\n",
                  "aXbX");
}

/* regex_replace: no match returns original */
static void test_regex_replace_no_match(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(regex_replace(\"[0-9]+\", \"abc\", \"X\"))\n"
                  "}\n",
                  "abc");
}

/* regex_replace: replace all whitespace */
static void test_regex_replace_whitespace(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(regex_replace(\"[ ]+\", \"hello   world   foo\", \"-\"))\n"
                  "}\n",
                  "hello-world-foo");
}

/* regex_match: bad pattern returns error */
static void test_regex_match_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    print(regex_match(\"[\", \"test\"))\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* regex_replace: empty replacement (deletion) */
static void test_regex_replace_delete(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(regex_replace(\"[0-9]\", \"a1b2c3\", \"\"))\n"
                  "}\n",
                  "abc");
}

/* regex_match: case-insensitive flag */
static void test_regex_case_insensitive(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(regex_match(\"[a-z]+\", \"HELLO\", \"i\"))\n"
                  "}\n",
                  "true");
}

/* regex_find_all: multiline flag */
static void test_regex_multiline(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let matches = regex_find_all(\"^line\", \"line1\\nline2\", \"m\")\n"
                  "    print(len(matches))\n"
                  "}\n",
                  "2");
}

/* regex_match: combined flags "im" */
static void test_regex_combined_flags(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(regex_match(\"^hello\", \"world\\nHELLO\", \"im\"))\n"
                  "}\n",
                  "true");
}

/* regex_replace: case-insensitive flag */
static void test_regex_replace_flags(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(regex_replace(\"hello\", \"HELLO world\", \"hi\", \"i\"))\n"
                  "}\n",
                  "hi world");
}

/* regex: no flags backward compatibility */
static void test_regex_no_flags_backward_compat(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(regex_match(\"[0-9]+\", \"abc123\"))\n"
                  "    print(regex_find_all(\"[a-z]+\", \"foo123bar\"))\n"
                  "    print(regex_replace(\"[0-9]\", \"a1b2\", \"X\"))\n"
                  "}\n",
                  "true\n[foo, bar]\naXbX");
}

/* regex_match: invalid flag returns error */
static void test_regex_invalid_flag(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    print(regex_match(\"x\", \"x\", \"z\"))\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* ======================================================================
 * format() Builtin
 * ====================================================================== */

static void test_format_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(format(\"Hello, {}!\", \"world\"))\n"
                  "}\n",
                  "Hello, world!");
}

static void test_format_multiple(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(format(\"{} + {} = {}\", 1, 2, 3))\n"
                  "}\n",
                  "1 + 2 = 3");
}

static void test_format_no_placeholders(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(format(\"empty\"))\n"
                  "}\n",
                  "empty");
}

static void test_format_escaped_braces(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(format(\"{{literal}}\"))\n"
                  "}\n",
                  "{literal}");
}

static void test_format_bool(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(format(\"{}\", true))\n"
                  "}\n",
                  "true");
}

static void test_format_too_few_args(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    format(\"{} {}\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_format_mixed_types(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(format(\"{} is {} and {}\", \"pi\", 3.14, true))\n"
                  "}\n",
                  "pi is 3.14 and true");
}

static void test_format_error_non_string_fmt(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    format(42)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* ======================================================================
 * Crypto / Base64 Tests
 * ====================================================================== */

#ifdef LATTICE_HAS_TLS
static void test_sha256_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(sha256(\"\"))\n"
                  "}\n",
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

static void test_sha256_hello(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(sha256(\"hello\"))\n"
                  "}\n",
                  "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

static void test_md5_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(md5(\"\"))\n"
                  "}\n",
                  "d41d8cd98f00b204e9800998ecf8427e");
}

static void test_md5_hello(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(md5(\"hello\"))\n"
                  "}\n",
                  "5d41402abc4b2a76b9719d911017c592");
}
#endif

static void test_sha256_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    sha256(123)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_md5_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    md5(123)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_base64_encode_hello(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(base64_encode(\"Hello\"))\n"
                  "}\n",
                  "SGVsbG8=");
}

static void test_base64_encode_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(base64_encode(\"\"))\n"
                  "}\n",
                  "");
}

static void test_base64_decode_hello(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(base64_decode(\"SGVsbG8=\"))\n"
                  "}\n",
                  "Hello");
}

static void test_base64_decode_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(base64_decode(\"\"))\n"
                  "}\n",
                  "");
}

static void test_base64_roundtrip(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(base64_decode(base64_encode(\"test\")))\n"
                  "}\n",
                  "test");
}

static void test_base64_roundtrip_longer(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(base64_decode(base64_encode(\"Hello, World!\")))\n"
                  "}\n",
                  "Hello, World!");
}

static void test_base64_encode_padding(void) {
    /* 1 byte -> 4 chars with == padding */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(base64_encode(\"a\"))\n"
                  "}\n",
                  "YQ==");
    /* 2 bytes -> 4 chars with = padding */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(base64_encode(\"ab\"))\n"
                  "}\n",
                  "YWI=");
    /* 3 bytes -> 4 chars, no padding */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(base64_encode(\"abc\"))\n"
                  "}\n",
                  "YWJj");
}

static void test_base64_decode_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    base64_decode(\"!!!\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_base64_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    base64_encode(123)\n"
                              "}\n",
                              "EVAL_ERROR:");
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    base64_decode(123)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* ======================================================================
 * Array: sort, flat, reduce, slice
 * ====================================================================== */

static void test_array_sort_int(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([3, 1, 2].sort())\n"
                  "}\n",
                  "[1, 2, 3]");
}

static void test_array_sort_string(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([\"c\", \"a\", \"b\"].sort())\n"
                  "}\n",
                  "[a, b, c]");
}

static void test_array_sort_float(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([3.1, 1.5, 2.7].sort())\n"
                  "}\n",
                  "[1.5, 2.7, 3.1]");
}

static void test_array_sort_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([].sort())\n"
                  "}\n",
                  "[]");
}

static void test_array_sort_mixed_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    [1, \"a\"].sort()\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_array_flat_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, [2, 3], [4]].flat())\n"
                  "}\n",
                  "[1, 2, 3, 4]");
}

static void test_array_flat_no_nesting(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3].flat())\n"
                  "}\n",
                  "[1, 2, 3]");
}

static void test_array_flat_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([].flat())\n"
                  "}\n",
                  "[]");
}

static void test_array_reduce_sum(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3].reduce(|acc, x| { acc + x }, 0))\n"
                  "}\n",
                  "6");
}

static void test_array_reduce_product(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3, 4].reduce(|acc, x| { acc * x }, 1))\n"
                  "}\n",
                  "24");
}

static void test_array_reduce_string_concat(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([\"a\", \"b\", \"c\"].reduce(|acc, x| { acc + x }, \"\"))\n"
                  "}\n",
                  "abc");
}

static void test_array_reduce_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([].reduce(|acc, x| { acc + x }, 42))\n"
                  "}\n",
                  "42");
}

static void test_array_slice_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3, 4, 5].slice(1, 3))\n"
                  "}\n",
                  "[2, 3]");
}

static void test_array_slice_full(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3].slice(0, 3))\n"
                  "}\n",
                  "[1, 2, 3]");
}

static void test_array_slice_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3].slice(1, 1))\n"
                  "}\n",
                  "[]");
}

static void test_array_slice_clamped(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3].slice(0, 100))\n"
                  "}\n",
                  "[1, 2, 3]");
}

/* ======================================================================
 * Date/Time Formatting Tests
 * ====================================================================== */

/* time_parse returns a positive Int for a valid date */
static void test_time_parse_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ms = time_parse(\"2024-01-15\", \"%Y-%m-%d\")\n"
                  "    print(ms > 0)\n"
                  "}\n",
                  "true");
}

/* time_format produces a non-empty string */
static void test_time_format_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = time_format(0, \"%Y\")\n"
                  "    print(s.len() == 4)\n"
                  "}\n",
                  "true");
}

/* Round-trip: format then parse should recover the same timestamp (to second precision) */
static void test_time_roundtrip(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ms = time_parse(\"2024-06-15 12:30:45\", \"%Y-%m-%d %H:%M:%S\")\n"
                  "    let formatted = time_format(ms, \"%Y-%m-%d %H:%M:%S\")\n"
                  "    let ms2 = time_parse(formatted, \"%Y-%m-%d %H:%M:%S\")\n"
                  "    print(ms == ms2)\n"
                  "}\n",
                  "true");
}

/* time_format with ISO date format produces expected length (10 chars for YYYY-MM-DD) */
static void test_time_format_iso_date(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = time_format(1000000000000, \"%Y-%m-%d\")\n"
                  "    print(s.len() == 10)\n"
                  "}\n",
                  "true");
}

/* time_parse error: invalid date string */
static void test_time_parse_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    time_parse(\"not-a-date\", \"%Y-%m-%d\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* time_format error: wrong arg types */
static void test_time_format_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    time_format(\"bad\", \"%Y\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* time_parse error: wrong arg types */
static void test_time_parse_type_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    time_parse(123, \"%Y\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* time_format with time components */
static void test_time_format_time_components(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = time_format(1000000000000, \"%H:%M:%S\")\n"
                  "    print(s.len() == 8)\n"
                  "}\n",
                  "true");
}

/* ======================================================================
 * Path Operations
 * ====================================================================== */

/* test_path_join - join path components */
static void test_path_join(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(path_join(\"foo\", \"bar\", \"baz.txt\"))\n"
                  "}\n",
                  "foo/bar/baz.txt");
    /* Single argument */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(path_join(\"hello\"))\n"
                  "}\n",
                  "hello");
    /* Avoid double slashes */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(path_join(\"foo/\", \"/bar\"))\n"
                  "}\n",
                  "foo/bar");
    /* Absolute path */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(path_join(\"/usr\", \"local\", \"bin\"))\n"
                  "}\n",
                  "/usr/local/bin");
}

/* test_path_dir - extract directory portion */
static void test_path_dir(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(path_dir(\"/foo/bar.txt\"))\n"
                  "}\n",
                  "/foo");
    /* No slash returns "." */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(path_dir(\"bar.txt\"))\n"
                  "}\n",
                  ".");
    /* Root path */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(path_dir(\"/\"))\n"
                  "}\n",
                  "/");
    /* Nested */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(path_dir(\"/a/b/c/d.txt\"))\n"
                  "}\n",
                  "/a/b/c");
}

/* test_path_base - extract base filename */
static void test_path_base(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(path_base(\"/foo/bar.txt\"))\n"
                  "}\n",
                  "bar.txt");
    /* No directory */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(path_base(\"file.txt\"))\n"
                  "}\n",
                  "file.txt");
    /* Trailing slash returns empty */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(path_base(\"/foo/\"))\n"
                  "}\n",
                  "");
}

/* test_path_ext - extract file extension */
static void test_path_ext(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(path_ext(\"file.tar.gz\"))\n"
                  "}\n",
                  ".gz");
    /* No extension */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(path_ext(\"Makefile\"))\n"
                  "}\n",
                  "");
    /* Hidden file (dot-prefixed, no extension) */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(path_ext(\".hidden\"))\n"
                  "}\n",
                  "");
    /* Simple extension */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(path_ext(\"foo.txt\"))\n"
                  "}\n",
                  ".txt");
    /* Extension in path with directory */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(path_ext(\"/usr/local/foo.c\"))\n"
                  "}\n",
                  ".c");
}

/* test_path_error_handling - bad arg types produce eval errors */
static void test_path_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    path_join(123)\n"
                              "}\n",
                              "EVAL_ERROR:");
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    path_dir(123)\n"
                              "}\n",
                              "EVAL_ERROR:");
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    path_base(123)\n"
                              "}\n",
                              "EVAL_ERROR:");
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    path_ext(123)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* ======================================================================
 * Channel & Scope Tests
 * ====================================================================== */

static void test_channel_basic_send_recv(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ch = Channel::new()\n"
                  "    ch.send(freeze(42))\n"
                  "    let val = ch.recv()\n"
                  "    print(val)\n"
                  "}\n",
                  "42");
}

static void test_scope_two_spawns_channels(void) {
    ASSERT_OUTPUT("fn compute_a() -> Int { return 10 }\n"
                  "fn compute_b() -> Int { return 20 }\n"
                  "fn main() {\n"
                  "    let ch1 = Channel::new()\n"
                  "    let ch2 = Channel::new()\n"
                  "    scope {\n"
                  "        spawn { ch1.send(freeze(compute_a())) }\n"
                  "        spawn { ch2.send(freeze(compute_b())) }\n"
                  "    }\n"
                  "    let a = ch1.recv()\n"
                  "    let b = ch2.recv()\n"
                  "    print(a + b)\n"
                  "}\n",
                  "30");
}

static void test_channel_close_recv_unit(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ch = Channel::new()\n"
                  "    ch.send(freeze(1))\n"
                  "    ch.close()\n"
                  "    let a = ch.recv()\n"
                  "    let b = ch.recv()\n"
                  "    print(a)\n"
                  "    print(typeof(b))\n"
                  "}\n",
                  "1\nUnit");
}

static void test_channel_crystal_only_send(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    let ch = Channel::new()\n"
                              "    flux arr = [1, 2, 3]\n"
                              "    ch.send(arr)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_scope_no_spawns_sequential(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let x = scope {\n"
                  "        let a = 10\n"
                  "        let b = 20\n"
                  "        a + b\n"
                  "    }\n"
                  "    print(x)\n"
                  "}\n",
                  "30");
}

static void test_spawn_outside_scope(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let x = spawn {\n"
                  "        let a = 5\n"
                  "        let b = 10\n"
                  "        return a + b\n"
                  "    }\n"
                  "    print(x)\n"
                  "}\n",
                  "15");
}

static void test_channel_multiple_sends_fifo(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ch = Channel::new()\n"
                  "    ch.send(freeze(1))\n"
                  "    ch.send(freeze(2))\n"
                  "    ch.send(freeze(3))\n"
                  "    print(ch.recv())\n"
                  "    print(ch.recv())\n"
                  "    print(ch.recv())\n"
                  "}\n",
                  "1\n2\n3");
}

static void test_scope_spawn_error_propagates(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn bad() -> Int {\n"
                              "    let x = 1 / 0\n"
                              "    return x\n"
                              "}\n"
                              "fn main() {\n"
                              "    scope {\n"
                              "        spawn { bad() }\n"
                              "    }\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_cannot_freeze_channel(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    let ch = Channel::new()\n"
                              "    let frozen = freeze(ch)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_channel_typeof(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ch = Channel::new()\n"
                  "    print(typeof(ch))\n"
                  "}\n",
                  "Channel");
}

/* ── Array method tests ── */

static void test_array_pop(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux arr = [1, 2, 3]\n"
                  "    print(arr.pop())\n"
                  "    print(arr)\n"
                  "}\n",
                  "3\n[1, 2]");
}

static void test_array_index_of(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let arr = [10, 20, 30]\n"
                  "    print(arr.index_of(20))\n"
                  "    print(arr.index_of(99))\n"
                  "}\n",
                  "1\n-1");
}

static void test_array_any_all(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let arr = [1, 2, 3]\n"
                  "    print(arr.any(|x| { x > 2 }))\n"
                  "    print(arr.all(|x| { x > 0 }))\n"
                  "    print(arr.all(|x| { x > 1 }))\n"
                  "    print(arr.any(|x| { x > 10 }))\n"
                  "}\n",
                  "true\ntrue\nfalse\nfalse");
}

static void test_array_zip(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2, 3]\n"
                  "    let b = [4, 5]\n"
                  "    print(a.zip(b))\n"
                  "}\n",
                  "[[1, 4], [2, 5]]");
}

static void test_array_unique(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3, 2, 1, 4].unique())\n"
                  "}\n",
                  "[1, 2, 3, 4]");
}

static void test_array_insert(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux arr = [1, 2, 3]\n"
                  "    arr.insert(1, 10)\n"
                  "    print(arr)\n"
                  "}\n",
                  "[1, 10, 2, 3]");
}

static void test_array_remove_at(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux arr = [1, 2, 3]\n"
                  "    print(arr.remove_at(1))\n"
                  "    print(arr)\n"
                  "}\n",
                  "2\n[1, 3]");
}

static void test_array_sort_by(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let arr = [3, 1, 4, 1, 5]\n"
                  "    print(arr.sort_by(|a, b| { a - b }))\n"
                  "    print(arr.sort_by(|a, b| { b - a }))\n"
                  "}\n",
                  "[1, 1, 3, 4, 5]\n[5, 4, 3, 1, 1]");
}

/* ── Map method tests ── */

static void test_map_entries(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m.set(\"a\", 1)\n"
                  "    let e = m.entries()\n"
                  "    print(len(e))\n"
                  "    print(e[0][0])\n"
                  "    print(e[0][1])\n"
                  "}\n",
                  "1\na\n1");
}

static void test_map_merge(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m1 = Map::new()\n"
                  "    m1.set(\"a\", 1)\n"
                  "    flux m2 = Map::new()\n"
                  "    m2.set(\"b\", 2)\n"
                  "    m1.merge(m2)\n"
                  "    print(m1.has(\"b\"))\n"
                  "    print(m1.get(\"b\"))\n"
                  "}\n",
                  "true\n2");
}

static void test_map_for_each(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m.set(\"x\", 10)\n"
                  "    m.for_each(|k, v| { print(format(\"{} -> {}\", k, v)) })\n"
                  "}\n",
                  "x -> 10");
}

/* ── String method tests ── */

static void test_str_trim_start(void) { ASSERT_OUTPUT("fn main() { print(\"  hi  \".trim_start()) }\n", "hi  "); }

static void test_str_trim_end(void) { ASSERT_OUTPUT("fn main() { print(\"  hi  \".trim_end()) }\n", "  hi"); }

static void test_str_pad_left(void) { ASSERT_OUTPUT("fn main() { print(\"42\".pad_left(5, \"0\")) }\n", "00042"); }

static void test_str_pad_right(void) { ASSERT_OUTPUT("fn main() { print(\"hi\".pad_right(5, \".\")) }\n", "hi..."); }

/* ── Math function tests ── */

static void test_math_log(void) { ASSERT_OUTPUT("fn main() { print(log(math_e())) }\n", "1"); }

static void test_math_log2(void) { ASSERT_OUTPUT("fn main() { print(log2(8)) }\n", "3"); }

static void test_math_log10(void) { ASSERT_OUTPUT("fn main() { print(log10(1000)) }\n", "3"); }

static void test_math_trig(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(sin(0.0))\n"
                  "    print(cos(0.0))\n"
                  "    print(tan(0.0))\n"
                  "}\n",
                  "0\n1\n0");
}

static void test_math_atan2(void) { ASSERT_OUTPUT("fn main() { print(atan2(0.0, 1.0)) }\n", "0"); }

static void test_math_clamp(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(clamp(5, 1, 10))\n"
                  "    print(clamp(-3, 0, 100))\n"
                  "    print(clamp(200, 0, 100))\n"
                  "}\n",
                  "5\n0\n100");
}

static void test_math_pi_e(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(math_pi() > 3.14)\n"
                  "    print(math_e() > 2.71)\n"
                  "}\n",
                  "true\ntrue");
}

static void test_math_inverse_trig(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(format(\"{}\", asin(0.0)))\n"
                  "    print(format(\"{}\", acos(1.0)))\n"
                  "    print(format(\"{}\", atan(0.0)))\n"
                  "}\n",
                  "0\n0\n0");
}

static void test_math_exp(void) { ASSERT_OUTPUT("fn main() { print(format(\"{}\", exp(0.0))) }\n", "1"); }

static void test_math_sign(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(sign(-5))\n"
                  "    print(sign(0))\n"
                  "    print(sign(42))\n"
                  "}\n",
                  "-1\n0\n1");
}

static void test_math_gcd_lcm(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(gcd(12, 8))\n"
                  "    print(lcm(4, 6))\n"
                  "}\n",
                  "4\n12");
}

static void test_is_nan_inf(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(is_nan(0.0 / 0.0))\n"
                  "    print(is_nan(1.0))\n"
                  "    print(is_inf(1.0 / 0.0))\n"
                  "    print(is_inf(1.0))\n"
                  "}\n",
                  "true\nfalse\ntrue\nfalse");
}

static void test_math_lerp(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(lerp(0.0, 10.0, 0.5))\n"
                  "    print(lerp(0, 100, 0.25))\n"
                  "}\n",
                  "5\n25");
}

static void test_math_hyperbolic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(format(\"{}\", sinh(0.0)))\n"
                  "    print(format(\"{}\", cosh(0.0)))\n"
                  "    print(format(\"{}\", tanh(0.0)))\n"
                  "}\n",
                  "0\n1\n0");
}

/* ── System/FS tests ── */

static void test_cwd_builtin(void) {
    char *out = run_capture("fn main() { print(cwd()) }\n");
    ASSERT(strlen(out) > 0);
    ASSERT(out[0] == '/');
    free(out);
}

static void test_is_dir_file(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(is_dir(\".\"))\n"
                  "    print(is_file(\"Makefile\"))\n"
                  "    print(is_dir(\"Makefile\"))\n"
                  "    print(is_file(\"nonexistent\"))\n"
                  "}\n",
                  "true\ntrue\nfalse\nfalse");
}

static void test_mkdir_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let dir = \"/tmp/lattice_test_mkdir_\" + to_string(time())\n"
                  "    print(mkdir(dir))\n"
                  "    print(is_dir(dir))\n"
                  "}\n",
                  "true\ntrue");
}

static void test_rename_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let f1 = \"/tmp/lattice_rename_src_\" + to_string(time())\n"
                  "    let f2 = \"/tmp/lattice_rename_dst_\" + to_string(time())\n"
                  "    write_file(f1, \"hello\")\n"
                  "    print(rename(f1, f2))\n"
                  "    print(file_exists(f1))\n"
                  "    print(file_exists(f2))\n"
                  "    delete_file(f2)\n"
                  "}\n",
                  "true\nfalse\ntrue");
}

static void test_assert_pass(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    assert(true)\n"
                  "    assert(1 + 1 == 2, \"math works\")\n"
                  "    print(\"ok\")\n"
                  "}\n",
                  "ok");
}

static void test_assert_fail(void) {
    char *out = run_capture("fn main() { assert(false, \"should fail\") }\n");
    ASSERT(strstr(out, "EVAL_ERROR") != NULL);
    ASSERT(strstr(out, "should fail") != NULL);
    free(out);
}

static void test_args_builtin(void) {
    char *out = run_capture("fn main() { print(typeof(args())) }\n");
    ASSERT_STR_EQ(out, "Array");
    free(out);
}

/* ── Map .filter() and .map() tests ── */

static void test_map_filter(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m.set(\"a\", 1)\n"
                  "    m.set(\"b\", 2)\n"
                  "    m.set(\"c\", 3)\n"
                  "    let filtered = m.filter(|k, v| { v > 1 })\n"
                  "    print(filtered.len())\n"
                  "}\n",
                  "2");
    /* Filter that matches nothing */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m.set(\"x\", 10)\n"
                  "    let filtered = m.filter(|k, v| { v > 100 })\n"
                  "    print(filtered.len())\n"
                  "}\n",
                  "0");
}

static void test_map_map(void) {
    /* Single-entry map for deterministic output */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m.set(\"x\", 5)\n"
                  "    let doubled = m.map(|k, v| { v * 2 })\n"
                  "    print(doubled.get(\"x\"))\n"
                  "}\n",
                  "10");
    /* Map preserves all keys */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m.set(\"a\", 1)\n"
                  "    m.set(\"b\", 2)\n"
                  "    m.set(\"c\", 3)\n"
                  "    let mapped = m.map(|k, v| { v + 10 })\n"
                  "    print(mapped.len())\n"
                  "}\n",
                  "3");
}

/* ── String .count() and .is_empty() tests ── */

static void test_str_count(void) {
    ASSERT_OUTPUT("fn main() { print(\"hello world hello\".count(\"hello\")) }\n", "2");
    /* Zero matches */
    ASSERT_OUTPUT("fn main() { print(\"abcdef\".count(\"xyz\")) }\n", "0");
    /* Overlapping: non-overlapping count */
    ASSERT_OUTPUT("fn main() { print(\"aaa\".count(\"aa\")) }\n", "1");
}

static void test_str_is_empty(void) {
    ASSERT_OUTPUT("fn main() { print(\"\".is_empty()) }\n", "true");
    ASSERT_OUTPUT("fn main() { print(\"hello\".is_empty()) }\n", "false");
}

/* ── process exec/shell builtins ── */

static void test_exec_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let result = exec(\"echo hello\")\n"
                  "    print(result.trim())\n"
                  "}\n",
                  "hello");
}

static void test_shell_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = shell(\"echo hello\")\n"
                  "    print(r.get(\"stdout\").trim())\n"
                  "    print(r.get(\"exit_code\"))\n"
                  "}\n",
                  "hello\n0");
}

static void test_shell_stderr(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = shell(\"echo err >&2\")\n"
                  "    print(r.get(\"stderr\").trim())\n"
                  "    print(r.get(\"exit_code\"))\n"
                  "}\n",
                  "err\n0");
}

static void test_exec_failure(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = shell(\"exit 42\")\n"
                  "    print(r.get(\"exit_code\"))\n"
                  "}\n",
                  "42");
}

/* ======================================================================
 * New filesystem builtins: rmdir, glob, stat, copy_file, realpath, tempdir, tempfile
 * ====================================================================== */

static void test_rmdir_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let dir = \"/tmp/lattice_test_rmdir_\" + to_string(time())\n"
                  "    mkdir(dir)\n"
                  "    print(rmdir(dir))\n"
                  "    print(is_dir(dir))\n"
                  "}\n",
                  "true\nfalse");
}

static void test_rmdir_error(void) {
    char *out = run_capture("fn main() { rmdir(\"/tmp/nonexistent_lattice_dir_999\") }\n");
    ASSERT(strstr(out, "EVAL_ERROR") != NULL);
    ASSERT(strstr(out, "rmdir") != NULL);
    free(out);
}

static void test_glob_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let dir = \"/tmp/lattice_test_glob_\" + to_string(time())\n"
                  "    mkdir(dir)\n"
                  "    write_file(dir + \"/a.txt\", \"hello\")\n"
                  "    write_file(dir + \"/b.txt\", \"world\")\n"
                  "    write_file(dir + \"/c.log\", \"other\")\n"
                  "    let matches = glob(dir + \"/*.txt\")\n"
                  "    print(len(matches))\n"
                  "    delete_file(dir + \"/a.txt\")\n"
                  "    delete_file(dir + \"/b.txt\")\n"
                  "    delete_file(dir + \"/c.log\")\n"
                  "    rmdir(dir)\n"
                  "}\n",
                  "2");
}

static void test_glob_no_match(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let matches = glob(\"/tmp/lattice_nonexistent_glob_*.xyz\")\n"
                  "    print(len(matches))\n"
                  "}\n",
                  "0");
}

static void test_stat_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let f = \"/tmp/lattice_test_stat_\" + to_string(time())\n"
                  "    write_file(f, \"hello\")\n"
                  "    let s = stat(f)\n"
                  "    print(s.get(\"size\"))\n"
                  "    print(s.get(\"type\"))\n"
                  "    print(s.get(\"mtime\") > 0)\n"
                  "    print(s.get(\"permissions\") > 0)\n"
                  "    delete_file(f)\n"
                  "}\n",
                  "5\nfile\ntrue\ntrue");
}

static void test_stat_dir(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = stat(\".\")\n"
                  "    print(s.get(\"type\"))\n"
                  "}\n",
                  "dir");
}

static void test_stat_error(void) {
    char *out = run_capture("fn main() { stat(\"/tmp/nonexistent_lattice_stat_999\") }\n");
    ASSERT(strstr(out, "EVAL_ERROR") != NULL);
    ASSERT(strstr(out, "stat") != NULL);
    free(out);
}

static void test_copy_file_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let src = \"/tmp/lattice_test_cp_src_\" + to_string(time())\n"
                  "    let dst = \"/tmp/lattice_test_cp_dst_\" + to_string(time())\n"
                  "    write_file(src, \"copy me\")\n"
                  "    print(copy_file(src, dst))\n"
                  "    print(read_file(dst))\n"
                  "    delete_file(src)\n"
                  "    delete_file(dst)\n"
                  "}\n",
                  "true\ncopy me");
}

static void test_copy_file_error(void) {
    char *out = run_capture("fn main() { copy_file(\"/tmp/nonexistent_lattice_cp_999\", \"/tmp/out\") }\n");
    ASSERT(strstr(out, "EVAL_ERROR") != NULL);
    ASSERT(strstr(out, "copy_file") != NULL);
    free(out);
}

static void test_realpath_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let rp = realpath(\".\")\n"
                  "    print(rp.starts_with(\"/\"))\n"
                  "}\n",
                  "true");
}

static void test_realpath_error(void) {
    char *out = run_capture("fn main() { realpath(\"/tmp/nonexistent_lattice_rp_999\") }\n");
    ASSERT(strstr(out, "EVAL_ERROR") != NULL);
    ASSERT(strstr(out, "realpath") != NULL);
    free(out);
}

static void test_tempdir_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let d = tempdir()\n"
                  "    print(is_dir(d))\n"
                  "    rmdir(d)\n"
                  "}\n",
                  "true");
}

static void test_tempfile_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let f = tempfile()\n"
                  "    print(is_file(f))\n"
                  "    delete_file(f)\n"
                  "}\n",
                  "true");
}

static void test_chmod_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let f = tempfile()\n"
                  "    print(chmod(f, 448))\n"
                  "    delete_file(f)\n"
                  "}\n",
                  "true");
}

static void test_file_size_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let f = \"/tmp/lattice_size_test_\" + to_string(time())\n"
                  "    write_file(f, \"hello\")\n"
                  "    print(file_size(f))\n"
                  "    delete_file(f)\n"
                  "}\n",
                  "5");
}

static void test_file_size_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    file_size(\"/nonexistent_path_xyz\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* ======================================================================
 * Array: flat_map, chunk, group_by, sum, min, max, first, last
 * ====================================================================== */

static void test_array_flat_map(void) {
    ASSERT_OUTPUT("fn main() { print([1, 2, 3].flat_map(|x| { [x, x * 10] })) }\n", "[1, 10, 2, 20, 3, 30]");
    /* Single values (not arrays) are kept as-is */
    ASSERT_OUTPUT("fn main() { print([1, 2, 3].flat_map(|x| { x + 1 })) }\n", "[2, 3, 4]");
    /* Empty array */
    ASSERT_OUTPUT("fn main() { print([].flat_map(|x| { [x] })) }\n", "[]");
}

static void test_array_chunk(void) {
    /* Even split */
    ASSERT_OUTPUT("fn main() { print([1, 2, 3, 4].chunk(2)) }\n", "[[1, 2], [3, 4]]");
    /* Uneven split */
    ASSERT_OUTPUT("fn main() { print([1, 2, 3, 4, 5].chunk(2)) }\n", "[[1, 2], [3, 4], [5]]");
    /* Chunk size larger than array */
    ASSERT_OUTPUT("fn main() { print([1, 2].chunk(5)) }\n", "[[1, 2]]");
    /* Empty array */
    ASSERT_OUTPUT("fn main() { print([].chunk(3)) }\n", "[]");
}

static void test_array_group_by(void) {
    /* Group by even/odd using map access to avoid order issues */
    ASSERT_OUTPUT("fn main() {\n"
                  "    let g = [1, 2, 3, 4, 5].group_by(|x| { x % 2 })\n"
                  "    print(g.get(\"0\"))\n"
                  "    print(g.get(\"1\"))\n"
                  "}\n",
                  "[2, 4]\n[1, 3, 5]");
}

static void test_array_sum(void) {
    /* Integer sum */
    ASSERT_OUTPUT("fn main() { print([1, 2, 3, 4, 5].sum()) }\n", "15");
    /* Float sum */
    ASSERT_OUTPUT("fn main() { print([1.5, 2.5, 3.0].sum()) }\n", "7");
    /* Empty array */
    ASSERT_OUTPUT("fn main() { print([].sum()) }\n", "0");
}

static void test_array_min_max(void) {
    /* Int min/max */
    ASSERT_OUTPUT("fn main() { print([3, 1, 4, 1, 5].min()) }\n", "1");
    ASSERT_OUTPUT("fn main() { print([3, 1, 4, 1, 5].max()) }\n", "5");
    /* Float min/max */
    ASSERT_OUTPUT("fn main() { print([3.5, 1.2, 4.8].min()) }\n", "1.2");
    ASSERT_OUTPUT("fn main() { print([3.5, 1.2, 4.8].max()) }\n", "4.8");
    /* Error on empty array */
    ASSERT_OUTPUT_STARTS_WITH("fn main() { print([].min()) }\n", "EVAL_ERROR");
    ASSERT_OUTPUT_STARTS_WITH("fn main() { print([].max()) }\n", "EVAL_ERROR");
}

static void test_array_first_last(void) {
    /* Non-empty */
    ASSERT_OUTPUT("fn main() { print([10, 20, 30].first()) }\n", "10");
    ASSERT_OUTPUT("fn main() { print([10, 20, 30].last()) }\n", "30");
    /* Empty returns unit */
    ASSERT_OUTPUT("fn main() { print([].first()) }\n", "()");
    ASSERT_OUTPUT("fn main() { print([].last()) }\n", "()");
}

/* ======================================================================
 * range() builtin
 * ====================================================================== */

static void test_range_basic(void) {
    ASSERT_OUTPUT("fn main() { print(range(0, 5)) }\n", "[0, 1, 2, 3, 4]");
    /* Negative direction auto-detects step */
    ASSERT_OUTPUT("fn main() { print(range(5, 0)) }\n", "[5, 4, 3, 2, 1]");
}

static void test_range_with_step(void) {
    ASSERT_OUTPUT("fn main() { print(range(0, 10, 3)) }\n", "[0, 3, 6, 9]");
    /* Negative step */
    ASSERT_OUTPUT("fn main() { print(range(10, 0, -2)) }\n", "[10, 8, 6, 4, 2]");
}

static void test_range_empty(void) {
    /* Wrong direction for step produces empty */
    ASSERT_OUTPUT("fn main() { print(range(0, 5, -1)) }\n", "[]");
    /* Same start and end */
    ASSERT_OUTPUT("fn main() { print(range(3, 3)) }\n", "[]");
}

static void test_range_step_zero(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() { print(range(0, 5, 0)) }\n", "EVAL_ERROR");
}

/* ======================================================================
 * String .bytes()
 * ====================================================================== */

static void test_str_bytes(void) { ASSERT_OUTPUT("fn main() { print(\"ABC\".bytes()) }\n", "[65, 66, 67]"); }

/* ======================================================================
 * Array .take() and .drop()
 * ====================================================================== */

static void test_array_take(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3, 4, 5].take(3))\n"
                  "    print([1, 2].take(5))\n"
                  "    print([1, 2, 3].take(0))\n"
                  "}\n",
                  "[1, 2, 3]\n[1, 2]\n[]");
}

static void test_array_drop(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3, 4, 5].drop(2))\n"
                  "    print([1, 2].drop(5))\n"
                  "    print([1, 2, 3].drop(0))\n"
                  "}\n",
                  "[3, 4, 5]\n[]\n[1, 2, 3]");
}

/* ======================================================================
 * error() and is_error() builtins
 * ====================================================================== */

static void test_error_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let e = error(\"something went wrong\")\n"
                  "    print(is_error(e))\n"
                  "    print(is_error(42))\n"
                  "    print(is_error(\"hello\"))\n"
                  "}\n",
                  "true\nfalse\nfalse");
}

/* ======================================================================
 * System info builtins: platform, hostname, pid
 * ====================================================================== */

static void test_platform_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let p = platform()\n"
                  "    print(len(p) > 0)\n"
                  "}\n",
                  "true");
}

static void test_hostname_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let h = hostname()\n"
                  "    print(len(h) > 0)\n"
                  "}\n",
                  "true");
}

static void test_pid_builtin(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let p = pid()\n"
                  "    print(p > 0)\n"
                  "}\n",
                  "true");
}

/* ======================================================================
 * env_keys builtin
 * ====================================================================== */

static void test_env_keys(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    env_set(\"LATTICE_TEST_KEY\", \"1\")\n"
                  "    let keys = env_keys()\n"
                  "    print(keys.contains(\"LATTICE_TEST_KEY\"))\n"
                  "}\n",
                  "true");
}

/* ======================================================================
 * URL encoding builtins
 * ====================================================================== */

static void test_url_encode(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(url_encode(\"hello world\"))\n"
                  "    print(url_encode(\"foo=bar&baz=1\"))\n"
                  "}\n",
                  "hello%20world\nfoo%3Dbar%26baz%3D1");
}

static void test_url_decode(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(url_decode(\"hello%20world\"))\n"
                  "    print(url_decode(\"foo+bar\"))\n"
                  "}\n",
                  "hello world\nfoo bar");
}

/* ======================================================================
 * CSV builtins
 * ====================================================================== */

static void test_csv_parse(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let data = csv_parse(\"a,b,c\\n1,2,3\\n\")\n"
                  "    print(len(data))\n"
                  "    print(data[0])\n"
                  "    print(data[1])\n"
                  "}\n",
                  "2\n[a, b, c]\n[1, 2, 3]");
}

static void test_csv_parse_quoted(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let data = csv_parse(\"name,desc\\n\" + \"\\\"Alice\\\",\\\"has, comma\\\"\\n\")\n"
                  "    print(data[1][0])\n"
                  "    print(data[1][1])\n"
                  "}\n",
                  "Alice\nhas, comma");
}

static void test_csv_stringify(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let data = [[\"name\", \"age\"], [\"Alice\", \"30\"]]\n"
                  "    print(csv_stringify(data).trim())\n"
                  "}\n",
                  "name,age\nAlice,30");
}

static void test_csv_roundtrip(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let csv = \"a,b\\n1,2\\n\"\n"
                  "    let data = csv_parse(csv)\n"
                  "    let result = csv_stringify(data)\n"
                  "    print(result == csv)\n"
                  "}\n",
                  "true");
}

/* ── Functional programming builtins ── */

static void test_identity(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(identity(42))\n"
                  "    print(identity(\"hello\"))\n"
                  "}\n",
                  "42\nhello");
}

static void test_pipe(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let double = |x| { x * 2 }\n"
                  "    let add1 = |x| { x + 1 }\n"
                  "    print(pipe(5, double, add1))\n"
                  "    print(pipe(3, add1, double, add1))\n"
                  "}\n",
                  "11\n9");
}

static void test_compose(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let double = |x| { x * 2 }\n"
                  "    let add1 = |x| { x + 1 }\n"
                  "    let f = compose(double, add1)\n"
                  "    print(f(5))\n"
                  "}\n",
                  "12");
}

/* ======================================================================
 * String Interpolation
 * ====================================================================== */

static void test_interp_simple_var(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let name = \"world\"\n"
                  "    print(\"hello ${name}\")\n"
                  "}\n",
                  "hello world");
}

static void test_interp_expression(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"2 + 2 = ${2 + 2}\")\n"
                  "}\n",
                  "2 + 2 = 4");
}

static void test_interp_multiple(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = \"hello\"\n"
                  "    let b = \"world\"\n"
                  "    print(\"${a} and ${b}\")\n"
                  "}\n",
                  "hello and world");
}

static void test_interp_escaped(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"literal \\${not interpolated}\")\n"
                  "}\n",
                  "literal ${not interpolated}");
}

static void test_interp_adjacent(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = \"foo\"\n"
                  "    let b = \"bar\"\n"
                  "    print(\"${a}${b}\")\n"
                  "}\n",
                  "foobar");
}

static void test_interp_only_expr(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let x = 42\n"
                  "    print(\"${x}\")\n"
                  "}\n",
                  "42");
}

static void test_interp_method_call(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let name = \"world\"\n"
                  "    print(\"${name.to_upper()}\")\n"
                  "}\n",
                  "WORLD");
}

static void test_interp_nested_braces(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"len = ${[1,2,3].len()}\")\n"
                  "}\n",
                  "len = 3");
}

/* ======================================================================
 * Default Parameters
 * ====================================================================== */

static void test_closure_default_param(void) {
    ASSERT_OUTPUT("let greet = |name, greeting = \"hello\"| \"${greeting}, ${name}!\"\n"
                  "fn main() { print(greet(\"world\")) }\n",
                  "hello, world!");
}

static void test_closure_default_param_override(void) {
    ASSERT_OUTPUT("let greet = |name, greeting = \"hello\"| \"${greeting}, ${name}!\"\n"
                  "fn main() { print(greet(\"world\", \"hi\")) }\n",
                  "hi, world!");
}

static void test_closure_multiple_defaults(void) {
    ASSERT_OUTPUT("let f = |a = 1, b = 2, c = 3| a + b + c\n"
                  "fn main() { print(f()) }\n",
                  "6");
}

static void test_closure_partial_defaults(void) {
    ASSERT_OUTPUT("let f = |a, b = 10| a + b\n"
                  "fn main() { print(f(5)) }\n",
                  "15");
}

static void test_fn_default_param(void) {
    ASSERT_OUTPUT("fn greet(name: String, greeting: String = \"hi\") {\n"
                  "    print(\"${greeting} ${name}\")\n"
                  "}\n"
                  "fn main() { greet(\"world\") }\n",
                  "hi world");
}

static void test_fn_default_param_override(void) {
    ASSERT_OUTPUT("fn greet(name: String, greeting: String = \"hi\") {\n"
                  "    print(\"${greeting} ${name}\")\n"
                  "}\n"
                  "fn main() { greet(\"world\", \"hey\") }\n",
                  "hey world");
}

/* ======================================================================
 * Variadic Functions
 * ====================================================================== */

static void test_closure_variadic_basic(void) {
    ASSERT_OUTPUT("let sum = |...nums| {\n"
                  "    let total = 0\n"
                  "    for n in nums { total = total + n }\n"
                  "    total\n"
                  "}\n"
                  "fn main() { print(sum(1, 2, 3)) }\n",
                  "6");
}

static void test_closure_variadic_empty(void) {
    ASSERT_OUTPUT("let f = |...args| args.len()\n"
                  "fn main() { print(f()) }\n",
                  "0");
}

static void test_closure_variadic_with_required(void) {
    ASSERT_OUTPUT("let f = |first, ...rest| \"${first}: ${rest}\"\n"
                  "fn main() { print(f(\"a\", \"b\", \"c\")) }\n",
                  "a: [b, c]");
}

static void test_fn_variadic_basic(void) {
    ASSERT_OUTPUT("fn sum(...nums: Array) {\n"
                  "    let total = 0\n"
                  "    for n in nums { total = total + n }\n"
                  "    return total\n"
                  "}\n"
                  "fn main() { print(sum(10, 20, 30)) }\n",
                  "60");
}

static void test_fn_variadic_empty(void) {
    ASSERT_OUTPUT("fn count(...items: Array) { return items.len() }\n"
                  "fn main() { print(count()) }\n",
                  "0");
}

static void test_closure_default_and_variadic(void) {
    ASSERT_OUTPUT("let f = |prefix = \"log\", ...items| \"${prefix}: ${items}\"\n"
                  "fn main() {\n"
                  "    print(f(\"err\", 1, 2))\n"
                  "    print(f())\n"
                  "}\n",
                  "err: [1, 2]\nlog: []");
}

static void test_fn_default_and_variadic(void) {
    ASSERT_OUTPUT("fn f(tag: String = \"info\", ...vals: Array) {\n"
                  "    print(\"${tag}: ${vals.len()}\")\n"
                  "}\n"
                  "fn main() {\n"
                  "    f(\"warn\", 1, 2, 3)\n"
                  "    f()\n"
                  "}\n",
                  "warn: 3\ninfo: 0");
}

static void test_dotdotdot_token(void) {
    ASSERT_OUTPUT("let f = |...x| x.len()\n"
                  "fn main() { print(f(1, 2)) }\n",
                  "2");
}

/* ======================================================================
 * Test Framework (test blocks parsed but ignored in normal exec)
 * ====================================================================== */

static void test_test_block_ignored(void) {
    /* test blocks should be silently skipped in normal execution */
    ASSERT_OUTPUT("test \"should be ignored\" {\n"
                  "    assert(false)\n"
                  "}\n"
                  "fn main() { print(\"ok\") }\n",
                  "ok");
}

static void test_test_block_with_fn(void) {
    /* test blocks alongside normal functions */
    ASSERT_OUTPUT("fn add(a: Int, b: Int) { return a + b }\n"
                  "test \"add works\" { assert(add(1, 2) == 3) }\n"
                  "fn main() { print(add(5, 3)) }\n",
                  "8");
}

/* ======================================================================
 * Pattern Matching
 * ====================================================================== */

static void test_match_literal_int(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = match 2 { 1 => \"one\", 2 => \"two\", _ => \"other\" }\n"
                  "    print(r)\n"
                  "}\n",
                  "two");
}

static void test_match_wildcard(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = match 99 { 1 => \"one\", _ => \"default\" }\n"
                  "    print(r)\n"
                  "}\n",
                  "default");
}

static void test_match_string(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = match \"hi\" { \"hello\" => 1, \"hi\" => 2, _ => 0 }\n"
                  "    print(r)\n"
                  "}\n",
                  "2");
}

static void test_match_range(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = match 5 { 1..3 => \"low\", 4..6 => \"mid\", _ => \"high\" }\n"
                  "    print(r)\n"
                  "}\n",
                  "mid");
}

static void test_match_guard(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = match 15 {\n"
                  "        x if x % 3 == 0 => \"fizz\",\n"
                  "        _ => \"nope\"\n"
                  "    }\n"
                  "    print(r)\n"
                  "}\n",
                  "fizz");
}

static void test_match_binding(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = match 42 { x => x + 1 }\n"
                  "    print(r)\n"
                  "}\n",
                  "43");
}

static void test_match_negative_literal(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = match -5 { -5 => \"yes\", _ => \"no\" }\n"
                  "    print(r)\n"
                  "}\n",
                  "yes");
}

static void test_match_block_body(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = match 3 {\n"
                  "        x if x > 0 => {\n"
                  "            let doubled = x * 2\n"
                  "            doubled\n"
                  "        },\n"
                  "        _ => 0\n"
                  "    }\n"
                  "    print(r)\n"
                  "}\n",
                  "6");
}

/* ── Destructuring ── */

static void test_destructure_array_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let [a, b, c] = [1, 2, 3]\n"
                  "    print(a)\n"
                  "    print(b)\n"
                  "    print(c)\n"
                  "}\n",
                  "1\n2\n3");
}

static void test_destructure_array_rest(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let [first, ...rest] = [10, 20, 30, 40]\n"
                  "    print(first)\n"
                  "    print(rest)\n"
                  "}\n",
                  "10\n[20, 30, 40]");
}

static void test_destructure_array_rest_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let [a, b, ...rest] = [1, 2]\n"
                  "    print(a)\n"
                  "    print(b)\n"
                  "    print(rest)\n"
                  "}\n",
                  "1\n2\n[]");
}

static void test_destructure_struct_basic(void) {
    ASSERT_OUTPUT("struct Point { x: Int, y: Int }\n"
                  "fn main() {\n"
                  "    let p = Point { x: 10, y: 20 }\n"
                  "    let { x, y } = p\n"
                  "    print(x)\n"
                  "    print(y)\n"
                  "}\n",
                  "10\n20");
}

static void test_destructure_map(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m.set(\"name\", \"Alice\")\n"
                  "    m.set(\"age\", 30)\n"
                  "    let { name, age } = m\n"
                  "    print(name)\n"
                  "    print(age)\n"
                  "}\n",
                  "Alice\n30");
}

static void test_destructure_flux(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux [a, b] = [1, 2]\n"
                  "    a = 99\n"
                  "    print(a)\n"
                  "    print(b)\n"
                  "}\n",
                  "99\n2");
}

static void test_destructure_array_from_fn(void) {
    ASSERT_OUTPUT("fn pair() { return [\"hello\", \"world\"] }\n"
                  "fn main() {\n"
                  "    let [a, b] = pair()\n"
                  "    print(a)\n"
                  "    print(b)\n"
                  "}\n",
                  "hello\nworld");
}

static void test_destructure_nested_array(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let [a, b, ...rest] = [1, 2, 3, 4, 5]\n"
                  "    print(a + b)\n"
                  "    print(rest.len())\n"
                  "}\n",
                  "3\n3");
}

/* ------ Enum / Sum Types ------ */

static void test_enum_unit_variant(void) {
    ASSERT_OUTPUT("enum Color { Red, Green, Blue }\n"
                  "fn main() {\n"
                  "    let r = Color::Red\n"
                  "    print(r)\n"
                  "}\n",
                  "Color::Red");
}

static void test_enum_tuple_variant(void) {
    ASSERT_OUTPUT("enum Shape { Circle(Number), Rect(Number, Number) }\n"
                  "fn main() {\n"
                  "    let c = Shape::Circle(5)\n"
                  "    let r = Shape::Rect(3, 4)\n"
                  "    print(c)\n"
                  "    print(r)\n"
                  "}\n",
                  "Shape::Circle(5)\nShape::Rect(3, 4)");
}

static void test_enum_equality(void) {
    ASSERT_OUTPUT("enum Color { Red, Green, Blue }\n"
                  "fn main() {\n"
                  "    let r1 = Color::Red\n"
                  "    let r2 = Color::Red\n"
                  "    let g = Color::Green\n"
                  "    print(r1 == r2)\n"
                  "    print(r1 == g)\n"
                  "    print(r1 != g)\n"
                  "}\n",
                  "true\nfalse\ntrue");
}

static void test_enum_variant_name(void) {
    ASSERT_OUTPUT("enum Color { Red, Green, Blue }\n"
                  "fn main() {\n"
                  "    let r = Color::Red\n"
                  "    print(r.variant_name())\n"
                  "}\n",
                  "Red");
}

static void test_enum_enum_name(void) {
    ASSERT_OUTPUT("enum Color { Red, Green, Blue }\n"
                  "fn main() {\n"
                  "    let r = Color::Red\n"
                  "    print(r.enum_name())\n"
                  "}\n",
                  "Color");
}

static void test_enum_is_variant(void) {
    ASSERT_OUTPUT("enum Color { Red, Green, Blue }\n"
                  "fn main() {\n"
                  "    let r = Color::Red\n"
                  "    print(r.is_variant(\"Red\"))\n"
                  "    print(r.is_variant(\"Green\"))\n"
                  "}\n",
                  "true\nfalse");
}

static void test_enum_payload(void) {
    ASSERT_OUTPUT("enum Shape { Circle(Number), Rect(Number, Number) }\n"
                  "fn main() {\n"
                  "    let c = Shape::Circle(42)\n"
                  "    let p = c.payload()\n"
                  "    print(p[0])\n"
                  "    let r = Shape::Rect(3, 4)\n"
                  "    let rp = r.payload()\n"
                  "    print(rp[0])\n"
                  "    print(rp[1])\n"
                  "}\n",
                  "42\n3\n4");
}

static void test_enum_typeof(void) {
    ASSERT_OUTPUT("enum Color { Red, Green }\n"
                  "fn main() {\n"
                  "    let r = Color::Red\n"
                  "    print(typeof(r))\n"
                  "}\n",
                  "Enum");
}

/* ------ Set Data Type ------ */

static void test_set_new(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = Set::new()\n"
                  "    print(s.len())\n"
                  "    print(typeof(s))\n"
                  "}\n",
                  "0\nSet");
}

static void test_set_from(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = Set::from([1, 2, 2, 3])\n"
                  "    print(s.len())\n"
                  "}\n",
                  "3");
}

static void test_set_add_has(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux s = Set::new()\n"
                  "    s.add(42)\n"
                  "    s.add(\"hello\")\n"
                  "    print(s.has(42))\n"
                  "    print(s.has(\"hello\"))\n"
                  "    print(s.has(99))\n"
                  "}\n",
                  "true\ntrue\nfalse");
}

static void test_set_remove(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux s = Set::from([1, 2, 3])\n"
                  "    s.remove(2)\n"
                  "    print(s.len())\n"
                  "    print(s.has(2))\n"
                  "}\n",
                  "2\nfalse");
}

static void test_set_to_array(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = Set::from([42])\n"
                  "    let arr = s.to_array()\n"
                  "    print(arr[0])\n"
                  "    print(len(arr))\n"
                  "}\n",
                  "42\n1");
}

static void test_set_union(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s1 = Set::from([1, 2])\n"
                  "    let s2 = Set::from([2, 3])\n"
                  "    let u = s1.union(s2)\n"
                  "    print(u.len())\n"
                  "}\n",
                  "3");
}

static void test_set_intersection(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s1 = Set::from([1, 2, 3])\n"
                  "    let s2 = Set::from([2, 3, 4])\n"
                  "    let i = s1.intersection(s2)\n"
                  "    print(i.len())\n"
                  "    print(i.has(2))\n"
                  "    print(i.has(1))\n"
                  "}\n",
                  "2\ntrue\nfalse");
}

static void test_set_difference(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s1 = Set::from([1, 2, 3])\n"
                  "    let s2 = Set::from([2, 3, 4])\n"
                  "    let d = s1.difference(s2)\n"
                  "    print(d.len())\n"
                  "    print(d.has(1))\n"
                  "}\n",
                  "1\ntrue");
}

static void test_set_symmetric_difference(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s1 = Set::from([1, 2, 3])\n"
                  "    let s2 = Set::from([2, 3, 4])\n"
                  "    let sd = s1.symmetric_difference(s2)\n"
                  "    print(sd.len())\n"
                  "    print(sd.has(1))\n"
                  "    print(sd.has(4))\n"
                  "    print(sd.has(2))\n"
                  "    print(sd.has(3))\n"
                  "}\n",
                  "2\ntrue\ntrue\nfalse\nfalse");
}

static void test_set_symmetric_difference_disjoint(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s1 = Set::from([1, 2])\n"
                  "    let s2 = Set::from([3, 4])\n"
                  "    let sd = s1.symmetric_difference(s2)\n"
                  "    print(sd.len())\n"
                  "    print(sd.has(1))\n"
                  "    print(sd.has(2))\n"
                  "    print(sd.has(3))\n"
                  "    print(sd.has(4))\n"
                  "}\n",
                  "4\ntrue\ntrue\ntrue\ntrue");
}

static void test_set_symmetric_difference_identical(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s1 = Set::from([1, 2, 3])\n"
                  "    let s2 = Set::from([1, 2, 3])\n"
                  "    let sd = s1.symmetric_difference(s2)\n"
                  "    print(sd.len())\n"
                  "}\n",
                  "0");
}

static void test_set_symmetric_difference_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s1 = Set::from([1, 2, 3])\n"
                  "    let s2 = Set::new()\n"
                  "    let sd = s1.symmetric_difference(s2)\n"
                  "    print(sd.len())\n"
                  "    print(sd.has(1))\n"
                  "    print(sd.has(2))\n"
                  "    print(sd.has(3))\n"
                  "}\n",
                  "3\ntrue\ntrue\ntrue");
}

static void test_set_subset_superset(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s1 = Set::from([1, 2])\n"
                  "    let s2 = Set::from([1, 2, 3])\n"
                  "    print(s1.is_subset(s2))\n"
                  "    print(s2.is_superset(s1))\n"
                  "    print(s2.is_subset(s1))\n"
                  "}\n",
                  "true\ntrue\nfalse");
}

static void test_set_for_in(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = Set::from([10])\n"
                  "    for item in s {\n"
                  "        print(item)\n"
                  "    }\n"
                  "}\n",
                  "10");
}

static void test_set_duplicate_add(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux s = Set::new()\n"
                  "    s.add(1)\n"
                  "    s.add(1)\n"
                  "    s.add(1)\n"
                  "    print(s.len())\n"
                  "}\n",
                  "1");
}

static void test_set_typeof(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = Set::new()\n"
                  "    print(typeof(s))\n"
                  "}\n",
                  "Set");
}

/* ── HTTP mock server + integration tests ── */

/* Helper: run a mock HTTP server in a forked child.
 * Accepts one connection on server_fd, reads the request, writes `response`, closes. */
static void mock_http_server(int server_fd, const char *response) {
    char *err = NULL;
    int client = net_tcp_accept(server_fd, &err);
    if (client < 0) _exit(1);

    /* Read request (just drain it) */
    char *req = net_tcp_read(client, &err);
    free(req);

    /* Write canned response */
    net_tcp_write(client, response, strlen(response), &err);
    net_tcp_close(client);
    net_tcp_close(server_fd);
    _exit(0);
}

/* Helper: listen on random port, return server_fd and set *port_out */
static int mock_listen(int *port_out) {
    char *err = NULL;
    int server = net_tcp_listen("127.0.0.1", 0, &err);
    if (server < 0) return -1;
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    getsockname(server, (struct sockaddr *)&addr, &alen);
    *port_out = ntohs(addr.sin_port);
    return server;
}

static void test_http_url_parse_basic(void) {
    char *err = NULL;
    HttpUrl url;

    /* http URL */
    ASSERT(http_parse_url("http://example.com/foo", &url, &err));
    ASSERT(err == NULL);
    ASSERT_STR_EQ(url.scheme, "http");
    ASSERT_STR_EQ(url.host, "example.com");
    ASSERT(url.port == 80);
    ASSERT_STR_EQ(url.path, "/foo");
    http_url_free(&url);

    /* https URL */
    ASSERT(http_parse_url("https://secure.io/bar", &url, &err));
    ASSERT_STR_EQ(url.scheme, "https");
    ASSERT_STR_EQ(url.host, "secure.io");
    ASSERT(url.port == 443);
    ASSERT_STR_EQ(url.path, "/bar");
    http_url_free(&url);

    /* No path defaults to "/" */
    ASSERT(http_parse_url("http://nopath.com", &url, &err));
    ASSERT_STR_EQ(url.path, "/");
    http_url_free(&url);
}

static void test_http_url_parse_custom_port(void) {
    char *err = NULL;
    HttpUrl url;

    ASSERT(http_parse_url("http://localhost:8080/api?q=1", &url, &err));
    ASSERT_STR_EQ(url.host, "localhost");
    ASSERT(url.port == 8080);
    ASSERT_STR_EQ(url.path, "/api?q=1");
    http_url_free(&url);
}

static void test_http_url_parse_errors(void) {
    char *err = NULL;
    HttpUrl url;

    /* Bad scheme */
    ASSERT(!http_parse_url("ftp://x.com", &url, &err));
    ASSERT(err != NULL);
    free(err);
    err = NULL;

    /* Empty host */
    ASSERT(!http_parse_url("http:///path", &url, &err));
    ASSERT(err != NULL);
    free(err);
    err = NULL;

    /* Bad port */
    ASSERT(!http_parse_url("http://host:99999/x", &url, &err));
    ASSERT(err != NULL);
    free(err);
    err = NULL;

    /* Port 0 */
    ASSERT(!http_parse_url("http://host:0/x", &url, &err));
    ASSERT(err != NULL);
    free(err);
}

static void test_http_execute_get(void) {
    int port;
    int server = mock_listen(&port);
    ASSERT(server >= 0);

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        mock_http_server(server, "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/plain\r\n"
                                 "Content-Length: 5\r\n"
                                 "\r\n"
                                 "hello");
    }

    /* Parent: close server fd (child owns it) and make request */
    net_tcp_close(server);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/test", port);

    HttpRequest req = {0};
    req.method = "GET";
    req.url = url;

    char *err = NULL;
    HttpResponse *resp = http_execute(&req, &err);
    ASSERT(resp != NULL);
    ASSERT(err == NULL);
    ASSERT(resp->status_code == 200);
    ASSERT_STR_EQ(resp->body, "hello");
    ASSERT(resp->body_len == 5);

    /* Check headers were parsed */
    bool found_ct = false;
    for (size_t i = 0; i < resp->header_count; i++) {
        if (strcmp(resp->header_keys[i], "content-type") == 0) {
            ASSERT_STR_EQ(resp->header_values[i], "text/plain");
            found_ct = true;
        }
    }
    ASSERT(found_ct);

    http_response_free(resp);

    int status;
    waitpid(pid, &status, 0);
}

static void test_http_execute_post_body(void) {
    int port;
    int server = mock_listen(&port);
    ASSERT(server >= 0);

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        /* Child: accept, read request (verify body), send response */
        char *err = NULL;
        int client = net_tcp_accept(server, &err);
        if (client < 0) _exit(1);

        /* Read request */
        char *req_data = net_tcp_read(client, &err);
        /* Verify POST body was received */
        int ok = (req_data && strstr(req_data, "test body") != NULL);
        free(req_data);

        const char *resp = ok ? "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
                              : "HTTP/1.1 400 Bad Request\r\nContent-Length: 4\r\n\r\nfail";
        net_tcp_write(client, resp, strlen(resp), &err);
        net_tcp_close(client);
        net_tcp_close(server);
        _exit(0);
    }

    net_tcp_close(server);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/post", port);

    HttpRequest req = {0};
    req.method = "POST";
    req.url = url;
    req.body = "test body";
    req.body_len = 9;

    char *err = NULL;
    HttpResponse *resp = http_execute(&req, &err);
    ASSERT(resp != NULL);
    ASSERT(resp->status_code == 200);
    ASSERT_STR_EQ(resp->body, "ok");

    http_response_free(resp);

    int status;
    waitpid(pid, &status, 0);
}

static void test_http_execute_custom_headers(void) {
    int port;
    int server = mock_listen(&port);
    ASSERT(server >= 0);

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        /* Child: verify custom header was sent */
        char *err = NULL;
        int client = net_tcp_accept(server, &err);
        if (client < 0) _exit(1);

        char *req_data = net_tcp_read(client, &err);
        int ok = (req_data && strstr(req_data, "X-Custom: myval") != NULL);
        free(req_data);

        const char *resp = ok ? "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ngood"
                              : "HTTP/1.1 400 Bad Request\r\nContent-Length: 3\r\n\r\nbad";
        net_tcp_write(client, resp, strlen(resp), &err);
        net_tcp_close(client);
        net_tcp_close(server);
        _exit(0);
    }

    net_tcp_close(server);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/hdrs", port);

    char *keys[] = {"X-Custom"};
    char *vals[] = {"myval"};

    HttpRequest req = {0};
    req.method = "GET";
    req.url = url;
    req.header_keys = keys;
    req.header_values = vals;
    req.header_count = 1;

    char *err = NULL;
    HttpResponse *resp = http_execute(&req, &err);
    ASSERT(resp != NULL);
    ASSERT(resp->status_code == 200);
    ASSERT_STR_EQ(resp->body, "good");

    http_response_free(resp);

    int status;
    waitpid(pid, &status, 0);
}

static void test_http_execute_chunked(void) {
    int port;
    int server = mock_listen(&port);
    ASSERT(server >= 0);

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        mock_http_server(server, "HTTP/1.1 200 OK\r\n"
                                 "Transfer-Encoding: chunked\r\n"
                                 "\r\n"
                                 "5\r\n"
                                 "Hello\r\n"
                                 "7\r\n"
                                 " World!\r\n"
                                 "0\r\n"
                                 "\r\n");
    }

    net_tcp_close(server);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/chunked", port);

    HttpRequest req = {0};
    req.method = "GET";
    req.url = url;

    char *err = NULL;
    HttpResponse *resp = http_execute(&req, &err);
    ASSERT(resp != NULL);
    ASSERT(resp->status_code == 200);
    ASSERT_STR_EQ(resp->body, "Hello World!");
    ASSERT(resp->body_len == 12);

    http_response_free(resp);

    int status;
    waitpid(pid, &status, 0);
}

static void test_http_execute_multi_headers(void) {
    int port;
    int server = mock_listen(&port);
    ASSERT(server >= 0);

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        mock_http_server(server, "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "X-Request-Id: abc123\r\n"
                                 "X-Server: testmock\r\n"
                                 "Content-Length: 2\r\n"
                                 "\r\n"
                                 "{}");
    }

    net_tcp_close(server);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/multi", port);

    HttpRequest req = {0};
    req.method = "GET";
    req.url = url;

    char *err = NULL;
    HttpResponse *resp = http_execute(&req, &err);
    ASSERT(resp != NULL);
    ASSERT(resp->status_code == 200);
    ASSERT(resp->header_count >= 4);

    /* Headers should be lowercased */
    bool found_rid = false;
    for (size_t i = 0; i < resp->header_count; i++) {
        if (strcmp(resp->header_keys[i], "x-request-id") == 0) {
            ASSERT_STR_EQ(resp->header_values[i], "abc123");
            found_rid = true;
        }
    }
    ASSERT(found_rid);

    http_response_free(resp);

    int status;
    waitpid(pid, &status, 0);
}

static void test_http_execute_non_standard_port(void) {
    int port;
    int server = mock_listen(&port);
    ASSERT(server >= 0);

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        /* Child: verify Host header includes port */
        char *err = NULL;
        int client = net_tcp_accept(server, &err);
        if (client < 0) _exit(1);

        char *req_data = net_tcp_read(client, &err);
        char expected_host[64];
        snprintf(expected_host, sizeof(expected_host), "Host: 127.0.0.1:%d", port);
        int ok = (req_data && strstr(req_data, expected_host) != NULL);
        free(req_data);

        const char *resp = ok ? "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
                              : "HTTP/1.1 400 Bad Request\r\nContent-Length: 3\r\n\r\nbad";
        net_tcp_write(client, resp, strlen(resp), &err);
        net_tcp_close(client);
        net_tcp_close(server);
        _exit(0);
    }

    net_tcp_close(server);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/port", port);

    HttpRequest req = {0};
    req.method = "GET";
    req.url = url;

    char *err = NULL;
    HttpResponse *resp = http_execute(&req, &err);
    ASSERT(resp != NULL);
    ASSERT(resp->status_code == 200);
    ASSERT_STR_EQ(resp->body, "ok");

    http_response_free(resp);

    int status;
    waitpid(pid, &status, 0);
}

static void test_http_execute_connect_refused(void) {
    /* Use a port that nothing is listening on */
    char *err = NULL;
    HttpRequest req = {0};
    req.method = "GET";
    req.url = "http://127.0.0.1:1/refused";

    HttpResponse *resp = http_execute(&req, &err);
    ASSERT(resp == NULL);
    ASSERT(err != NULL);
    free(err);
}

static void test_http_execute_lattice_get(void) {
    int port;
    int server = mock_listen(&port);
    ASSERT(server >= 0);

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        mock_http_server(server, "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/plain\r\n"
                                 "Content-Length: 11\r\n"
                                 "\r\n"
                                 "from server");
    }

    net_tcp_close(server);

    char source[256];
    snprintf(source, sizeof(source),
             "fn main() {\n"
             "    let r = http_get(\"http://127.0.0.1:%d/lat\")\n"
             "    print(r[\"status\"])\n"
             "    print(r[\"body\"])\n"
             "}\n",
             port);

    char *out = run_capture(source);
    ASSERT_STR_EQ(out, "200\nfrom server");
    free(out);

    int status;
    waitpid(pid, &status, 0);
}

static void test_http_execute_lattice_post(void) {
    int port;
    int server = mock_listen(&port);
    ASSERT(server >= 0);

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        /* Child: accept, drain request, respond with 201 */
        mock_http_server(server, "HTTP/1.1 201 Created\r\n"
                                 "Content-Length: 7\r\n"
                                 "\r\n"
                                 "created");
    }

    net_tcp_close(server);

    char source[256];
    snprintf(source, sizeof(source),
             "fn main() {\n"
             "    let r = http_post(\"http://127.0.0.1:%d/lat\", \"payload\")\n"
             "    print(r[\"status\"])\n"
             "    print(r[\"body\"])\n"
             "}\n",
             port);

    char *out = run_capture(source);
    ASSERT_STR_EQ(out, "201\ncreated");
    free(out);

    int status;
    waitpid(pid, &status, 0);
}

/* ── HTTP client error tests ── */

static void test_http_get_wrong_type(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    try {\n"
                  "        http_get(123)\n"
                  "    } catch e {\n"
                  "        print(e.message)\n"
                  "    }\n"
                  "}\n",
                  "http_get() expects (url: String)");
}

static void test_http_get_no_args(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    try {\n"
                  "        http_get()\n"
                  "    } catch e {\n"
                  "        print(e.message)\n"
                  "    }\n"
                  "}\n",
                  "http_get() expects (url: String)");
}

static void test_http_get_invalid_url(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    try {\n"
                  "        http_get(\"ftp://example.com\")\n"
                  "    } catch e {\n"
                  "        print(e.message)\n"
                  "    }\n"
                  "}\n",
                  "invalid URL: must start with http:// or https://");
}

static void test_http_post_wrong_type(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    try {\n"
                  "        http_post(123)\n"
                  "    } catch e {\n"
                  "        print(e.message)\n"
                  "    }\n"
                  "}\n",
                  "http_post() expects (url: String, options?: Map)");
}

static void test_http_post_invalid_url(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    try {\n"
                  "        http_post(\"not-a-url\")\n"
                  "    } catch e {\n"
                  "        print(e.message)\n"
                  "    }\n"
                  "}\n",
                  "invalid URL: must start with http:// or https://");
}

static void test_http_request_wrong_type(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    try {\n"
                  "        http_request(123, 456)\n"
                  "    } catch e {\n"
                  "        print(e.message)\n"
                  "    }\n"
                  "}\n",
                  "http_request() expects (method: String, url: String, options?: Map)");
}

static void test_http_request_too_few_args(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    try {\n"
                  "        http_request(\"GET\")\n"
                  "    } catch e {\n"
                  "        print(e.message)\n"
                  "    }\n"
                  "}\n",
                  "http_request() expects (method: String, url: String, options?: Map)");
}

static void test_http_request_invalid_url(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    try {\n"
                  "        http_request(\"PUT\", \"bad://url\")\n"
                  "    } catch e {\n"
                  "        print(e.message)\n"
                  "    }\n"
                  "}\n",
                  "invalid URL: must start with http:// or https://");
}

/* ── TOML tests ── */

static void test_toml_parse_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let t = toml_parse(\"title = \\\"Test\\\"\\ncount = 42\")\n"
                  "    print(t.get(\"title\"))\n"
                  "    print(t.get(\"count\"))\n"
                  "}\n",
                  "Test\n42");
}

static void test_toml_parse_table(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let t = toml_parse(\"[server]\\nhost = \\\"localhost\\\"\\nport = 8080\")\n"
                  "    let srv = t.get(\"server\")\n"
                  "    print(srv.get(\"host\"))\n"
                  "    print(srv.get(\"port\"))\n"
                  "}\n",
                  "localhost\n8080");
}

static void test_toml_parse_bool(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let t = toml_parse(\"enabled = true\\ndebug = false\")\n"
                  "    print(t.get(\"enabled\"))\n"
                  "    print(t.get(\"debug\"))\n"
                  "}\n",
                  "true\nfalse");
}

static void test_toml_parse_array(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let t = toml_parse(\"nums = [1, 2, 3]\")\n"
                  "    print(t.get(\"nums\"))\n"
                  "}\n",
                  "[1, 2, 3]");
}

static void test_toml_stringify_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let m = Map::new()\n"
                  "    m.set(\"name\", \"Alice\")\n"
                  "    m.set(\"age\", 30)\n"
                  "    let s = toml_stringify(m)\n"
                  "    let t2 = toml_parse(s)\n"
                  "    print(t2.get(\"name\"))\n"
                  "    print(t2.get(\"age\"))\n"
                  "}\n",
                  "Alice\n30");
}

static void test_toml_parse_wrong_type(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    try {\n"
                  "        toml_parse(123)\n"
                  "    } catch e {\n"
                  "        print(e.message)\n"
                  "    }\n"
                  "}\n",
                  "toml_parse() expects (String)");
}

static void test_toml_stringify_wrong_type(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    try {\n"
                  "        toml_stringify(123)\n"
                  "    } catch e {\n"
                  "        print(e.message)\n"
                  "    }\n"
                  "}\n",
                  "toml_stringify: value must be a Map");
}

/* ── YAML tests ── */

static void test_yaml_parse_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let y = yaml_parse(\"name: John\\nage: 30\")\n"
                  "    print(y.get(\"name\"))\n"
                  "    print(y.get(\"age\"))\n"
                  "}\n",
                  "John\n30");
}

static void test_yaml_parse_bool(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let y = yaml_parse(\"active: true\\ndone: false\")\n"
                  "    print(y.get(\"active\"))\n"
                  "    print(y.get(\"done\"))\n"
                  "}\n",
                  "true\nfalse");
}

static void test_yaml_parse_sequence(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let y = yaml_parse(\"- apple\\n- banana\\n- cherry\")\n"
                  "    print(y.len())\n"
                  "    print(y[0])\n"
                  "    print(y[2])\n"
                  "}\n",
                  "3\napple\ncherry");
}

static void test_yaml_parse_nested(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let y = yaml_parse(\"server:\\n  host: localhost\\n  port: 8080\")\n"
                  "    let srv = y.get(\"server\")\n"
                  "    print(srv.get(\"host\"))\n"
                  "    print(srv.get(\"port\"))\n"
                  "}\n",
                  "localhost\n8080");
}

static void test_yaml_stringify_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let m = Map::new()\n"
                  "    m.set(\"color\", \"blue\")\n"
                  "    m.set(\"size\", 10)\n"
                  "    let s = yaml_stringify(m)\n"
                  "    let y2 = yaml_parse(s)\n"
                  "    print(y2.get(\"color\"))\n"
                  "    print(y2.get(\"size\"))\n"
                  "}\n",
                  "blue\n10");
}

static void test_yaml_parse_wrong_type(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    try {\n"
                  "        yaml_parse(123)\n"
                  "    } catch e {\n"
                  "        print(e.message)\n"
                  "    }\n"
                  "}\n",
                  "yaml_parse() expects (String)");
}

static void test_yaml_stringify_wrong_type(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    try {\n"
                  "        yaml_stringify(123)\n"
                  "    } catch e {\n"
                  "        print(e.message)\n"
                  "    }\n"
                  "}\n",
                  "yaml_stringify: value must be a Map or Array");
}

static void test_yaml_parse_flow_seq(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let y = yaml_parse(\"[1, 2, 3]\")\n"
                  "    print(y.len())\n"
                  "    print(y[1])\n"
                  "}\n",
                  "3\n2");
}

/* ======================================================================
 * Test Registration
 * ====================================================================== */

/* ── Single-quoted strings ── */

static void test_single_quote_basic(void) { ASSERT_OUTPUT("fn main() { print('hello world') }", "hello world"); }

static void test_single_quote_double_quotes_inside(void) {
    ASSERT_OUTPUT("fn main() { print('say \"hi\"') }", "say \"hi\"");
}

static void test_single_quote_escaped(void) { ASSERT_OUTPUT("fn main() { print('it\\'s fine') }", "it's fine"); }

static void test_single_quote_no_interpolation(void) {
    ASSERT_OUTPUT("fn main() { print('${not interpolated}') }", "${not interpolated}");
}

static void test_single_quote_newline_escape(void) {
    ASSERT_OUTPUT("fn main() { print('line1\\nline2') }", "line1\nline2");
}

static void test_single_quote_empty(void) { ASSERT_OUTPUT("fn main() { print(len('')) }", "0"); }

static void test_single_quote_concat(void) {
    ASSERT_OUTPUT("fn main() { print('hello' + \" world\") }", "hello world");
}

static void test_single_quote_json(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let obj = json_parse('{\"key\": \"value\"}')\n"
                  "    print(obj.get(\"key\"))\n"
                  "}\n",
                  "value");
}

/* ── Nil value and ?? operator ── */

static void test_nil_literal(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let x = nil\n"
                  "    print(x)\n"
                  "}\n",
                  "nil");
}

static void test_nil_typeof(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(typeof(nil))\n"
                  "}\n",
                  "Nil");
}

static void test_nil_equality(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(nil == nil)\n"
                  "    print(nil != nil)\n"
                  "    print(nil == 0)\n"
                  "    print(nil == false)\n"
                  "}\n",
                  "true\nfalse\nfalse\nfalse");
}

static void test_nil_truthiness(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    if nil { print(\"yes\") } else { print(\"no\") }\n"
                  "}\n",
                  "no");
}

static void test_nil_coalesce(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let x = nil\n"
                  "    print(x ?? \"default\")\n"
                  "}\n",
                  "default");
}

static void test_nil_coalesce_non_nil(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let x = 42\n"
                  "    print(x ?? 0)\n"
                  "}\n",
                  "42");
}

static void test_nil_map_get(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let m = json_parse('{\"a\": 1}')\n"
                  "    print(m.get(\"a\"))\n"
                  "    print(m.get(\"b\"))\n"
                  "    print(m.get(\"b\") ?? \"missing\")\n"
                  "}\n",
                  "1\nnil\nmissing");
}

static void test_nil_match(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let x = nil\n"
                  "    let result = match x {\n"
                  "        nil => \"nothing\",\n"
                  "        _ => \"something\"\n"
                  "    }\n"
                  "    print(result)\n"
                  "}\n",
                  "nothing");
}

static void test_nil_json_roundtrip(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = json_stringify(nil)\n"
                  "    print(s)\n"
                  "    let v = json_parse(s)\n"
                  "    print(v == nil)\n"
                  "}\n",
                  "null\ntrue");
}

static void test_nil_coalesce_chain(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = nil\n"
                  "    let b = nil\n"
                  "    let c = 3\n"
                  "    print(a ?? b ?? c)\n"
                  "}\n",
                  "3");
}

/* ── Triple-quoted strings ── */

static void test_triple_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = \"\"\"hello world\"\"\"\n"
                  "    print(s)\n"
                  "}\n",
                  "hello world");
}

static void test_triple_multiline(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = \"\"\"\n"
                  "    line 1\n"
                  "    line 2\n"
                  "    line 3\n"
                  "    \"\"\"\n"
                  "    print(s)\n"
                  "}\n",
                  "line 1\nline 2\nline 3");
}

static void test_triple_dedent(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = \"\"\"\n"
                  "        deeper\n"
                  "        lines\n"
                  "    \"\"\"\n"
                  "    print(s)\n"
                  "}\n",
                  "    deeper\n    lines");
}

static void test_triple_interpolation(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let x = 42\n"
                  "    let s = \"\"\"value: ${x}\"\"\"\n"
                  "    print(s)\n"
                  "}\n",
                  "value: 42");
}

static void test_triple_multiline_interpolation(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let name = \"Lattice\"\n"
                  "    let s = \"\"\"\n"
                  "    Hello, ${name}!\n"
                  "    Version ${version()}\n"
                  "    \"\"\"\n"
                  "    print(s)\n"
                  "}\n",
                  "Hello, Lattice!\nVersion 0.3.27");
}

static void test_triple_embedded_quotes(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = \"\"\"\n"
                  "    She said \"hello\" and I said 'hi'\n"
                  "    \"\"\"\n"
                  "    print(s)\n"
                  "}\n",
                  "She said \"hello\" and I said 'hi'");
}

static void test_triple_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = \"\"\"\"\"\"\n"
                  "    print(s.len())\n"
                  "}\n",
                  "0");
}

static void test_triple_escape(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = \"\"\"hello\\tworld\"\"\"\n"
                  "    print(s)\n"
                  "}\n",
                  "hello\tworld");
}

static void test_triple_json(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = \"\"\"\n"
                  "    {\"name\": \"Lattice\", \"version\": 1}\n"
                  "    \"\"\"\n"
                  "    let obj = json_parse(s)\n"
                  "    print(obj.get(\"name\"))\n"
                  "}\n",
                  "Lattice");
}

/* ── Spread operator ── */

static void test_spread_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2, 3]\n"
                  "    let b = [0, ...a, 4]\n"
                  "    print(b)\n"
                  "}\n",
                  "[0, 1, 2, 3, 4]");
}

static void test_spread_multiple(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2]\n"
                  "    let b = [3, 4]\n"
                  "    let c = [...a, ...b]\n"
                  "    print(c)\n"
                  "}\n",
                  "[1, 2, 3, 4]");
}

static void test_spread_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let empty = []\n"
                  "    let b = [1, ...empty, 2]\n"
                  "    print(b)\n"
                  "}\n",
                  "[1, 2]");
}

static void test_spread_only(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2, 3]\n"
                  "    let b = [...a]\n"
                  "    print(b)\n"
                  "}\n",
                  "[1, 2, 3]");
}

static void test_spread_with_expr(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let result = [...[1, 2].map(|x| x * 10)]\n"
                  "    print(result)\n"
                  "}\n",
                  "[10, 20]");
}

static void test_spread_nested(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2]\n"
                  "    let b = [3, 4]\n"
                  "    let c = [5, 6]\n"
                  "    let all = [...a, ...b, ...c]\n"
                  "    print(all.len())\n"
                  "}\n",
                  "6");
}

/* ── Tuples ── */

static void test_tuple_creation(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let t = (1, \"hello\", true)\n"
                  "    print(t)\n"
                  "}\n",
                  "(1, hello, true)");
}

static void test_tuple_single(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let t = (42,)\n"
                  "    print(t)\n"
                  "}\n",
                  "(42,)");
}

static void test_tuple_field_access(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let t = (10, 20, 30)\n"
                  "    print(t.0)\n"
                  "    print(t.1)\n"
                  "    print(t.2)\n"
                  "}\n",
                  "10\n20\n30");
}

static void test_tuple_len(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let t = (1, 2, 3, 4, 5)\n"
                  "    print(t.len())\n"
                  "}\n",
                  "5");
}

static void test_tuple_typeof(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let t = (1, 2)\n"
                  "    print(typeof(t))\n"
                  "}\n",
                  "Tuple");
}

static void test_tuple_equality(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = (1, 2, 3)\n"
                  "    let b = (1, 2, 3)\n"
                  "    let c = (1, 2, 4)\n"
                  "    print(a == b)\n"
                  "    print(a == c)\n"
                  "}\n",
                  "true\nfalse");
}

static void test_tuple_nested(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let t = (1, (2, 3), 4)\n"
                  "    let inner = t.1\n"
                  "    print(inner.0)\n"
                  "    print(inner.1)\n"
                  "}\n",
                  "2\n3");
}

static void test_tuple_phase(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let t = (1, 2, 3)\n"
                  "    print(phase_of(t))\n"
                  "}\n",
                  "crystal");
}

/* ── Bitwise operators ── */

static void test_bitwise_and(void) {
    /* 255 & 15 = 15 */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(255 & 15)\n"
                  "}\n",
                  "15");
}

static void test_bitwise_or(void) {
    /* 15 | 240 = 255 */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(15 | 240)\n"
                  "}\n",
                  "255");
}

static void test_bitwise_xor(void) {
    /* 255 ^ 15 = 240 */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(255 ^ 15)\n"
                  "}\n",
                  "240");
}

static void test_bitwise_not(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(~0)\n"
                  "}\n",
                  "-1");
}

static void test_bitwise_lshift(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(1 << 8)\n"
                  "}\n",
                  "256");
}

static void test_bitwise_rshift(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(256 >> 4)\n"
                  "}\n",
                  "16");
}

static void test_bitwise_compound_assign(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 255\n"
                  "    x &= 15\n"
                  "    print(x)\n"
                  "    x |= 240\n"
                  "    print(x)\n"
                  "    x ^= 255\n"
                  "    print(x)\n"
                  "    x = 1\n"
                  "    x <<= 4\n"
                  "    print(x)\n"
                  "    x >>= 2\n"
                  "    print(x)\n"
                  "}\n",
                  "15\n255\n0\n16\n4");
}

static void test_bitwise_precedence(void) {
    /* & binds tighter than ^, ^ binds tighter than | */
    /* 15 | (16 & 31) = 15 | 16 = 31 */
    /* 1 | (2 ^ 3) = 1 | 1 = 1 */
    /* (6 & 3) ^ 5 = 2 ^ 5 = 7 */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(15 | 16 & 31)\n"
                  "    print(1 | 2 ^ 3)\n"
                  "    print(6 & 3 ^ 5)\n"
                  "}\n",
                  "31\n1\n7");
}

static void test_bitwise_shift_range(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let result = try { 1 << 64 } catch e { e.message }\n"
                  "    print(result)\n"
                  "}\n",
                  "shift amount out of range (0..63)");
}

static void test_bitwise_not_double(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(~~42)\n"
                  "}\n",
                  "42");
}

static void test_bitwise_with_negative(void) {
    /* -1 & 255 = 255 */
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(-1 & 255)\n"
                  "}\n",
                  "255");
}

/* ── Import / module system ── */

static void test_import_full(void) {
    ASSERT_OUTPUT("import \"tests/modules/math_utils\" as math\n"
                  "fn main() {\n"
                  "    print(math.add(10, 5))\n"
                  "    print(math.sub(10, 5))\n"
                  "}\n",
                  "15\n5");
}

static void test_import_variable(void) {
    ASSERT_OUTPUT("import \"tests/modules/math_utils\" as math\n"
                  "fn main() {\n"
                  "    print(math.PI)\n"
                  "}\n",
                  "3.14159");
}

static void test_import_selective(void) {
    ASSERT_OUTPUT("import { add, sub } from \"tests/modules/math_utils\"\n"
                  "fn main() {\n"
                  "    print(add(3, 4))\n"
                  "    print(sub(10, 3))\n"
                  "}\n",
                  "7\n7");
}

static void test_import_selective_variable(void) {
    ASSERT_OUTPUT("import { PI } from \"tests/modules/math_utils\"\n"
                  "fn main() {\n"
                  "    print(PI)\n"
                  "}\n",
                  "3.14159");
}

static void test_import_closure_capture(void) {
    ASSERT_OUTPUT("import \"tests/modules/greeter\" as g\n"
                  "fn main() {\n"
                  "    print(g.greet(\"World\"))\n"
                  "}\n",
                  "Hello, World!");
}

static void test_import_cached(void) {
    /* Importing the same module twice should use cache */
    ASSERT_OUTPUT("import \"tests/modules/math_utils\" as m1\n"
                  "import \"tests/modules/math_utils\" as m2\n"
                  "fn main() {\n"
                  "    print(m1.add(1, 2))\n"
                  "    print(m2.sub(5, 3))\n"
                  "}\n",
                  "3\n2");
}

static void test_import_not_found(void) {
    ASSERT_OUTPUT("import \"nonexistent_module\" as m\n"
                  "fn main() {\n"
                  "    print(m.x)\n"
                  "}\n",
                  "EVAL_ERROR:import: cannot find 'nonexistent_module.lat'");
}

static void test_import_missing_export(void) {
    ASSERT_OUTPUT("import { nonexistent } from \"tests/modules/math_utils\"\n"
                  "fn main() {\n"
                  "    print(nonexistent)\n"
                  "}\n",
                  "EVAL_ERROR:module 'tests/modules/math_utils' does not export 'nonexistent'");
}

/* ── Export system ── */

static void test_export_selective_visible(void) {
    ASSERT_OUTPUT("import { add } from \"tests/modules/export_explicit\"\n"
                  "fn main() {\n"
                  "    print(add(1, 2))\n"
                  "}\n",
                  "3");
}

static void test_export_selective_variable(void) {
    ASSERT_OUTPUT("import { PI } from \"tests/modules/export_explicit\"\n"
                  "fn main() {\n"
                  "    print(PI)\n"
                  "}\n",
                  "3.14159");
}

static void test_export_hidden_name(void) {
    ASSERT_OUTPUT("import { secret } from \"tests/modules/export_explicit\"\n"
                  "fn main() {\n"
                  "    print(secret)\n"
                  "}\n",
                  "EVAL_ERROR:module 'tests/modules/export_explicit' does not export 'secret'");
}

static void test_export_alias_filters(void) {
    ASSERT_OUTPUT("import \"tests/modules/export_explicit\" as m\n"
                  "fn main() {\n"
                  "    print(m.add(1, 2))\n"
                  "    print(m.PI)\n"
                  "}\n",
                  "3\n3.14159");
}

static void test_export_alias_hidden(void) {
    /* Non-exported names should not be in the module map.
     * VM backends return nil, tree-walk throws error — both indicate hidden. */
    ASSERT_OUTPUT("import \"tests/modules/export_explicit\" as m\n"
                  "fn main() {\n"
                  "    try {\n"
                  "        let v = m.secret\n"
                  "        print(v == nil)\n"
                  "    } catch e {\n"
                  "        print(true)\n"
                  "    }\n"
                  "}\n",
                  "true");
}

static void test_no_export_keyword_legacy(void) {
    /* math_utils.lat has no export keywords → everything exported */
    ASSERT_OUTPUT("import { add, sub, PI } from \"tests/modules/math_utils\"\n"
                  "fn main() {\n"
                  "    print(add(1, 2))\n"
                  "    print(sub(5, 3))\n"
                  "    print(PI)\n"
                  "}\n",
                  "3\n2\n3.14159");
}

static void test_export_struct(void) {
    ASSERT_OUTPUT("import { make_point } from \"tests/modules/export_struct\"\n"
                  "fn main() {\n"
                  "    let p = make_point(3, 4)\n"
                  "    print(p.x)\n"
                  "    print(p.y)\n"
                  "}\n",
                  "3\n4");
}

/* ── Transitive imports (module imports another module) ── */

static void test_import_transitive(void) {
    /* mid_layer.lat imports base_utils.lat internally.
     * quadruple(x) = double(double(x)), add_base(x) = x + BASE_CONST (100). */
    ASSERT_OUTPUT("import \"tests/modules/mid_layer\" as mid\n"
                  "fn main() {\n"
                  "    print(mid.quadruple(5))\n"
                  "    print(mid.add_base(50))\n"
                  "    print(mid.MID_CONST)\n"
                  "}\n",
                  "20\n150\n200");
}

static void test_import_transitive_selective(void) {
    /* Selective import from a module that itself uses transitive imports.
     * Only import functions that don't reference module-level variables
     * (selective import extracts functions without full module scope). */
    ASSERT_OUTPUT("import { quadruple } from \"tests/modules/mid_layer\"\n"
                  "fn main() {\n"
                  "    print(quadruple(3))\n"
                  "    print(quadruple(10))\n"
                  "}\n",
                  "12\n40");
}

/* ── Import with alias edge cases ── */

static void test_import_alias_multiple_modules(void) {
    /* Import two different modules with different aliases */
    ASSERT_OUTPUT("import \"tests/modules/math_utils\" as math\n"
                  "import \"tests/modules/base_utils\" as base\n"
                  "fn main() {\n"
                  "    print(math.add(1, 2))\n"
                  "    print(base.double(5))\n"
                  "}\n",
                  "3\n10");
}

static void test_import_alias_same_module_twice(void) {
    /* Same module imported under two different aliases (should use cache) */
    ASSERT_OUTPUT("import \"tests/modules/math_utils\" as a\n"
                  "import \"tests/modules/math_utils\" as b\n"
                  "fn main() {\n"
                  "    print(a.add(10, 20))\n"
                  "    print(b.sub(10, 3))\n"
                  "}\n",
                  "30\n7");
}

static void test_import_alias_and_selective_different_modules(void) {
    /* Mix alias import and selective import from different modules */
    ASSERT_OUTPUT("import \"tests/modules/math_utils\" as math\n"
                  "import { double } from \"tests/modules/base_utils\"\n"
                  "fn main() {\n"
                  "    print(math.add(1, 2))\n"
                  "    print(double(7))\n"
                  "}\n",
                  "3\n14");
}

/* ── Re-export: module A exports something it imported from module B ── */

static void test_import_reexport_function(void) {
    /* reexporter.lat imports double from base_utils and wraps it */
    ASSERT_OUTPUT("import { double_it } from \"tests/modules/reexporter\"\n"
                  "fn main() {\n"
                  "    print(double_it(7))\n"
                  "}\n",
                  "14");
}

static void test_import_reexport_variable(void) {
    /* reexporter.lat re-exports BASE_CONST from base_utils as REEXPORTED_CONST */
    ASSERT_OUTPUT("import { REEXPORTED_CONST } from \"tests/modules/reexporter\"\n"
                  "fn main() {\n"
                  "    print(REEXPORTED_CONST)\n"
                  "}\n",
                  "100");
}

static void test_import_reexport_via_alias(void) {
    /* Access re-exported members through alias */
    ASSERT_OUTPUT("import \"tests/modules/reexporter\" as r\n"
                  "fn main() {\n"
                  "    print(r.double_it(9))\n"
                  "    print(r.REEXPORTED_CONST)\n"
                  "}\n",
                  "18\n100");
}

/* ── Selective import from module with many exports ── */

static void test_import_selective_subset(void) {
    /* Import only a few functions from a module with many exports */
    ASSERT_OUTPUT("import { alpha, gamma, VAL_TWO } from \"tests/modules/many_exports\"\n"
                  "fn main() {\n"
                  "    print(alpha())\n"
                  "    print(gamma())\n"
                  "    print(VAL_TWO)\n"
                  "}\n",
                  "a\nc\n2");
}

static void test_import_selective_single(void) {
    /* Import just one function from a module with many exports */
    ASSERT_OUTPUT("import { epsilon } from \"tests/modules/many_exports\"\n"
                  "fn main() {\n"
                  "    print(epsilon())\n"
                  "}\n",
                  "e");
}

static void test_import_selective_only_variables(void) {
    /* Import only variables, no functions */
    ASSERT_OUTPUT("import { VAL_ONE, VAL_THREE } from \"tests/modules/many_exports\"\n"
                  "fn main() {\n"
                  "    print(VAL_ONE)\n"
                  "    print(VAL_THREE)\n"
                  "}\n",
                  "1\n3");
}

/* ── Module that exports structs with methods (closure fields) ── */

static void test_import_struct_with_method(void) {
    /* Vec2 has a mag2 closure field (squared magnitude). make_vec2 creates instances. */
    ASSERT_OUTPUT("import { make_vec2 } from \"tests/modules/export_struct_methods\"\n"
                  "fn main() {\n"
                  "    let v = make_vec2(3, 4)\n"
                  "    print(v.x)\n"
                  "    print(v.y)\n"
                  "    print(v.mag2())\n"
                  "}\n",
                  "3\n4\n25");
}

static void test_import_struct_method_with_operations(void) {
    /* vec2_add combines two Vec2 structs */
    ASSERT_OUTPUT("import { make_vec2, vec2_add } from \"tests/modules/export_struct_methods\"\n"
                  "fn main() {\n"
                  "    let a = make_vec2(1, 2)\n"
                  "    let b = make_vec2(3, 4)\n"
                  "    let c = vec2_add(a, b)\n"
                  "    print(c.x)\n"
                  "    print(c.y)\n"
                  "    print(c.mag2())\n"
                  "}\n",
                  "4\n6\n52");
}

static void test_import_struct_via_alias(void) {
    /* Access struct factory via alias */
    ASSERT_OUTPUT("import \"tests/modules/export_struct_methods\" as vec\n"
                  "fn main() {\n"
                  "    let v = vec.make_vec2(5, 12)\n"
                  "    print(v.x)\n"
                  "    print(v.y)\n"
                  "}\n",
                  "5\n12");
}

/* ── Module that exports enums (via factory functions) ── */

static void test_import_enum_via_factory(void) {
    /* Use factory functions to create enum values from imported module */
    ASSERT_OUTPUT("import { make_red, make_green, color_name } from \"tests/modules/export_enum\"\n"
                  "fn main() {\n"
                  "    let r = make_red()\n"
                  "    print(color_name(r))\n"
                  "    let g = make_green()\n"
                  "    print(color_name(g))\n"
                  "}\n",
                  "red\ngreen");
}

static void test_import_enum_equality_from_module(void) {
    /* Enum values from module can be compared for equality */
    ASSERT_OUTPUT("import { make_red, make_green, colors_equal } from \"tests/modules/export_enum\"\n"
                  "fn main() {\n"
                  "    let r1 = make_red()\n"
                  "    let r2 = make_red()\n"
                  "    let g = make_green()\n"
                  "    print(colors_equal(r1, r2))\n"
                  "    print(colors_equal(r1, g))\n"
                  "}\n",
                  "true\nfalse");
}

static void test_import_enum_tuple_variant_from_module(void) {
    /* Enum tuple variants created by module factory */
    ASSERT_OUTPUT("import { make_circle } from \"tests/modules/export_enum\"\n"
                  "fn main() {\n"
                  "    let c = make_circle(5)\n"
                  "    print(c)\n"
                  "}\n",
                  "Shape::Circle(5)");
}

static void test_import_enum_via_alias(void) {
    /* Access enum factories via alias */
    ASSERT_OUTPUT("import \"tests/modules/export_enum\" as e\n"
                  "fn main() {\n"
                  "    let r = e.make_red()\n"
                  "    print(e.color_name(r))\n"
                  "}\n",
                  "red");
}

/* ── Import error edge cases ── */

static void test_import_selective_missing_from_many(void) {
    /* Importing a nonexistent name from a module with many exports */
    ASSERT_OUTPUT("import { alpha, nonexistent } from \"tests/modules/many_exports\"\n"
                  "fn main() {\n"
                  "    print(alpha())\n"
                  "}\n",
                  "EVAL_ERROR:module 'tests/modules/many_exports' does not export 'nonexistent'");
}

static void test_import_hidden_function(void) {
    /* Trying to selectively import a non-exported function */
    ASSERT_OUTPUT("import { private_helper } from \"tests/modules/mixed_exports\"\n"
                  "fn main() {\n"
                  "    print(private_helper())\n"
                  "}\n",
                  "EVAL_ERROR:module 'tests/modules/mixed_exports' does not export 'private_helper'");
}

static void test_import_hidden_variable(void) {
    /* Trying to selectively import a non-exported variable */
    ASSERT_OUTPUT("import { internal_state } from \"tests/modules/mixed_exports\"\n"
                  "fn main() {\n"
                  "    print(internal_state)\n"
                  "}\n",
                  "EVAL_ERROR:module 'tests/modules/mixed_exports' does not export 'internal_state'");
}

static void test_import_hidden_via_alias(void) {
    /* Non-exported names should not be visible via alias.
     * VM backends return nil, tree-walk throws error — both indicate hidden. */
    ASSERT_OUTPUT("import \"tests/modules/many_exports\" as m\n"
                  "fn main() {\n"
                  "    try {\n"
                  "        let v = m.internal_only\n"
                  "        print(v == nil)\n"
                  "    } catch e {\n"
                  "        print(true)\n"
                  "    }\n"
                  "}\n",
                  "true");
}

/* ── Module namespace isolation ── */

static void test_import_ns_no_collision_aliased(void) {
    /* Two modules with same struct/enum names imported under different aliases.
     * Both should work without collision. (tree-walk only: module-qualified
     * struct/enum construction is in the tree-walk evaluator.) */
    if (test_backend != BACKEND_TREE_WALK) return;
    ASSERT_OUTPUT("import \"tests/modules/ns_mod_a\" as a\n"
                  "import \"tests/modules/ns_mod_b\" as b\n"
                  "fn main() {\n"
                  "    let sa = a.get_status()\n"
                  "    let sb = b.get_status()\n"
                  "    print(sa.variant_name())\n"
                  "    print(sb.variant_name())\n"
                  "}\n",
                  "Active\nPending");
}

static void test_import_ns_qualified_struct(void) {
    /* Module-qualified struct construction: mod.StructName { ... } */
    if (test_backend != BACKEND_TREE_WALK) return;
    ASSERT_OUTPUT("import \"tests/modules/ns_mod_a\" as a\n"
                  "import \"tests/modules/ns_mod_b\" as b\n"
                  "fn main() {\n"
                  "    let ia = a.Item { name: \"apple\", value: 42 }\n"
                  "    let ib = b.Item { id: 1, label: \"box\", weight: 3.5 }\n"
                  "    print(ia.name)\n"
                  "    print(ia.value)\n"
                  "    print(ib.id)\n"
                  "    print(ib.label)\n"
                  "    print(ib.weight)\n"
                  "}\n",
                  "apple\n42\n1\nbox\n3.5");
}

static void test_import_ns_qualified_enum(void) {
    /* Module-qualified enum variant: mod.EnumName::Variant */
    if (test_backend != BACKEND_TREE_WALK) return;
    ASSERT_OUTPUT("import \"tests/modules/ns_mod_a\" as a\n"
                  "import \"tests/modules/ns_mod_b\" as b\n"
                  "fn main() {\n"
                  "    let sa = a.Status::Active\n"
                  "    let sb = b.Status::Failed\n"
                  "    print(sa.variant_name())\n"
                  "    print(sb.variant_name())\n"
                  "}\n",
                  "Active\nFailed");
}

static void test_import_ns_qualified_struct_error(void) {
    /* Accessing a non-existent struct via module alias should error */
    if (test_backend != BACKEND_TREE_WALK) return;
    ASSERT_OUTPUT_STARTS_WITH("import \"tests/modules/ns_mod_a\" as a\n"
                              "fn main() {\n"
                              "    let x = a.NonExistent { foo: 1 }\n"
                              "    print(x)\n"
                              "}\n",
                              "EVAL_ERROR:module 'a' has no struct 'NonExistent'");
}

static void test_import_ns_qualified_enum_error(void) {
    /* Accessing a non-existent enum via module alias should error */
    if (test_backend != BACKEND_TREE_WALK) return;
    ASSERT_OUTPUT_STARTS_WITH("import \"tests/modules/ns_mod_a\" as a\n"
                              "fn main() {\n"
                              "    let x = a.NonExistent::Variant\n"
                              "    print(x)\n"
                              "}\n",
                              "EVAL_ERROR:module 'a' has no enum 'NonExistent'");
}

static void test_import_ns_selective_no_collision(void) {
    /* Selective import of a specific struct from one module while the other
     * is aliased — no collision on the selective name */
    if (test_backend != BACKEND_TREE_WALK) return;
    ASSERT_OUTPUT("import { make_item } from \"tests/modules/ns_mod_a\"\n"
                  "import \"tests/modules/ns_mod_b\" as b\n"
                  "fn main() {\n"
                  "    let ia = make_item(\"grape\", 10)\n"
                  "    let ib = b.make_item(2, \"crate\")\n"
                  "    print(ia.name)\n"
                  "    print(ib.label)\n"
                  "}\n",
                  "grape\ncrate");
}

static void test_import_ns_backward_compat(void) {
    /* Single import works exactly as before (backward compat) */
    ASSERT_OUTPUT("import \"tests/modules/export_struct\" as s\n"
                  "fn main() {\n"
                  "    let p = s.make_point(3, 4)\n"
                  "    print(p.x)\n"
                  "    print(p.y)\n"
                  "}\n",
                  "3\n4");
}

/* ── Module with no export keyword (legacy: everything exported) ── */

static void test_no_export_all_functions_visible(void) {
    /* math_utils.lat has no export keywords → all functions exported */
    ASSERT_OUTPUT("import \"tests/modules/math_utils\" as m\n"
                  "fn main() {\n"
                  "    print(m.add(100, 200))\n"
                  "    print(m.sub(50, 25))\n"
                  "}\n",
                  "300\n25");
}

static void test_no_export_variable_visible(void) {
    /* Legacy module: variables are also exported */
    ASSERT_OUTPUT("import \"tests/modules/math_utils\" as m\n"
                  "fn main() {\n"
                  "    print(m.PI)\n"
                  "}\n",
                  "3.14159");
}

/* ── Module with both functions and variables exported ── */

static void test_mixed_exports_via_alias(void) {
    /* Access both exported functions and variables via alias */
    ASSERT_OUTPUT("import \"tests/modules/mixed_exports\" as m\n"
                  "fn main() {\n"
                  "    print(m.VERSION)\n"
                  "    print(m.MAX_SIZE)\n"
                  "    print(m.greet(\"Alice\"))\n"
                  "    print(m.compute(2, 3))\n"
                  "}\n",
                  "1.0.0\n256\nHi, Alice\n262");
}

static void test_mixed_exports_selective(void) {
    /* Selectively import a mix of functions and variables.
     * Use greet() which doesn't reference module-level variables
     * (selective import extracts functions without full module scope). */
    ASSERT_OUTPUT("import { VERSION, greet } from \"tests/modules/mixed_exports\"\n"
                  "fn main() {\n"
                  "    print(VERSION)\n"
                  "    print(greet(\"Bob\"))\n"
                  "}\n",
                  "1.0.0\nHi, Bob");
}

static void test_mixed_exports_hidden_not_in_alias(void) {
    /* Private members should not appear when using alias */
    ASSERT_OUTPUT("import \"tests/modules/mixed_exports\" as m\n"
                  "fn main() {\n"
                  "    try {\n"
                  "        let v = m.private_helper\n"
                  "        print(v == nil)\n"
                  "    } catch e {\n"
                  "        print(true)\n"
                  "    }\n"
                  "}\n",
                  "true");
}

/* ── repr() builtin ── */

static void test_repr_int(void) { ASSERT_OUTPUT("fn main() { print(repr(42)) }", "42"); }

static void test_repr_string(void) { ASSERT_OUTPUT("fn main() { print(repr(\"hello\")) }", "\"hello\""); }

static void test_repr_array(void) { ASSERT_OUTPUT("fn main() { print(repr([1, 2, 3])) }", "[1, 2, 3]"); }

static void test_repr_struct_default(void) {
    ASSERT_OUTPUT("struct Point { x: Int, y: Int }\n"
                  "fn main() {\n"
                  "    let p = Point { x: 10, y: 20 }\n"
                  "    print(repr(p))\n"
                  "}",
                  "Point { x: 10, y: 20 }");
}

static void test_repr_struct_custom(void) {
    ASSERT_OUTPUT("struct Point { x: Int, y: Int, repr: Closure }\n"
                  "fn main() {\n"
                  "    let p = Point { x: 10, y: 20, repr: |self| { \"(\" + to_string(self.x) + \", \" + "
                  "to_string(self.y) + \")\" } }\n"
                  "    print(repr(p))\n"
                  "}",
                  "(10, 20)");
}

static void test_repr_struct_custom_non_string(void) {
    /* If repr closure returns non-string, fall back to default.
     * Note: RegVM bytecode closures don't carry param_names (they're
     * only needed for display, not execution), so accept either
     * <closure|self|> or <closure||> depending on backend. */
    ASSERT_OUTPUT_STARTS_WITH("struct Foo { val: Int, repr: Closure }\n"
                              "fn main() {\n"
                              "    let f = Foo { val: 99, repr: |self| { 42 } }\n"
                              "    print(repr(f))\n"
                              "}",
                              "Foo { val: 99, repr: <closure");
}

static void test_repr_nil(void) { ASSERT_OUTPUT("fn main() { print(repr(nil)) }", "nil"); }

static void test_repr_bool(void) { ASSERT_OUTPUT("fn main() { print(repr(true)) }", "true"); }

/* ======================================================================
 * Native Extension System (require_ext)
 * ====================================================================== */

static void test_require_ext_missing(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    let ext = require_ext(\"nonexistent_extension_xyz\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_require_ext_wrong_type(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    let ext = require_ext(42)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_require_ext_no_args(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    let ext = require_ext()\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_require_ext_empty_string(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    let ext = require_ext(\"\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_require_ext_path_traversal(void) {
    /* Path separator in name should be rejected */
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    let ext = require_ext(\"../etc/passwd\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_require_ext_backslash_path(void) {
    /* Backslash path separator should also be rejected */
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    let ext = require_ext(\"..\\\\evil\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_require_ext_too_many_args(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    let ext = require_ext(\"foo\", \"bar\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_require_ext_bool_arg(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    let ext = require_ext(true)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_require_ext_nil_arg(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    let ext = require_ext(nil)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_require_ext_array_arg(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    let ext = require_ext([1, 2, 3])\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_require_ext_double_load(void) {
    /* Loading the same missing extension twice should give consistent errors.
     * Both attempts should produce the same error message, verifying that
     * failed loads are not cached as successes. */
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r1 = try { require_ext(\"nonexistent_double_xyz\") } catch e { e.message }\n"
                  "    let r2 = try { require_ext(\"nonexistent_double_xyz\") } catch e { e.message }\n"
                  "    print(r1 == r2)\n"
                  "}\n",
                  "true");
}

static void test_require_ext_not_a_dylib(void) {
    /* A file that exists but is not a valid shared library */
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    let ext = require_ext(\"not_a_real_extension_abc\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_require_ext_error_message_contains_name(void) {
    /* Error message should include the extension name for debugging */
    char *out = run_capture("fn main() {\n"
                            "    let ext = require_ext(\"my_missing_ext_42\")\n"
                            "}\n");
    ASSERT(strstr(out, "my_missing_ext_42") != NULL);
    free(out);
}

/* ======================================================================
 * SQLite Extension
 * ====================================================================== */

/* Helper: common preamble that loads the sqlite extension and extracts fns */
#define SQLITE_PREAMBLE                    \
    "let db = require_ext(\"sqlite\")\n"   \
    "let open_fn = db.get(\"open\")\n"     \
    "let close_fn = db.get(\"close\")\n"   \
    "let query_fn = db.get(\"query\")\n"   \
    "let exec_fn = db.get(\"exec\")\n"     \
    "let status_fn = db.get(\"status\")\n" \
    "let rowid_fn = db.get(\"last_insert_rowid\")\n"

static void test_sqlite_open_close(void) {
    ASSERT_OUTPUT("fn main() {\n" SQLITE_PREAMBLE "    let conn = open_fn(\":memory:\")\n"
                  "    print(typeof(conn))\n"
                  "    close_fn(conn)\n"
                  "    print(\"done\")\n"
                  "}\n",
                  "Int\ndone");
}

static void test_sqlite_status(void) {
    ASSERT_OUTPUT("fn main() {\n" SQLITE_PREAMBLE "    let conn = open_fn(\":memory:\")\n"
                  "    let s1 = status_fn(conn)\n"
                  "    print(s1)\n"
                  "    close_fn(conn)\n"
                  "    let s2 = status_fn(conn)\n"
                  "    print(s2)\n"
                  "}\n",
                  "ok\nclosed");
}

static void test_sqlite_exec_create(void) {
    ASSERT_OUTPUT("fn main() {\n" SQLITE_PREAMBLE "    let conn = open_fn(\":memory:\")\n"
                  "    let r = exec_fn(conn, \"CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)\")\n"
                  "    print(r)\n"
                  "    close_fn(conn)\n"
                  "}\n",
                  "0");
}

static void test_sqlite_exec_insert(void) {
    ASSERT_OUTPUT("fn main() {\n" SQLITE_PREAMBLE "    let conn = open_fn(\":memory:\")\n"
                  "    exec_fn(conn, \"CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)\")\n"
                  "    let r1 = exec_fn(conn, \"INSERT INTO t (name) VALUES ('Alice')\")\n"
                  "    print(r1)\n"
                  "    let r2 = exec_fn(conn, \"INSERT INTO t (name) VALUES ('Bob')\")\n"
                  "    print(r2)\n"
                  "    close_fn(conn)\n"
                  "}\n",
                  "1\n1");
}

static void test_sqlite_query_basic(void) {
    ASSERT_OUTPUT("fn main() {\n" SQLITE_PREAMBLE "    let conn = open_fn(\":memory:\")\n"
                  "    exec_fn(conn, \"CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)\")\n"
                  "    exec_fn(conn, \"INSERT INTO t (name) VALUES ('Alice')\")\n"
                  "    exec_fn(conn, \"INSERT INTO t (name) VALUES ('Bob')\")\n"
                  "    let rows = query_fn(conn, \"SELECT * FROM t ORDER BY id\")\n"
                  "    print(len(rows))\n"
                  "    print(rows.first().get(\"name\"))\n"
                  "    print(rows.last().get(\"name\"))\n"
                  "    close_fn(conn)\n"
                  "}\n",
                  "2\nAlice\nBob");
}

static void test_sqlite_query_types(void) {
    ASSERT_OUTPUT("fn main() {\n" SQLITE_PREAMBLE "    let conn = open_fn(\":memory:\")\n"
                  "    exec_fn(conn, \"CREATE TABLE t (i INTEGER, f REAL, s TEXT, n BLOB)\")\n"
                  "    exec_fn(conn, \"INSERT INTO t VALUES (42, 3.14, 'hello', NULL)\")\n"
                  "    let rows = query_fn(conn, \"SELECT * FROM t\")\n"
                  "    let row = rows.first()\n"
                  "    print(typeof(row.get(\"i\")))\n"
                  "    print(row.get(\"i\"))\n"
                  "    print(typeof(row.get(\"f\")))\n"
                  "    print(row.get(\"f\"))\n"
                  "    print(typeof(row.get(\"s\")))\n"
                  "    print(row.get(\"s\"))\n"
                  "    print(typeof(row.get(\"n\")))\n"
                  "    close_fn(conn)\n"
                  "}\n",
                  "Int\n42\nFloat\n3.14\nString\nhello\nNil");
}

static void test_sqlite_query_empty(void) {
    ASSERT_OUTPUT("fn main() {\n" SQLITE_PREAMBLE "    let conn = open_fn(\":memory:\")\n"
                  "    exec_fn(conn, \"CREATE TABLE t (id INTEGER)\")\n"
                  "    let rows = query_fn(conn, \"SELECT * FROM t\")\n"
                  "    print(len(rows))\n"
                  "    close_fn(conn)\n"
                  "}\n",
                  "0");
}

static void test_sqlite_exec_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n" SQLITE_PREAMBLE "    let conn = open_fn(\":memory:\")\n"
                              "    exec_fn(conn, \"INSERT INTO nonexistent VALUES (1)\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_sqlite_query_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n" SQLITE_PREAMBLE "    let conn = open_fn(\":memory:\")\n"
                              "    query_fn(conn, \"SELECT * FROM nonexistent\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_sqlite_close_invalid(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n" SQLITE_PREAMBLE "    close_fn(9999)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_sqlite_multiple_tables(void) {
    ASSERT_OUTPUT(
        "fn main() {\n" SQLITE_PREAMBLE "    let conn = open_fn(\":memory:\")\n"
        "    exec_fn(conn, \"CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)\")\n"
        "    exec_fn(conn, \"CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INTEGER, amount REAL)\")\n"
        "    exec_fn(conn, \"INSERT INTO users (name) VALUES ('Alice')\")\n"
        "    exec_fn(conn, \"INSERT INTO orders (user_id, amount) VALUES (1, 99.95)\")\n"
        "    let rows = query_fn(conn, \"SELECT u.name, o.amount FROM users u JOIN orders o ON u.id = o.user_id\")\n"
        "    print(len(rows))\n"
        "    let row = rows.first()\n"
        "    print(row.get(\"name\"))\n"
        "    print(row.get(\"amount\"))\n"
        "    close_fn(conn)\n"
        "}\n",
        "1\nAlice\n99.95");
}

/* ======================================================================
 * Struct Reflection Builtins
 * ====================================================================== */

static void test_struct_name(void) {
    ASSERT_OUTPUT("struct User { name: String, age: Int }\n"
                  "fn main() {\n"
                  "    let u = User { name: \"Alice\", age: 30 }\n"
                  "    print(struct_name(u))\n"
                  "}\n",
                  "User");
}

static void test_struct_fields(void) {
    ASSERT_OUTPUT("struct Point { x: Int, y: Int }\n"
                  "fn main() {\n"
                  "    let p = Point { x: 1, y: 2 }\n"
                  "    print(struct_fields(p))\n"
                  "}\n",
                  "[x, y]");
}

static void test_struct_to_map(void) {
    ASSERT_OUTPUT("struct User { name: String, age: Int }\n"
                  "fn main() {\n"
                  "    let u = User { name: \"Alice\", age: 30 }\n"
                  "    let m = struct_to_map(u)\n"
                  "    print(m.get(\"name\"))\n"
                  "    print(m.get(\"age\"))\n"
                  "}\n",
                  "Alice\n30");
}

static void test_struct_from_map(void) {
    ASSERT_OUTPUT("struct User { name: String, age: Int }\n"
                  "fn main() {\n"
                  "    let u = User { name: \"Alice\", age: 30 }\n"
                  "    let m = struct_to_map(u)\n"
                  "    let u2 = struct_from_map(\"User\", m)\n"
                  "    print(u2.name)\n"
                  "    print(u2.age)\n"
                  "}\n",
                  "Alice\n30");
}

static void test_struct_from_map_missing(void) {
    ASSERT_OUTPUT("struct User { name: String, age: Int }\n"
                  "fn main() {\n"
                  "    let m = Map::new()\n"
                  "    m.set(\"name\", \"Bob\")\n"
                  "    let u = struct_from_map(\"User\", m)\n"
                  "    print(u.name)\n"
                  "    print(u.age)\n"
                  "}\n",
                  "Bob\nnil");
}

static void test_struct_from_map_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    let m = Map::new()\n"
                              "    struct_from_map(\"NonExistent\", m)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* ======================================================================
 * SQLite Parameterized Queries
 * ====================================================================== */

static void test_sqlite_param_query(void) {
    ASSERT_OUTPUT("fn main() {\n" SQLITE_PREAMBLE "    let conn = open_fn(\":memory:\")\n"
                  "    exec_fn(conn, \"CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)\")\n"
                  "    exec_fn(conn, \"INSERT INTO t VALUES (1, 'Alice')\")\n"
                  "    exec_fn(conn, \"INSERT INTO t VALUES (2, 'Bob')\")\n"
                  "    let rows = query_fn(conn, \"SELECT * FROM t WHERE name = ?\", [\"Alice\"])\n"
                  "    print(len(rows))\n"
                  "    print(rows.first().get(\"name\"))\n"
                  "    close_fn(conn)\n"
                  "}\n",
                  "1\nAlice");
}

static void test_sqlite_param_exec(void) {
    ASSERT_OUTPUT("fn main() {\n" SQLITE_PREAMBLE "    let conn = open_fn(\":memory:\")\n"
                  "    exec_fn(conn, \"CREATE TABLE t (name TEXT, age INTEGER)\")\n"
                  "    exec_fn(conn, \"INSERT INTO t (name, age) VALUES (?, ?)\", [\"Alice\", 30])\n"
                  "    let rows = query_fn(conn, \"SELECT * FROM t\")\n"
                  "    print(rows.first().get(\"name\"))\n"
                  "    print(rows.first().get(\"age\"))\n"
                  "    close_fn(conn)\n"
                  "}\n",
                  "Alice\n30");
}

static void test_sqlite_param_types(void) {
    ASSERT_OUTPUT("fn main() {\n" SQLITE_PREAMBLE "    let conn = open_fn(\":memory:\")\n"
                  "    exec_fn(conn, \"CREATE TABLE t (i INTEGER, f REAL, s TEXT, n TEXT)\")\n"
                  "    exec_fn(conn, \"INSERT INTO t VALUES (?, ?, ?, ?)\", [42, 3.14, \"hello\", nil])\n"
                  "    let rows = query_fn(conn, \"SELECT * FROM t\")\n"
                  "    let row = rows.first()\n"
                  "    print(typeof(row.get(\"i\")))\n"
                  "    print(row.get(\"i\"))\n"
                  "    print(typeof(row.get(\"f\")))\n"
                  "    print(row.get(\"f\"))\n"
                  "    print(row.get(\"s\"))\n"
                  "    print(typeof(row.get(\"n\")))\n"
                  "    close_fn(conn)\n"
                  "}\n",
                  "Int\n42\nFloat\n3.14\nhello\nNil");
}

static void test_sqlite_last_insert_rowid(void) {
    ASSERT_OUTPUT("fn main() {\n" SQLITE_PREAMBLE "    let conn = open_fn(\":memory:\")\n"
                  "    exec_fn(conn, \"CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)\")\n"
                  "    exec_fn(conn, \"INSERT INTO t (name) VALUES (?)\", [\"Alice\"])\n"
                  "    print(rowid_fn(conn))\n"
                  "    exec_fn(conn, \"INSERT INTO t (name) VALUES (?)\", [\"Bob\"])\n"
                  "    print(rowid_fn(conn))\n"
                  "    close_fn(conn)\n"
                  "}\n",
                  "1\n2");
}

/* ======================================================================
 * Phase Constraints
 * ====================================================================== */

static void test_phase_constraint_flux_accepts_fluid(void) {
    ASSERT_OUTPUT("fn mutate(data: ~Map) { print(\"ok\") }\n"
                  "fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    mutate(m)\n"
                  "}\n",
                  "ok");
}

static void test_phase_constraint_flux_rejects_crystal(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn mutate(data: ~Map) { print(\"ok\") }\n"
                              "fn main() {\n"
                              "    fix m = freeze(Map::new())\n"
                              "    mutate(m)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_phase_constraint_fix_accepts_crystal(void) {
    ASSERT_OUTPUT("fn inspect(data: *Map) { print(\"ok\") }\n"
                  "fn main() {\n"
                  "    fix m = freeze(Map::new())\n"
                  "    inspect(m)\n"
                  "}\n",
                  "ok");
}

static void test_phase_constraint_fix_rejects_fluid(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn inspect(data: *Map) { print(\"ok\") }\n"
                              "fn main() {\n"
                              "    flux m = Map::new()\n"
                              "    inspect(m)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_phase_constraint_unphased_accepts_any(void) {
    ASSERT_OUTPUT("fn process(data: Map) { print(\"ok\") }\n"
                  "fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    process(m)\n"
                  "    fix m2 = freeze(Map::new())\n"
                  "    process(m2)\n"
                  "}\n",
                  "ok\nok");
}

static void test_phase_constraint_flux_keyword_syntax(void) {
    ASSERT_OUTPUT("fn mutate(data: flux Map) { print(\"flux keyword\") }\n"
                  "fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    mutate(m)\n"
                  "}\n",
                  "flux keyword");
}

static void test_phase_constraint_fix_keyword_syntax(void) {
    ASSERT_OUTPUT("fn inspect(data: fix Map) { print(\"fix keyword\") }\n"
                  "fn main() {\n"
                  "    fix m = freeze(Map::new())\n"
                  "    inspect(m)\n"
                  "}\n",
                  "fix keyword");
}

/* ======================================================================
 * Phase-Dependent Dispatch
 * ====================================================================== */

static void test_phase_dispatch_fluid_to_flux(void) {
    ASSERT_OUTPUT("fn process(data: ~Map) { print(\"flux path\") }\n"
                  "fn process(data: *Map) { print(\"fix path\") }\n"
                  "fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    process(m)\n"
                  "}\n",
                  "flux path");
}

static void test_phase_dispatch_crystal_to_fix(void) {
    ASSERT_OUTPUT("fn process(data: ~Map) { print(\"flux path\") }\n"
                  "fn process(data: *Map) { print(\"fix path\") }\n"
                  "fn main() {\n"
                  "    fix m = freeze(Map::new())\n"
                  "    process(m)\n"
                  "}\n",
                  "fix path");
}

static void test_phase_dispatch_no_match_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn process(data: ~Map) { print(\"flux path\") }\n"
                              "fn main() {\n"
                              "    fix m = freeze(Map::new())\n"
                              "    process(m)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_phase_dispatch_same_sig_replaces(void) {
    ASSERT_OUTPUT("fn greet() { print(\"first\") }\n"
                  "fn greet() { print(\"second\") }\n"
                  "fn main() {\n"
                  "    greet()\n"
                  "}\n",
                  "second");
}

static void test_phase_dispatch_unphased_fallback(void) {
    ASSERT_OUTPUT("fn process(data: ~Map) { print(\"flux path\") }\n"
                  "fn process(data: Map) { print(\"fallback\") }\n"
                  "fn main() {\n"
                  "    let m = Map::new()\n"
                  "    process(m)\n"
                  "}\n",
                  "fallback");
}

/* ======================================================================
 * Crystallization Contracts
 * ====================================================================== */

static void test_contract_basic_pass(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 42\n"
                  "    freeze(x) where |v| { if v < 0 { throw(\"must be non-negative\") } }\n"
                  "    print(x)\n"
                  "}\n",
                  "42");
}

static void test_contract_basic_fail(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux x = -5\n"
                              "    freeze(x) where |v| { if v < 0 { throw(\"must be non-negative\") } }\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_contract_map_validation(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"name\"] = \"Alice\"\n"
                  "    m[\"age\"] = 30\n"
                  "    freeze(m) where |v| {\n"
                  "        if !v.has(\"name\") { throw(\"missing name\") }\n"
                  "        if !v.has(\"age\") { throw(\"missing age\") }\n"
                  "    }\n"
                  "    print(m[\"name\"])\n"
                  "}\n",
                  "Alice");
}

static void test_contract_map_fail(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux m = Map::new()\n"
                              "    m[\"name\"] = \"Alice\"\n"
                              "    freeze(m) where |v| {\n"
                              "        if !v.has(\"age\") { throw(\"missing age field\") }\n"
                              "    }\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_contract_no_contract_compat(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 10\n"
                  "    freeze(x)\n"
                  "    print(x)\n"
                  "}\n",
                  "10");
}

static void test_contract_array(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux arr = [1, 2, 3]\n"
                  "    freeze(arr) where |v| {\n"
                  "        if len(v) == 0 { throw(\"array cannot be empty\") }\n"
                  "    }\n"
                  "    print(len(arr))\n"
                  "}\n",
                  "3");
}

static void test_contract_nonident_expr(void) {
    /* freeze(expr) where contract — non-identifier expression */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"x\"] = 5\n"
                  "    fix result = freeze(clone(m)) where |v| {\n"
                  "        if !v.has(\"x\") { throw(\"no x\") }\n"
                  "    }\n"
                  "    print(result[\"x\"])\n"
                  "}\n",
                  "5");
}

static void test_contract_error_message(void) {
    /* Verify the error message includes "freeze contract failed:" prefix */
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux x = 0\n"
                              "    freeze(x) where |v| { if true { 1 / 0 } }\n"
                              "}\n",
                              "EVAL_ERROR:freeze contract failed:");
}

/* ======================================================================
 * Phase Propagation (Bonds)
 * ====================================================================== */

static void test_bond_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = 1\n"
                  "    flux b = 2\n"
                  "    bond(a, b)\n"
                  "    freeze(a)\n"
                  "    print(phase_of(b))\n"
                  "}\n",
                  "crystal");
}

static void test_bond_multiple_deps(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 10\n"
                  "    flux y = 20\n"
                  "    flux z = 30\n"
                  "    bond(x, y, z)\n"
                  "    freeze(x)\n"
                  "    print(phase_of(y))\n"
                  "    print(phase_of(z))\n"
                  "}\n",
                  "crystal\ncrystal");
}

static void test_unbond(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = 1\n"
                  "    flux b = 2\n"
                  "    bond(a, b)\n"
                  "    unbond(a, b)\n"
                  "    freeze(a)\n"
                  "    print(phase_of(b))\n"
                  "}\n",
                  "fluid");
}

static void test_bond_already_frozen_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    fix a = freeze(1)\n"
                              "    flux b = 2\n"
                              "    bond(a, b)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_bond_phase_of_after_cascade(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux config = Map::new()\n"
                  "    config[\"host\"] = \"localhost\"\n"
                  "    flux port = 8080\n"
                  "    bond(config, port)\n"
                  "    freeze(config)\n"
                  "    print(phase_of(config))\n"
                  "    print(phase_of(port))\n"
                  "}\n",
                  "crystal\ncrystal");
}

static void test_bond_transitive(void) {
    /* a -> b -> c: freezing a should freeze b, which freezes c */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = 1\n"
                  "    flux b = 2\n"
                  "    flux c = 3\n"
                  "    bond(a, b)\n"
                  "    bond(b, c)\n"
                  "    freeze(a)\n"
                  "    print(phase_of(c))\n"
                  "}\n",
                  "crystal");
}

static void test_bond_non_ident_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux a = 1\n"
                              "    bond(a, 42)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_bond_undefined_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux a = 1\n"
                              "    bond(a, nonexistent)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* ======================================================================
 * Phase Reactions
 * ====================================================================== */

static void test_react_freeze_fires(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 42\n"
                  "    react(x, |phase, val| { print(phase) })\n"
                  "    freeze(x)\n"
                  "}\n",
                  "crystal");
}

static void test_react_thaw_fires(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 42\n"
                  "    react(x, |phase, val| { print(phase) })\n"
                  "    freeze(x)\n"
                  "    thaw(x)\n"
                  "}\n",
                  "crystal\nfluid");
}

static void test_react_value_passed(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = [1, 2, 3]\n"
                  "    react(x, |phase, val| { print(to_string(val)) })\n"
                  "    freeze(x)\n"
                  "}\n",
                  "[1, 2, 3]");
}

static void test_react_multiple_callbacks(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 10\n"
                  "    react(x, |phase, val| { print(\"first\") })\n"
                  "    react(x, |phase, val| { print(\"second\") })\n"
                  "    freeze(x)\n"
                  "}\n",
                  "first\nsecond");
}

static void test_react_anneal_fires(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 10\n"
                  "    freeze(x)\n"
                  "    react(x, |phase, val| { print(phase + \":\" + to_string(val)) })\n"
                  "    anneal(x) |v| { v + 5 }\n"
                  "}\n",
                  "crystal:15");
}

static void test_react_cascade_fires(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = 1\n"
                  "    flux b = 2\n"
                  "    bond(a, b)\n"
                  "    react(b, |phase, val| { print(\"b:\" + phase) })\n"
                  "    freeze(a)\n"
                  "}\n",
                  "b:crystal");
}

static void test_unreact_removes(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 42\n"
                  "    react(x, |phase, val| { print(\"fired\") })\n"
                  "    unreact(x)\n"
                  "    freeze(x)\n"
                  "}\n",
                  "");
}

static void test_react_error_propagates(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux x = 1\n"
                              "    react(x, |phase, val| { panic(\"boom\") })\n"
                              "    freeze(x)\n"
                              "}\n",
                              "EVAL_ERROR:reaction error:");
}

/* ======================================================================
 * Phase History / Temporal Values
 * ====================================================================== */

static void test_track_phases_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 10\n"
                  "    track(\"x\")\n"
                  "    x = 20\n"
                  "    x = 30\n"
                  "    let h = phases(\"x\")\n"
                  "    print(len(h))\n"
                  "    print(h[0][\"value\"])\n"
                  "    print(h[2][\"value\"])\n"
                  "}\n",
                  "3\n10\n30");
}

static void test_rewind_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 100\n"
                  "    track(\"x\")\n"
                  "    x = 200\n"
                  "    x = 300\n"
                  "    print(rewind(\"x\", 0))\n"
                  "    print(rewind(\"x\", 1))\n"
                  "    print(rewind(\"x\", 2))\n"
                  "}\n",
                  "300\n200\n100");
}

static void test_track_untracked_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let h = phases(\"nonexistent\")\n"
                  "    print(len(h))\n"
                  "}\n",
                  "0");
}

static void test_track_freeze_thaw(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 1\n"
                  "    track(\"x\")\n"
                  "    freeze(x)\n"
                  "    thaw(x)\n"
                  "    let h = phases(\"x\")\n"
                  "    print(len(h))\n"
                  "    print(h[0][\"phase\"])\n"
                  "    print(h[1][\"phase\"])\n"
                  "    print(h[2][\"phase\"])\n"
                  "}\n",
                  "3\nfluid\ncrystal\nfluid");
}

static void test_rewind_out_of_bounds(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 5\n"
                  "    track(\"x\")\n"
                  "    print(rewind(\"x\", 99))\n"
                  "}\n",
                  "nil");
}

static void test_track_different_types(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 1\n"
                  "    track(\"x\")\n"
                  "    x = \"hello\"\n"
                  "    x = true\n"
                  "    let h = phases(\"x\")\n"
                  "    print(len(h))\n"
                  "    print(h[0][\"value\"])\n"
                  "    print(h[1][\"value\"])\n"
                  "    print(h[2][\"value\"])\n"
                  "}\n",
                  "3\n1\nhello\ntrue");
}

static void test_track_undefined_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    track(\"nonexistent\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_phases_output_format(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 42\n"
                  "    track(\"x\")\n"
                  "    let h = phases(\"x\")\n"
                  "    print(h[0][\"phase\"])\n"
                  "    print(h[0][\"value\"])\n"
                  "}\n",
                  "fluid\n42");
}

/* ======================================================================
 * history() and identifier arguments
 * ====================================================================== */

static void test_history_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 10\n"
                  "    track(\"x\")\n"
                  "    x = 20\n"
                  "    freeze(x)\n"
                  "    let h = history(\"x\")\n"
                  "    print(len(h))\n"
                  "    print(h[0][\"phase\"])\n"
                  "    print(h[0][\"value\"])\n"
                  "    print(h[2][\"phase\"])\n"
                  "    print(h[2][\"value\"])\n"
                  "}\n",
                  "3\nfluid\n10\ncrystal\n20");
}

static void test_history_has_line_and_fn(void) {
    /* Tree-walker sets line=0, fn=nil; StackVM sets real values.
     * This test verifies the keys exist in the map. */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 10\n"
                  "    track(\"x\")\n"
                  "    x = 20\n"
                  "    let h = history(\"x\")\n"
                  "    print(typeof(h[0][\"line\"]))\n"
                  "    print(typeof(h[1][\"line\"]))\n"
                  "    print(h[0][\"phase\"])\n"
                  "}\n",
                  "Int\nInt\nfluid");
}

static void test_history_ident_arg(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 42\n"
                  "    track(x)\n"
                  "    x = 99\n"
                  "    let h = history(x)\n"
                  "    print(len(h))\n"
                  "    print(h[0][\"value\"])\n"
                  "    print(h[1][\"value\"])\n"
                  "}\n",
                  "2\n42\n99");
}

static void test_track_ident_arg(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 5\n"
                  "    track(x)\n"
                  "    x = 10\n"
                  "    let h = phases(x)\n"
                  "    print(len(h))\n"
                  "    print(h[1][\"value\"])\n"
                  "}\n",
                  "2\n10");
}

static void test_history_untracked_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let h = history(\"nonexistent\")\n"
                  "    print(len(h))\n"
                  "}\n",
                  "0");
}

static void test_phases_still_works_with_string(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 1\n"
                  "    track(\"x\")\n"
                  "    x = 2\n"
                  "    let h = phases(\"x\")\n"
                  "    print(len(h))\n"
                  "    print(h[0][\"value\"])\n"
                  "    print(h[1][\"value\"])\n"
                  "}\n",
                  "2\n1\n2");
}

static void test_rewind_ident_arg(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 100\n"
                  "    track(x)\n"
                  "    x = 200\n"
                  "    x = 300\n"
                  "    print(rewind(x, 0))\n"
                  "    print(rewind(x, 1))\n"
                  "    print(rewind(x, 2))\n"
                  "}\n",
                  "300\n200\n100");
}

static void test_phases_has_line_and_fn(void) {
    /* Tree-walker sets line=0, fn=nil; verify keys exist. */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux y = 5\n"
                  "    track(y)\n"
                  "    y = 10\n"
                  "    let h = phases(y)\n"
                  "    print(typeof(h[0][\"line\"]))\n"
                  "    print(typeof(h[0][\"phase\"]))\n"
                  "    print(h[0][\"value\"])\n"
                  "}\n",
                  "Int\nString\n5");
}

/* ======================================================================
 * Annealing
 * ====================================================================== */

static void test_anneal_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 42\n"
                  "    freeze(x)\n"
                  "    anneal(x) |v| { v + 8 }\n"
                  "    print(x)\n"
                  "}\n",
                  "50");
}

static void test_anneal_stays_crystal(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 10\n"
                  "    freeze(x)\n"
                  "    anneal(x) |v| { v * 2 }\n"
                  "    print(phase_of(x))\n"
                  "}\n",
                  "crystal");
}

static void test_anneal_map(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"a\"] = 1\n"
                  "    freeze(m)\n"
                  "    anneal(m) |c| {\n"
                  "        c[\"b\"] = 2\n"
                  "        c\n"
                  "    }\n"
                  "    print(m[\"b\"])\n"
                  "}\n",
                  "2");
}

static void test_anneal_non_crystal_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux x = 5\n"
                              "    anneal(x) |v| { v }\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_anneal_expr_target(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = anneal(freeze(100)) |v| { v + 1 }\n"
                  "    print(r)\n"
                  "}\n",
                  "101");
}

static void test_anneal_closure_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux x = 1\n"
                              "    freeze(x)\n"
                              "    anneal(x) |v| { 1 / 0 }\n"
                              "}\n",
                              "EVAL_ERROR:anneal failed:");
}

/* ======================================================================
 * Partial Crystallization
 * ====================================================================== */

static void test_partial_freeze_struct_field(void) {
    ASSERT_OUTPUT("struct Cfg { host: String, port: Int }\n"
                  "fn main() {\n"
                  "    flux s = Cfg { host: \"localhost\", port: 8080 }\n"
                  "    freeze(s.host)\n"
                  "    s.port = 9090\n"
                  "    print(s.host)\n"
                  "    print(s.port)\n"
                  "}\n",
                  "localhost\n9090");
}

static void test_partial_freeze_struct_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("struct Cfg { host: String, port: Int }\n"
                              "fn main() {\n"
                              "    flux s = Cfg { host: \"localhost\", port: 8080 }\n"
                              "    freeze(s.host)\n"
                              "    s.host = \"other\"\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_partial_freeze_map_key(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"name\"] = \"Alice\"\n"
                  "    m[\"age\"] = 30\n"
                  "    freeze(m[\"name\"])\n"
                  "    m[\"age\"] = 31\n"
                  "    print(m[\"age\"])\n"
                  "}\n",
                  "31");
}

static void test_partial_freeze_map_key_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux m = Map::new()\n"
                              "    m[\"name\"] = \"Alice\"\n"
                              "    freeze(m[\"name\"])\n"
                              "    m[\"name\"] = \"Bob\"\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_partial_freeze_full_overrides(void) {
    /* Freezing entire struct overrides field phases */
    ASSERT_OUTPUT_STARTS_WITH("struct Cfg { host: String, port: Int }\n"
                              "fn main() {\n"
                              "    flux s = Cfg { host: \"localhost\", port: 8080 }\n"
                              "    freeze(s)\n"
                              "    s.port = 9090\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_partial_freeze_contract(void) {
    ASSERT_OUTPUT("struct Cfg { host: String, port: Int }\n"
                  "fn main() {\n"
                  "    flux s = Cfg { host: \"localhost\", port: 8080 }\n"
                  "    freeze(s.host) where |v| {\n"
                  "        if v.len() == 0 { 1 / 0 }\n"
                  "    }\n"
                  "    print(s.host)\n"
                  "}\n",
                  "localhost");
}

/* ======================================================================
 * Phase Patterns in Match
 * ====================================================================== */

static void test_phase_match_crystal_wildcard(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let x = freeze(42)\n"
                  "    let r = match x {\n"
                  "        fluid _ => \"fluid\"\n"
                  "        crystal _ => \"crystal\"\n"
                  "        _ => \"other\"\n"
                  "    }\n"
                  "    print(r)\n"
                  "}\n",
                  "crystal");
}

static void test_phase_match_fluid_wildcard(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 42\n"
                  "    let r = match x {\n"
                  "        crystal _ => \"crystal\"\n"
                  "        fluid _ => \"fluid\"\n"
                  "        _ => \"other\"\n"
                  "    }\n"
                  "    print(r)\n"
                  "}\n",
                  "fluid");
}

static void test_phase_match_literal(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let x = freeze(42)\n"
                  "    let r = match x {\n"
                  "        crystal 42 => \"frozen 42\"\n"
                  "        fluid 42 => \"fluid 42\"\n"
                  "        _ => \"other\"\n"
                  "    }\n"
                  "    print(r)\n"
                  "}\n",
                  "frozen 42");
}

static void test_phase_match_binding(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let x = freeze(99)\n"
                  "    let r = match x {\n"
                  "        crystal n => n + 1\n"
                  "        _ => 0\n"
                  "    }\n"
                  "    print(r)\n"
                  "}\n",
                  "100");
}

static void test_phase_match_no_match(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let x = freeze(5)\n"
                  "    let r = match x {\n"
                  "        fluid _ => \"fluid\"\n"
                  "        _ => \"fallback\"\n"
                  "    }\n"
                  "    print(r)\n"
                  "}\n",
                  "fallback");
}

static void test_phase_match_unqualified_any(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let x = freeze(10)\n"
                  "    let r = match x {\n"
                  "        v => v\n"
                  "    }\n"
                  "    print(r)\n"
                  "}\n",
                  "10");
}

/* ======================================================================
 * lib/test.lat — Test runner library
 * ====================================================================== */

static void test_lib_test_assert_eq_pass(void) {
    ASSERT_OUTPUT("import \"lib/test\" as t\n"
                  "fn main() {\n"
                  "    t.assert_eq(42, 42)\n"
                  "    print(\"ok\")\n"
                  "}\n",
                  "ok");
}

static void test_lib_test_assert_eq_fail(void) {
    ASSERT_OUTPUT_STARTS_WITH("import \"lib/test\" as t\n"
                              "fn main() {\n"
                              "    t.assert_eq(1, 2)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_lib_test_assert_neq(void) {
    ASSERT_OUTPUT("import \"lib/test\" as t\n"
                  "fn main() {\n"
                  "    t.assert_neq(1, 2)\n"
                  "    print(\"ok\")\n"
                  "}\n",
                  "ok");
}

static void test_lib_test_assert_gt(void) {
    ASSERT_OUTPUT("import \"lib/test\" as t\n"
                  "fn main() {\n"
                  "    t.assert_gt(5, 3)\n"
                  "    t.assert_lt(1, 10)\n"
                  "    t.assert_gte(5, 5)\n"
                  "    t.assert_lte(3, 3)\n"
                  "    print(\"ok\")\n"
                  "}\n",
                  "ok");
}

static void test_lib_test_assert_near(void) {
    ASSERT_OUTPUT("import \"lib/test\" as t\n"
                  "fn main() {\n"
                  "    t.assert_near(3.14159, 3.14, 0.01)\n"
                  "    print(\"ok\")\n"
                  "}\n",
                  "ok");
}

static void test_lib_test_assert_contains(void) {
    ASSERT_OUTPUT("import \"lib/test\" as t\n"
                  "fn main() {\n"
                  "    t.assert_contains(\"hello world\", \"world\")\n"
                  "    t.assert_contains([1, 2, 3], 2)\n"
                  "    print(\"ok\")\n"
                  "}\n",
                  "ok");
}

static void test_lib_test_assert_throws(void) {
    ASSERT_OUTPUT("import \"lib/test\" as t\n"
                  "fn main() {\n"
                  "    t.assert_throws(|_| { 1 / 0 })\n"
                  "    print(\"ok\")\n"
                  "}\n",
                  "ok");
}

static void test_lib_test_assert_type(void) {
    ASSERT_OUTPUT("import \"lib/test\" as t\n"
                  "fn main() {\n"
                  "    t.assert_type(42, \"Int\")\n"
                  "    t.assert_type(\"hi\", \"String\")\n"
                  "    t.assert_type(true, \"Bool\")\n"
                  "    print(\"ok\")\n"
                  "}\n",
                  "ok");
}

static void test_lib_test_assert_nil(void) {
    ASSERT_OUTPUT("import \"lib/test\" as t\n"
                  "fn main() {\n"
                  "    t.assert_nil(nil)\n"
                  "    t.assert_not_nil(42)\n"
                  "    t.assert_true(true)\n"
                  "    t.assert_false(false)\n"
                  "    print(\"ok\")\n"
                  "}\n",
                  "ok");
}

static void test_lib_test_describe_it(void) {
    ASSERT_OUTPUT("import \"lib/test\" as t\n"
                  "fn main() {\n"
                  "    let tc = t.it(\"my test\", |_| { 1 + 1 })\n"
                  "    print(tc.get(\"name\"))\n"
                  "}\n",
                  "my test");
}

static void test_lib_test_describe_suite(void) {
    ASSERT_OUTPUT("import \"lib/test\" as t\n"
                  "fn main() {\n"
                  "    let suite = t.describe(\"Math\", |_| {\n"
                  "        return [t.it(\"add\", |_| { t.assert_eq(1+1, 2) })]\n"
                  "    })\n"
                  "    print(suite.get(\"name\"))\n"
                  "    print(len(suite.get(\"tests\")))\n"
                  "}\n",
                  "Math\n1");
}

static void test_lib_test_run_pass(void) {
    /* Check that run() prints pass results */
    char *out = run_capture("import \"lib/test\" as t\n"
                            "fn main() {\n"
                            "    t.run([\n"
                            "        t.describe(\"Suite\", |_| {\n"
                            "            return [t.it(\"passes\", |_| { t.assert_eq(1, 1) })]\n"
                            "        })\n"
                            "    ])\n"
                            "}\n");
    ASSERT(strstr(out, "1 passed") != NULL);
    ASSERT(strstr(out, "0 failed") != NULL);
    free(out);
}

static void test_lib_test_run_fail(void) {
    /* Check that run() reports failures without crashing */
    char *out = run_capture("import \"lib/test\" as t\n"
                            "fn main() {\n"
                            "    t.run([\n"
                            "        t.describe(\"Suite\", |_| {\n"
                            "            return [t.it(\"fails\", |_| { t.assert_eq(1, 2) })]\n"
                            "        })\n"
                            "    ])\n"
                            "}\n");
    ASSERT(strstr(out, "1 failed") != NULL);
    free(out);
}

/* ======================================================================
 * lib/dotenv.lat — Dotenv library
 * ====================================================================== */

static void test_lib_dotenv_parse_basic(void) {
    ASSERT_OUTPUT("import \"lib/dotenv\" as dotenv\n"
                  "fn main() {\n"
                  "    let vars = dotenv.parse_string(\"DB_HOST=localhost\\nDB_PORT=5432\")\n"
                  "    print(vars.get(\"DB_HOST\"))\n"
                  "    print(vars.get(\"DB_PORT\"))\n"
                  "}\n",
                  "localhost\n5432");
}

static void test_lib_dotenv_parse_double_quoted(void) {
    ASSERT_OUTPUT("import \"lib/dotenv\" as dotenv\n"
                  "fn main() {\n"
                  "    let vars = dotenv.parse_string(\"KEY=\\\"hello world\\\"\")\n"
                  "    print(vars.get(\"KEY\"))\n"
                  "}\n",
                  "hello world");
}

static void test_lib_dotenv_parse_single_quoted(void) {
    ASSERT_OUTPUT("import \"lib/dotenv\" as dotenv\n"
                  "fn main() {\n"
                  "    let vars = dotenv.parse_string(\"KEY='literal $VAR'\")\n"
                  "    print(vars.get(\"KEY\"))\n"
                  "}\n",
                  "literal $VAR");
}

static void test_lib_dotenv_parse_comments(void) {
    ASSERT_OUTPUT("import \"lib/dotenv\" as dotenv\n"
                  "fn main() {\n"
                  "    let vars = dotenv.parse_string(\"# comment\\nKEY=value\")\n"
                  "    print(vars.get(\"KEY\"))\n"
                  "}\n",
                  "value");
}

static void test_lib_dotenv_parse_inline_comment(void) {
    ASSERT_OUTPUT("import \"lib/dotenv\" as dotenv\n"
                  "fn main() {\n"
                  "    let vars = dotenv.parse_string(\"DEBUG=true #enable\")\n"
                  "    print(vars.get(\"DEBUG\"))\n"
                  "}\n",
                  "true");
}

static void test_lib_dotenv_parse_export(void) {
    ASSERT_OUTPUT("import \"lib/dotenv\" as dotenv\n"
                  "fn main() {\n"
                  "    let vars = dotenv.parse_string(\"export SECRET=abc123\")\n"
                  "    print(vars.get(\"SECRET\"))\n"
                  "}\n",
                  "abc123");
}

static void test_lib_dotenv_parse_variable_expansion(void) {
    ASSERT_OUTPUT("import \"lib/dotenv\" as dotenv\n"
                  "fn main() {\n"
                  "    let content = 'HOST=localhost\\nURL=\"http://${HOST}/api\"'\n"
                  "    let vars = dotenv.parse_string(content)\n"
                  "    print(vars.get(\"URL\"))\n"
                  "}\n",
                  "http://localhost/api");
}

static void test_lib_dotenv_parse_escape_sequences(void) {
    ASSERT_OUTPUT("import \"lib/dotenv\" as dotenv\n"
                  "fn main() {\n"
                  "    let vars = dotenv.parse_string(\"KEY=\\\"line1\\\\nline2\\\"\")\n"
                  "    print(vars.get(\"KEY\"))\n"
                  "}\n",
                  "line1\nline2");
}

static void test_lib_dotenv_parse_whitespace(void) {
    ASSERT_OUTPUT("import \"lib/dotenv\" as dotenv\n"
                  "fn main() {\n"
                  "    let vars = dotenv.parse_string(\"KEY = value\")\n"
                  "    print(vars.get(\"KEY\"))\n"
                  "}\n",
                  "value");
}

static void test_lib_dotenv_parse_empty(void) {
    ASSERT_OUTPUT("import \"lib/dotenv\" as dotenv\n"
                  "fn main() {\n"
                  "    let vars = dotenv.parse_string(\"\")\n"
                  "    print(len(vars.keys()))\n"
                  "}\n",
                  "0");
}

/* ======================================================================
 * lib/validate.lat — Validation library
 * ====================================================================== */

static void test_lib_validate_string_valid(void) {
    ASSERT_OUTPUT("import \"lib/validate\" as v\n"
                  "fn main() {\n"
                  "    let s = v.string()\n"
                  "    print(v.is_valid(s, \"hello\"))\n"
                  "}\n",
                  "true");
}

static void test_lib_validate_string_invalid(void) {
    ASSERT_OUTPUT("import \"lib/validate\" as v\n"
                  "fn main() {\n"
                  "    let s = v.string()\n"
                  "    print(v.is_valid(s, 42))\n"
                  "}\n",
                  "false");
}

static void test_lib_validate_number_valid(void) {
    ASSERT_OUTPUT("import \"lib/validate\" as v\n"
                  "fn main() {\n"
                  "    let n = v.number()\n"
                  "    print(v.is_valid(n, 42))\n"
                  "    print(v.is_valid(n, 3.14))\n"
                  "}\n",
                  "true\ntrue");
}

static void test_lib_validate_number_min_max(void) {
    ASSERT_OUTPUT("import \"lib/validate\" as v\n"
                  "fn main() {\n"
                  "    let n = v.max(v.min(v.number(), 0), 100)\n"
                  "    print(v.is_valid(n, 50))\n"
                  "    print(v.is_valid(n, -1))\n"
                  "    print(v.is_valid(n, 101))\n"
                  "}\n",
                  "true\nfalse\nfalse");
}

static void test_lib_validate_string_min_max_len(void) {
    ASSERT_OUTPUT("import \"lib/validate\" as v\n"
                  "fn main() {\n"
                  "    let s = v.max_len(v.min_len(v.string(), 2), 5)\n"
                  "    print(v.is_valid(s, \"abc\"))\n"
                  "    print(v.is_valid(s, \"a\"))\n"
                  "    print(v.is_valid(s, \"abcdef\"))\n"
                  "}\n",
                  "true\nfalse\nfalse");
}

static void test_lib_validate_boolean(void) {
    ASSERT_OUTPUT("import \"lib/validate\" as v\n"
                  "fn main() {\n"
                  "    let b = v.boolean()\n"
                  "    print(v.is_valid(b, true))\n"
                  "    print(v.is_valid(b, \"yes\"))\n"
                  "}\n",
                  "true\nfalse");
}

static void test_lib_validate_optional(void) {
    ASSERT_OUTPUT("import \"lib/validate\" as v\n"
                  "fn main() {\n"
                  "    let s = v.opt(v.string())\n"
                  "    print(v.is_valid(s, nil))\n"
                  "    print(v.is_valid(s, \"hello\"))\n"
                  "}\n",
                  "true\ntrue");
}

static void test_lib_validate_required_nil(void) {
    ASSERT_OUTPUT("import \"lib/validate\" as v\n"
                  "fn main() {\n"
                  "    let s = v.string()\n"
                  "    let r = v.check(s, nil)\n"
                  "    print(r.get(\"valid\"))\n"
                  "    print(r.get(\"errors\")[0])\n"
                  "}\n",
                  "false\nvalue: is required");
}

static void test_lib_validate_one_of(void) {
    ASSERT_OUTPUT("import \"lib/validate\" as v\n"
                  "fn main() {\n"
                  "    let s = v.one_of(v.string(), [\"admin\", \"user\"])\n"
                  "    print(v.is_valid(s, \"admin\"))\n"
                  "    print(v.is_valid(s, \"root\"))\n"
                  "}\n",
                  "true\nfalse");
}

static void test_lib_validate_array(void) {
    ASSERT_OUTPUT("import \"lib/validate\" as v\n"
                  "fn main() {\n"
                  "    let a = v.array(v.number())\n"
                  "    print(v.is_valid(a, [1, 2, 3]))\n"
                  "    print(v.is_valid(a, \"not array\"))\n"
                  "}\n",
                  "true\nfalse");
}

static void test_lib_validate_array_item_errors(void) {
    ASSERT_OUTPUT("import \"lib/validate\" as v\n"
                  "fn main() {\n"
                  "    let a = v.array(v.number())\n"
                  "    let r = v.check(a, [1, \"bad\", 3])\n"
                  "    print(r.get(\"valid\"))\n"
                  "    print(r.get(\"errors\")[0])\n"
                  "}\n",
                  "false\nvalue[1]: expected number, got String");
}

static void test_lib_validate_object(void) {
    ASSERT_OUTPUT("import \"lib/validate\" as v\n"
                  "fn main() {\n"
                  "    flux fields = Map::new()\n"
                  "    fields.set(\"name\", v.string())\n"
                  "    fields.set(\"age\", v.number())\n"
                  "    let schema = v.object(fields)\n"
                  "    flux data = Map::new()\n"
                  "    data.set(\"name\", \"Alice\")\n"
                  "    data.set(\"age\", 30)\n"
                  "    print(v.is_valid(schema, data))\n"
                  "}\n",
                  "true");
}

static void test_lib_validate_object_errors(void) {
    ASSERT_OUTPUT("import \"lib/validate\" as v\n"
                  "fn main() {\n"
                  "    flux fields = Map::new()\n"
                  "    fields.set(\"name\", v.string())\n"
                  "    fields.set(\"age\", v.number())\n"
                  "    let schema = v.object(fields)\n"
                  "    flux data = Map::new()\n"
                  "    data.set(\"name\", 123)\n"
                  "    let r = v.check(schema, data)\n"
                  "    print(r.get(\"valid\"))\n"
                  "}\n",
                  "false");
}

static void test_lib_validate_default_val(void) {
    ASSERT_OUTPUT("import \"lib/validate\" as v\n"
                  "fn main() {\n"
                  "    flux fields = Map::new()\n"
                  "    fields.set(\"role\", v.default_val(v.string(), \"user\"))\n"
                  "    let schema = v.object(fields)\n"
                  "    let data = Map::new()\n"
                  "    let filled = v.apply_defaults(schema, data)\n"
                  "    print(filled.get(\"role\"))\n"
                  "}\n",
                  "user");
}

static void test_lib_validate_integer(void) {
    ASSERT_OUTPUT("import \"lib/validate\" as v\n"
                  "fn main() {\n"
                  "    let n = v.integer(v.number())\n"
                  "    print(v.is_valid(n, 42))\n"
                  "    print(v.is_valid(n, 3.14))\n"
                  "}\n",
                  "true\nfalse");
}

/* ======================================================================
 * lib/fn.lat — Functional collections library
 * ====================================================================== */

static void test_lib_fn_range(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let r = F.collect(F.range(0, 5))\n"
                  "    print(r)\n"
                  "}\n",
                  "[0, 1, 2, 3, 4]");
}

static void test_lib_fn_range_step(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let r = F.collect(F.range(0, 10, 3))\n"
                  "    print(r)\n"
                  "}\n",
                  "[0, 3, 6, 9]");
}

static void test_lib_fn_from_array(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let r = F.collect(F.from_array([10, 20, 30]))\n"
                  "    print(r)\n"
                  "}\n",
                  "[10, 20, 30]");
}

static void test_lib_fn_fmap(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let r = F.collect(F.fmap(F.range(1, 4), |x| { x * x }))\n"
                  "    print(r)\n"
                  "}\n",
                  "[1, 4, 9]");
}

static void test_lib_fn_select(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let r = F.collect(F.select(F.range(1, 10), |x| { x % 2 == 0 }))\n"
                  "    print(r)\n"
                  "}\n",
                  "[2, 4, 6, 8]");
}

static void test_lib_fn_take(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let r = F.collect(F.take(F.range(0, 100), 3))\n"
                  "    print(r)\n"
                  "}\n",
                  "[0, 1, 2]");
}

static void test_lib_fn_drop(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let r = F.collect(F.drop(F.range(0, 5), 3))\n"
                  "    print(r)\n"
                  "}\n",
                  "[3, 4]");
}

static void test_lib_fn_take_while(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let r = F.collect(F.take_while(F.range(1, 100), |x| { x < 5 }))\n"
                  "    print(r)\n"
                  "}\n",
                  "[1, 2, 3, 4]");
}

static void test_lib_fn_zip(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let r = F.collect(F.zip(F.range(1, 4), F.from_array([\"a\", \"b\", \"c\"])))\n"
                  "    print(r)\n"
                  "}\n",
                  "[[1, a], [2, b], [3, c]]");
}

static void test_lib_fn_fold(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let sum = F.fold(F.range(1, 6), 0, |acc, x| { acc + x })\n"
                  "    print(sum)\n"
                  "}\n",
                  "15");
}

static void test_lib_fn_count(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    print(F.count(F.range(0, 100)))\n"
                  "}\n",
                  "100");
}

static void test_lib_fn_repeat_take(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let r = F.collect(F.take(F.repeat(7), 4))\n"
                  "    print(r)\n"
                  "}\n",
                  "[7, 7, 7, 7]");
}

static void test_lib_fn_iterate(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let r = F.collect(F.take(F.iterate(1, |x| { x * 2 }), 5))\n"
                  "    print(r)\n"
                  "}\n",
                  "[1, 2, 4, 8, 16]");
}

static void test_lib_fn_result_ok(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let r = F.ok(42)\n"
                  "    print(F.is_ok(r))\n"
                  "    print(F.unwrap(r))\n"
                  "}\n",
                  "true\n42");
}

static void test_lib_fn_result_err(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let r = F.err(\"oops\")\n"
                  "    print(F.is_err(r))\n"
                  "    print(F.unwrap_or(r, 0))\n"
                  "}\n",
                  "true\n0");
}

static void test_lib_fn_try_fn(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let r1 = F.try_fn(|_| { 42 })\n"
                  "    let r2 = F.try_fn(|_| { 1 / 0 })\n"
                  "    print(F.is_ok(r1))\n"
                  "    print(F.is_err(r2))\n"
                  "}\n",
                  "true\ntrue");
}

static void test_lib_fn_map_result(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let r = F.map_result(F.ok(5), |x| { x * 2 })\n"
                  "    print(F.unwrap(r))\n"
                  "    let e = F.map_result(F.err(\"e\"), |x| { x * 2 })\n"
                  "    print(F.is_err(e))\n"
                  "}\n",
                  "10\ntrue");
}

static void test_lib_fn_partial(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let add = |a, b| { a + b }\n"
                  "    let add5 = F.partial(add, 5)\n"
                  "    print(add5(3))\n"
                  "}\n",
                  "8");
}

static void test_lib_fn_comp(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let double = |x| { x * 2 }\n"
                  "    let inc = |x| { x + 1 }\n"
                  "    let double_then_inc = F.comp(inc, double)\n"
                  "    print(double_then_inc(3))\n"
                  "}\n",
                  "7");
}

static void test_lib_fn_apply_n(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    print(F.apply_n(|x| { x * 2 }, 4, 1))\n"
                  "}\n",
                  "16");
}

static void test_lib_fn_flip(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let div = |a, b| { a / b }\n"
                  "    let inv_div = F.flip(div)\n"
                  "    print(inv_div(2, 10))\n"
                  "}\n",
                  "5");
}

static void test_lib_fn_constant(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let f = F.constant(42)\n"
                  "    print(f(99))\n"
                  "    print(f(\"anything\"))\n"
                  "}\n",
                  "42\n42");
}

static void test_lib_fn_group_by(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let g = F.group_by([1,2,3,4,5,6], |x| { if x % 2 == 0 { \"even\" } else { \"odd\" } })\n"
                  "    print(g.get(\"even\"))\n"
                  "    print(g.get(\"odd\"))\n"
                  "}\n",
                  "[2, 4, 6]\n[1, 3, 5]");
}

static void test_lib_fn_partition(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let p = F.partition([1,2,3,4,5], |x| { x > 3 })\n"
                  "    print(p[0])\n"
                  "    print(p[1])\n"
                  "}\n",
                  "[4, 5]\n[1, 2, 3]");
}

static void test_lib_fn_frequencies(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let f = F.frequencies([\"a\", \"b\", \"a\", \"c\", \"b\", \"a\"])\n"
                  "    print(f.get(\"a\"))\n"
                  "    print(f.get(\"b\"))\n"
                  "    print(f.get(\"c\"))\n"
                  "}\n",
                  "3\n2\n1");
}

static void test_lib_fn_chunk(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    print(F.chunk([1,2,3,4,5], 2))\n"
                  "}\n",
                  "[[1, 2], [3, 4], [5]]");
}

static void test_lib_fn_flatten(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    print(F.flatten([[1,2], [3], [4,5]]))\n"
                  "}\n",
                  "[1, 2, 3, 4, 5]");
}

static void test_lib_fn_uniq_by(void) {
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    print(F.uniq_by([1,2,3,4,5], |x| { x % 3 }))\n"
                  "}\n",
                  "[1, 2, 3]");
}

static void test_lib_fn_chain(void) {
    /* Test chaining: range -> select -> fmap -> collect */
    ASSERT_OUTPUT("import \"lib/fn\" as F\n"
                  "fn main() {\n"
                  "    let r = F.collect(\n"
                  "        F.fmap(\n"
                  "            F.select(F.range(1, 20), |x| { x % 3 == 0 }),\n"
                  "            |x| { x * x }\n"
                  "        )\n"
                  "    )\n"
                  "    print(r)\n"
                  "}\n",
                  "[9, 36, 81, 144, 225, 324]");
}

/* ── Feature 1: crystallize block ── */

static void test_crystallize_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux data = [1, 2, 3]\n"
                  "    crystallize(data) {\n"
                  "        print(phase_of(data))\n"
                  "    }\n"
                  "    print(phase_of(data))\n"
                  "    data.push(4)\n"
                  "    print(data.len())\n"
                  "}\n",
                  "crystal\nfluid\n4");
}

static void test_crystallize_already_crystal(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    fix data = [1, 2, 3]\n"
                  "    crystallize(data) {\n"
                  "        print(phase_of(data))\n"
                  "    }\n"
                  "    print(phase_of(data))\n"
                  "}\n",
                  "crystal\ncrystal");
}

static void test_crystallize_nested(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = [1]\n"
                  "    flux b = [2]\n"
                  "    crystallize(a) {\n"
                  "        crystallize(b) {\n"
                  "            print(phase_of(a))\n"
                  "            print(phase_of(b))\n"
                  "        }\n"
                  "        print(phase_of(b))\n"
                  "    }\n"
                  "    print(phase_of(a))\n"
                  "}\n",
                  "crystal\ncrystal\nfluid\nfluid");
}

/* ── Phase borrowing (borrow) ── */

static void test_borrow_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let data = freeze([1, 2, 3])\n"
                  "    borrow(data) {\n"
                  "        data.push(4)\n"
                  "        print(phase_of(data))\n"
                  "    }\n"
                  "    print(phase_of(data))\n"
                  "    print(data.len())\n"
                  "}\n",
                  "fluid\ncrystal\n4");
}

static void test_borrow_already_fluid(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux data = [1, 2, 3]\n"
                  "    borrow(data) {\n"
                  "        data.push(4)\n"
                  "        print(phase_of(data))\n"
                  "    }\n"
                  "    print(phase_of(data))\n"
                  "    print(data.len())\n"
                  "}\n",
                  "fluid\nfluid\n4");
}

static void test_borrow_nested(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    fix a = freeze([1])\n"
                  "    fix b = freeze([2])\n"
                  "    borrow(a) {\n"
                  "        borrow(b) {\n"
                  "            print(phase_of(a))\n"
                  "            print(phase_of(b))\n"
                  "        }\n"
                  "        print(phase_of(b))\n"
                  "    }\n"
                  "    print(phase_of(a))\n"
                  "}\n",
                  "fluid\nfluid\ncrystal\ncrystal");
}

static void test_borrow_mutation_persists(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let data = freeze([1, 2, 3])\n"
                  "    borrow(data) {\n"
                  "        data.push(4)\n"
                  "        data.push(5)\n"
                  "    }\n"
                  "    print(data.len())\n"
                  "    print(phase_of(data))\n"
                  "}\n",
                  "5\ncrystal");
}

/* ── @fluid/@crystal annotations ── */

static void test_annotation_crystal_binding(void) {
    ASSERT_OUTPUT("@crystal\n"
                  "fix config = freeze([1, 2, 3])\n"
                  "fn main() {\n"
                  "    print(phase_of(config))\n"
                  "}\n",
                  "crystal");
}

static void test_annotation_fluid_fn(void) {
    ASSERT_OUTPUT("@fluid\n"
                  "fn process(data: any) {\n"
                  "    print(data)\n"
                  "}\n"
                  "fn main() {\n"
                  "    process(42)\n"
                  "}\n",
                  "42");
}

static void test_annotation_parse_error(void) {
    ASSERT_OUTPUT_STARTS_WITH("@invalid\n"
                              "let x = 1\n"
                              "fn main() {}\n",
                              "PARSE_ERROR:");
}

/* ── Composite phase constraints ── */

static void test_composite_fluid_or_crystal(void) {
    ASSERT_OUTPUT("fn process(data: (~|*) any) {\n"
                  "    print(phase_of(data))\n"
                  "}\n"
                  "fn main() {\n"
                  "    flux a = [1, 2, 3]\n"
                  "    fix b = freeze([4, 5, 6])\n"
                  "    process(a)\n"
                  "    process(b)\n"
                  "}\n",
                  "fluid\ncrystal");
}

/* ── Feature 2: freeze except (defects) ── */

static void test_freeze_except_struct(void) {
    ASSERT_OUTPUT("struct User { name: String, age: Int, score: Int }\n"
                  "fn main() {\n"
                  "    flux u = User { name: \"Alice\", age: 30, score: 0 }\n"
                  "    freeze(u) except [\"score\"]\n"
                  "    u.score = 100\n"
                  "    print(u.score)\n"
                  "}\n",
                  "100");
}

static void test_freeze_except_struct_blocks_frozen_field(void) {
    ASSERT_OUTPUT_STARTS_WITH("struct User { name: String, age: Int, score: Int }\n"
                              "fn main() {\n"
                              "    flux u = User { name: \"Alice\", age: 30, score: 0 }\n"
                              "    freeze(u) except [\"score\"]\n"
                              "    u.name = \"Bob\"\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_freeze_except_map(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"host\"] = \"localhost\"\n"
                  "    m[\"port\"] = 8080\n"
                  "    m[\"retries\"] = 0\n"
                  "    freeze(m) except [\"retries\"]\n"
                  "    m[\"retries\"] = 5\n"
                  "    print(m[\"retries\"])\n"
                  "}\n",
                  "5");
}

/* ── Feature 3: seed / grow ── */

static void test_seed_grow_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux config = Map::new()\n"
                  "    config[\"host\"] = \"localhost\"\n"
                  "    config[\"port\"] = 8080\n"
                  "    seed(config, |v| { v[\"port\"] > 0 })\n"
                  "    config[\"port\"] = 3000\n"
                  "    grow(\"config\")\n"
                  "    print(phase_of(config))\n"
                  "}\n",
                  "crystal");
}

static void test_seed_contract_fail(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux val = 0\n"
                              "    seed(val, |v| { v > 10 })\n"
                              "    grow(\"val\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_unseed(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 42\n"
                  "    seed(x, |v| { v > 100 })\n"
                  "    unseed(x)\n"
                  "    freeze(x)\n"
                  "    print(phase_of(x))\n"
                  "}\n",
                  "crystal");
}

static void test_seed_validates_on_freeze(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux x = 5\n"
                              "    seed(x, |v| { v > 10 })\n"
                              "    freeze(x)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* ── Feature 4: sublimation ── */

static void test_sublimate_array_no_push(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux items = [1, 2, 3]\n"
                              "    sublimate(items)\n"
                              "    items.push(4)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_sublimate_map_no_set(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux m = Map::new()\n"
                              "    m[\"a\"] = 1\n"
                              "    m[\"b\"] = 2\n"
                              "    sublimate(m)\n"
                              "    m[\"c\"] = 3\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_sublimate_thaw_restores(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux items = [1, 2, 3]\n"
                  "    sublimate(items)\n"
                  "    thaw(items)\n"
                  "    items.push(4)\n"
                  "    print(items.len())\n"
                  "}\n",
                  "4");
}

static void test_sublimate_phase_of(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = [1, 2]\n"
                  "    sublimate(x)\n"
                  "    print(phase_of(x))\n"
                  "}\n",
                  "sublimated");
}

static void test_sublimate_array_no_pop(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux items = [1, 2, 3]\n"
                              "    sublimate(items)\n"
                              "    items.pop()\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* ── Feature 5: phase pressure ── */

static void test_pressure_no_grow_blocks_push(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux data = [1, 2, 3]\n"
                              "    pressurize(data, \"no_grow\")\n"
                              "    data.push(4)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_pressure_no_grow_allows_index_assign(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux data = [1, 2, 3]\n"
                  "    pressurize(data, \"no_grow\")\n"
                  "    data[0] = 10\n"
                  "    print(data[0])\n"
                  "}\n",
                  "10");
}

static void test_pressure_no_shrink_blocks_pop(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux data = [1, 2, 3]\n"
                              "    pressurize(data, \"no_shrink\")\n"
                              "    data.pop()\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_pressure_no_resize_blocks_both(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux data = [1, 2, 3]\n"
                              "    pressurize(data, \"no_resize\")\n"
                              "    data.push(4)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_depressurize(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux data = [1, 2, 3]\n"
                  "    pressurize(data, \"no_grow\")\n"
                  "    depressurize(data)\n"
                  "    data.push(4)\n"
                  "    print(data.len())\n"
                  "}\n",
                  "4");
}

static void test_pressure_of_returns_mode(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux data = [1, 2, 3]\n"
                  "    pressurize(data, \"no_grow\")\n"
                  "    print(pressure_of(\"data\"))\n"
                  "}\n",
                  "no_grow");
}

static void test_pressure_of_returns_nil(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux data = [1, 2, 3]\n"
                  "    print(pressure_of(\"data\"))\n"
                  "}\n",
                  "nil");
}

/* ── Feature 6: phase interference (bond strategies) ── */

static void test_bond_mirror_default(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = 1\n"
                  "    flux b = 2\n"
                  "    bond(a, b)\n"
                  "    freeze(a)\n"
                  "    print(phase_of(b))\n"
                  "}\n",
                  "crystal");
}

static void test_bond_inverse(void) {
    /* When a freezes with inverse bond, b should thaw */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = 1\n"
                  "    flux b = 2\n"
                  "    bond(a, b, \"inverse\")\n"
                  "    freeze(b)\n"
                  "    freeze(a)\n"
                  "    print(phase_of(b))\n"
                  "}\n",
                  "fluid");
}

static void test_bond_gate_fails_when_dep_not_crystal(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux a = 1\n"
                              "    flux b = 2\n"
                              "    bond(a, b, \"gate\")\n"
                              "    freeze(a)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_bond_gate_succeeds_when_dep_crystal(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = 1\n"
                  "    flux b = 2\n"
                  "    bond(a, b, \"gate\")\n"
                  "    freeze(b)\n"
                  "    freeze(a)\n"
                  "    print(phase_of(a))\n"
                  "}\n",
                  "crystal");
}

static void test_bond_multiple_deps_strategy(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 10\n"
                  "    flux y = 20\n"
                  "    flux z = 30\n"
                  "    bond(x, y, z)\n"
                  "    freeze(x)\n"
                  "    print(phase_of(y))\n"
                  "    print(phase_of(z))\n"
                  "}\n",
                  "crystal\ncrystal");
}

/* ── Feature 7: alloys (struct field phase declarations) ── */

static void test_alloy_fix_field_rejects_mutation(void) {
    ASSERT_OUTPUT_STARTS_WITH("struct Config {\n"
                              "    host: fix String,\n"
                              "    retries: flux Int,\n"
                              "}\n"
                              "fn main() {\n"
                              "    let cfg = Config { host: \"localhost\", retries: 0 }\n"
                              "    cfg.host = \"other\"\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_alloy_flux_field_allows_mutation(void) {
    ASSERT_OUTPUT("struct Config {\n"
                  "    host: fix String,\n"
                  "    retries: flux Int,\n"
                  "}\n"
                  "fn main() {\n"
                  "    let cfg = Config { host: \"localhost\", retries: 0 }\n"
                  "    cfg.retries = 5\n"
                  "    print(cfg.retries)\n"
                  "}\n",
                  "5");
}

static void test_alloy_mixed_phases(void) {
    ASSERT_OUTPUT("struct Settings {\n"
                  "    name: fix String,\n"
                  "    count: flux Int,\n"
                  "}\n"
                  "fn main() {\n"
                  "    let s = Settings { name: \"test\", count: 0 }\n"
                  "    s.count = 42\n"
                  "    print(s.name)\n"
                  "    print(s.count)\n"
                  "}\n",
                  "test\n42");
}

/* ======================================================================
 * Ref Type Tests
 * ====================================================================== */

static void test_ref_new(void) {
    ASSERT_OUTPUT("let r = Ref::new(42)\n"
                  "print(typeof(r))\n",
                  "Ref");
}

static void test_ref_get_set(void) {
    ASSERT_OUTPUT("let r = Ref::new(10)\n"
                  "print(r.get())\n"
                  "r.set(20)\n"
                  "print(r.get())\n",
                  "10\n20");
}

static void test_ref_shared(void) {
    ASSERT_OUTPUT("let r = Ref::new(1)\n"
                  "let f = |x| { x.set(99) }\n"
                  "f(r)\n"
                  "print(r.get())\n",
                  "99");
}

static void test_ref_shared_map(void) {
    ASSERT_OUTPUT("let r = Ref::new(Map::new())\n"
                  "let add = |t, k, v| { t[k] = v }\n"
                  "add(r, \"x\", 42)\n"
                  "print(r[\"x\"])\n",
                  "42");
}

static void test_ref_map_proxy_index(void) {
    ASSERT_OUTPUT("let r = Ref::new(Map::new())\n"
                  "r[\"a\"] = 1\n"
                  "r[\"b\"] = 2\n"
                  "print(r[\"a\"])\n"
                  "print(r[\"b\"])\n",
                  "1\n2");
}

static void test_ref_map_proxy_methods(void) {
    ASSERT_OUTPUT("let r = Ref::new(Map::new())\n"
                  "r[\"x\"] = 10\n"
                  "r[\"y\"] = 20\n"
                  "print(r.len())\n"
                  "print(r.has(\"x\"))\n"
                  "print(r.has(\"z\"))\n",
                  "2\ntrue\nfalse");
}

static void test_ref_array_proxy(void) {
    ASSERT_OUTPUT("let r = Ref::new([1, 2, 3])\n"
                  "r.push(4)\n"
                  "print(r.len())\n"
                  "print(r[0])\n"
                  "r[0] = 99\n"
                  "print(r[0])\n"
                  "print(r.pop())\n"
                  "print(r.len())\n",
                  "4\n1\n99\n4\n3");
}

static void test_ref_equality(void) {
    ASSERT_OUTPUT("let a = Ref::new(42)\n"
                  "let b = a\n"
                  "let c = Ref::new(42)\n"
                  "print(a == b)\n"
                  "print(a == c)\n",
                  "true\nfalse");
}

static void test_ref_deref(void) {
    ASSERT_OUTPUT("let r = Ref::new(\"hello\")\n"
                  "print(r.deref())\n",
                  "hello");
}

static void test_ref_inner_type(void) {
    ASSERT_OUTPUT("let r = Ref::new(Map::new())\n"
                  "print(r.inner_type())\n"
                  "let r2 = Ref::new(42)\n"
                  "print(r2.inner_type())\n",
                  "Map\nInt");
}

static void test_ref_freeze(void) {
    ASSERT_OUTPUT_STARTS_WITH("let r = freeze(Ref::new(Map::new()))\n"
                              "r[\"x\"] = 1\n",
                              "EVAL_ERROR:");
}

static void test_ref_display(void) {
    ASSERT_OUTPUT("let r = Ref::new(Map::new())\n"
                  "print(r)\n"
                  "let r2 = Ref::new(42)\n"
                  "print(r2)\n",
                  "Ref<Map>\nRef<Int>");
}

/* ======================================================================
 * Bytecode Serialization (.latc)
 * ====================================================================== */

/* Helper: compile source to a Chunk, round-trip through serialize/deserialize,
 * then run the deserialized chunk and capture stdout. */
static char *latc_round_trip_capture(const char *source) {
    /* Compile */
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        free(lex_err);
        lat_vec_free(&tokens);
        return strdup("LEX_ERROR");
    }
    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    if (parse_err) {
        free(parse_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return strdup("PARSE_ERROR");
    }

    value_set_heap(NULL);
    value_set_arena(NULL);

    char *comp_err = NULL;
    Chunk *chunk = stack_compile(&prog, &comp_err);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    if (!chunk) {
        free(comp_err);
        return strdup("COMPILE_ERROR");
    }

    /* Serialize → Deserialize */
    size_t buf_len;
    uint8_t *buf = chunk_serialize(chunk, &buf_len);
    chunk_free(chunk);

    char *deser_err = NULL;
    Chunk *loaded = chunk_deserialize(buf, buf_len, &deser_err);
    free(buf);
    if (!loaded) {
        size_t msglen = strlen(deser_err) + 20;
        char *msg = malloc(msglen);
        snprintf(msg, msglen, "DESER_ERROR:%s", deser_err);
        free(deser_err);
        return msg;
    }

    /* Run the deserialized chunk, capturing stdout */
    fflush(stdout);
    FILE *tmp = tmpfile();
    int old_stdout = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    LatRuntime rt;
    lat_runtime_init(&rt);
    StackVM vm;
    stackvm_init(&vm, &rt);
    LatValue result;
    StackVMResult vm_res = stackvm_run(&vm, loaded, &result);

    fflush(stdout);
    dup2(old_stdout, fileno(stdout));
    close(old_stdout);

    fseek(tmp, 0, SEEK_END);
    long len = ftell(tmp);
    fseek(tmp, 0, SEEK_SET);
    char *output = malloc((size_t)len + 1);
    size_t n = fread(output, 1, (size_t)len, tmp);
    output[n] = '\0';
    fclose(tmp);
    if (n > 0 && output[n - 1] == '\n') output[n - 1] = '\0';

    if (vm_res != STACKVM_OK) {
        free(output);
        size_t elen = strlen(vm.error) + 16;
        output = malloc(elen);
        snprintf(output, elen, "VM_ERROR:%s", vm.error);
    } else {
        value_free(&result);
    }
    stackvm_free(&vm);
    lat_runtime_free(&rt);
    chunk_free(loaded);
    return output;
}

static void test_latc_round_trip_int(void) {
    char *out = latc_round_trip_capture("fn main() { print(42) }");
    ASSERT_STR_EQ(out, "42");
    free(out);
}

static void test_latc_round_trip_string(void) {
    char *out = latc_round_trip_capture("fn main() { print(\"hello latc\") }");
    ASSERT_STR_EQ(out, "hello latc");
    free(out);
}

static void test_latc_round_trip_closure(void) {
    char *out = latc_round_trip_capture("let add = |a, b| { a + b }\n"
                                        "fn main() { print(add(2, 3)) }\n");
    ASSERT_STR_EQ(out, "5");
    free(out);
}

static void test_latc_round_trip_nested(void) {
    char *out = latc_round_trip_capture("fn outer() {\n"
                                        "    let inner = |x| { x * 2 }\n"
                                        "    return inner(21)\n"
                                        "}\n"
                                        "fn main() { print(outer()) }\n");
    ASSERT_STR_EQ(out, "42");
    free(out);
}

static void test_latc_round_trip_program(void) {
    char *out = latc_round_trip_capture("let fib = |n| {\n"
                                        "    if n < 2 { n } else { fib(n - 1) + fib(n - 2) }\n"
                                        "}\n"
                                        "fn main() { print(fib(10)) }\n");
    ASSERT_STR_EQ(out, "55");
    free(out);
}

static void test_latc_invalid_magic(void) {
    uint8_t bad_data[] = {'N', 'O', 'P', 'E', 0, 0, 0, 0};
    char *err = NULL;
    Chunk *c = chunk_deserialize(bad_data, sizeof(bad_data), &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    ASSERT(strstr(err, "magic") != NULL || strstr(err, "not a .latc") != NULL);
    free(err);
}

static void test_latc_truncated(void) {
    uint8_t trunc[] = {'L', 'A', 'T', 'C', 1, 0};
    char *err = NULL;
    Chunk *c = chunk_deserialize(trunc, sizeof(trunc), &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

static void test_latc_file_save_load(void) {
    /* Compile a program */
    const char *source = "fn main() { print(\"file round-trip\") }";
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
    Chunk *chunk = stack_compile(&prog, &comp_err);
    ASSERT(chunk != NULL);

    /* Save to temp file */
    const char *tmp_path = "/tmp/test_latc_save_load.latc";
    ASSERT(chunk_save(chunk, tmp_path) == 0);
    chunk_free(chunk);

    /* Load back */
    char *load_err = NULL;
    Chunk *loaded = chunk_load(tmp_path, &load_err);
    ASSERT(loaded != NULL);

    /* Run it */
    fflush(stdout);
    FILE *tmp = tmpfile();
    int old_stdout = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    LatRuntime rt;
    lat_runtime_init(&rt);
    StackVM vm;
    stackvm_init(&vm, &rt);
    LatValue result;
    StackVMResult vm_res = stackvm_run(&vm, loaded, &result);

    fflush(stdout);
    dup2(old_stdout, fileno(stdout));
    close(old_stdout);

    fseek(tmp, 0, SEEK_END);
    long len = ftell(tmp);
    fseek(tmp, 0, SEEK_SET);
    char *output = malloc((size_t)len + 1);
    size_t n = fread(output, 1, (size_t)len, tmp);
    output[n] = '\0';
    fclose(tmp);
    if (n > 0 && output[n - 1] == '\n') output[n - 1] = '\0';

    ASSERT(vm_res == STACKVM_OK);
    ASSERT_STR_EQ(output, "file round-trip");

    free(output);
    value_free(&result);
    stackvm_free(&vm);
    lat_runtime_free(&rt);
    chunk_free(loaded);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    unlink(tmp_path);
}

/* ======================================================================
 * RegVM Bytecode Serialization (.rlatc)
 * ====================================================================== */

/* Helper: compile source to a RegChunk, round-trip through serialize/deserialize,
 * then run the deserialized chunk and capture stdout. */
static char *rlatc_round_trip_capture(const char *source) {
    /* Compile */
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        free(lex_err);
        lat_vec_free(&tokens);
        return strdup("LEX_ERROR");
    }
    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    if (parse_err) {
        free(parse_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return strdup("PARSE_ERROR");
    }

    value_set_heap(NULL);
    value_set_arena(NULL);

    char *comp_err = NULL;
    RegChunk *rchunk = reg_compile(&prog, &comp_err);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    if (!rchunk) {
        free(comp_err);
        return strdup("COMPILE_ERROR");
    }

    /* Serialize → Deserialize */
    size_t buf_len;
    uint8_t *buf = regchunk_serialize(rchunk, &buf_len);
    regchunk_free(rchunk);

    char *deser_err = NULL;
    RegChunk *loaded = regchunk_deserialize(buf, buf_len, &deser_err);
    free(buf);
    if (!loaded) {
        size_t msglen = strlen(deser_err) + 20;
        char *msg = malloc(msglen);
        snprintf(msg, msglen, "DESER_ERROR:%s", deser_err);
        free(deser_err);
        return msg;
    }

    /* Run the deserialized chunk, capturing stdout */
    fflush(stdout);
    FILE *tmp = tmpfile();
    int old_stdout = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    LatRuntime rt;
    lat_runtime_init(&rt);
    RegVM rvm;
    regvm_init(&rvm, &rt);
    LatValue result;
    RegVMResult rvm_res = regvm_run(&rvm, loaded, &result);

    fflush(stdout);
    dup2(old_stdout, fileno(stdout));
    close(old_stdout);

    fseek(tmp, 0, SEEK_END);
    long len = ftell(tmp);
    fseek(tmp, 0, SEEK_SET);
    char *output = malloc((size_t)len + 1);
    size_t n = fread(output, 1, (size_t)len, tmp);
    output[n] = '\0';
    fclose(tmp);
    if (n > 0 && output[n - 1] == '\n') output[n - 1] = '\0';

    if (rvm_res != REGVM_OK) {
        free(output);
        size_t elen = strlen(rvm.error) + 16;
        output = malloc(elen);
        snprintf(output, elen, "VM_ERROR:%s", rvm.error);
    } else {
        value_free(&result);
    }
    regvm_free(&rvm);
    lat_runtime_free(&rt);
    regchunk_free(loaded);
    return output;
}

static void test_rlatc_round_trip_int(void) {
    char *out = rlatc_round_trip_capture("fn main() { print(42) }");
    ASSERT_STR_EQ(out, "42");
    free(out);
}

static void test_rlatc_round_trip_string(void) {
    char *out = rlatc_round_trip_capture("fn main() { print(\"hello rlatc\") }");
    ASSERT_STR_EQ(out, "hello rlatc");
    free(out);
}

static void test_rlatc_round_trip_closure(void) {
    char *out = rlatc_round_trip_capture("let add = |a, b| { a + b }\n"
                                         "fn main() { print(add(2, 3)) }\n");
    ASSERT_STR_EQ(out, "5");
    free(out);
}

static void test_rlatc_round_trip_nested(void) {
    char *out = rlatc_round_trip_capture("fn outer() {\n"
                                         "    let inner = |x| { x * 2 }\n"
                                         "    return inner(21)\n"
                                         "}\n"
                                         "fn main() { print(outer()) }\n");
    ASSERT_STR_EQ(out, "42");
    free(out);
}

static void test_rlatc_round_trip_program(void) {
    char *out = rlatc_round_trip_capture("let fib = |n| {\n"
                                         "    if n < 2 { n } else { fib(n - 1) + fib(n - 2) }\n"
                                         "}\n"
                                         "fn main() { print(fib(10)) }\n");
    ASSERT_STR_EQ(out, "55");
    free(out);
}

static void test_rlatc_invalid_magic(void) {
    uint8_t bad_data[] = {'N', 'O', 'P', 'E', 0, 0, 0, 0};
    char *err = NULL;
    RegChunk *c = regchunk_deserialize(bad_data, sizeof(bad_data), &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    ASSERT(strstr(err, "magic") != NULL || strstr(err, "not a .rlatc") != NULL);
    free(err);
}

static void test_rlatc_truncated(void) {
    uint8_t trunc[] = {'R', 'L', 'A', 'T', 1, 0};
    char *err = NULL;
    RegChunk *c = regchunk_deserialize(trunc, sizeof(trunc), &err);
    ASSERT(c == NULL);
    ASSERT(err != NULL);
    free(err);
}

static void test_rlatc_file_save_load(void) {
    /* Compile a program */
    const char *source = "fn main() { print(\"regvm file round-trip\") }";
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

    /* Save to temp file */
    const char *tmp_path = "/tmp/test_rlatc_save_load.latc";
    ASSERT(regchunk_save(rchunk, tmp_path) == 0);
    regchunk_free(rchunk);

    /* Load back */
    char *load_err = NULL;
    RegChunk *loaded = regchunk_load(tmp_path, &load_err);
    ASSERT(loaded != NULL);

    /* Run it */
    fflush(stdout);
    FILE *tmp = tmpfile();
    int old_stdout = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    LatRuntime rt;
    lat_runtime_init(&rt);
    RegVM rvm;
    regvm_init(&rvm, &rt);
    LatValue result;
    RegVMResult rvm_res = regvm_run(&rvm, loaded, &result);

    fflush(stdout);
    dup2(old_stdout, fileno(stdout));
    close(old_stdout);

    fseek(tmp, 0, SEEK_END);
    long len = ftell(tmp);
    fseek(tmp, 0, SEEK_SET);
    char *output = malloc((size_t)len + 1);
    size_t n = fread(output, 1, (size_t)len, tmp);
    output[n] = '\0';
    fclose(tmp);
    if (n > 0 && output[n - 1] == '\n') output[n - 1] = '\0';

    ASSERT(rvm_res == REGVM_OK);
    ASSERT_STR_EQ(output, "regvm file round-trip");

    free(output);
    value_free(&result);
    regvm_free(&rvm);
    lat_runtime_free(&rt);
    regchunk_free(loaded);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    unlink(tmp_path);
}

/* ── RegVM .rlat scope/select round-trip tests ── */

static void test_rlatc_round_trip_scope_spawn(void) {
    /* scope { spawn { } } round-trips through serialize/deserialize */
    char *out = rlatc_round_trip_capture("let x = 10\n"
                                         "scope {\n"
                                         "    spawn { print(x + 1) }\n"
                                         "    spawn { print(x + 2) }\n"
                                         "}\n"
                                         "print(\"done\")\n");
    /* Spawns may execute in any order, but "done" always comes last.
     * Check that the output contains both spawn results and "done". */
    ASSERT(strstr(out, "11") != NULL);
    ASSERT(strstr(out, "12") != NULL);
    ASSERT(strstr(out, "done") != NULL);
    free(out);
}

static void test_rlatc_round_trip_scope_sync(void) {
    /* scope with sync body (no spawns) round-trips correctly */
    char *out = rlatc_round_trip_capture("let r = scope {\n"
                                         "    42\n"
                                         "}\n"
                                         "print(r)\n");
    ASSERT_STR_EQ(out, "42");
    free(out);
}

static void test_rlatc_round_trip_select(void) {
    /* select with channel arms round-trips through serialize/deserialize */
    char *out = rlatc_round_trip_capture("let ch = Channel::new()\n"
                                         "scope {\n"
                                         "    spawn { ch.send(99) }\n"
                                         "}\n"
                                         "select {\n"
                                         "    val from ch => {\n"
                                         "        print(\"got: ${val}\")\n"
                                         "    }\n"
                                         "}\n"
                                         "print(\"done\")\n");
    ASSERT(strstr(out, "got: 99") != NULL);
    ASSERT(strstr(out, "done") != NULL);
    free(out);
}

static void test_rlatc_scope_file_save_load(void) {
    /* Compile scope/spawn to .rlat file, load back and run */
    const char *source = "let x = 100\n"
                         "scope {\n"
                         "    spawn { print(x + 1) }\n"
                         "}\n"
                         "print(\"ok\")\n";

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

    /* Save to temp .rlat file */
    const char *tmp_path = "/tmp/test_rlatc_scope.rlat";
    ASSERT(regchunk_save(rchunk, tmp_path) == 0);
    regchunk_free(rchunk);

    /* Load back */
    char *load_err = NULL;
    RegChunk *loaded = regchunk_load(tmp_path, &load_err);
    ASSERT(loaded != NULL);

    /* Run it */
    fflush(stdout);
    FILE *tmp = tmpfile();
    int old_stdout = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    LatRuntime rt;
    lat_runtime_init(&rt);
    RegVM rvm;
    regvm_init(&rvm, &rt);
    LatValue result;
    RegVMResult rvm_res = regvm_run(&rvm, loaded, &result);

    fflush(stdout);
    dup2(old_stdout, fileno(stdout));
    close(old_stdout);

    fseek(tmp, 0, SEEK_END);
    long len = ftell(tmp);
    fseek(tmp, 0, SEEK_SET);
    char *output = malloc((size_t)len + 1);
    size_t n = fread(output, 1, (size_t)len, tmp);
    output[n] = '\0';
    fclose(tmp);
    if (n > 0 && output[n - 1] == '\n') output[n - 1] = '\0';

    ASSERT(rvm_res == REGVM_OK);
    ASSERT(strstr(output, "101") != NULL);
    ASSERT(strstr(output, "ok") != NULL);

    free(output);
    value_free(&result);
    regvm_free(&rvm);
    lat_runtime_free(&rt);
    regchunk_free(loaded);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    unlink(tmp_path);
}

/* ── Built-in stdlib module tests ── */

static void test_builtin_math_sin(void) {
    char *out = run_capture("import { sin } from \"math\"\n"
                            "fn main() { print(sin(0)) }\n");
    ASSERT(out != NULL);
    ASSERT_STR_EQ(out, "0");
    free(out);
}

static void test_builtin_math_pi(void) {
    char *out = run_capture("import { PI } from \"math\"\n"
                            "fn main() { print(PI()) }\n");
    ASSERT(out != NULL);
    ASSERT(strncmp(out, "3.14159", 7) == 0);
    free(out);
}

static void test_builtin_math_alias(void) {
    char *out = run_capture("import \"math\" as m\n"
                            "fn main() { print(m.sqrt(4)) }\n");
    ASSERT(out != NULL);
    ASSERT_STR_EQ(out, "2");
    free(out);
}

static void test_builtin_fs_exists(void) {
    char *out = run_capture("import { file_exists } from \"fs\"\n"
                            "fn main() { print(file_exists(\"Makefile\")) }\n");
    ASSERT(out != NULL);
    ASSERT_STR_EQ(out, "true");
    free(out);
}

static void test_builtin_json_parse(void) {
    char *out = run_capture("import { parse } from \"json\"\n"
                            "fn main() {\n"
                            "  let arr = parse(\"[1,2,3]\")\n"
                            "  print(len(arr))\n"
                            "}\n");
    ASSERT(out != NULL);
    ASSERT_STR_EQ(out, "3");
    free(out);
}

static void test_builtin_path_join(void) {
    char *out = run_capture("import { join } from \"path\"\n"
                            "fn main() { print(join(\"a\", \"b\")) }\n");
    ASSERT(out != NULL);
    ASSERT_STR_EQ(out, "a/b");
    free(out);
}

static void test_builtin_time_now(void) {
    char *out = run_capture("import { now } from \"time\"\n"
                            "fn main() { print(now() > 0) }\n");
    ASSERT(out != NULL);
    ASSERT_STR_EQ(out, "true");
    free(out);
}

static void test_builtin_regex_match(void) {
    char *out = run_capture("import { find_all } from \"regex\"\n"
                            "fn main() { print(len(find_all(\"[0-9]+\", \"abc123\"))) }\n");
    ASSERT(out != NULL);
    ASSERT_STR_EQ(out, "1");
    free(out);
}

static void test_builtin_os_platform(void) {
    char *out = run_capture("import { platform } from \"os\"\n"
                            "fn main() { print(len(platform()) > 0) }\n");
    ASSERT(out != NULL);
    ASSERT_STR_EQ(out, "true");
    free(out);
}

static void test_builtin_crypto_base64(void) {
    char *out = run_capture("import { base64_encode } from \"crypto\"\n"
                            "fn main() { print(base64_encode(\"hello\")) }\n");
    ASSERT(out != NULL);
    ASSERT_STR_EQ(out, "aGVsbG8=");
    free(out);
}

static void test_builtin_legacy_sin(void) {
    /* Flat native sin() should still work */
    char *out = run_capture("fn main() { print(sin(0)) }\n");
    ASSERT(out != NULL);
    ASSERT_STR_EQ(out, "0");
    free(out);
}

static void test_builtin_full_module_access(void) {
    /* import "math" as m gives a Map; access via dot notation */
    char *out = run_capture("import \"math\" as m\n"
                            "fn main() { print(m.floor(3.7)) }\n");
    ASSERT(out != NULL);
    ASSERT_STR_EQ(out, "3");
    free(out);
}

/* ======================================================================
 * .length() alias tests
 * ====================================================================== */

static void test_length_alias_array(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3].length())\n"
                  "}\n",
                  "3");
}

static void test_length_alias_string(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".length())\n"
                  "}\n",
                  "5");
}

static void test_length_alias_buffer(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let b = Buffer::new(16)\n"
                  "    print(b.length())\n"
                  "}\n",
                  "16");
}

/* ======================================================================
 * Additional crypto tests (SHA-512, HMAC-SHA256, random_bytes)
 * ====================================================================== */

static void test_sha512_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(sha512(\"\"))\n"
                  "}\n",
                  "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                  "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
}

static void test_sha512_hello(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(sha512(\"hello\"))\n"
                  "}\n",
                  "9b71d224bd62f3785d96d46ad3ea3d73319bfbc2890caadae2dff72519673ca7"
                  "2323c3d99ba5c11d7c7acc6e14b8c5da0c4663475c2e5c3adef46f73bcdec043");
}

static void test_hmac_sha256_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(hmac_sha256(\"key\", \"hello\"))\n"
                  "}\n",
                  "9307b3b915efb5171ff14d8cb55fbcc798c6c0ef1456d66ded1a6aa723a58b7b");
}

static void test_random_bytes_length(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let b = random_bytes(16)\n"
                  "    print(len(b))\n"
                  "}\n",
                  "16");
}

/* ======================================================================
 * Buffer read methods
 * ====================================================================== */

static void test_buffer_read_i8(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let b = Buffer::new(2)\n"
                  "    b.write_u8(0, 200)\n"
                  "    print(b.read_i8(0))\n"
                  "}\n",
                  "-56");
}

static void test_buffer_read_f32(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let b = Buffer::new(4)\n"
                  "    b.write_u8(0, 0)\n"
                  "    b.write_u8(1, 0)\n"
                  "    b.write_u8(2, 72)\n"
                  "    b.write_u8(3, 65)\n"
                  "    print(b.read_f32(0))\n"
                  "}\n",
                  "12.5");
}

/* ======================================================================
 * String transform methods
 * ====================================================================== */

static void test_str_snake_case(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"helloWorld\".snake_case())\n"
                  "}\n",
                  "hello_world");
}

static void test_str_camel_case(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello_world\".camel_case())\n"
                  "}\n",
                  "helloWorld");
}

static void test_str_title_case(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello world\".title_case())\n"
                  "}\n",
                  "Hello World");
}

static void test_str_capitalize(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".capitalize())\n"
                  "}\n",
                  "Hello");
}

static void test_str_kebab_case(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"helloWorld\".kebab_case())\n"
                  "}\n",
                  "hello-world");
}

/* ======================================================================
 * Date/time component tests
 * ====================================================================== */

static void test_time_year(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let t = time_parse(\"2024-03-15T12:00:00\", \"%Y-%m-%dT%H:%M:%S\")\n"
                  "    print(time_year(t))\n"
                  "}\n",
                  "2024");
}

static void test_time_month(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let t = time_parse(\"2024-03-15T12:00:00\", \"%Y-%m-%dT%H:%M:%S\")\n"
                  "    print(time_month(t))\n"
                  "}\n",
                  "3");
}

static void test_is_leap_year(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"${is_leap_year(2024)},${is_leap_year(2023)}\")\n"
                  "}\n",
                  "true,false");
}

/* ======================================================================
 * Nested index assignment
 * ====================================================================== */

static void test_nested_index_assign(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux arr = [[1, 2], [3, 4], [5, 6]]\n"
                  "    arr[0][1] = 99\n"
                  "    print(arr[0][1])\n"
                  "}\n",
                  "99");
}

static void test_nested_index_assign_3d(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux grid = [[[0, 1], [2, 3]], [[4, 5], [6, 7]]]\n"
                  "    grid[1][0][1] = 42\n"
                  "    print(grid[1][0][1])\n"
                  "}\n",
                  "42");
}

/* ======================================================================
 * .length() alias for Map and Tuple
 * ====================================================================== */

static void test_length_alias_map(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"a\"] = 1\n"
                  "    m[\"b\"] = 2\n"
                  "    print(m.length())\n"
                  "}\n",
                  "2");
}

static void test_length_alias_tuple(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let t = (1, 2, 3, 4, 5)\n"
                  "    print(t.length())\n"
                  "}\n",
                  "5");
}

/* ======================================================================
 * compose() multi-call and chained
 * ====================================================================== */

static void test_compose_multi_call(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let double = |x| { x * 2 }\n"
                  "    let add1 = |x| { x + 1 }\n"
                  "    let f = compose(double, add1)\n"
                  "    print(\"${f(5)},${f(10)},${f(0)}\")\n"
                  "}\n",
                  "12,22,2");
}

static void test_compose_chained(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let add1 = |x| { x + 1 }\n"
                  "    let double = |x| { x * 2 }\n"
                  "    let square = |x| { x * x }\n"
                  "    let f = compose(square, compose(double, add1))\n"
                  "    print(f(3))\n"
                  "}\n",
                  "64");
}

/* ======================================================================
 * String concat in loop
 * ====================================================================== */

static void test_string_concat_loop(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux s = \"\"\n"
                  "    flux i = 0\n"
                  "    while i < 5 {\n"
                  "        s = s + to_string(i)\n"
                  "        i += 1\n"
                  "    }\n"
                  "    print(s)\n"
                  "}\n",
                  "01234");
}

/* ======================================================================
 * to_int / to_float builtins
 * ====================================================================== */

static void test_to_int_from_float(void) { ASSERT_OUTPUT("fn main() { print(to_int(3.9)) }\n", "3"); }

static void test_to_int_from_string(void) { ASSERT_OUTPUT("fn main() { print(to_int(\"42\")) }\n", "42"); }

static void test_to_float_from_int(void) { ASSERT_OUTPUT("fn main() { print(to_float(42)) }\n", "42"); }

static void test_to_float_from_string(void) { ASSERT_OUTPUT("fn main() { print(to_float(\"3.14\")) }\n", "3.14"); }

/* ======================================================================
 * float_to_bits / bits_to_float roundtrip
 * ====================================================================== */

static void test_float_bits_roundtrip(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let bits = float_to_bits(1.0)\n"
                  "    let f = bits_to_float(bits)\n"
                  "    print(f)\n"
                  "}\n",
                  "1");
}

static void test_float_bits_zero(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let bits = float_to_bits(0.0)\n"
                  "    print(bits)\n"
                  "}\n",
                  "0");
}

/* ======================================================================
 * panic() builtin
 * ====================================================================== */

static void test_panic_message(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    try {\n"
                  "        panic(\"test boom\")\n"
                  "    } catch e {\n"
                  "        print(e.message)\n"
                  "    }\n"
                  "}\n",
                  "test boom");
}

/* ======================================================================
 * Phase System Extended Tests
 * ====================================================================== */

/* ── 1. Crystallization contracts: freeze(x) where |v| { ... } ── */

static void test_freeze_contract_passes(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 42\n"
                  "    freeze(x) where |v| { if v < 0 { panic(\"must be non-negative\") } }\n"
                  "    print(phase_of(x))\n"
                  "}\n",
                  "crystal");
}

static void test_freeze_contract_rejects(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux x = -5\n"
                              "    freeze(x) where |v| { if v < 0 { panic(\"must be non-negative\") } }\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_freeze_contract_with_string(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux name = \"Alice\"\n"
                  "    freeze(name) where |v| { if v.len() == 0 { panic(\"empty\") } }\n"
                  "    print(name)\n"
                  "}\n",
                  "Alice");
}

static void test_freeze_contract_rejects_empty_string(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux name = \"\"\n"
                              "    freeze(name) where |v| { if v.len() == 0 { panic(\"cannot be empty\") } }\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_freeze_contract_with_array(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux items = [1, 2, 3]\n"
                  "    freeze(items) where |v| { if v.len() < 1 { panic(\"need items\") } }\n"
                  "    print(items.len())\n"
                  "}\n",
                  "3");
}

static void test_freeze_contract_checks_array_content(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux items = []\n"
                              "    freeze(items) where |v| { if v.len() < 1 { panic(\"need at least one item\") } }\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_freeze_contract_error_message_propagated(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux x = 0\n"
                              "    freeze(x) where |v| { panic(\"custom rejection\") }\n"
                              "}\n",
                              "EVAL_ERROR:freeze contract failed:");
}

/* ── 2. Phase reactions chaining ── */

static void test_react_ordering_preserved(void) {
    /* Multiple reactions should fire in registration order */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 10\n"
                  "    react(x, |phase, val| { print(\"A\") })\n"
                  "    react(x, |phase, val| { print(\"B\") })\n"
                  "    react(x, |phase, val| { print(\"C\") })\n"
                  "    freeze(x)\n"
                  "}\n",
                  "A\nB\nC");
}

static void test_react_freeze_and_thaw_sequence(void) {
    /* Reaction fires on both freeze and thaw */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 5\n"
                  "    react(x, |phase, val| { print(phase + \":\" + to_string(val)) })\n"
                  "    freeze(x)\n"
                  "    thaw(x)\n"
                  "    freeze(x)\n"
                  "}\n",
                  "crystal:5\nfluid:5\ncrystal:5");
}

static void test_react_on_bond_cascade(void) {
    /* Reactions on bonded variable fire during cascade */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = 1\n"
                  "    flux b = 2\n"
                  "    flux c = 3\n"
                  "    bond(a, b)\n"
                  "    bond(b, c)\n"
                  "    react(b, |phase, val| { print(\"b:\" + phase) })\n"
                  "    react(c, |phase, val| { print(\"c:\" + phase) })\n"
                  "    freeze(a)\n"
                  "}\n",
                  "b:crystal\nc:crystal");
}

static void test_react_value_changes_between_events(void) {
    /* Reaction sees current value at each event */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 10\n"
                  "    react(x, |phase, val| { print(to_string(val)) })\n"
                  "    freeze(x)\n"
                  "    thaw(x)\n"
                  "}\n",
                  "10\n10");
}

static void test_react_anneal_reaction_fires(void) {
    /* Anneal triggers reaction with updated value */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 100\n"
                  "    freeze(x)\n"
                  "    react(x, |phase, val| { print(\"reacted:\" + to_string(val)) })\n"
                  "    anneal(x) |v| { v * 2 }\n"
                  "}\n",
                  "reacted:200");
}

/* ── 3. Bond strategies ── */

static void test_bond_mirror_cascades_freeze(void) {
    /* Mirror (default) cascades freeze to all deps */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = 1\n"
                  "    flux b = 2\n"
                  "    flux c = 3\n"
                  "    bond(a, b, c)\n"
                  "    freeze(a)\n"
                  "    print(phase_of(a))\n"
                  "    print(phase_of(b))\n"
                  "    print(phase_of(c))\n"
                  "}\n",
                  "crystal\ncrystal\ncrystal");
}

static void test_bond_inverse_thaws_frozen_dep(void) {
    /* Inverse bond: bond must be set before freezing the dep.
     * Freezing a thaws b (inverse cascade). */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = 1\n"
                  "    flux b = 2\n"
                  "    bond(a, b, \"inverse\")\n"
                  "    freeze(b)\n"
                  "    freeze(a)\n"
                  "    print(phase_of(a))\n"
                  "    print(phase_of(b))\n"
                  "}\n",
                  "crystal\nfluid");
}

static void test_bond_inverse_skips_fluid_dep(void) {
    /* Inverse: if dep is already fluid, nothing happens (no error) */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = 1\n"
                  "    flux b = 2\n"
                  "    bond(a, b, \"inverse\")\n"
                  "    freeze(a)\n"
                  "    print(phase_of(b))\n"
                  "}\n",
                  "fluid");
}

static void test_bond_gate_blocks_freeze_when_dep_fluid(void) {
    /* Gate: cannot freeze target if dep is not crystal */
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux x = 10\n"
                              "    flux guard = 20\n"
                              "    bond(x, guard, \"gate\")\n"
                              "    freeze(x)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_bond_gate_allows_when_dep_crystal(void) {
    /* Gate: freeze allowed when dep is already crystal */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 10\n"
                  "    flux guard = 20\n"
                  "    bond(x, guard, \"gate\")\n"
                  "    freeze(guard)\n"
                  "    freeze(x)\n"
                  "    print(phase_of(x))\n"
                  "}\n",
                  "crystal");
}

static void test_bond_transitive_three_levels(void) {
    /* a->b->c->d: freezing a should cascade through all */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = 1\n"
                  "    flux b = 2\n"
                  "    flux c = 3\n"
                  "    flux d = 4\n"
                  "    bond(a, b)\n"
                  "    bond(b, c)\n"
                  "    bond(c, d)\n"
                  "    freeze(a)\n"
                  "    print(phase_of(d))\n"
                  "}\n",
                  "crystal");
}

static void test_bond_mirror_with_reaction(void) {
    /* Bond cascade + reaction: reaction fires on cascaded dep */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = 1\n"
                  "    flux b = 2\n"
                  "    bond(a, b)\n"
                  "    react(a, |phase, val| { print(\"a:\" + phase) })\n"
                  "    react(b, |phase, val| { print(\"b:\" + phase) })\n"
                  "    freeze(a)\n"
                  "}\n",
                  "b:crystal\na:crystal");
}

/* ── 4. Seed/grow ── */

static void test_seed_grow_contract_validates(void) {
    /* grow() validates seed contract and freezes */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux port = 8080\n"
                  "    seed(port, |v| { v > 0 && v < 65536 })\n"
                  "    grow(\"port\")\n"
                  "    print(phase_of(port))\n"
                  "    print(port)\n"
                  "}\n",
                  "crystal\n8080");
}

static void test_seed_grow_fails_on_invalid(void) {
    /* grow() fails when seed contract returns false */
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux port = -1\n"
                              "    seed(port, |v| { v > 0 })\n"
                              "    grow(\"port\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_seed_multiple_contracts(void) {
    /* Multiple seeds on same variable: all must pass */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = 50\n"
                  "    seed(x, |v| { v > 0 })\n"
                  "    seed(x, |v| { v < 100 })\n"
                  "    grow(\"x\")\n"
                  "    print(phase_of(x))\n"
                  "}\n",
                  "crystal");
}

static void test_seed_multiple_one_fails(void) {
    /* Multiple seeds: if any fails, grow errors */
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux x = 200\n"
                              "    seed(x, |v| { v > 0 })\n"
                              "    seed(x, |v| { v < 100 })\n"
                              "    grow(\"x\")\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_unseed_removes_contract(void) {
    /* unseed() removes the contract so freeze proceeds without checking */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = -5\n"
                  "    seed(x, |v| { v > 0 })\n"
                  "    unseed(x)\n"
                  "    freeze(x)\n"
                  "    print(phase_of(x))\n"
                  "}\n",
                  "crystal");
}

static void test_seed_validates_on_direct_freeze(void) {
    /* Seed contracts are also checked on freeze() (not just grow()) */
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux x = -1\n"
                              "    seed(x, |v| { v > 0 })\n"
                              "    freeze(x)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_seed_with_map_contract(void) {
    /* Seed contract validates map contents */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux cfg = Map::new()\n"
                  "    cfg[\"port\"] = 3000\n"
                  "    seed(cfg, |v| { v[\"port\"] > 0 })\n"
                  "    grow(\"cfg\")\n"
                  "    print(phase_of(cfg))\n"
                  "}\n",
                  "crystal");
}

/* ── 5. Alloy structs ── */

static void test_alloy_fix_field_is_crystal_on_creation(void) {
    /* fix fields are automatically crystal phase upon struct construction */
    ASSERT_OUTPUT("struct Cfg {\n"
                  "    name: fix String,\n"
                  "    count: flux Int,\n"
                  "}\n"
                  "fn main() {\n"
                  "    let c = Cfg { name: \"test\", count: 0 }\n"
                  "    print(c.name)\n"
                  "    c.count = 10\n"
                  "    print(c.count)\n"
                  "}\n",
                  "test\n10");
}

static void test_alloy_fix_field_rejects_different_value(void) {
    /* Attempting to set a fix field to any value should error */
    ASSERT_OUTPUT_STARTS_WITH("struct Config {\n"
                              "    host: fix String,\n"
                              "    retries: flux Int,\n"
                              "}\n"
                              "fn main() {\n"
                              "    let cfg = Config { host: \"localhost\", retries: 0 }\n"
                              "    cfg.host = \"remotehost\"\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_alloy_multiple_fix_fields(void) {
    /* All fix fields should reject mutation */
    ASSERT_OUTPUT_STARTS_WITH("struct Server {\n"
                              "    host: fix String,\n"
                              "    port: fix Int,\n"
                              "    retries: flux Int,\n"
                              "}\n"
                              "fn main() {\n"
                              "    let s = Server { host: \"localhost\", port: 8080, retries: 0 }\n"
                              "    s.port = 9090\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_alloy_multiple_flux_fields(void) {
    /* All flux fields should accept mutation */
    ASSERT_OUTPUT("struct Counter {\n"
                  "    label: fix String,\n"
                  "    count: flux Int,\n"
                  "    max: flux Int,\n"
                  "}\n"
                  "fn main() {\n"
                  "    let c = Counter { label: \"hits\", count: 0, max: 100 }\n"
                  "    c.count = 42\n"
                  "    c.max = 200\n"
                  "    print(c.count)\n"
                  "    print(c.max)\n"
                  "}\n",
                  "42\n200");
}

static void test_alloy_read_fix_field(void) {
    /* Reading fix field should always work */
    ASSERT_OUTPUT("struct Immutable {\n"
                  "    value: fix Int,\n"
                  "}\n"
                  "fn main() {\n"
                  "    let x = Immutable { value: 99 }\n"
                  "    print(x.value)\n"
                  "    print(x.value + 1)\n"
                  "}\n",
                  "99\n100");
}

/* ── 6. Sublimation ── */

static void test_sublimate_array_index_assign_blocked(void) {
    /* Sublimated array blocks index assignment */
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux items = [1, 2, 3]\n"
                              "    sublimate(items)\n"
                              "    items[0] = 99\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_sublimate_allows_read(void) {
    /* Sublimated collection still allows reading */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux items = [10, 20, 30]\n"
                  "    sublimate(items)\n"
                  "    print(items[0])\n"
                  "    print(items.len())\n"
                  "}\n",
                  "10\n3");
}

static void test_sublimate_map_allows_read(void) {
    /* Sublimated map still allows reading existing keys */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"key\"] = \"value\"\n"
                  "    sublimate(m)\n"
                  "    print(m[\"key\"])\n"
                  "}\n",
                  "value");
}

static void test_sublimate_struct_field_blocked(void) {
    /* Sublimated struct blocks field assignment */
    ASSERT_OUTPUT_STARTS_WITH("struct Point { x: Int, y: Int }\n"
                              "fn main() {\n"
                              "    flux p = Point { x: 1, y: 2 }\n"
                              "    sublimate(p)\n"
                              "    p.x = 10\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_sublimate_thaw_then_push(void) {
    /* Thawing a sublimated collection restores mutability */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux items = [1, 2]\n"
                  "    sublimate(items)\n"
                  "    thaw(items)\n"
                  "    items.push(3)\n"
                  "    print(items.len())\n"
                  "    print(items[2])\n"
                  "}\n",
                  "3\n3");
}

static void test_sublimate_fires_reaction(void) {
    /* Sublimate fires a reaction with "sublimated" phase name */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux x = [1, 2]\n"
                  "    react(x, |phase, val| { print(phase) })\n"
                  "    sublimate(x)\n"
                  "}\n",
                  "sublimated");
}

/* ── 7. Phase pressure ── */

static void test_pressure_no_grow_blocks_insert(void) {
    /* no_grow should also block insert */
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux data = [1, 2, 3]\n"
                              "    pressurize(data, \"no_grow\")\n"
                              "    data.insert(0, 99)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_pressure_no_grow_allows_pop(void) {
    /* no_grow should allow shrink operations (pop) */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux data = [1, 2, 3]\n"
                  "    pressurize(data, \"no_grow\")\n"
                  "    data.pop()\n"
                  "    print(data.len())\n"
                  "}\n",
                  "2");
}

static void test_pressure_no_shrink_allows_push(void) {
    /* no_shrink should allow grow operations (push) */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux data = [1, 2, 3]\n"
                  "    pressurize(data, \"no_shrink\")\n"
                  "    data.push(4)\n"
                  "    print(data.len())\n"
                  "}\n",
                  "4");
}

static void test_pressure_no_shrink_blocks_remove_at(void) {
    /* no_shrink should block remove_at */
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux data = [1, 2, 3]\n"
                              "    pressurize(data, \"no_shrink\")\n"
                              "    data.remove_at(0)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_pressure_no_resize_blocks_pop(void) {
    /* no_resize should block pop as well */
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux data = [1, 2, 3]\n"
                              "    pressurize(data, \"no_resize\")\n"
                              "    data.pop()\n"
                              "}\n",
                              "EVAL_ERROR:");
}

static void test_pressure_no_resize_allows_index_assign(void) {
    /* no_resize should allow in-place index assignment */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux data = [1, 2, 3]\n"
                  "    pressurize(data, \"no_resize\")\n"
                  "    data[1] = 99\n"
                  "    print(data[1])\n"
                  "}\n",
                  "99");
}

static void test_depressurize_then_push(void) {
    /* depressurize() removes pressure constraint */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux data = [1, 2]\n"
                  "    pressurize(data, \"no_resize\")\n"
                  "    depressurize(data)\n"
                  "    data.push(3)\n"
                  "    data.pop()\n"
                  "    print(data.len())\n"
                  "}\n",
                  "2");
}

static void test_pressure_of_after_change(void) {
    /* pressure_of reflects the current pressure mode */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux data = [1, 2, 3]\n"
                  "    pressurize(data, \"no_shrink\")\n"
                  "    print(pressure_of(\"data\"))\n"
                  "}\n",
                  "no_shrink");
}

/* ======================================================================
 * Cross-Backend Parity Tests
 *
 * These tests verify that all three backends (tree-walk, stack VM,
 * register VM) produce identical results for every builtin method.
 * The test binary runs each test on all backends in sequence, so a
 * single ASSERT_OUTPUT here effectively checks 3-way parity.
 * ====================================================================== */

/* ── Array Higher-Order Methods ── */

static void test_parity_array_map_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2, 3]\n"
                  "    print(a.map(|x| x * 2))\n"
                  "}\n",
                  "[2, 4, 6]");
}

static void test_parity_array_map_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = []\n"
                  "    print(a.map(|x| x + 1))\n"
                  "}\n",
                  "[]");
}

static void test_parity_array_map_strings(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [\"hello\", \"world\"]\n"
                  "    print(a.map(|s| s.to_upper()))\n"
                  "}\n",
                  "[HELLO, WORLD]");
}

static void test_parity_array_filter_basic(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2, 3, 4, 5, 6]\n"
                  "    print(a.filter(|x| x > 3))\n"
                  "}\n",
                  "[4, 5, 6]");
}

static void test_parity_array_filter_none(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2, 3]\n"
                  "    print(a.filter(|x| x > 10))\n"
                  "}\n",
                  "[]");
}

static void test_parity_array_filter_all(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2, 3]\n"
                  "    print(a.filter(|x| x > 0))\n"
                  "}\n",
                  "[1, 2, 3]");
}

static void test_parity_array_reduce_sum(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2, 3, 4]\n"
                  "    print(a.reduce(|acc, x| acc + x, 0))\n"
                  "}\n",
                  "10");
}

static void test_parity_array_reduce_string(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [\"a\", \"b\", \"c\"]\n"
                  "    print(a.reduce(|acc, x| acc + x, \"\"))\n"
                  "}\n",
                  "abc");
}

static void test_parity_array_reduce_product(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2, 3, 4]\n"
                  "    print(a.reduce(|acc, x| acc * x, 1))\n"
                  "}\n",
                  "24");
}

static void test_parity_array_each_side_effect(void) {
    /* Use for_each which is supported on all backends */
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [10, 20, 30]\n"
                  "    a.for_each(|x| print(x))\n"
                  "}\n",
                  "10\n20\n30");
}

static void test_parity_array_sort_default(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [5, 2, 8, 1, 9]\n"
                  "    print(a.sort())\n"
                  "}\n",
                  "[1, 2, 5, 8, 9]");
}

static void test_parity_array_sort_strings(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [\"cherry\", \"apple\", \"banana\"]\n"
                  "    print(a.sort())\n"
                  "}\n",
                  "[apple, banana, cherry]");
}

static void test_parity_array_sort_already_sorted(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2, 3]\n"
                  "    print(a.sort())\n"
                  "}\n",
                  "[1, 2, 3]");
}

static void test_parity_array_find_present(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2, 3, 4]\n"
                  "    print(a.find(|x| x > 2))\n"
                  "}\n",
                  "3");
}

static void test_parity_array_find_absent(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2, 3]\n"
                  "    print(a.find(|x| x > 10))\n"
                  "}\n",
                  "()");
}

static void test_parity_array_any_true(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2, 3]\n"
                  "    print(a.any(|x| x == 2))\n"
                  "}\n",
                  "true");
}

static void test_parity_array_any_false(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2, 3]\n"
                  "    print(a.any(|x| x > 10))\n"
                  "}\n",
                  "false");
}

static void test_parity_array_all_true(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [2, 4, 6]\n"
                  "    print(a.all(|x| x > 0))\n"
                  "}\n",
                  "true");
}

static void test_parity_array_all_false(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [2, 4, 5]\n"
                  "    print(a.all(|x| x % 2 == 0))\n"
                  "}\n",
                  "false");
}

static void test_parity_array_flat_map(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2, 3]\n"
                  "    print(a.flat_map(|x| [x, x * 10]))\n"
                  "}\n",
                  "[1, 10, 2, 20, 3, 30]");
}

static void test_parity_array_sort_by(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [\"cherry\", \"ab\", \"date\"]\n"
                  "    print(a.sort_by(|a, b| a.len() - b.len()))\n"
                  "}\n",
                  "[ab, date, cherry]");
}

static void test_parity_array_group_by(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [1, 2, 3, 4, 5, 6]\n"
                  "    let g = a.group_by(|x| x % 2 == 0)\n"
                  "    print(g[\"true\"])\n"
                  "    print(g[\"false\"])\n"
                  "}\n",
                  "[2, 4, 6]\n[1, 3, 5]");
}

static void test_parity_array_for_each(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    [\"a\", \"b\", \"c\"].for_each(|x| print(x))\n"
                  "}\n",
                  "a\nb\nc");
}

/* ── Array Non-Closure Methods ── */

static void test_parity_array_push_pop(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = [1, 2]\n"
                  "    a.push(3)\n"
                  "    print(a)\n"
                  "    let v = a.pop()\n"
                  "    print(v)\n"
                  "    print(a)\n"
                  "}\n",
                  "[1, 2, 3]\n3\n[1, 2]");
}

static void test_parity_array_reverse(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3, 4].reverse())\n"
                  "}\n",
                  "[4, 3, 2, 1]");
}

static void test_parity_array_contains(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [10, 20, 30]\n"
                  "    print(a.contains(20))\n"
                  "    print(a.contains(99))\n"
                  "}\n",
                  "true\nfalse");
}

static void test_parity_array_index_of(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [\"a\", \"b\", \"c\", \"b\"]\n"
                  "    print(a.index_of(\"b\"))\n"
                  "    print(a.index_of(\"z\"))\n"
                  "}\n",
                  "1\n-1");
}

static void test_parity_array_slice(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [10, 20, 30, 40, 50]\n"
                  "    print(a.slice(1, 4))\n"
                  "}\n",
                  "[20, 30, 40]");
}

static void test_parity_array_join(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([\"a\", \"b\", \"c\"].join(\", \"))\n"
                  "}\n",
                  "a, b, c");
}

static void test_parity_array_unique(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 2, 3, 3, 3].unique())\n"
                  "}\n",
                  "[1, 2, 3]");
}

static void test_parity_array_zip(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3].zip([\"a\", \"b\", \"c\"]))\n"
                  "}\n",
                  "[[1, a], [2, b], [3, c]]");
}

static void test_parity_array_chunk(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3, 4, 5].chunk(2))\n"
                  "}\n",
                  "[[1, 2], [3, 4], [5]]");
}

static void test_parity_array_take(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3, 4, 5].take(3))\n"
                  "}\n",
                  "[1, 2, 3]");
}

static void test_parity_array_drop(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3, 4, 5].drop(2))\n"
                  "}\n",
                  "[3, 4, 5]");
}

static void test_parity_array_flat(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([[1, 2], [3], [4, 5]].flat())\n"
                  "}\n",
                  "[1, 2, 3, 4, 5]");
}

static void test_parity_array_first_last(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [10, 20, 30]\n"
                  "    print(a.first())\n"
                  "    print(a.last())\n"
                  "}\n",
                  "10\n30");
}

static void test_parity_array_sum(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([1, 2, 3, 4].sum())\n"
                  "}\n",
                  "10");
}

static void test_parity_array_min_max(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [3, 1, 4, 1, 5]\n"
                  "    print(a.min())\n"
                  "    print(a.max())\n"
                  "}\n",
                  "1\n5");
}

static void test_parity_array_insert(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = [1, 2, 4]\n"
                  "    a.insert(2, 3)\n"
                  "    print(a)\n"
                  "}\n",
                  "[1, 2, 3, 4]");
}

static void test_parity_array_remove_at(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux a = [1, 2, 3, 4]\n"
                  "    let removed = a.remove_at(1)\n"
                  "    print(removed)\n"
                  "    print(a)\n"
                  "}\n",
                  "2\n[1, 3, 4]");
}

static void test_parity_array_enumerate(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print([\"a\", \"b\"].enumerate())\n"
                  "}\n",
                  "[[0, a], [1, b]]");
}

/* ── Array Method Chaining ── */

static void test_parity_array_map_filter_chain(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = [1, 2, 3, 4, 5].map(|x| x * 2).filter(|x| x > 4)\n"
                  "    print(r)\n"
                  "}\n",
                  "[6, 8, 10]");
}

static void test_parity_array_filter_map_chain(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = [1, 2, 3, 4, 5, 6].filter(|x| x % 2 == 0).map(|x| x * 10)\n"
                  "    print(r)\n"
                  "}\n",
                  "[20, 40, 60]");
}

static void test_parity_array_map_reduce_chain(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = [1, 2, 3].map(|x| x * x).reduce(|a, b| a + b, 0)\n"
                  "    print(r)\n"
                  "}\n",
                  "14");
}

/* ── String Methods (Allocation-Heavy) ── */

static void test_parity_str_replace(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello world\".replace(\"world\", \"there\"))\n"
                  "}\n",
                  "hello there");
}

static void test_parity_str_split(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"a,b,c\".split(\",\"))\n"
                  "}\n",
                  "[a, b, c]");
}

static void test_parity_str_split_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"\".split(\",\"))\n"
                  "}\n",
                  "[]");
}

static void test_parity_str_chars(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"abc\".chars())\n"
                  "}\n",
                  "[a, b, c]");
}

static void test_parity_str_bytes(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"ABC\".bytes())\n"
                  "}\n",
                  "[65, 66, 67]");
}

static void test_parity_str_substring(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello world\".substring(0, 5))\n"
                  "}\n",
                  "hello");
}

static void test_parity_str_index_of(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello\".index_of(\"ll\"))\n"
                  "    print(\"hello\".index_of(\"xyz\"))\n"
                  "}\n",
                  "2\n-1");
}

static void test_parity_str_repeat(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"ab\".repeat(3))\n"
                  "}\n",
                  "ababab");
}

static void test_parity_str_pad_left(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"42\".pad_left(5, \"0\"))\n"
                  "}\n",
                  "00042");
}

static void test_parity_str_pad_right(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hi\".pad_right(5, \".\"))\n"
                  "}\n",
                  "hi...");
}

static void test_parity_str_trim_start(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"  hello  \".trim_start())\n"
                  "}\n",
                  "hello  ");
}

static void test_parity_str_trim_end(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"  hello  \".trim_end())\n"
                  "}\n",
                  "  hello");
}

static void test_parity_str_reverse(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"abcdef\".reverse())\n"
                  "}\n",
                  "fedcba");
}

static void test_parity_str_count(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"banana\".count(\"a\"))\n"
                  "}\n",
                  "3");
}

static void test_parity_str_is_empty(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"\".is_empty())\n"
                  "    print(\"x\".is_empty())\n"
                  "}\n",
                  "true\nfalse");
}

static void test_parity_str_capitalize(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello world\".capitalize())\n"
                  "}\n",
                  "Hello world");
}

static void test_parity_str_title_case(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello world\".title_case())\n"
                  "}\n",
                  "Hello World");
}

static void test_parity_str_snake_case(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"helloWorld\".snake_case())\n"
                  "}\n",
                  "hello_world");
}

static void test_parity_str_camel_case(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello_world\".camel_case())\n"
                  "}\n",
                  "helloWorld");
}

static void test_parity_str_kebab_case(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    print(\"hello_world\".kebab_case())\n"
                  "}\n",
                  "hello-world");
}

/* ── Map Methods ── */

static void test_parity_map_keys_values(void) {
    /* Map iteration order is insertion order in Lattice */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"a\"] = 1\n"
                  "    m[\"b\"] = 2\n"
                  "    m[\"c\"] = 3\n"
                  "    print(m.len())\n"
                  "}\n",
                  "3");
}

static void test_parity_map_has(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"x\"] = 10\n"
                  "    m[\"y\"] = 20\n"
                  "    print(m.has(\"x\"))\n"
                  "    print(m.has(\"z\"))\n"
                  "}\n",
                  "true\nfalse");
}

static void test_parity_map_remove(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"a\"] = 1\n"
                  "    m[\"b\"] = 2\n"
                  "    m.remove(\"a\")\n"
                  "    print(m.len())\n"
                  "    print(m.has(\"a\"))\n"
                  "}\n",
                  "1\nfalse");
}

static void test_parity_map_merge(void) {
    /* merge() is in-place: it mutates m1 and returns unit */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m1 = Map::new()\n"
                  "    m1[\"a\"] = 1\n"
                  "    flux m2 = Map::new()\n"
                  "    m2[\"b\"] = 2\n"
                  "    m1.merge(m2)\n"
                  "    print(m1.len())\n"
                  "    print(m1[\"a\"])\n"
                  "    print(m1[\"b\"])\n"
                  "}\n",
                  "2\n1\n2");
}

static void test_parity_map_entries(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"x\"] = 42\n"
                  "    let e = m.entries()\n"
                  "    print(e.len())\n"
                  "    print(e[0][0])\n"
                  "    print(e[0][1])\n"
                  "}\n",
                  "1\nx\n42");
}

static void test_parity_map_for_each(void) {
    /* for_each iterates over key-value pairs */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"x\"] = 42\n"
                  "    m.for_each(|k, v| print(k + \"=\" + to_string(v)))\n"
                  "}\n",
                  "x=42");
}

static void test_parity_map_filter(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"a\"] = 1\n"
                  "    m[\"b\"] = 20\n"
                  "    m[\"c\"] = 3\n"
                  "    let f = m.filter(|k, v| v > 5)\n"
                  "    print(f.len())\n"
                  "    print(f[\"b\"])\n"
                  "}\n",
                  "1\n20");
}

/* ── Set Methods ── */

static void test_parity_set_add_has_remove(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux s = Set::new()\n"
                  "    s.add(1)\n"
                  "    s.add(2)\n"
                  "    s.add(2)\n"
                  "    print(s.len())\n"
                  "    print(s.has(1))\n"
                  "    print(s.has(3))\n"
                  "    s.remove(1)\n"
                  "    print(s.len())\n"
                  "}\n",
                  "2\ntrue\nfalse\n1");
}

static void test_parity_set_union(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = Set::from([1, 2, 3])\n"
                  "    let b = Set::from([3, 4, 5])\n"
                  "    let u = a.union(b)\n"
                  "    print(u.len())\n"
                  "}\n",
                  "5");
}

static void test_parity_set_intersection(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = Set::from([1, 2, 3, 4])\n"
                  "    let b = Set::from([3, 4, 5, 6])\n"
                  "    let i = a.intersection(b)\n"
                  "    print(i.len())\n"
                  "    print(i.has(3))\n"
                  "    print(i.has(4))\n"
                  "    print(i.has(1))\n"
                  "}\n",
                  "2\ntrue\ntrue\nfalse");
}

static void test_parity_set_difference(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = Set::from([1, 2, 3, 4])\n"
                  "    let b = Set::from([3, 4, 5])\n"
                  "    let d = a.difference(b)\n"
                  "    print(d.len())\n"
                  "    print(d.has(1))\n"
                  "    print(d.has(2))\n"
                  "    print(d.has(3))\n"
                  "}\n",
                  "2\ntrue\ntrue\nfalse");
}

static void test_parity_set_symmetric_difference(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = Set::from([1, 2, 3])\n"
                  "    let b = Set::from([2, 3, 4])\n"
                  "    let sd = a.symmetric_difference(b)\n"
                  "    print(sd.len())\n"
                  "    print(sd.has(1))\n"
                  "    print(sd.has(4))\n"
                  "    print(sd.has(2))\n"
                  "}\n",
                  "2\ntrue\ntrue\nfalse");
}

static void test_parity_set_subset_superset(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = Set::from([1, 2])\n"
                  "    let b = Set::from([1, 2, 3])\n"
                  "    print(a.is_subset(b))\n"
                  "    print(b.is_superset(a))\n"
                  "    print(b.is_subset(a))\n"
                  "}\n",
                  "true\ntrue\nfalse");
}

static void test_parity_set_to_array(void) {
    /* Set::from single element to guarantee order */
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = Set::from([42])\n"
                  "    let a = s.to_array()\n"
                  "    print(a.len())\n"
                  "    print(a[0])\n"
                  "}\n",
                  "1\n42");
}

/* ── Buffer Methods ── */

static void test_parity_buffer_write_read_u8(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux b = Buffer::new(4)\n"
                  "    b.write_u8(0, 255)\n"
                  "    b.write_u8(1, 42)\n"
                  "    print(b.read_u8(0))\n"
                  "    print(b.read_u8(1))\n"
                  "    print(b.len())\n"
                  "}\n",
                  "255\n42\n4");
}

static void test_parity_buffer_write_read_u16(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux b = Buffer::new(4)\n"
                  "    b.write_u16(0, 1000)\n"
                  "    print(b.read_u16(0))\n"
                  "}\n",
                  "1000");
}

static void test_parity_buffer_write_read_u32(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux b = Buffer::new(8)\n"
                  "    b.write_u32(0, 100000)\n"
                  "    print(b.read_u32(0))\n"
                  "}\n",
                  "100000");
}

static void test_parity_buffer_push(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux b = Buffer::new(0)\n"
                  "    b.push(10)\n"
                  "    b.push(20)\n"
                  "    print(b.len())\n"
                  "    print(b.read_u8(0))\n"
                  "    print(b.read_u8(1))\n"
                  "}\n",
                  "2\n10\n20");
}

static void test_parity_buffer_slice(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux b = Buffer::new(4)\n"
                  "    b.write_u8(0, 10)\n"
                  "    b.write_u8(1, 20)\n"
                  "    b.write_u8(2, 30)\n"
                  "    b.write_u8(3, 40)\n"
                  "    let s = b.slice(1, 3)\n"
                  "    print(s.len())\n"
                  "    print(s.read_u8(0))\n"
                  "    print(s.read_u8(1))\n"
                  "}\n",
                  "2\n20\n30");
}

static void test_parity_buffer_to_array(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux b = Buffer::new(3)\n"
                  "    b.write_u8(0, 1)\n"
                  "    b.write_u8(1, 2)\n"
                  "    b.write_u8(2, 3)\n"
                  "    print(b.to_array())\n"
                  "}\n",
                  "[1, 2, 3]");
}

static void test_parity_buffer_clear_fill(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux b = Buffer::new(3)\n"
                  "    b.fill(99)\n"
                  "    print(b.read_u8(0))\n"
                  "    print(b.read_u8(2))\n"
                  "    b.clear()\n"
                  "    print(b.len())\n"
                  "}\n",
                  "99\n99\n0");
}

static void test_parity_buffer_to_hex(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux b = Buffer::new(3)\n"
                  "    b.write_u8(0, 255)\n"
                  "    b.write_u8(1, 0)\n"
                  "    b.write_u8(2, 171)\n"
                  "    print(b.to_hex())\n"
                  "}\n",
                  "ff00ab");
}

static void test_parity_buffer_read_i8(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux b = Buffer::new(2)\n"
                  "    b.write_u8(0, 200)\n"
                  "    print(b.read_i8(0))\n"
                  "}\n",
                  "-56");
}

/* ── Nested / Edge Case Parity ── */

static void test_parity_nested_array_methods(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = [[3, 1], [2, 5], [4]]\n"
                  "    let flat_sorted = a.flat().sort()\n"
                  "    print(flat_sorted)\n"
                  "}\n",
                  "[1, 2, 3, 4, 5]");
}

static void test_parity_str_split_join_roundtrip(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = \"hello world foo\"\n"
                  "    let parts = s.split(\" \")\n"
                  "    let rejoined = parts.join(\" \")\n"
                  "    print(rejoined)\n"
                  "}\n",
                  "hello world foo");
}

static void test_parity_array_empty_operations(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = []\n"
                  "    print(a.len())\n"
                  "    print(a.reverse())\n"
                  "    print(a.unique())\n"
                  "    print(a.flat())\n"
                  "    print(a.sum())\n"
                  "}\n",
                  "0\n[]\n[]\n[]\n0");
}

static void test_parity_map_empty_operations(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let m = Map::new()\n"
                  "    print(m.len())\n"
                  "    print(m.keys())\n"
                  "    print(m.values())\n"
                  "    print(m.entries())\n"
                  "}\n",
                  "0\n[]\n[]\n[]");
}

static void test_parity_str_methods_chain(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = \"  Hello World  \"\n"
                  "    print(s.trim().to_lower())\n"
                  "}\n",
                  "hello world");
}

static void test_parity_array_map_with_index(void) {
    /* Verify map + enumerate interaction */
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = [10, 20, 30].enumerate().map(|pair| pair[0] + pair[1])\n"
                  "    print(r)\n"
                  "}\n",
                  "[10, 21, 32]");
}

/* ======================================================================
 * Cross-Backend Parity: Core Language Features
 *
 * Ported from tests/regvm_parity.sh — arithmetic, comparison, bitwise,
 * variables, control flow, functions, closures, enums, exceptions,
 * defer, destructuring, phases, and other fundamentals.
 * ====================================================================== */

/* ── Arithmetic ── */

static void test_parity_int_add(void) { ASSERT_OUTPUT("print(1 + 2)", "3"); }
static void test_parity_int_sub(void) { ASSERT_OUTPUT("print(10 - 3)", "7"); }
static void test_parity_int_mul(void) { ASSERT_OUTPUT("print(4 * 5)", "20"); }
static void test_parity_int_div(void) { ASSERT_OUTPUT("print(15 / 3)", "5"); }
static void test_parity_int_mod(void) { ASSERT_OUTPUT("print(17 % 5)", "2"); }
static void test_parity_int_neg(void) { ASSERT_OUTPUT("print(-42)", "-42"); }
static void test_parity_int_precedence(void) { ASSERT_OUTPUT("print(2 + 3 * 4)", "14"); }
static void test_parity_int_parens(void) { ASSERT_OUTPUT("print((2 + 3) * 4)", "20"); }
static void test_parity_float_add(void) { ASSERT_OUTPUT("print(3.14 + 1.0)", "4.14"); }
static void test_parity_float_div(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let r = 10.0 / 3.0\n"
                  "    print(r)\n"
                  "}\n",
                  "3.33333");
}

/* ── Comparison ── */

static void test_parity_cmp_lt(void) { ASSERT_OUTPUT("print(1 < 2)", "true"); }
static void test_parity_cmp_gt(void) { ASSERT_OUTPUT("print(2 > 1)", "true"); }
static void test_parity_cmp_lteq(void) { ASSERT_OUTPUT("print(1 <= 1)", "true"); }
static void test_parity_cmp_gteq(void) { ASSERT_OUTPUT("print(1 >= 2)", "false"); }
static void test_parity_cmp_eq(void) { ASSERT_OUTPUT("print(1 == 1)", "true"); }
static void test_parity_cmp_neq(void) { ASSERT_OUTPUT("print(1 != 2)", "true"); }
static void test_parity_logic_and(void) { ASSERT_OUTPUT("print(true && false)", "false"); }
static void test_parity_logic_or(void) { ASSERT_OUTPUT("print(true || false)", "true"); }
static void test_parity_logic_not(void) { ASSERT_OUTPUT("print(!true)", "false"); }

/* ── Bitwise ── */

static void test_parity_bit_and(void) { ASSERT_OUTPUT("print(5 & 3)", "1"); }
static void test_parity_bit_or(void) { ASSERT_OUTPUT("print(5 | 3)", "7"); }
static void test_parity_bit_xor(void) { ASSERT_OUTPUT("print(5 ^ 3)", "6"); }
static void test_parity_bit_not(void) { ASSERT_OUTPUT("print(~0)", "-1"); }
static void test_parity_bit_lshift(void) { ASSERT_OUTPUT("print(1 << 4)", "16"); }
static void test_parity_bit_rshift(void) { ASSERT_OUTPUT("print(16 >> 2)", "4"); }

/* ── String basics ── */

static void test_parity_str_concat_basic(void) { ASSERT_OUTPUT("print(\"hello\" + \" \" + \"world\")", "hello world"); }
static void test_parity_str_len_basic(void) { ASSERT_OUTPUT("print(\"abc\".len())", "3"); }
static void test_parity_str_upper(void) { ASSERT_OUTPUT("print(\"hello\".to_upper())", "HELLO"); }
static void test_parity_str_lower(void) { ASSERT_OUTPUT("print(\"HELLO\".to_lower())", "hello"); }
static void test_parity_str_trim_basic(void) { ASSERT_OUTPUT("print(\"  trim  \".trim())", "trim"); }
static void test_parity_str_contains_basic(void) { ASSERT_OUTPUT("print(\"hello\".contains(\"ell\"))", "true"); }
static void test_parity_str_starts_with(void) { ASSERT_OUTPUT("print(\"hello\".starts_with(\"hel\"))", "true"); }
static void test_parity_str_ends_with(void) { ASSERT_OUTPUT("print(\"hello\".ends_with(\"llo\"))", "true"); }
static void test_parity_str_interpolation(void) {
    ASSERT_OUTPUT("let x = 42\n"
                  "print(\"val = ${x}\")",
                  "val = 42");
}
static void test_parity_str_interp_expr(void) { ASSERT_OUTPUT("print(\"1 + 2 = ${1 + 2}\")", "1 + 2 = 3"); }

/* ── Variables ── */

static void test_parity_let_var(void) {
    ASSERT_OUTPUT("let x = 42\n"
                  "print(x)",
                  "42");
}
static void test_parity_flux_var(void) {
    ASSERT_OUTPUT("flux y = 10\n"
                  "y = 20\n"
                  "print(y)",
                  "20");
}

/* ── Structs ── */

static void test_parity_struct_basic(void) {
    ASSERT_OUTPUT("struct Point { x: int, y: int }\n"
                  "let p = Point { x: 10, y: 20 }\n"
                  "print(p.x)\n"
                  "print(p.y)",
                  "10\n20");
}
static void test_parity_struct_method(void) {
    ASSERT_OUTPUT("struct Counter { value: int, inc: any }\n"
                  "let c = Counter { value: 0, inc: |self| { self.value + 1 } }\n"
                  "print(c.inc())",
                  "1");
}

/* ── Functions ── */

static void test_parity_fn_basic(void) {
    ASSERT_OUTPUT("fn add(a: any, b: any) { return a + b }\n"
                  "print(add(3, 4))",
                  "7");
}
static void test_parity_fn_string(void) {
    ASSERT_OUTPUT("fn greet(name: any) { return \"Hello, \" + name + \"!\" }\n"
                  "print(greet(\"World\"))",
                  "Hello, World!");
}
static void test_parity_fn_recursive(void) {
    ASSERT_OUTPUT("fn factorial(n: any) {\n"
                  "    if n <= 1 { return 1 }\n"
                  "    return n * factorial(n - 1)\n"
                  "}\n"
                  "print(factorial(10))",
                  "3628800");
}

/* ── Closures ── */

static void test_parity_closure_capture(void) {
    ASSERT_OUTPUT("fn make_adder(n: any) {\n"
                  "    return |x| { x + n }\n"
                  "}\n"
                  "let add5 = make_adder(5)\n"
                  "print(add5(10))",
                  "15");
}
static void test_parity_closure_higher_order(void) {
    ASSERT_OUTPUT("fn apply(f: any, val: any) { return f(val) }\n"
                  "print(apply(|x| { x * 3 }, 7))",
                  "21");
}

/* ── Control flow ── */

static void test_parity_if_true(void) { ASSERT_OUTPUT("print(if true { \"yes\" } else { \"no\" })", "yes"); }
static void test_parity_if_false(void) { ASSERT_OUTPUT("print(if false { \"yes\" } else { \"no\" })", "no"); }
static void test_parity_match_basic(void) {
    ASSERT_OUTPUT("let x = \"A\"\n"
                  "let r = match x {\n"
                  "    \"A\" => \"first\",\n"
                  "    \"B\" => \"second\",\n"
                  "    _ => \"other\",\n"
                  "}\n"
                  "print(r)",
                  "first");
}

/* ── Loops ── */

static void test_parity_while_loop(void) {
    ASSERT_OUTPUT("flux i = 0\n"
                  "flux sum = 0\n"
                  "while i < 5 {\n"
                  "    sum = sum + i\n"
                  "    i = i + 1\n"
                  "}\n"
                  "print(sum)",
                  "10");
}
static void test_parity_for_loop(void) {
    ASSERT_OUTPUT("flux sum = 0\n"
                  "for n in [10, 20, 30] {\n"
                  "    sum = sum + n\n"
                  "}\n"
                  "print(sum)",
                  "60");
}
static void test_parity_for_strings(void) {
    ASSERT_OUTPUT("for s in [\"a\", \"b\", \"c\"] {\n"
                  "    print(s)\n"
                  "}",
                  "a\nb\nc");
}
static void test_parity_loop_break(void) {
    ASSERT_OUTPUT("flux c = 0\n"
                  "loop {\n"
                  "    if c >= 3 { break }\n"
                  "    c = c + 1\n"
                  "}\n"
                  "print(c)",
                  "3");
}
static void test_parity_for_range(void) {
    ASSERT_OUTPUT("flux sum = 0\n"
                  "for n in 1..5 {\n"
                  "    sum = sum + n\n"
                  "}\n"
                  "print(sum)",
                  "10");
}
static void test_parity_nested_for(void) {
    ASSERT_OUTPUT("flux result = []\n"
                  "for i in [1, 2, 3] {\n"
                  "    for j in [10, 20] {\n"
                  "        result.push(i * j)\n"
                  "    }\n"
                  "}\n"
                  "print(result)",
                  "[10, 20, 20, 40, 30, 60]");
}

/* ── Enums ── */

static void test_parity_enum_basic(void) {
    ASSERT_OUTPUT("enum Color { Red, Green, Blue }\n"
                  "let c = Color::Red\n"
                  "print(c)",
                  "Color::Red");
}
static void test_parity_enum_tag(void) {
    ASSERT_OUTPUT("enum Color { Red, Green, Blue }\n"
                  "print(Color::Red.tag())",
                  "Red");
}
static void test_parity_enum_name(void) {
    ASSERT_OUTPUT("enum Color { Red, Green, Blue }\n"
                  "print(Color::Red.enum_name())",
                  "Color");
}
static void test_parity_enum_payload(void) {
    ASSERT_OUTPUT("enum Shape { Circle(r) }\n"
                  "let s = Shape::Circle(5.0)\n"
                  "print(s.tag())\n"
                  "print(s.payload())",
                  "Circle\n[5]");
}

/* ── Exception handling ── */

static void test_parity_try_catch(void) {
    ASSERT_OUTPUT("let r = try {\n"
                  "    let x = 10 / 0\n"
                  "    \"ok\"\n"
                  "} catch err {\n"
                  "    \"caught\"\n"
                  "}\n"
                  "print(r)",
                  "caught");
}
static void test_parity_throw_catch(void) {
    ASSERT_OUTPUT("fn boom(a: any) { return a / 0 }\n"
                  "let r = try {\n"
                  "    boom(1)\n"
                  "} catch err {\n"
                  "    \"caught error\"\n"
                  "}\n"
                  "print(r)",
                  "caught error");
}

/* ── Nil coalescing ── */

static void test_parity_nil_coalesce(void) {
    ASSERT_OUTPUT("let v = nil ?? \"default\"\n"
                  "print(v)",
                  "default");
}
static void test_parity_nil_coalesce_non_nil(void) {
    ASSERT_OUTPUT("let v = 42 ?? \"default\"\n"
                  "print(v)",
                  "42");
}

/* ── Tuples ── */

static void test_parity_tuple_basic(void) {
    ASSERT_OUTPUT("let t = (1, \"hello\", true)\n"
                  "print(t)",
                  "(1, hello, true)");
}
static void test_parity_tuple_len(void) {
    ASSERT_OUTPUT("let t = (1, 2, 3)\n"
                  "print(t.len())",
                  "3");
}

/* ── Defer ── */

static void test_parity_defer_basic(void) {
    ASSERT_OUTPUT("fn with_defer() {\n"
                  "    defer { print(\"deferred\") }\n"
                  "    print(\"before\")\n"
                  "}\n"
                  "with_defer()",
                  "before\ndeferred");
}
static void test_parity_defer_lifo(void) {
    ASSERT_OUTPUT("fn two_defers() {\n"
                  "    defer { print(\"second\") }\n"
                  "    defer { print(\"first\") }\n"
                  "    print(\"body\")\n"
                  "}\n"
                  "two_defers()",
                  "body\nfirst\nsecond");
}

/* ── Destructuring ── */

static void test_parity_destructure_array(void) {
    ASSERT_OUTPUT("let [a, b, c] = [10, 20, 30]\n"
                  "print(a)\n"
                  "print(b)\n"
                  "print(c)",
                  "10\n20\n30");
}

/* ── Phases ── */

static void test_parity_flux_phase(void) {
    ASSERT_OUTPUT("flux x = 1\n"
                  "x = 2\n"
                  "print(x)",
                  "2");
}
static void test_parity_fix_phase(void) {
    ASSERT_OUTPUT("fix x = 42\n"
                  "print(x)",
                  "42");
}
static void test_parity_clone_value(void) {
    ASSERT_OUTPUT("let a = [1, 2, 3]\n"
                  "let b = clone(a)\n"
                  "print(b)",
                  "[1, 2, 3]");
}

/* ── Index assignment ── */

static void test_parity_index_assign_local(void) {
    ASSERT_OUTPUT("fn test_fn() {\n"
                  "    let a = [0, 0, 0]\n"
                  "    a[0] = 10\n"
                  "    a[1] = 20\n"
                  "    a[2] = 30\n"
                  "    print(a)\n"
                  "}\n"
                  "test_fn()",
                  "[10, 20, 30]");
}
static void test_parity_index_assign_global(void) {
    ASSERT_OUTPUT("flux a = [0, 0, 0]\n"
                  "a[0] = 10\n"
                  "a[1] = 20\n"
                  "a[2] = 30\n"
                  "print(a)",
                  "[10, 20, 30]");
}

/* ── Mixed / pipeline ── */

static void test_parity_complex_pipeline(void) {
    ASSERT_OUTPUT("fn process(items: any) {\n"
                  "    return items.map(|x| { x * 2 }).filter(|x| { x > 4 })\n"
                  "}\n"
                  "print(process([1, 2, 3, 4, 5]))",
                  "[6, 8, 10]");
}
static void test_parity_fn_return_array(void) {
    ASSERT_OUTPUT("fn divmod(a: any, b: any) {\n"
                  "    return [a / b, a % b]\n"
                  "}\n"
                  "let r = divmod(17, 5)\n"
                  "print(r[0])\n"
                  "print(r[1])",
                  "3\n2");
}

/* ── Global mutation ── */

static void test_parity_global_push(void) {
    ASSERT_OUTPUT("flux arr = []\n"
                  "arr.push(1)\n"
                  "arr.push(2)\n"
                  "arr.push(3)\n"
                  "print(arr)\n"
                  "print(arr.len())",
                  "[1, 2, 3]\n3");
}

/* ── Map basics ── */

static void test_parity_map_basic(void) {
    ASSERT_OUTPUT("let m = Map::new()\n"
                  "m[\"a\"] = 1\n"
                  "m[\"b\"] = 2\n"
                  "print(m[\"a\"])\n"
                  "print(m[\"b\"])",
                  "1\n2");
}
static void test_parity_map_len(void) {
    ASSERT_OUTPUT("let m = Map::new()\n"
                  "m[\"x\"] = 1\n"
                  "print(m.len())",
                  "1");
}

/* ── Array literal / index ── */

static void test_parity_array_literal(void) { ASSERT_OUTPUT("print([1, 2, 3])", "[1, 2, 3]"); }
static void test_parity_array_index(void) {
    ASSERT_OUTPUT("let a = [10, 20, 30]\n"
                  "print(a[1])",
                  "20");
}

/* ======================================================================
 * Error Message Diagnostics
 * ====================================================================== */

/* Test: undefined variable suggests similar name */
static void test_err_undefined_var_suggestion(void) {
    ASSERT_OUTPUT_STARTS_WITH("let get_users = 42\n"
                              "fn main() { print(get_user) }",
                              "EVAL_ERROR:undefined variable 'get_user' (did you mean 'get_users'?)");
}

/* Test: undefined variable with no similar name gives plain error */
static void test_err_undefined_var_no_suggestion(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() { print(xyzzy_does_not_exist) }",
                              "EVAL_ERROR:undefined variable 'xyzzy_does_not_exist'");
}

/* Test: method typo on Array suggests correct method */
static void test_err_method_suggestion_array(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    let arr = [1, 2, 3]\n"
                              "    arr.pussh(4)\n"
                              "}",
                              "EVAL_ERROR:");
    /* Verify the suggestion is present */
    char *out = run_capture("fn main() {\n"
                            "    let arr = [1, 2, 3]\n"
                            "    arr.pussh(4)\n"
                            "}");
    ASSERT(strstr(out, "did you mean 'push'?") != NULL);
    free(out);
}

/* Test: method typo on String suggests correct method */
static void test_err_method_suggestion_string(void) {
    char *out = run_capture("fn main() {\n"
                            "    let s = \"hello\"\n"
                            "    s.triim()\n"
                            "}");
    ASSERT(strstr(out, "did you mean 'trim'?") != NULL);
    free(out);
}

/* Test: method with no close match gives plain error */
static void test_err_method_no_suggestion(void) {
    char *out = run_capture("fn main() {\n"
                            "    let arr = [1, 2, 3]\n"
                            "    arr.xyzzy()\n"
                            "}");
    ASSERT(strstr(out, "EVAL_ERROR:") != NULL);
    ASSERT(strstr(out, "did you mean") == NULL);
    free(out);
}

/* Test: phase violation message includes variable name and thaw hint */
static void test_err_phase_violation_hint(void) {
    if (test_backend == BACKEND_TREE_WALK) return; /* tree-walk has different phase error path */
    char *out = run_capture("let arr = freeze([1, 2, 3])\n"
                            "fn main() { arr.push(4) }");
    ASSERT(strstr(out, "EVAL_ERROR:") != NULL);
    ASSERT(strstr(out, "crystal") != NULL);
    ASSERT(strstr(out, "thaw(arr)") != NULL);
    free(out);
}

/* ======================================================================
 * LAT-17: Concurrent scope/spawn edge case tests
 * ====================================================================== */

/* scope with multiple spawns writing to shared channels, sync body reads */
static void test_scope_multi_spawn_shared_channels(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ch1 = Channel::new()\n"
                  "    let ch2 = Channel::new()\n"
                  "    let ch3 = Channel::new()\n"
                  "    scope {\n"
                  "        spawn { ch1.send(freeze(10)) }\n"
                  "        spawn { ch2.send(freeze(20)) }\n"
                  "        spawn { ch3.send(freeze(30)) }\n"
                  "    }\n"
                  "    let total = ch1.recv() + ch2.recv() + ch3.recv()\n"
                  "    print(total)\n"
                  "}\n",
                  "60");
}

/* scope where sync body runs alongside spawns */
static void test_scope_sync_body_with_spawns(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ch = Channel::new()\n"
                  "    scope {\n"
                  "        spawn { ch.send(freeze(100)) }\n"
                  "        print(\"sync\")\n"
                  "    }\n"
                  "    let val = ch.recv()\n"
                  "    print(val)\n"
                  "}\n",
                  "sync\n100");
}

/* nested scope: scope inside scope */
static void test_scope_nested(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ch = Channel::new()\n"
                  "    scope {\n"
                  "        spawn {\n"
                  "            let inner_ch = Channel::new()\n"
                  "            scope {\n"
                  "                spawn { inner_ch.send(freeze(5)) }\n"
                  "            }\n"
                  "            let v = inner_ch.recv()\n"
                  "            ch.send(freeze(v * 2))\n"
                  "        }\n"
                  "    }\n"
                  "    print(ch.recv())\n"
                  "}\n",
                  "10");
}

/* scope with error in one spawn propagates */
static void test_scope_spawn_error_div_zero(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    let ch = Channel::new()\n"
                              "    scope {\n"
                              "        spawn { ch.send(freeze(42)) }\n"
                              "        spawn {\n"
                              "            let x = 1 / 0\n"
                              "            ch.send(freeze(x))\n"
                              "        }\n"
                              "    }\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* scope as expression captures return value of sync body */
static void test_scope_as_expression(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let result = scope {\n"
                  "        let a = 3\n"
                  "        let b = 7\n"
                  "        a * b\n"
                  "    }\n"
                  "    print(result)\n"
                  "}\n",
                  "21");
}

/* spawn with closure capturing outer variable */
static void test_spawn_captures_outer_variable(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ch = Channel::new()\n"
                  "    let multiplier = 5\n"
                  "    scope {\n"
                  "        spawn {\n"
                  "            let result = multiplier * 10\n"
                  "            ch.send(freeze(result))\n"
                  "        }\n"
                  "    }\n"
                  "    print(ch.recv())\n"
                  "}\n",
                  "50");
}

/* scope with many spawns all sending to one channel */
static void test_scope_many_spawns_one_channel(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ch = Channel::new()\n"
                  "    scope {\n"
                  "        spawn { ch.send(freeze(1)) }\n"
                  "        spawn { ch.send(freeze(2)) }\n"
                  "        spawn { ch.send(freeze(3)) }\n"
                  "        spawn { ch.send(freeze(4)) }\n"
                  "    }\n"
                  "    flux total = 0\n"
                  "    total = total + ch.recv()\n"
                  "    total = total + ch.recv()\n"
                  "    total = total + ch.recv()\n"
                  "    total = total + ch.recv()\n"
                  "    print(total)\n"
                  "}\n",
                  "10");
}

/* spawn with closure that captures a computed value */
static void test_spawn_closure_factory(void) {
    ASSERT_OUTPUT("fn compute(x: Int) -> Int { return x * x }\n"
                  "fn main() {\n"
                  "    let ch = Channel::new()\n"
                  "    let val = compute(7)\n"
                  "    scope {\n"
                  "        spawn { ch.send(freeze(val)) }\n"
                  "    }\n"
                  "    print(ch.recv())\n"
                  "}\n",
                  "49");
}

/* scope with spawns: sync body side effects execute, spawns complete */
static void test_scope_expr_with_spawns(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ch = Channel::new()\n"
                  "    scope {\n"
                  "        spawn { ch.send(freeze(1)) }\n"
                  "        print(\"sync_done\")\n"
                  "    }\n"
                  "    print(ch.recv())\n"
                  "}\n",
                  "sync_done\n1");
}

/* ======================================================================
 * LAT-17: Select statement tests
 * ====================================================================== */

/* select with default arm when no channel data */
static void test_select_default_arm(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ch = Channel::new()\n"
                  "    let result = select {\n"
                  "        val from ch => { val }\n"
                  "        default => { \"none\" }\n"
                  "    }\n"
                  "    print(result)\n"
                  "}\n",
                  "none");
}

/* select with timeout arm */
static void test_select_timeout_arm(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ch = Channel::new()\n"
                  "    let result = select {\n"
                  "        val from ch => { val }\n"
                  "        timeout(10) => { \"timed_out\" }\n"
                  "    }\n"
                  "    print(result)\n"
                  "}\n",
                  "timed_out");
}

/* select with channel that has data ready */
static void test_select_channel_ready(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ch = Channel::new()\n"
                  "    ch.send(freeze(42))\n"
                  "    let result = select {\n"
                  "        val from ch => { val }\n"
                  "        default => { \"none\" }\n"
                  "    }\n"
                  "    print(result)\n"
                  "}\n",
                  "42");
}

/* select with multiple channel arms */
static void test_select_multiple_channels(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ch1 = Channel::new()\n"
                  "    let ch2 = Channel::new()\n"
                  "    ch2.send(freeze(\"from_ch2\"))\n"
                  "    let result = select {\n"
                  "        v from ch1 => { v }\n"
                  "        v from ch2 => { v }\n"
                  "    }\n"
                  "    print(result)\n"
                  "}\n",
                  "from_ch2");
}

/* select inside a loop */
static void test_select_in_loop(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ch = Channel::new()\n"
                  "    ch.send(freeze(1))\n"
                  "    ch.send(freeze(2))\n"
                  "    ch.send(freeze(3))\n"
                  "    ch.close()\n"
                  "    flux sum = 0\n"
                  "    flux done = false\n"
                  "    while !done {\n"
                  "        let result = select {\n"
                  "            v from ch => { v }\n"
                  "            default => { \"done\" }\n"
                  "        }\n"
                  "        if typeof(result) == \"Int\" {\n"
                  "            sum = sum + result\n"
                  "        } else {\n"
                  "            done = true\n"
                  "        }\n"
                  "    }\n"
                  "    print(sum)\n"
                  "}\n",
                  "6");
}

/* select as expression capturing value */
static void test_select_as_expression(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    let ch = Channel::new()\n"
                  "    ch.send(freeze(\"hello\"))\n"
                  "    let msg = select {\n"
                  "        v from ch => { \"got: \" + v }\n"
                  "        default => { \"nothing\" }\n"
                  "    }\n"
                  "    print(msg)\n"
                  "}\n",
                  "got: hello");
}

/* ======================================================================
 * LAT-17: Phase edge case tests
 * ====================================================================== */

/* freeze on nested structures: array of arrays */
static void test_freeze_nested_array(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux outer = [[1, 2], [3, 4]]\n"
                  "    freeze(outer)\n"
                  "    print(phase_of(outer))\n"
                  "    print(outer)\n"
                  "}\n",
                  "crystal\n[[1, 2], [3, 4]]");
}

/* thaw and re-mutation */
static void test_thaw_and_remutate(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux arr = [1, 2, 3]\n"
                  "    freeze(arr)\n"
                  "    print(phase_of(arr))\n"
                  "    thaw(arr)\n"
                  "    print(phase_of(arr))\n"
                  "    arr.push(4)\n"
                  "    print(arr)\n"
                  "}\n",
                  "crystal\nfluid\n[1, 2, 3, 4]");
}

/* phase transition across function boundaries */
static void test_phase_across_function(void) {
    ASSERT_OUTPUT("fn make_frozen() {\n"
                  "    flux data = [10, 20]\n"
                  "    freeze(data)\n"
                  "    return data\n"
                  "}\n"
                  "fn main() {\n"
                  "    let result = make_frozen()\n"
                  "    print(phase_of(result))\n"
                  "    print(result)\n"
                  "}\n",
                  "crystal\n[10, 20]");
}

/* sublimated array blocks push */
static void test_sublimate_blocks_push_and_pop(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux items = [1, 2, 3]\n"
                              "    sublimate(items)\n"
                              "    items.push(4)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* phase pressure: no_grow blocks push on crystal value */
static void test_pressure_no_grow_on_crystal(void) {
    ASSERT_OUTPUT_STARTS_WITH("fn main() {\n"
                              "    flux data = [1, 2, 3]\n"
                              "    pressurize(data, \"no_grow\")\n"
                              "    data.push(4)\n"
                              "}\n",
                              "EVAL_ERROR:");
}

/* freeze preserves array content identity */
static void test_freeze_preserves_content(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"a\"] = 1\n"
                  "    m[\"b\"] = 2\n"
                  "    freeze(m)\n"
                  "    print(phase_of(m))\n"
                  "    print(m[\"a\"])\n"
                  "    print(m[\"b\"])\n"
                  "}\n",
                  "crystal\n1\n2");
}

/* thaw a frozen map and add keys */
static void test_thaw_frozen_map_add_keys(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"x\"] = 10\n"
                  "    freeze(m)\n"
                  "    thaw(m)\n"
                  "    m[\"y\"] = 20\n"
                  "    print(m.len())\n"
                  "}\n",
                  "2");
}

/* ======================================================================
 * LAT-17: Closure capture edge case tests
 * ====================================================================== */

/* closure captures outer variable by value at definition time */
static void test_closure_upvalue_nested_mutation(void) {
    ASSERT_OUTPUT("fn make_adder(base: Int) {\n"
                  "    return |x| { base + x }\n"
                  "}\n"
                  "fn main() {\n"
                  "    let add10 = make_adder(10)\n"
                  "    let add20 = make_adder(20)\n"
                  "    print(add10(5))\n"
                  "    print(add20(5))\n"
                  "    print(add10(100))\n"
                  "}\n",
                  "15\n25\n110");
}

/* closure capturing loop variable (classic bug) */
static void test_closure_capture_loop_var(void) {
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux fns = []\n"
                  "    for i in [0, 1, 2] {\n"
                  "        let val = i\n"
                  "        let f = |unused| { val }\n"
                  "        fns.push(f)\n"
                  "    }\n"
                  "    let f0 = fns[0]\n"
                  "    let f1 = fns[1]\n"
                  "    let f2 = fns[2]\n"
                  "    print(f0(0))\n"
                  "    print(f1(0))\n"
                  "    print(f2(0))\n"
                  "}\n",
                  "0\n1\n2");
}

/* closure captures outer value and can be called */
static void test_closure_in_spawn(void) {
    ASSERT_OUTPUT("fn make_greeter(greeting: String) {\n"
                  "    return |name| { greeting + \" \" + name }\n"
                  "}\n"
                  "fn main() {\n"
                  "    let greet = make_greeter(\"hello\")\n"
                  "    print(greet(\"world\"))\n"
                  "    print(greet(\"lattice\"))\n"
                  "}\n",
                  "hello world\nhello lattice");
}

/* recursive function (factorial) */
static void test_closure_recursive(void) {
    ASSERT_OUTPUT("fn fact(n: Int) -> Int {\n"
                  "    if n <= 1 { return 1 }\n"
                  "    return n * fact(n - 1)\n"
                  "}\n"
                  "fn main() {\n"
                  "    print(fact(5))\n"
                  "}\n",
                  "120");
}

/* counter closure (higher-order returning closure) */
static void test_closure_counter(void) {
    ASSERT_OUTPUT("fn make_counter() {\n"
                  "    flux count = 0\n"
                  "    return |_| {\n"
                  "        count = count + 1\n"
                  "        count\n"
                  "    }\n"
                  "}\n"
                  "fn main() {\n"
                  "    let counter = make_counter()\n"
                  "    print(counter(0))\n"
                  "    print(counter(0))\n"
                  "    print(counter(0))\n"
                  "}\n",
                  "1\n2\n3");
}

/* ======================================================================
 * LAT-17: Error propagation tests
 * ====================================================================== */

/* try/catch around division by zero in a function */
static void test_try_catch_in_function(void) {
    ASSERT_OUTPUT("fn risky(a: Int, b: Int) -> Int {\n"
                  "    return a / b\n"
                  "}\n"
                  "fn main() {\n"
                  "    let result = try {\n"
                  "        risky(10, 0)\n"
                  "    } catch e {\n"
                  "        \"caught: \" + e.message\n"
                  "    }\n"
                  "    print(result)\n"
                  "}\n",
                  "caught: division by zero");
}

/* error in deferred cleanup still prints deferred output */
static void test_defer_with_error(void) {
    ASSERT_OUTPUT("fn cleanup_fn() {\n"
                  "    defer { print(\"cleaned\") }\n"
                  "    print(\"before\")\n"
                  "}\n"
                  "fn main() {\n"
                  "    cleanup_fn()\n"
                  "}\n",
                  "before\ncleaned");
}

/* error propagation through multiple call frames */
static void test_error_multi_frame(void) {
    ASSERT_OUTPUT("fn inner() -> Int { return 1 / 0 }\n"
                  "fn middle() -> Int { return inner() }\n"
                  "fn outer() -> Int { return middle() }\n"
                  "fn main() {\n"
                  "    let result = try {\n"
                  "        outer()\n"
                  "    } catch e {\n"
                  "        e.message\n"
                  "    }\n"
                  "    print(result)\n"
                  "}\n",
                  "division by zero");
}

/* try/catch with conditional error generation */
static void test_try_catch_conditional_error(void) {
    ASSERT_OUTPUT("fn validate(x: Int) -> Int {\n"
                  "    if x < 0 { return 1 / 0 }\n"
                  "    return x\n"
                  "}\n"
                  "fn main() {\n"
                  "    let r = try {\n"
                  "        validate(-5)\n"
                  "        \"ok\"\n"
                  "    } catch e {\n"
                  "        \"caught: \" + e.message\n"
                  "    }\n"
                  "    print(r)\n"
                  "}\n",
                  "caught: division by zero");
}

/* ═══════════════════════════════════════════════════════════════════════
 * LAT-22: Polymorphic inline cache (PIC) tests — exercise method dispatch
 * caching through repeated and polymorphic method calls.
 * ═══════════════════════════════════════════════════════════════════════ */

/* Test: chain multiple array methods (exercises monomorphic PIC for Array type) */
static void test_pic_method_chain_array(void) {
    ASSERT_OUTPUT("let a = [5, 3, 1, 4, 2]\n"
                  "let r = a.reverse().take(3).join(\", \")\n"
                  "print(r)",
                  "2, 4, 1");
}

/* Test: chain multiple string methods (exercises monomorphic PIC for String type) */
static void test_pic_method_chain_string(void) {
    ASSERT_OUTPUT("let s = \"  Hello World  \"\n"
                  "let r = s.trim().to_lower().replace(\"world\", \"lattice\")\n"
                  "print(r)",
                  "hello lattice");
}

/* Test: call the same method in a loop (PIC should cache after first iteration) */
static void test_pic_loop_same_method(void) {
    ASSERT_OUTPUT("let items = [[1,2], [3,4,5], [6]]\n"
                  "flux result = []\n"
                  "for item in items {\n"
                  "    result.push(item.len())\n"
                  "}\n"
                  "print(result)",
                  "[2, 3, 1]");
}

/* Test: .len() on different types in the same code path (polymorphic PIC) */
static void test_pic_polymorphic_len(void) {
    ASSERT_OUTPUT("fn get_len(x: any) -> Int {\n"
                  "    return x.len()\n"
                  "}\n"
                  "print(get_len([1, 2, 3]))\n"
                  "print(get_len(\"hello\"))\n"
                  "print(get_len([1, 2, 3, 4]))\n"
                  "print(get_len(\"hi\"))",
                  "3\n5\n4\n2");
}

/* Test: struct field access + callable struct field (PIC caches NOT_BUILTIN) */
static void test_pic_struct_method_fallthrough(void) {
    ASSERT_OUTPUT("struct Greeter { name: String, greet: Fn }\n"
                  "let g = Greeter { name: \"World\", greet: |self| { \"Hello, \" + self.name } }\n"
                  "print(g.greet())",
                  "Hello, World");
}

/* Test: global array push in a loop (exercises INVOKE_GLOBAL PIC) */
static void test_pic_global_push_loop(void) {
    ASSERT_OUTPUT("flux arr = []\n"
                  "for i in 0..5 {\n"
                  "    arr.push(i * i)\n"
                  "}\n"
                  "print(arr)",
                  "[0, 1, 4, 9, 16]");
}

/* Test: mixed method chain across map/array/string (full polymorphic exercise) */
static void test_pic_mixed_chain(void) {
    ASSERT_OUTPUT("let words = [\"hello\", \"world\", \"foo\"]\n"
                  "let result = words.map(|w| { w.to_upper() }).join(\" \")\n"
                  "print(result)\n"
                  "print(words.contains(\"foo\"))\n"
                  "print(\"hello world\".contains(\"world\"))",
                  "HELLO WORLD FOO\ntrue\ntrue");
}

/* ── LAT-27: Duration/DateTime/Calendar/Timezone tests ── */

static void test_duration_create(void) {
    ASSERT_OUTPUT("let d = duration(2, 30, 15, 0)\n"
                  "print(d[\"hours\"])\n"
                  "print(d[\"minutes\"])\n"
                  "print(d[\"seconds\"])\n"
                  "print(d[\"millis\"])\n",
                  "2\n30\n15\n0");
}

static void test_duration_from_seconds(void) {
    ASSERT_OUTPUT("let d = duration_from_seconds(3661)\n"
                  "print(d[\"hours\"])\n"
                  "print(d[\"minutes\"])\n"
                  "print(d[\"seconds\"])\n",
                  "1\n1\n1");
}

static void test_duration_from_millis(void) {
    ASSERT_OUTPUT("let d = duration_from_millis(5500)\n"
                  "print(d[\"hours\"])\n"
                  "print(d[\"seconds\"])\n"
                  "print(d[\"millis\"])\n",
                  "0\n5\n500");
}

static void test_duration_add(void) {
    ASSERT_OUTPUT("let d1 = duration(1, 0, 0, 0)\n"
                  "let d2 = duration(0, 30, 0, 0)\n"
                  "let d3 = duration_add(d1, d2)\n"
                  "print(d3[\"hours\"])\n"
                  "print(d3[\"minutes\"])\n",
                  "1\n30");
}

static void test_duration_sub(void) {
    ASSERT_OUTPUT("let d1 = duration(2, 0, 0, 0)\n"
                  "let d2 = duration(0, 30, 0, 0)\n"
                  "let d3 = duration_sub(d1, d2)\n"
                  "print(d3[\"hours\"])\n"
                  "print(d3[\"minutes\"])\n",
                  "1\n30");
}

static void test_duration_to_string(void) {
    ASSERT_OUTPUT("let d = duration(2, 30, 15, 0)\n"
                  "print(duration_to_string(d))\n",
                  "2h 30m 15s");
}

static void test_duration_to_string_with_millis(void) {
    ASSERT_OUTPUT("let d = duration(0, 0, 5, 500)\n"
                  "print(duration_to_string(d))\n",
                  "0h 0m 5s 500ms");
}

static void test_duration_field_accessors(void) {
    ASSERT_OUTPUT("let d = duration(3, 45, 12, 100)\n"
                  "print(duration_hours(d))\n"
                  "print(duration_minutes(d))\n"
                  "print(duration_seconds(d))\n"
                  "print(duration_millis(d))\n",
                  "3\n45\n12\n100");
}

static void test_datetime_from_epoch(void) {
    ASSERT_OUTPUT("let dt = datetime_from_epoch(0)\n"
                  "print(dt[\"year\"])\n"
                  "print(dt[\"month\"])\n"
                  "print(dt[\"day\"])\n"
                  "print(dt[\"hour\"])\n"
                  "print(dt[\"tz_offset\"])\n",
                  "1970\n1\n1\n0\n0");
}

static void test_datetime_to_epoch(void) {
    ASSERT_OUTPUT("let dt = datetime_from_epoch(1000000)\n"
                  "let ep = datetime_to_epoch(dt)\n"
                  "print(ep)\n",
                  "1000000");
}

static void test_datetime_from_iso(void) {
    ASSERT_OUTPUT("let dt = datetime_from_iso(\"2026-02-24T10:30:00Z\")\n"
                  "print(dt[\"year\"])\n"
                  "print(dt[\"month\"])\n"
                  "print(dt[\"day\"])\n"
                  "print(dt[\"hour\"])\n"
                  "print(dt[\"minute\"])\n",
                  "2026\n2\n24\n10\n30");
}

static void test_datetime_to_iso(void) {
    ASSERT_OUTPUT("let dt = datetime_from_epoch(0)\n"
                  "print(datetime_to_iso(dt))\n",
                  "1970-01-01T00:00:00Z");
}

static void test_datetime_iso_roundtrip(void) {
    ASSERT_OUTPUT("let iso = \"2026-06-15T08:45:30Z\"\n"
                  "let dt = datetime_from_iso(iso)\n"
                  "let back = datetime_to_iso(dt)\n"
                  "print(back)\n",
                  "2026-06-15T08:45:30Z");
}

static void test_datetime_add_duration(void) {
    ASSERT_OUTPUT("let dt = datetime_from_epoch(0)\n"
                  "let dur = duration(1, 30, 0, 0)\n"
                  "let dt2 = datetime_add_duration(dt, dur)\n"
                  "print(dt2[\"hour\"])\n"
                  "print(dt2[\"minute\"])\n",
                  "1\n30");
}

static void test_datetime_sub(void) {
    ASSERT_OUTPUT("let dt1 = datetime_from_iso(\"2026-02-24T12:00:00Z\")\n"
                  "let dt2 = datetime_from_iso(\"2026-02-24T10:00:00Z\")\n"
                  "let dur = datetime_sub(dt1, dt2)\n"
                  "print(dur[\"hours\"])\n"
                  "print(dur[\"minutes\"])\n",
                  "2\n0");
}

static void test_datetime_format_map(void) {
    ASSERT_OUTPUT("let dt = datetime_from_iso(\"2026-02-24T10:30:00Z\")\n"
                  "print(datetime_format(dt, \"%Y-%m-%d\"))\n",
                  "2026-02-24");
}

static void test_datetime_to_utc(void) {
    ASSERT_OUTPUT("let dt = datetime_from_epoch(0)\n"
                  "let utc = datetime_to_utc(dt)\n"
                  "print(utc[\"year\"])\n"
                  "print(utc[\"tz_offset\"])\n",
                  "1970\n0");
}

static void test_days_in_month(void) {
    ASSERT_OUTPUT("print(days_in_month(2024, 2))\n"
                  "print(days_in_month(2025, 2))\n"
                  "print(days_in_month(2026, 1))\n"
                  "print(days_in_month(2026, 4))\n",
                  "29\n28\n31\n30");
}

static void test_day_of_week(void) {
    /* 2026-02-24 is a Tuesday = 2 */
    ASSERT_OUTPUT("print(day_of_week(2026, 2, 24))\n", "2");
}

static void test_day_of_year(void) {
    /* Jan 1 = 1, Feb 24 in non-leap = 31+24 = 55 */
    ASSERT_OUTPUT("print(day_of_year(2026, 1, 1))\n"
                  "print(day_of_year(2026, 2, 24))\n",
                  "1\n55");
}

static void test_timezone_offset(void) {
    /* Just verify it returns an integer and doesn't crash */
    ASSERT_OUTPUT("let tz = timezone_offset()\n"
                  "print(typeof(tz))\n",
                  "Int");
}

static void test_is_leap_year_extended(void) {
    ASSERT_OUTPUT("print(is_leap_year(2024))\n"
                  "print(is_leap_year(2025))\n"
                  "print(is_leap_year(2000))\n"
                  "print(is_leap_year(1900))\n",
                  "true\nfalse\ntrue\nfalse");
}

/* ======================================================================
 * LAT-25: String interning in bytecode VMs
 * ====================================================================== */

static void test_intern_string_equality(void) {
    /* String constants loaded from the constant pool should be interned,
     * enabling fast pointer-equality checks. */
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = \"hello\"\n"
                  "    let b = \"hello\"\n"
                  "    print(a == b)\n"
                  "}\n",
                  "true");
}

static void test_intern_concat_short(void) {
    /* Short string concat results (<= 64 bytes) should be interned. */
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = \"hello\" + \" \" + \"world\"\n"
                  "    let b = \"hello world\"\n"
                  "    print(a == b)\n"
                  "}\n",
                  "true");
}

static void test_intern_interpolation(void) {
    /* String interpolation (OP_CONCAT) results should work correctly. */
    ASSERT_OUTPUT("fn main() {\n"
                  "    let name = \"lattice\"\n"
                  "    let msg = \"hello ${name}\"\n"
                  "    print(msg)\n"
                  "}\n",
                  "hello lattice");
}

static void test_intern_concat_loop(void) {
    /* Repeated concat in a loop should produce correct results. */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux s = \"\"\n"
                  "    flux i = 0\n"
                  "    while i < 10 {\n"
                  "        s = s + to_string(i)\n"
                  "        i += 1\n"
                  "    }\n"
                  "    print(s)\n"
                  "}\n",
                  "0123456789");
}

static void test_intern_equality_after_concat(void) {
    /* Two strings built by different concat paths should still be equal. */
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = \"foo\" + \"bar\"\n"
                  "    let b = \"fo\" + \"obar\"\n"
                  "    print(a == b)\n"
                  "}\n",
                  "true");
}

static void test_intern_not_equal(void) {
    /* Interned strings with different content should not be equal. */
    ASSERT_OUTPUT("fn main() {\n"
                  "    let a = \"hello\"\n"
                  "    let b = \"world\"\n"
                  "    print(a != b)\n"
                  "}\n",
                  "true");
}

static void test_intern_string_methods(void) {
    /* String methods should work correctly on interned strings. */
    ASSERT_OUTPUT("fn main() {\n"
                  "    let s = \"Hello World\"\n"
                  "    print(s.len())\n"
                  "    print(s.to_upper())\n"
                  "    print(s.to_lower())\n"
                  "    print(s.contains(\"World\"))\n"
                  "}\n",
                  "11\nHELLO WORLD\nhello world\ntrue");
}

static void test_intern_map_string_keys(void) {
    /* Interned strings should work correctly as map keys. */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux m = Map::new()\n"
                  "    m[\"key\"] = 42\n"
                  "    let k = \"key\"\n"
                  "    print(m[k])\n"
                  "}\n",
                  "42");
}

static void test_intern_long_string_not_interned(void) {
    /* Long strings (> 64 bytes) should still work correctly even though
     * they won't be interned. */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let long_str = \"abcdefghijklmnopqrstuvwxyz\" + \"ABCDEFGHIJKLMNOPQRSTUVWXYZ\" + "
        "\"01234567890123456789\"\n"
        "    print(long_str.len())\n"
        "    let same = \"abcdefghijklmnopqrstuvwxyz\" + \"ABCDEFGHIJKLMNOPQRSTUVWXYZ\" + \"01234567890123456789\"\n"
        "    print(long_str == same)\n"
        "}\n",
        "72\ntrue");
}

static void test_intern_heavy_string_ops(void) {
    /* Heavy string operations: build many strings, compare them,
     * exercise both interned and non-interned paths. */
    ASSERT_OUTPUT("fn main() {\n"
                  "    flux results = []\n"
                  "    flux i = 0\n"
                  "    while i < 50 {\n"
                  "        let s = \"item_\" + to_string(i)\n"
                  "        results.push(s)\n"
                  "        i += 1\n"
                  "    }\n"
                  "    print(results.len())\n"
                  "    print(results[0])\n"
                  "    print(results[49])\n"
                  "    // Verify equality with freshly built string\n"
                  "    print(results[25] == \"item_25\")\n"
                  "}\n",
                  "50\nitem_0\nitem_49\ntrue");
}

/* ======================================================================
 * LAT-26: Type, enum variant, and keyword "did you mean?" suggestions
 * ====================================================================== */

/* Test: typo in type annotation suggests correct type name */
static void test_err_type_suggestion(void) {
    char *out = run_capture("fn foo(x: Intx) { print(x) }\n"
                            "fn main() { foo(5) }");
    ASSERT(strstr(out, "did you mean 'Int'?") != NULL);
    free(out);
}

/* Test: typo in return type annotation suggests correct type */
static void test_err_return_type_suggestion(void) {
    char *out = run_capture("fn foo(x: Int) -> Strig {\n"
                            "    return \"hello\"\n"
                            "}\n"
                            "fn main() { foo(5) }");
    ASSERT(strstr(out, "did you mean 'String'?") != NULL);
    free(out);
}

/* Test: valid type with wrong value gives no type name suggestion */
static void test_err_type_no_false_suggestion(void) {
    char *out = run_capture("fn foo(x: Int) { print(x) }\n"
                            "fn main() { foo(\"hello\") }");
    ASSERT(strstr(out, "expects type Int, got String") != NULL);
    /* Should NOT suggest a different type name since "Int" is valid */
    ASSERT(strstr(out, "did you mean") == NULL);
    free(out);
}

/* Test: unknown enum variant suggests correct variant */
static void test_err_enum_variant_suggestion(void) {
    if (test_backend != BACKEND_TREE_WALK) return; /* enum variant validation only in tree-walker */
    char *out = run_capture("enum Color { Red, Green, Blue }\n"
                            "fn main() { let c = Color::Gren; print(c) }");
    ASSERT(strstr(out, "did you mean 'Green'?") != NULL);
    free(out);
}

/* Test: unknown enum variant with no close match gives plain error */
static void test_err_enum_variant_no_suggestion(void) {
    if (test_backend != BACKEND_TREE_WALK) return; /* enum variant validation only in tree-walker */
    char *out = run_capture("enum Color { Red, Green, Blue }\n"
                            "fn main() { let c = Color::Xyz; print(c) }");
    ASSERT(strstr(out, "has no variant 'Xyz'") != NULL);
    ASSERT(strstr(out, "did you mean") == NULL);
    free(out);
}

/* Test: keyword suggestion in parser when expecting 'fn' but getting close typo */
static void test_err_keyword_suggestion(void) {
    char *out = run_capture("trait Greeter { fnn greet() }\n"
                            "fn main() { print(42) }");
    ASSERT(strstr(out, "PARSE_ERROR:") != NULL);
    ASSERT(strstr(out, "did you mean 'fn'?") != NULL);
    free(out);
}

/* Test: keyword far from any known keyword gives no suggestion */
static void test_err_keyword_no_suggestion(void) {
    char *out = run_capture("trait Greeter { xyzmethod greet() }\n"
                            "fn main() { print(42) }");
    ASSERT(strstr(out, "PARSE_ERROR:") != NULL);
    ASSERT(strstr(out, "did you mean") == NULL);
    free(out);
}

/* Test: type suggestion with Boool -> Bool */
static void test_err_type_suggestion_bool(void) {
    char *out = run_capture("fn check(x: Boool) { print(x) }\n"
                            "fn main() { check(true) }");
    ASSERT(strstr(out, "did you mean 'Bool'?") != NULL);
    free(out);
}

void register_stdlib_tests(void) {
    /* String methods */
    register_test("test_str_len", test_str_len);
    register_test("test_str_contains", test_str_contains);
    register_test("test_str_starts_with", test_str_starts_with);
    register_test("test_str_ends_with", test_str_ends_with);
    register_test("test_str_trim", test_str_trim);
    register_test("test_str_to_upper", test_str_to_upper);
    register_test("test_str_to_lower", test_str_to_lower);
    register_test("test_str_replace", test_str_replace);
    register_test("test_str_split", test_str_split);
    register_test("test_str_index_of", test_str_index_of);
    register_test("test_str_substring", test_str_substring);
    register_test("test_str_chars", test_str_chars);
    register_test("test_str_reverse", test_str_reverse);
    register_test("test_str_repeat", test_str_repeat);

    /* String indexing and concatenation */
    register_test("test_str_index", test_str_index);
    register_test("test_str_concat", test_str_concat);
    register_test("test_str_range_slice", test_str_range_slice);

    /* Built-in functions */
    register_test("test_typeof", test_typeof);
    register_test("test_phase_of", test_phase_of);
    register_test("test_to_string", test_to_string);
    register_test("test_ord", test_ord);
    register_test("test_chr", test_chr);

    /* Try/catch */
    register_test("test_try_catch_no_error", test_try_catch_no_error);
    register_test("test_try_catch_div_zero", test_try_catch_div_zero);
    register_test("test_try_catch_undefined_var", test_try_catch_undefined_var);
    register_test("test_try_catch_nested", test_try_catch_nested);

    /* Lattice eval and tokenize builtins */
    register_test("test_eval_simple", test_eval_simple);
    register_test("test_eval_string", test_eval_string);
    register_test("test_tokenize", test_tokenize);

    /* Read/write file */
    register_test("test_write_and_read_file", test_write_and_read_file);

    /* Escape sequences */
    register_test("test_escape_hex", test_escape_hex);
    register_test("test_escape_carriage_return", test_escape_carriage_return);
    register_test("test_escape_null_byte", test_escape_null_byte);
    register_test("test_escape_hex_error", test_escape_hex_error);

    /* Compound assignment */
    register_test("test_compound_add_int", test_compound_add_int);
    register_test("test_compound_add_string", test_compound_add_string);
    register_test("test_compound_sub_mul_div_mod", test_compound_sub_mul_div_mod);
    register_test("test_compound_field", test_compound_field);
    register_test("test_compound_index", test_compound_index);

    /* Array methods */
    register_test("test_array_filter", test_array_filter);
    register_test("test_array_for_each", test_array_for_each);
    register_test("test_array_find", test_array_find);
    register_test("test_array_contains", test_array_contains);
    register_test("test_array_reverse", test_array_reverse);
    register_test("test_array_enumerate", test_array_enumerate);
    register_test("test_array_sort_int", test_array_sort_int);
    register_test("test_array_sort_string", test_array_sort_string);
    register_test("test_array_sort_float", test_array_sort_float);
    register_test("test_array_sort_empty", test_array_sort_empty);
    register_test("test_array_sort_mixed_error", test_array_sort_mixed_error);
    register_test("test_array_flat_basic", test_array_flat_basic);
    register_test("test_array_flat_no_nesting", test_array_flat_no_nesting);
    register_test("test_array_flat_empty", test_array_flat_empty);
    register_test("test_array_reduce_sum", test_array_reduce_sum);
    register_test("test_array_reduce_product", test_array_reduce_product);
    register_test("test_array_reduce_string_concat", test_array_reduce_string_concat);
    register_test("test_array_reduce_empty", test_array_reduce_empty);
    register_test("test_array_slice_basic", test_array_slice_basic);
    register_test("test_array_slice_full", test_array_slice_full);
    register_test("test_array_slice_empty", test_array_slice_empty);
    register_test("test_array_slice_clamped", test_array_slice_clamped);

    /* Parsing & utility built-ins */
    register_test("test_parse_int", test_parse_int);
    register_test("test_parse_float", test_parse_float);
    register_test("test_len", test_len);
    register_test("test_print_raw", test_print_raw);
    register_test("test_eprint", test_eprint);

    /* HashMap */
    register_test("test_map_new", test_map_new);
    register_test("test_map_set_get", test_map_set_get);
    register_test("test_map_has", test_map_has);
    register_test("test_map_remove", test_map_remove);
    register_test("test_map_keys_values", test_map_keys_values);
    register_test("test_map_len", test_map_len);
    register_test("test_map_index_read_write", test_map_index_read_write);
    register_test("test_map_for_in", test_map_for_in);
    register_test("test_map_display", test_map_display);
    register_test("test_map_freeze_thaw", test_map_freeze_thaw);
    register_test("test_map_len_builtin", test_map_len_builtin);

    /* Callable struct fields */
    register_test("test_callable_struct_field", test_callable_struct_field);
    register_test("test_callable_struct_field_with_args", test_callable_struct_field_with_args);
    register_test("test_callable_struct_field_returns", test_callable_struct_field_returns);
    register_test("test_callable_struct_non_closure_field", test_callable_struct_non_closure_field);

    /* Block closures and block expressions */
    register_test("test_block_closure_basic", test_block_closure_basic);
    register_test("test_block_closure_multi_stmt", test_block_closure_multi_stmt);
    register_test("test_block_closure_in_map", test_block_closure_in_map);
    register_test("test_block_expr_standalone", test_block_expr_standalone);
    register_test("test_callable_field_block_body", test_callable_field_block_body);

    /* is_complete builtin */
    register_test("test_is_complete_true", test_is_complete_true);
    register_test("test_is_complete_unclosed_brace", test_is_complete_unclosed_brace);
    register_test("test_is_complete_unclosed_paren", test_is_complete_unclosed_paren);
    register_test("test_is_complete_balanced", test_is_complete_balanced);

    /* lat_eval persistence (REPL support) */
    register_test("test_lat_eval_var_persistence", test_lat_eval_var_persistence);
    register_test("test_lat_eval_fn_persistence", test_lat_eval_fn_persistence);
    register_test("test_lat_eval_struct_persistence", test_lat_eval_struct_persistence);
    register_test("test_lat_eval_mutable_var", test_lat_eval_mutable_var);
    register_test("test_lat_eval_version", test_lat_eval_version);

    /* require() */
    register_test("test_require_basic", test_require_basic);
    register_test("test_require_with_extension", test_require_with_extension);
    register_test("test_require_dedup", test_require_dedup);
    register_test("test_require_structs", test_require_structs);
    register_test("test_require_missing", test_require_missing);
    register_test("test_require_nested", test_require_nested);

    /* TCP networking */
    register_test("test_tcp_listen_close", test_tcp_listen_close);
    register_test("test_tcp_connect_write_read", test_tcp_connect_write_read);
    register_test("test_tcp_peer_addr", test_tcp_peer_addr);
    register_test("test_tcp_set_timeout", test_tcp_set_timeout);
    register_test("test_tcp_invalid_fd", test_tcp_invalid_fd);
    register_test("test_tcp_lattice_integration", test_tcp_lattice_integration);
    register_test("test_tcp_error_handling", test_tcp_error_handling);

    /* TLS networking */
    register_test("test_tls_available", test_tls_available);
#ifdef LATTICE_HAS_TLS
    register_test("test_tls_connect_read", test_tls_connect_read);
#endif
    register_test("test_tls_invalid_fd", test_tls_invalid_fd);
    register_test("test_tls_lattice_integration", test_tls_lattice_integration);
    register_test("test_tls_error_handling", test_tls_error_handling);

    /* JSON */
    register_test("test_json_parse_object", test_json_parse_object);
    register_test("test_json_parse_array", test_json_parse_array);
    register_test("test_json_parse_nested", test_json_parse_nested);
    register_test("test_json_parse_primitives", test_json_parse_primitives);
    register_test("test_json_stringify_basic", test_json_stringify_basic);
    register_test("test_json_stringify_array", test_json_stringify_array);
    register_test("test_json_roundtrip", test_json_roundtrip);
    register_test("test_json_parse_error", test_json_parse_error);
    register_test("test_json_stringify_error", test_json_stringify_error);

    /* Math */
    register_test("test_math_abs", test_math_abs);
    register_test("test_math_floor_ceil_round", test_math_floor_ceil_round);
    register_test("test_math_sqrt", test_math_sqrt);
    register_test("test_math_sqrt_error", test_math_sqrt_error);
    register_test("test_math_pow", test_math_pow);
    register_test("test_math_min_max", test_math_min_max);
    register_test("test_math_random", test_math_random);
    register_test("test_math_random_int", test_math_random_int);

    /* Environment variables */
    register_test("test_env_get", test_env_get);
    register_test("test_env_get_missing", test_env_get_missing);
    register_test("test_env_set_get", test_env_set_get);
    register_test("test_env_error_handling", test_env_error_handling);

    /* Time */
    register_test("test_time_now", test_time_now);
    register_test("test_time_sleep", test_time_sleep);
    register_test("test_time_error_handling", test_time_error_handling);

    /* Filesystem operations */
    register_test("test_file_exists", test_file_exists);
    register_test("test_delete_file", test_delete_file);
    register_test("test_delete_file_error", test_delete_file_error);
    register_test("test_list_dir", test_list_dir);
    register_test("test_list_dir_error", test_list_dir_error);
    register_test("test_append_file", test_append_file);
    register_test("test_append_file_creates", test_append_file_creates);
    register_test("test_fs_error_handling", test_fs_error_handling);

    /* Regex */
    register_test("test_regex_match_true", test_regex_match_true);
    register_test("test_regex_match_false", test_regex_match_false);
    register_test("test_regex_match_anchored", test_regex_match_anchored);
    register_test("test_regex_find_all_basic", test_regex_find_all_basic);
    register_test("test_regex_find_all_no_match", test_regex_find_all_no_match);
    register_test("test_regex_find_all_words", test_regex_find_all_words);
    register_test("test_regex_replace_basic", test_regex_replace_basic);
    register_test("test_regex_replace_no_match", test_regex_replace_no_match);
    register_test("test_regex_replace_whitespace", test_regex_replace_whitespace);
    register_test("test_regex_match_error", test_regex_match_error);
    register_test("test_regex_replace_delete", test_regex_replace_delete);
    register_test("test_regex_case_insensitive", test_regex_case_insensitive);
    register_test("test_regex_multiline", test_regex_multiline);
    register_test("test_regex_combined_flags", test_regex_combined_flags);
    register_test("test_regex_replace_flags", test_regex_replace_flags);
    register_test("test_regex_no_flags_backward_compat", test_regex_no_flags_backward_compat);
    register_test("test_regex_invalid_flag", test_regex_invalid_flag);

    /* format() */
    register_test("test_format_basic", test_format_basic);
    register_test("test_format_multiple", test_format_multiple);
    register_test("test_format_no_placeholders", test_format_no_placeholders);
    register_test("test_format_escaped_braces", test_format_escaped_braces);
    register_test("test_format_bool", test_format_bool);
    register_test("test_format_too_few_args", test_format_too_few_args);
    register_test("test_format_mixed_types", test_format_mixed_types);
    register_test("test_format_error_non_string_fmt", test_format_error_non_string_fmt);

    /* Crypto / Base64 */
    register_test("test_sha256_empty", test_sha256_empty);
    register_test("test_sha256_hello", test_sha256_hello);
    register_test("test_md5_empty", test_md5_empty);
    register_test("test_md5_hello", test_md5_hello);
    register_test("test_sha256_error_handling", test_sha256_error_handling);
    register_test("test_md5_error_handling", test_md5_error_handling);
    register_test("test_base64_encode_hello", test_base64_encode_hello);
    register_test("test_base64_encode_empty", test_base64_encode_empty);
    register_test("test_base64_decode_hello", test_base64_decode_hello);
    register_test("test_base64_decode_empty", test_base64_decode_empty);
    register_test("test_base64_roundtrip", test_base64_roundtrip);
    register_test("test_base64_roundtrip_longer", test_base64_roundtrip_longer);
    register_test("test_base64_encode_padding", test_base64_encode_padding);
    register_test("test_base64_decode_error", test_base64_decode_error);
    register_test("test_base64_error_handling", test_base64_error_handling);

    /* Date/time formatting */
    register_test("test_time_parse_basic", test_time_parse_basic);
    register_test("test_time_format_basic", test_time_format_basic);
    register_test("test_time_roundtrip", test_time_roundtrip);
    register_test("test_time_format_iso_date", test_time_format_iso_date);
    register_test("test_time_parse_error", test_time_parse_error);
    register_test("test_time_format_error", test_time_format_error);
    register_test("test_time_parse_type_error", test_time_parse_type_error);
    register_test("test_time_format_time_components", test_time_format_time_components);

    /* Path operations */
    register_test("test_path_join", test_path_join);
    register_test("test_path_dir", test_path_dir);
    register_test("test_path_base", test_path_base);
    register_test("test_path_ext", test_path_ext);
    register_test("test_path_error_handling", test_path_error_handling);

    /* Channels & Concurrency */
    register_test("test_channel_basic_send_recv", test_channel_basic_send_recv);
    register_test("test_scope_two_spawns_channels", test_scope_two_spawns_channels);
    register_test("test_channel_close_recv_unit", test_channel_close_recv_unit);
    register_test("test_channel_crystal_only_send", test_channel_crystal_only_send);
    register_test("test_scope_no_spawns_sequential", test_scope_no_spawns_sequential);
    register_test("test_spawn_outside_scope", test_spawn_outside_scope);
    register_test("test_channel_multiple_sends_fifo", test_channel_multiple_sends_fifo);
    register_test("test_scope_spawn_error_propagates", test_scope_spawn_error_propagates);
    register_test("test_cannot_freeze_channel", test_cannot_freeze_channel);
    register_test("test_channel_typeof", test_channel_typeof);

    /* New array methods */
    register_test("test_array_pop", test_array_pop);
    register_test("test_array_index_of", test_array_index_of);
    register_test("test_array_any_all", test_array_any_all);
    register_test("test_array_zip", test_array_zip);
    register_test("test_array_unique", test_array_unique);
    register_test("test_array_insert", test_array_insert);
    register_test("test_array_remove_at", test_array_remove_at);
    register_test("test_array_sort_by", test_array_sort_by);

    /* New map methods */
    register_test("test_map_entries", test_map_entries);
    register_test("test_map_merge", test_map_merge);
    register_test("test_map_for_each", test_map_for_each);

    /* New string methods */
    register_test("test_str_trim_start", test_str_trim_start);
    register_test("test_str_trim_end", test_str_trim_end);
    register_test("test_str_pad_left", test_str_pad_left);
    register_test("test_str_pad_right", test_str_pad_right);

    /* New math functions */
    register_test("test_math_log", test_math_log);
    register_test("test_math_log2", test_math_log2);
    register_test("test_math_log10", test_math_log10);
    register_test("test_math_trig", test_math_trig);
    register_test("test_math_atan2", test_math_atan2);
    register_test("test_math_clamp", test_math_clamp);
    register_test("test_math_pi_e", test_math_pi_e);
    register_test("test_math_inverse_trig", test_math_inverse_trig);
    register_test("test_math_exp", test_math_exp);
    register_test("test_math_sign", test_math_sign);
    register_test("test_math_gcd_lcm", test_math_gcd_lcm);
    register_test("test_is_nan_inf", test_is_nan_inf);
    register_test("test_math_lerp", test_math_lerp);
    register_test("test_math_hyperbolic", test_math_hyperbolic);

    /* New system/fs builtins */
    register_test("test_cwd_builtin", test_cwd_builtin);
    register_test("test_is_dir_file", test_is_dir_file);
    register_test("test_mkdir_builtin", test_mkdir_builtin);
    register_test("test_rename_builtin", test_rename_builtin);
    register_test("test_assert_pass", test_assert_pass);
    register_test("test_assert_fail", test_assert_fail);
    register_test("test_args_builtin", test_args_builtin);

    /* Map .filter() and .map() */
    register_test("test_map_filter", test_map_filter);
    register_test("test_map_map", test_map_map);

    /* String .count() and .is_empty() */
    register_test("test_str_count", test_str_count);
    register_test("test_str_is_empty", test_str_is_empty);

    /* New filesystem builtins */
    register_test("test_rmdir_builtin", test_rmdir_builtin);
    register_test("test_rmdir_error", test_rmdir_error);
    register_test("test_glob_builtin", test_glob_builtin);
    register_test("test_glob_no_match", test_glob_no_match);
    register_test("test_stat_builtin", test_stat_builtin);
    register_test("test_stat_dir", test_stat_dir);
    register_test("test_stat_error", test_stat_error);
    register_test("test_copy_file_builtin", test_copy_file_builtin);
    register_test("test_copy_file_error", test_copy_file_error);
    register_test("test_realpath_builtin", test_realpath_builtin);
    register_test("test_realpath_error", test_realpath_error);
    register_test("test_tempdir_builtin", test_tempdir_builtin);
    register_test("test_tempfile_builtin", test_tempfile_builtin);
    register_test("test_chmod_builtin", test_chmod_builtin);
    register_test("test_file_size_builtin", test_file_size_builtin);
    register_test("test_file_size_error", test_file_size_error);

    /* Process exec/shell builtins */
    register_test("test_exec_builtin", test_exec_builtin);
    register_test("test_shell_builtin", test_shell_builtin);
    register_test("test_shell_stderr", test_shell_stderr);
    register_test("test_exec_failure", test_exec_failure);

    /* Array: flat_map, chunk, group_by, sum, min, max, first, last */
    register_test("test_array_flat_map", test_array_flat_map);
    register_test("test_array_chunk", test_array_chunk);
    register_test("test_array_group_by", test_array_group_by);
    register_test("test_array_sum", test_array_sum);
    register_test("test_array_min_max", test_array_min_max);
    register_test("test_array_first_last", test_array_first_last);

    /* range() builtin */
    register_test("test_range_basic", test_range_basic);
    register_test("test_range_with_step", test_range_with_step);
    register_test("test_range_empty", test_range_empty);
    register_test("test_range_step_zero", test_range_step_zero);

    /* String .bytes() */
    register_test("test_str_bytes", test_str_bytes);

    /* Array .take() and .drop() */
    register_test("test_array_take", test_array_take);
    register_test("test_array_drop", test_array_drop);

    /* error() and is_error() builtins */
    register_test("test_error_builtin", test_error_builtin);

    /* System info builtins */
    register_test("test_platform_builtin", test_platform_builtin);
    register_test("test_hostname_builtin", test_hostname_builtin);
    register_test("test_pid_builtin", test_pid_builtin);

    /* env_keys builtin */
    register_test("test_env_keys", test_env_keys);

    /* URL encoding builtins */
    register_test("test_url_encode", test_url_encode);
    register_test("test_url_decode", test_url_decode);

    /* CSV builtins */
    register_test("test_csv_parse", test_csv_parse);
    register_test("test_csv_parse_quoted", test_csv_parse_quoted);
    register_test("test_csv_stringify", test_csv_stringify);
    register_test("test_csv_roundtrip", test_csv_roundtrip);

    /* Functional programming builtins */
    register_test("test_identity", test_identity);
    register_test("test_pipe", test_pipe);
    register_test("test_compose", test_compose);

    /* String interpolation */
    register_test("test_interp_simple_var", test_interp_simple_var);
    register_test("test_interp_expression", test_interp_expression);
    register_test("test_interp_multiple", test_interp_multiple);
    register_test("test_interp_escaped", test_interp_escaped);
    register_test("test_interp_adjacent", test_interp_adjacent);
    register_test("test_interp_only_expr", test_interp_only_expr);
    register_test("test_interp_method_call", test_interp_method_call);
    register_test("test_interp_nested_braces", test_interp_nested_braces);

    /* Default parameters */
    register_test("test_closure_default_param", test_closure_default_param);
    register_test("test_closure_default_param_override", test_closure_default_param_override);
    register_test("test_closure_multiple_defaults", test_closure_multiple_defaults);
    register_test("test_closure_partial_defaults", test_closure_partial_defaults);
    register_test("test_fn_default_param", test_fn_default_param);
    register_test("test_fn_default_param_override", test_fn_default_param_override);

    /* Variadic functions */
    register_test("test_closure_variadic_basic", test_closure_variadic_basic);
    register_test("test_closure_variadic_empty", test_closure_variadic_empty);
    register_test("test_closure_variadic_with_required", test_closure_variadic_with_required);
    register_test("test_fn_variadic_basic", test_fn_variadic_basic);
    register_test("test_fn_variadic_empty", test_fn_variadic_empty);
    register_test("test_closure_default_and_variadic", test_closure_default_and_variadic);
    register_test("test_fn_default_and_variadic", test_fn_default_and_variadic);
    register_test("test_dotdotdot_token", test_dotdotdot_token);

    /* Test framework */
    register_test("test_test_block_ignored", test_test_block_ignored);
    register_test("test_test_block_with_fn", test_test_block_with_fn);

    /* Pattern matching */
    register_test("test_match_literal_int", test_match_literal_int);
    register_test("test_match_wildcard", test_match_wildcard);
    register_test("test_match_string", test_match_string);
    register_test("test_match_range", test_match_range);
    register_test("test_match_guard", test_match_guard);
    register_test("test_match_binding", test_match_binding);
    register_test("test_match_negative_literal", test_match_negative_literal);
    register_test("test_match_block_body", test_match_block_body);

    /* Destructuring */
    register_test("test_destructure_array_basic", test_destructure_array_basic);
    register_test("test_destructure_array_rest", test_destructure_array_rest);
    register_test("test_destructure_array_rest_empty", test_destructure_array_rest_empty);
    register_test("test_destructure_struct_basic", test_destructure_struct_basic);
    register_test("test_destructure_map", test_destructure_map);
    register_test("test_destructure_flux", test_destructure_flux);
    register_test("test_destructure_array_from_fn", test_destructure_array_from_fn);
    register_test("test_destructure_nested_array", test_destructure_nested_array);

    /* Enums / Sum types */
    register_test("test_enum_unit_variant", test_enum_unit_variant);
    register_test("test_enum_tuple_variant", test_enum_tuple_variant);
    register_test("test_enum_equality", test_enum_equality);
    register_test("test_enum_variant_name", test_enum_variant_name);
    register_test("test_enum_enum_name", test_enum_enum_name);
    register_test("test_enum_is_variant", test_enum_is_variant);
    register_test("test_enum_payload", test_enum_payload);
    register_test("test_enum_typeof", test_enum_typeof);

    /* Set data type */
    register_test("test_set_new", test_set_new);
    register_test("test_set_from", test_set_from);
    register_test("test_set_add_has", test_set_add_has);
    register_test("test_set_remove", test_set_remove);
    register_test("test_set_to_array", test_set_to_array);
    register_test("test_set_union", test_set_union);
    register_test("test_set_intersection", test_set_intersection);
    register_test("test_set_difference", test_set_difference);
    register_test("test_set_symmetric_difference", test_set_symmetric_difference);
    register_test("test_set_symmetric_difference_disjoint", test_set_symmetric_difference_disjoint);
    register_test("test_set_symmetric_difference_identical", test_set_symmetric_difference_identical);
    register_test("test_set_symmetric_difference_empty", test_set_symmetric_difference_empty);
    register_test("test_set_subset_superset", test_set_subset_superset);
    register_test("test_set_for_in", test_set_for_in);
    register_test("test_set_duplicate_add", test_set_duplicate_add);
    register_test("test_set_typeof", test_set_typeof);

    /* HTTP mock server integration tests */
    register_test("test_http_url_parse_basic", test_http_url_parse_basic);
    register_test("test_http_url_parse_custom_port", test_http_url_parse_custom_port);
    register_test("test_http_url_parse_errors", test_http_url_parse_errors);
    register_test("test_http_execute_get", test_http_execute_get);
    register_test("test_http_execute_post_body", test_http_execute_post_body);
    register_test("test_http_execute_custom_headers", test_http_execute_custom_headers);
    register_test("test_http_execute_chunked", test_http_execute_chunked);
    register_test("test_http_execute_multi_headers", test_http_execute_multi_headers);
    register_test("test_http_execute_non_standard_port", test_http_execute_non_standard_port);
    register_test("test_http_execute_connect_refused", test_http_execute_connect_refused);
    register_test("test_http_execute_lattice_get", test_http_execute_lattice_get);
    register_test("test_http_execute_lattice_post", test_http_execute_lattice_post);

    /* HTTP client error tests */
    register_test("test_http_get_wrong_type", test_http_get_wrong_type);
    register_test("test_http_get_no_args", test_http_get_no_args);
    register_test("test_http_get_invalid_url", test_http_get_invalid_url);
    register_test("test_http_post_wrong_type", test_http_post_wrong_type);
    register_test("test_http_post_invalid_url", test_http_post_invalid_url);
    register_test("test_http_request_wrong_type", test_http_request_wrong_type);
    register_test("test_http_request_too_few_args", test_http_request_too_few_args);
    register_test("test_http_request_invalid_url", test_http_request_invalid_url);

    /* TOML */
    register_test("test_toml_parse_basic", test_toml_parse_basic);
    register_test("test_toml_parse_table", test_toml_parse_table);
    register_test("test_toml_parse_bool", test_toml_parse_bool);
    register_test("test_toml_parse_array", test_toml_parse_array);
    register_test("test_toml_stringify_basic", test_toml_stringify_basic);
    register_test("test_toml_parse_wrong_type", test_toml_parse_wrong_type);
    register_test("test_toml_stringify_wrong_type", test_toml_stringify_wrong_type);

    /* YAML */
    register_test("test_yaml_parse_basic", test_yaml_parse_basic);
    register_test("test_yaml_parse_bool", test_yaml_parse_bool);
    register_test("test_yaml_parse_sequence", test_yaml_parse_sequence);
    register_test("test_yaml_parse_nested", test_yaml_parse_nested);
    register_test("test_yaml_stringify_basic", test_yaml_stringify_basic);
    register_test("test_yaml_parse_wrong_type", test_yaml_parse_wrong_type);
    register_test("test_yaml_stringify_wrong_type", test_yaml_stringify_wrong_type);
    register_test("test_yaml_parse_flow_seq", test_yaml_parse_flow_seq);

    /* Single-quoted strings */
    register_test("test_single_quote_basic", test_single_quote_basic);
    register_test("test_single_quote_double_quotes_inside", test_single_quote_double_quotes_inside);
    register_test("test_single_quote_escaped", test_single_quote_escaped);
    register_test("test_single_quote_no_interpolation", test_single_quote_no_interpolation);
    register_test("test_single_quote_newline_escape", test_single_quote_newline_escape);
    register_test("test_single_quote_empty", test_single_quote_empty);
    register_test("test_single_quote_concat", test_single_quote_concat);
    register_test("test_single_quote_json", test_single_quote_json);

    /* Nil and ?? operator */
    register_test("test_nil_literal", test_nil_literal);
    register_test("test_nil_typeof", test_nil_typeof);
    register_test("test_nil_equality", test_nil_equality);
    register_test("test_nil_truthiness", test_nil_truthiness);
    register_test("test_nil_coalesce", test_nil_coalesce);
    register_test("test_nil_coalesce_non_nil", test_nil_coalesce_non_nil);
    register_test("test_nil_map_get", test_nil_map_get);
    register_test("test_nil_match", test_nil_match);
    register_test("test_nil_json_roundtrip", test_nil_json_roundtrip);
    register_test("test_nil_coalesce_chain", test_nil_coalesce_chain);

    /* Triple-quoted strings */
    register_test("test_triple_basic", test_triple_basic);
    register_test("test_triple_multiline", test_triple_multiline);
    register_test("test_triple_dedent", test_triple_dedent);
    register_test("test_triple_interpolation", test_triple_interpolation);
    register_test("test_triple_multiline_interpolation", test_triple_multiline_interpolation);
    register_test("test_triple_embedded_quotes", test_triple_embedded_quotes);
    register_test("test_triple_empty", test_triple_empty);
    register_test("test_triple_escape", test_triple_escape);
    register_test("test_triple_json", test_triple_json);

    /* Spread operator */
    register_test("test_spread_basic", test_spread_basic);
    register_test("test_spread_multiple", test_spread_multiple);
    register_test("test_spread_empty", test_spread_empty);
    register_test("test_spread_only", test_spread_only);
    register_test("test_spread_with_expr", test_spread_with_expr);
    register_test("test_spread_nested", test_spread_nested);

    /* Tuples */
    register_test("test_tuple_creation", test_tuple_creation);
    register_test("test_tuple_single", test_tuple_single);
    register_test("test_tuple_field_access", test_tuple_field_access);
    register_test("test_tuple_len", test_tuple_len);
    register_test("test_tuple_typeof", test_tuple_typeof);
    register_test("test_tuple_equality", test_tuple_equality);
    register_test("test_tuple_nested", test_tuple_nested);
    register_test("test_tuple_phase", test_tuple_phase);

    /* Bitwise operators */
    register_test("test_bitwise_and", test_bitwise_and);
    register_test("test_bitwise_or", test_bitwise_or);
    register_test("test_bitwise_xor", test_bitwise_xor);
    register_test("test_bitwise_not", test_bitwise_not);
    register_test("test_bitwise_lshift", test_bitwise_lshift);
    register_test("test_bitwise_rshift", test_bitwise_rshift);
    register_test("test_bitwise_compound_assign", test_bitwise_compound_assign);
    register_test("test_bitwise_precedence", test_bitwise_precedence);
    register_test("test_bitwise_shift_range", test_bitwise_shift_range);
    register_test("test_bitwise_not_double", test_bitwise_not_double);
    register_test("test_bitwise_with_negative", test_bitwise_with_negative);

    /* Import / module system */
    register_test("test_import_full", test_import_full);
    register_test("test_import_variable", test_import_variable);
    register_test("test_import_selective", test_import_selective);
    register_test("test_import_selective_variable", test_import_selective_variable);
    register_test("test_import_closure_capture", test_import_closure_capture);
    register_test("test_import_cached", test_import_cached);
    register_test("test_import_not_found", test_import_not_found);
    register_test("test_import_missing_export", test_import_missing_export);

    /* Export system */
    register_test("test_export_selective_visible", test_export_selective_visible);
    register_test("test_export_selective_variable", test_export_selective_variable);
    register_test("test_export_hidden_name", test_export_hidden_name);
    register_test("test_export_alias_filters", test_export_alias_filters);
    register_test("test_export_alias_hidden", test_export_alias_hidden);
    register_test("test_no_export_keyword_legacy", test_no_export_keyword_legacy);
    register_test("test_export_struct", test_export_struct);

    /* Transitive imports */
    register_test("test_import_transitive", test_import_transitive);
    register_test("test_import_transitive_selective", test_import_transitive_selective);

    /* Import alias edge cases */
    register_test("test_import_alias_multiple_modules", test_import_alias_multiple_modules);
    register_test("test_import_alias_same_module_twice", test_import_alias_same_module_twice);
    register_test("test_import_alias_and_selective_different_modules",
                  test_import_alias_and_selective_different_modules);

    /* Re-export */
    register_test("test_import_reexport_function", test_import_reexport_function);
    register_test("test_import_reexport_variable", test_import_reexport_variable);
    register_test("test_import_reexport_via_alias", test_import_reexport_via_alias);

    /* Selective import from module with many exports */
    register_test("test_import_selective_subset", test_import_selective_subset);
    register_test("test_import_selective_single", test_import_selective_single);
    register_test("test_import_selective_only_variables", test_import_selective_only_variables);

    /* Struct exports with methods */
    register_test("test_import_struct_with_method", test_import_struct_with_method);
    register_test("test_import_struct_method_with_operations", test_import_struct_method_with_operations);
    register_test("test_import_struct_via_alias", test_import_struct_via_alias);

    /* Enum exports (via factory functions) */
    register_test("test_import_enum_via_factory", test_import_enum_via_factory);
    register_test("test_import_enum_equality_from_module", test_import_enum_equality_from_module);
    register_test("test_import_enum_tuple_variant_from_module", test_import_enum_tuple_variant_from_module);
    register_test("test_import_enum_via_alias", test_import_enum_via_alias);

    /* Import error edge cases */
    register_test("test_import_selective_missing_from_many", test_import_selective_missing_from_many);
    register_test("test_import_hidden_function", test_import_hidden_function);
    register_test("test_import_hidden_variable", test_import_hidden_variable);
    register_test("test_import_hidden_via_alias", test_import_hidden_via_alias);

    /* Module namespace isolation */
    register_test("test_import_ns_no_collision_aliased", test_import_ns_no_collision_aliased);
    register_test("test_import_ns_qualified_struct", test_import_ns_qualified_struct);
    register_test("test_import_ns_qualified_enum", test_import_ns_qualified_enum);
    register_test("test_import_ns_qualified_struct_error", test_import_ns_qualified_struct_error);
    register_test("test_import_ns_qualified_enum_error", test_import_ns_qualified_enum_error);
    register_test("test_import_ns_selective_no_collision", test_import_ns_selective_no_collision);
    register_test("test_import_ns_backward_compat", test_import_ns_backward_compat);

    /* No export keyword (legacy behavior) */
    register_test("test_no_export_all_functions_visible", test_no_export_all_functions_visible);
    register_test("test_no_export_variable_visible", test_no_export_variable_visible);

    /* Mixed functions and variables exported */
    register_test("test_mixed_exports_via_alias", test_mixed_exports_via_alias);
    register_test("test_mixed_exports_selective", test_mixed_exports_selective);
    register_test("test_mixed_exports_hidden_not_in_alias", test_mixed_exports_hidden_not_in_alias);

    /* repr() builtin */
    register_test("test_repr_int", test_repr_int);
    register_test("test_repr_string", test_repr_string);
    register_test("test_repr_array", test_repr_array);
    register_test("test_repr_struct_default", test_repr_struct_default);
    register_test("test_repr_struct_custom", test_repr_struct_custom);
    register_test("test_repr_struct_custom_non_string", test_repr_struct_custom_non_string);
    register_test("test_repr_nil", test_repr_nil);
    register_test("test_repr_bool", test_repr_bool);

    /* Native extensions (require_ext) */
    register_test("test_require_ext_missing", test_require_ext_missing);
    register_test("test_require_ext_wrong_type", test_require_ext_wrong_type);
    register_test("test_require_ext_no_args", test_require_ext_no_args);
    register_test("test_require_ext_empty_string", test_require_ext_empty_string);
    register_test("test_require_ext_path_traversal", test_require_ext_path_traversal);
    register_test("test_require_ext_backslash_path", test_require_ext_backslash_path);
    register_test("test_require_ext_too_many_args", test_require_ext_too_many_args);
    register_test("test_require_ext_bool_arg", test_require_ext_bool_arg);
    register_test("test_require_ext_nil_arg", test_require_ext_nil_arg);
    register_test("test_require_ext_array_arg", test_require_ext_array_arg);
    register_test("test_require_ext_double_load", test_require_ext_double_load);
    register_test("test_require_ext_not_a_dylib", test_require_ext_not_a_dylib);
    register_test("test_require_ext_error_message_contains_name", test_require_ext_error_message_contains_name);

    /* SQLite extension */
    register_test("test_sqlite_open_close", test_sqlite_open_close);
    register_test("test_sqlite_status", test_sqlite_status);
    register_test("test_sqlite_exec_create", test_sqlite_exec_create);
    register_test("test_sqlite_exec_insert", test_sqlite_exec_insert);
    register_test("test_sqlite_query_basic", test_sqlite_query_basic);
    register_test("test_sqlite_query_types", test_sqlite_query_types);
    register_test("test_sqlite_query_empty", test_sqlite_query_empty);
    register_test("test_sqlite_exec_error", test_sqlite_exec_error);
    register_test("test_sqlite_query_error", test_sqlite_query_error);
    register_test("test_sqlite_close_invalid", test_sqlite_close_invalid);
    register_test("test_sqlite_multiple_tables", test_sqlite_multiple_tables);

    /* SQLite parameterized queries */
    register_test("test_sqlite_param_query", test_sqlite_param_query);
    register_test("test_sqlite_param_exec", test_sqlite_param_exec);
    register_test("test_sqlite_param_types", test_sqlite_param_types);
    register_test("test_sqlite_last_insert_rowid", test_sqlite_last_insert_rowid);

    /* Struct reflection builtins */
    register_test("test_struct_name", test_struct_name);
    register_test("test_struct_fields", test_struct_fields);
    register_test("test_struct_to_map", test_struct_to_map);
    register_test("test_struct_from_map", test_struct_from_map);
    register_test("test_struct_from_map_missing", test_struct_from_map_missing);
    register_test("test_struct_from_map_error", test_struct_from_map_error);

    /* Phase constraints */
    register_test("test_phase_constraint_flux_accepts_fluid", test_phase_constraint_flux_accepts_fluid);
    register_test("test_phase_constraint_flux_rejects_crystal", test_phase_constraint_flux_rejects_crystal);
    register_test("test_phase_constraint_fix_accepts_crystal", test_phase_constraint_fix_accepts_crystal);
    register_test("test_phase_constraint_fix_rejects_fluid", test_phase_constraint_fix_rejects_fluid);
    register_test("test_phase_constraint_unphased_accepts_any", test_phase_constraint_unphased_accepts_any);
    register_test("test_phase_constraint_flux_keyword_syntax", test_phase_constraint_flux_keyword_syntax);
    register_test("test_phase_constraint_fix_keyword_syntax", test_phase_constraint_fix_keyword_syntax);

    /* Phase-dependent dispatch */
    register_test("test_phase_dispatch_fluid_to_flux", test_phase_dispatch_fluid_to_flux);
    register_test("test_phase_dispatch_crystal_to_fix", test_phase_dispatch_crystal_to_fix);
    register_test("test_phase_dispatch_no_match_error", test_phase_dispatch_no_match_error);
    register_test("test_phase_dispatch_same_sig_replaces", test_phase_dispatch_same_sig_replaces);
    register_test("test_phase_dispatch_unphased_fallback", test_phase_dispatch_unphased_fallback);

    /* Crystallization contracts */
    register_test("test_contract_basic_pass", test_contract_basic_pass);
    register_test("test_contract_basic_fail", test_contract_basic_fail);
    register_test("test_contract_map_validation", test_contract_map_validation);
    register_test("test_contract_map_fail", test_contract_map_fail);
    register_test("test_contract_no_contract_compat", test_contract_no_contract_compat);
    register_test("test_contract_array", test_contract_array);
    register_test("test_contract_nonident_expr", test_contract_nonident_expr);
    register_test("test_contract_error_message", test_contract_error_message);

    /* Phase propagation (bonds) */
    register_test("test_bond_basic", test_bond_basic);
    register_test("test_bond_multiple_deps", test_bond_multiple_deps);
    register_test("test_unbond", test_unbond);
    register_test("test_bond_already_frozen_error", test_bond_already_frozen_error);
    register_test("test_bond_phase_of_after_cascade", test_bond_phase_of_after_cascade);
    register_test("test_bond_transitive", test_bond_transitive);
    register_test("test_bond_non_ident_error", test_bond_non_ident_error);
    register_test("test_bond_undefined_error", test_bond_undefined_error);

    /* Phase reactions */
    register_test("test_react_freeze_fires", test_react_freeze_fires);
    register_test("test_react_thaw_fires", test_react_thaw_fires);
    register_test("test_react_value_passed", test_react_value_passed);
    register_test("test_react_multiple_callbacks", test_react_multiple_callbacks);
    register_test("test_react_anneal_fires", test_react_anneal_fires);
    register_test("test_react_cascade_fires", test_react_cascade_fires);
    register_test("test_unreact_removes", test_unreact_removes);
    register_test("test_react_error_propagates", test_react_error_propagates);

    /* Phase history / temporal values */
    register_test("test_track_phases_basic", test_track_phases_basic);
    register_test("test_rewind_basic", test_rewind_basic);
    register_test("test_track_untracked_empty", test_track_untracked_empty);
    register_test("test_track_freeze_thaw", test_track_freeze_thaw);
    register_test("test_rewind_out_of_bounds", test_rewind_out_of_bounds);
    register_test("test_track_different_types", test_track_different_types);
    register_test("test_track_undefined_error", test_track_undefined_error);
    register_test("test_phases_output_format", test_phases_output_format);

    /* history() and identifier arguments */
    register_test("test_history_basic", test_history_basic);
    register_test("test_history_has_line_and_fn", test_history_has_line_and_fn);
    register_test("test_history_ident_arg", test_history_ident_arg);
    register_test("test_track_ident_arg", test_track_ident_arg);
    register_test("test_history_untracked_empty", test_history_untracked_empty);
    register_test("test_phases_still_works_with_string", test_phases_still_works_with_string);
    register_test("test_rewind_ident_arg", test_rewind_ident_arg);
    register_test("test_phases_has_line_and_fn", test_phases_has_line_and_fn);

    /* Annealing */
    register_test("test_anneal_basic", test_anneal_basic);
    register_test("test_anneal_stays_crystal", test_anneal_stays_crystal);
    register_test("test_anneal_map", test_anneal_map);
    register_test("test_anneal_non_crystal_error", test_anneal_non_crystal_error);
    register_test("test_anneal_expr_target", test_anneal_expr_target);
    register_test("test_anneal_closure_error", test_anneal_closure_error);

    /* Partial Crystallization */
    register_test("test_partial_freeze_struct_field", test_partial_freeze_struct_field);
    register_test("test_partial_freeze_struct_error", test_partial_freeze_struct_error);
    register_test("test_partial_freeze_map_key", test_partial_freeze_map_key);
    register_test("test_partial_freeze_map_key_error", test_partial_freeze_map_key_error);
    register_test("test_partial_freeze_full_overrides", test_partial_freeze_full_overrides);
    register_test("test_partial_freeze_contract", test_partial_freeze_contract);

    /* Phase Patterns in Match */
    register_test("test_phase_match_crystal_wildcard", test_phase_match_crystal_wildcard);
    register_test("test_phase_match_fluid_wildcard", test_phase_match_fluid_wildcard);
    register_test("test_phase_match_literal", test_phase_match_literal);
    register_test("test_phase_match_binding", test_phase_match_binding);
    register_test("test_phase_match_no_match", test_phase_match_no_match);
    register_test("test_phase_match_unqualified_any", test_phase_match_unqualified_any);

    /* lib/test.lat — Test runner library */
    register_test("test_lib_test_assert_eq_pass", test_lib_test_assert_eq_pass);
    register_test("test_lib_test_assert_eq_fail", test_lib_test_assert_eq_fail);
    register_test("test_lib_test_assert_neq", test_lib_test_assert_neq);
    register_test("test_lib_test_assert_gt", test_lib_test_assert_gt);
    register_test("test_lib_test_assert_near", test_lib_test_assert_near);
    register_test("test_lib_test_assert_contains", test_lib_test_assert_contains);
    register_test("test_lib_test_assert_throws", test_lib_test_assert_throws);
    register_test("test_lib_test_assert_type", test_lib_test_assert_type);
    register_test("test_lib_test_assert_nil", test_lib_test_assert_nil);
    register_test("test_lib_test_describe_it", test_lib_test_describe_it);
    register_test("test_lib_test_describe_suite", test_lib_test_describe_suite);
    register_test("test_lib_test_run_pass", test_lib_test_run_pass);
    register_test("test_lib_test_run_fail", test_lib_test_run_fail);

    /* lib/dotenv.lat — Dotenv library */
    register_test("test_lib_dotenv_parse_basic", test_lib_dotenv_parse_basic);
    register_test("test_lib_dotenv_parse_double_quoted", test_lib_dotenv_parse_double_quoted);
    register_test("test_lib_dotenv_parse_single_quoted", test_lib_dotenv_parse_single_quoted);
    register_test("test_lib_dotenv_parse_comments", test_lib_dotenv_parse_comments);
    register_test("test_lib_dotenv_parse_inline_comment", test_lib_dotenv_parse_inline_comment);
    register_test("test_lib_dotenv_parse_export", test_lib_dotenv_parse_export);
    register_test("test_lib_dotenv_parse_variable_expansion", test_lib_dotenv_parse_variable_expansion);
    register_test("test_lib_dotenv_parse_escape_sequences", test_lib_dotenv_parse_escape_sequences);
    register_test("test_lib_dotenv_parse_whitespace", test_lib_dotenv_parse_whitespace);
    register_test("test_lib_dotenv_parse_empty", test_lib_dotenv_parse_empty);

    /* lib/validate.lat — Validation library */
    register_test("test_lib_validate_string_valid", test_lib_validate_string_valid);
    register_test("test_lib_validate_string_invalid", test_lib_validate_string_invalid);
    register_test("test_lib_validate_number_valid", test_lib_validate_number_valid);
    register_test("test_lib_validate_number_min_max", test_lib_validate_number_min_max);
    register_test("test_lib_validate_string_min_max_len", test_lib_validate_string_min_max_len);
    register_test("test_lib_validate_boolean", test_lib_validate_boolean);
    register_test("test_lib_validate_optional", test_lib_validate_optional);
    register_test("test_lib_validate_required_nil", test_lib_validate_required_nil);
    register_test("test_lib_validate_one_of", test_lib_validate_one_of);
    register_test("test_lib_validate_array", test_lib_validate_array);
    register_test("test_lib_validate_array_item_errors", test_lib_validate_array_item_errors);
    register_test("test_lib_validate_object", test_lib_validate_object);
    register_test("test_lib_validate_object_errors", test_lib_validate_object_errors);
    register_test("test_lib_validate_default_val", test_lib_validate_default_val);
    register_test("test_lib_validate_integer", test_lib_validate_integer);

    /* lib/fn.lat — Functional collections library */
    register_test("test_lib_fn_range", test_lib_fn_range);
    register_test("test_lib_fn_range_step", test_lib_fn_range_step);
    register_test("test_lib_fn_from_array", test_lib_fn_from_array);
    register_test("test_lib_fn_fmap", test_lib_fn_fmap);
    register_test("test_lib_fn_select", test_lib_fn_select);
    register_test("test_lib_fn_take", test_lib_fn_take);
    register_test("test_lib_fn_drop", test_lib_fn_drop);
    register_test("test_lib_fn_take_while", test_lib_fn_take_while);
    register_test("test_lib_fn_zip", test_lib_fn_zip);
    register_test("test_lib_fn_fold", test_lib_fn_fold);
    register_test("test_lib_fn_count", test_lib_fn_count);
    register_test("test_lib_fn_repeat_take", test_lib_fn_repeat_take);
    register_test("test_lib_fn_iterate", test_lib_fn_iterate);
    register_test("test_lib_fn_result_ok", test_lib_fn_result_ok);
    register_test("test_lib_fn_result_err", test_lib_fn_result_err);
    register_test("test_lib_fn_try_fn", test_lib_fn_try_fn);
    register_test("test_lib_fn_map_result", test_lib_fn_map_result);
    register_test("test_lib_fn_partial", test_lib_fn_partial);
    register_test("test_lib_fn_comp", test_lib_fn_comp);
    register_test("test_lib_fn_apply_n", test_lib_fn_apply_n);
    register_test("test_lib_fn_flip", test_lib_fn_flip);
    register_test("test_lib_fn_constant", test_lib_fn_constant);
    register_test("test_lib_fn_group_by", test_lib_fn_group_by);
    register_test("test_lib_fn_partition", test_lib_fn_partition);
    register_test("test_lib_fn_frequencies", test_lib_fn_frequencies);
    register_test("test_lib_fn_chunk", test_lib_fn_chunk);
    register_test("test_lib_fn_flatten", test_lib_fn_flatten);
    register_test("test_lib_fn_uniq_by", test_lib_fn_uniq_by);
    register_test("test_lib_fn_chain", test_lib_fn_chain);

    /* Phase system v0.2.8 features */
    /* Feature 1: crystallize */
    register_test("test_crystallize_basic", test_crystallize_basic);
    register_test("test_crystallize_already_crystal", test_crystallize_already_crystal);
    register_test("test_crystallize_nested", test_crystallize_nested);
    /* Feature 2: freeze except (defects) */
    register_test("test_freeze_except_struct", test_freeze_except_struct);
    register_test("test_freeze_except_struct_blocks_frozen_field", test_freeze_except_struct_blocks_frozen_field);
    register_test("test_freeze_except_map", test_freeze_except_map);
    /* Feature 3: seed / grow */
    register_test("test_seed_grow_basic", test_seed_grow_basic);
    register_test("test_seed_contract_fail", test_seed_contract_fail);
    register_test("test_unseed", test_unseed);
    register_test("test_seed_validates_on_freeze", test_seed_validates_on_freeze);
    /* Feature 4: sublimation */
    register_test("test_sublimate_array_no_push", test_sublimate_array_no_push);
    register_test("test_sublimate_map_no_set", test_sublimate_map_no_set);
    register_test("test_sublimate_thaw_restores", test_sublimate_thaw_restores);
    register_test("test_sublimate_phase_of", test_sublimate_phase_of);
    register_test("test_sublimate_array_no_pop", test_sublimate_array_no_pop);
    /* Feature 5: phase pressure */
    register_test("test_pressure_no_grow_blocks_push", test_pressure_no_grow_blocks_push);
    register_test("test_pressure_no_grow_allows_index_assign", test_pressure_no_grow_allows_index_assign);
    register_test("test_pressure_no_shrink_blocks_pop", test_pressure_no_shrink_blocks_pop);
    register_test("test_pressure_no_resize_blocks_both", test_pressure_no_resize_blocks_both);
    register_test("test_depressurize", test_depressurize);
    register_test("test_pressure_of_returns_mode", test_pressure_of_returns_mode);
    register_test("test_pressure_of_returns_nil", test_pressure_of_returns_nil);
    /* Feature 6: bond strategies */
    register_test("test_bond_mirror_default", test_bond_mirror_default);
    register_test("test_bond_inverse", test_bond_inverse);
    register_test("test_bond_gate_fails_when_dep_not_crystal", test_bond_gate_fails_when_dep_not_crystal);
    register_test("test_bond_gate_succeeds_when_dep_crystal", test_bond_gate_succeeds_when_dep_crystal);
    register_test("test_bond_multiple_deps_strategy", test_bond_multiple_deps_strategy);
    /* Feature 7: alloys */
    register_test("test_alloy_fix_field_rejects_mutation", test_alloy_fix_field_rejects_mutation);
    register_test("test_alloy_flux_field_allows_mutation", test_alloy_flux_field_allows_mutation);
    register_test("test_alloy_mixed_phases", test_alloy_mixed_phases);

    /* Ref type */
    register_test("test_ref_new", test_ref_new);
    register_test("test_ref_get_set", test_ref_get_set);
    register_test("test_ref_shared", test_ref_shared);
    register_test("test_ref_shared_map", test_ref_shared_map);
    register_test("test_ref_map_proxy_index", test_ref_map_proxy_index);
    register_test("test_ref_map_proxy_methods", test_ref_map_proxy_methods);
    register_test("test_ref_array_proxy", test_ref_array_proxy);
    register_test("test_ref_equality", test_ref_equality);
    register_test("test_ref_deref", test_ref_deref);
    register_test("test_ref_inner_type", test_ref_inner_type);
    register_test("test_ref_freeze", test_ref_freeze);
    register_test("test_ref_display", test_ref_display);

    /* Bytecode serialization (.latc) */
    register_test("test_latc_round_trip_int", test_latc_round_trip_int);
    register_test("test_latc_round_trip_string", test_latc_round_trip_string);
    register_test("test_latc_round_trip_closure", test_latc_round_trip_closure);
    register_test("test_latc_round_trip_nested", test_latc_round_trip_nested);
    register_test("test_latc_round_trip_program", test_latc_round_trip_program);
    register_test("test_latc_invalid_magic", test_latc_invalid_magic);
    register_test("test_latc_truncated", test_latc_truncated);
    register_test("test_latc_file_save_load", test_latc_file_save_load);

    /* RegVM bytecode serialization (.rlatc) */
    register_test("test_rlatc_round_trip_int", test_rlatc_round_trip_int);
    register_test("test_rlatc_round_trip_string", test_rlatc_round_trip_string);
    register_test("test_rlatc_round_trip_closure", test_rlatc_round_trip_closure);
    register_test("test_rlatc_round_trip_nested", test_rlatc_round_trip_nested);
    register_test("test_rlatc_round_trip_program", test_rlatc_round_trip_program);
    register_test("test_rlatc_invalid_magic", test_rlatc_invalid_magic);
    register_test("test_rlatc_truncated", test_rlatc_truncated);
    register_test("test_rlatc_file_save_load", test_rlatc_file_save_load);
    register_test("test_rlatc_round_trip_scope_spawn", test_rlatc_round_trip_scope_spawn);
    register_test("test_rlatc_round_trip_scope_sync", test_rlatc_round_trip_scope_sync);
    register_test("test_rlatc_round_trip_select", test_rlatc_round_trip_select);
    register_test("test_rlatc_scope_file_save_load", test_rlatc_scope_file_save_load);

    /* Built-in stdlib modules */
    register_test("test_builtin_math_sin", test_builtin_math_sin);
    register_test("test_builtin_math_pi", test_builtin_math_pi);
    register_test("test_builtin_math_alias", test_builtin_math_alias);
    register_test("test_builtin_fs_exists", test_builtin_fs_exists);
    register_test("test_builtin_json_parse", test_builtin_json_parse);
    register_test("test_builtin_path_join", test_builtin_path_join);
    register_test("test_builtin_time_now", test_builtin_time_now);
    register_test("test_builtin_regex_match", test_builtin_regex_match);
    register_test("test_builtin_os_platform", test_builtin_os_platform);
    register_test("test_builtin_crypto_base64", test_builtin_crypto_base64);
    register_test("test_builtin_legacy_sin", test_builtin_legacy_sin);
    register_test("test_builtin_full_module_access", test_builtin_full_module_access);

    /* .length() alias */
    register_test("test_length_alias_array", test_length_alias_array);
    register_test("test_length_alias_string", test_length_alias_string);
    register_test("test_length_alias_buffer", test_length_alias_buffer);

    /* Crypto: SHA-512, HMAC-SHA256, random_bytes */
    register_test("test_sha512_empty", test_sha512_empty);
    register_test("test_sha512_hello", test_sha512_hello);
    register_test("test_hmac_sha256_basic", test_hmac_sha256_basic);
    register_test("test_random_bytes_length", test_random_bytes_length);

    /* Buffer read methods */
    register_test("test_buffer_read_i8", test_buffer_read_i8);
    register_test("test_buffer_read_f32", test_buffer_read_f32);

    /* String transforms */
    register_test("test_str_snake_case", test_str_snake_case);
    register_test("test_str_camel_case", test_str_camel_case);
    register_test("test_str_title_case", test_str_title_case);
    register_test("test_str_capitalize", test_str_capitalize);
    register_test("test_str_kebab_case", test_str_kebab_case);

    /* Date/time components */
    register_test("test_time_year", test_time_year);
    register_test("test_time_month", test_time_month);
    register_test("test_is_leap_year", test_is_leap_year);

    /* Nested index assignment */
    register_test("test_nested_index_assign", test_nested_index_assign);
    register_test("test_nested_index_assign_3d", test_nested_index_assign_3d);

    /* .length() alias for Map and Tuple */
    register_test("test_length_alias_map", test_length_alias_map);
    register_test("test_length_alias_tuple", test_length_alias_tuple);

    /* compose() multi-call and chained */
    register_test("test_compose_multi_call", test_compose_multi_call);
    register_test("test_compose_chained", test_compose_chained);

    /* String concat in loop */
    register_test("test_string_concat_loop", test_string_concat_loop);

    /* to_int / to_float */
    register_test("test_to_int_from_float", test_to_int_from_float);
    register_test("test_to_int_from_string", test_to_int_from_string);
    register_test("test_to_float_from_int", test_to_float_from_int);
    register_test("test_to_float_from_string", test_to_float_from_string);

    /* float_to_bits / bits_to_float */
    register_test("test_float_bits_roundtrip", test_float_bits_roundtrip);
    register_test("test_float_bits_zero", test_float_bits_zero);

    /* panic() */
    register_test("test_panic_message", test_panic_message);

    /* Phase system extended tests */
    /* Crystallization contracts */
    register_test("test_freeze_contract_passes", test_freeze_contract_passes);
    register_test("test_freeze_contract_rejects", test_freeze_contract_rejects);
    register_test("test_freeze_contract_with_string", test_freeze_contract_with_string);
    register_test("test_freeze_contract_rejects_empty_string", test_freeze_contract_rejects_empty_string);
    register_test("test_freeze_contract_with_array", test_freeze_contract_with_array);
    register_test("test_freeze_contract_checks_array_content", test_freeze_contract_checks_array_content);
    register_test("test_freeze_contract_error_message_propagated", test_freeze_contract_error_message_propagated);
    /* Phase reactions chaining */
    register_test("test_react_ordering_preserved", test_react_ordering_preserved);
    register_test("test_react_freeze_and_thaw_sequence", test_react_freeze_and_thaw_sequence);
    register_test("test_react_on_bond_cascade", test_react_on_bond_cascade);
    register_test("test_react_value_changes_between_events", test_react_value_changes_between_events);
    register_test("test_react_anneal_reaction_fires", test_react_anneal_reaction_fires);
    /* Bond strategies */
    register_test("test_bond_mirror_cascades_freeze", test_bond_mirror_cascades_freeze);
    register_test("test_bond_inverse_thaws_frozen_dep", test_bond_inverse_thaws_frozen_dep);
    register_test("test_bond_inverse_skips_fluid_dep", test_bond_inverse_skips_fluid_dep);
    register_test("test_bond_gate_blocks_freeze_when_dep_fluid", test_bond_gate_blocks_freeze_when_dep_fluid);
    register_test("test_bond_gate_allows_when_dep_crystal", test_bond_gate_allows_when_dep_crystal);
    register_test("test_bond_transitive_three_levels", test_bond_transitive_three_levels);
    register_test("test_bond_mirror_with_reaction", test_bond_mirror_with_reaction);
    /* Seed/grow */
    register_test("test_seed_grow_contract_validates", test_seed_grow_contract_validates);
    register_test("test_seed_grow_fails_on_invalid", test_seed_grow_fails_on_invalid);
    register_test("test_seed_multiple_contracts", test_seed_multiple_contracts);
    register_test("test_seed_multiple_one_fails", test_seed_multiple_one_fails);
    register_test("test_unseed_removes_contract", test_unseed_removes_contract);
    register_test("test_seed_validates_on_direct_freeze", test_seed_validates_on_direct_freeze);
    register_test("test_seed_with_map_contract", test_seed_with_map_contract);
    /* Alloy structs */
    register_test("test_alloy_fix_field_is_crystal_on_creation", test_alloy_fix_field_is_crystal_on_creation);
    register_test("test_alloy_fix_field_rejects_different_value", test_alloy_fix_field_rejects_different_value);
    register_test("test_alloy_multiple_fix_fields", test_alloy_multiple_fix_fields);
    register_test("test_alloy_multiple_flux_fields", test_alloy_multiple_flux_fields);
    register_test("test_alloy_read_fix_field", test_alloy_read_fix_field);
    /* Sublimation */
    register_test("test_sublimate_array_index_assign_blocked", test_sublimate_array_index_assign_blocked);
    register_test("test_sublimate_allows_read", test_sublimate_allows_read);
    register_test("test_sublimate_map_allows_read", test_sublimate_map_allows_read);
    register_test("test_sublimate_struct_field_blocked", test_sublimate_struct_field_blocked);
    register_test("test_sublimate_thaw_then_push", test_sublimate_thaw_then_push);
    register_test("test_sublimate_fires_reaction", test_sublimate_fires_reaction);
    /* Phase pressure */
    register_test("test_pressure_no_grow_blocks_insert", test_pressure_no_grow_blocks_insert);
    register_test("test_pressure_no_grow_allows_pop", test_pressure_no_grow_allows_pop);
    register_test("test_pressure_no_shrink_allows_push", test_pressure_no_shrink_allows_push);
    register_test("test_pressure_no_shrink_blocks_remove_at", test_pressure_no_shrink_blocks_remove_at);
    register_test("test_pressure_no_resize_blocks_pop", test_pressure_no_resize_blocks_pop);
    register_test("test_pressure_no_resize_allows_index_assign", test_pressure_no_resize_allows_index_assign);
    register_test("test_depressurize_then_push", test_depressurize_then_push);
    register_test("test_pressure_of_after_change", test_pressure_of_after_change);

    /* Cross-backend parity: Array higher-order methods */
    register_test("test_parity_array_map_basic", test_parity_array_map_basic);
    register_test("test_parity_array_map_empty", test_parity_array_map_empty);
    register_test("test_parity_array_map_strings", test_parity_array_map_strings);
    register_test("test_parity_array_filter_basic", test_parity_array_filter_basic);
    register_test("test_parity_array_filter_none", test_parity_array_filter_none);
    register_test("test_parity_array_filter_all", test_parity_array_filter_all);
    register_test("test_parity_array_reduce_sum", test_parity_array_reduce_sum);
    register_test("test_parity_array_reduce_string", test_parity_array_reduce_string);
    register_test("test_parity_array_reduce_product", test_parity_array_reduce_product);
    register_test("test_parity_array_each_side_effect", test_parity_array_each_side_effect);
    register_test("test_parity_array_sort_default", test_parity_array_sort_default);
    register_test("test_parity_array_sort_strings", test_parity_array_sort_strings);
    register_test("test_parity_array_sort_already_sorted", test_parity_array_sort_already_sorted);
    register_test("test_parity_array_find_present", test_parity_array_find_present);
    register_test("test_parity_array_find_absent", test_parity_array_find_absent);
    register_test("test_parity_array_any_true", test_parity_array_any_true);
    register_test("test_parity_array_any_false", test_parity_array_any_false);
    register_test("test_parity_array_all_true", test_parity_array_all_true);
    register_test("test_parity_array_all_false", test_parity_array_all_false);
    register_test("test_parity_array_flat_map", test_parity_array_flat_map);
    register_test("test_parity_array_sort_by", test_parity_array_sort_by);
    register_test("test_parity_array_group_by", test_parity_array_group_by);
    register_test("test_parity_array_for_each", test_parity_array_for_each);

    /* Cross-backend parity: Array non-closure methods */
    register_test("test_parity_array_push_pop", test_parity_array_push_pop);
    register_test("test_parity_array_reverse", test_parity_array_reverse);
    register_test("test_parity_array_contains", test_parity_array_contains);
    register_test("test_parity_array_index_of", test_parity_array_index_of);
    register_test("test_parity_array_slice", test_parity_array_slice);
    register_test("test_parity_array_join", test_parity_array_join);
    register_test("test_parity_array_unique", test_parity_array_unique);
    register_test("test_parity_array_zip", test_parity_array_zip);
    register_test("test_parity_array_chunk", test_parity_array_chunk);
    register_test("test_parity_array_take", test_parity_array_take);
    register_test("test_parity_array_drop", test_parity_array_drop);
    register_test("test_parity_array_flat", test_parity_array_flat);
    register_test("test_parity_array_first_last", test_parity_array_first_last);
    register_test("test_parity_array_sum", test_parity_array_sum);
    register_test("test_parity_array_min_max", test_parity_array_min_max);
    register_test("test_parity_array_insert", test_parity_array_insert);
    register_test("test_parity_array_remove_at", test_parity_array_remove_at);
    register_test("test_parity_array_enumerate", test_parity_array_enumerate);

    /* Cross-backend parity: Array method chaining */
    register_test("test_parity_array_map_filter_chain", test_parity_array_map_filter_chain);
    register_test("test_parity_array_filter_map_chain", test_parity_array_filter_map_chain);
    register_test("test_parity_array_map_reduce_chain", test_parity_array_map_reduce_chain);

    /* Cross-backend parity: String methods */
    register_test("test_parity_str_replace", test_parity_str_replace);
    register_test("test_parity_str_split", test_parity_str_split);
    register_test("test_parity_str_split_empty", test_parity_str_split_empty);
    register_test("test_parity_str_chars", test_parity_str_chars);
    register_test("test_parity_str_bytes", test_parity_str_bytes);
    register_test("test_parity_str_substring", test_parity_str_substring);
    register_test("test_parity_str_index_of", test_parity_str_index_of);
    register_test("test_parity_str_repeat", test_parity_str_repeat);
    register_test("test_parity_str_pad_left", test_parity_str_pad_left);
    register_test("test_parity_str_pad_right", test_parity_str_pad_right);
    register_test("test_parity_str_trim_start", test_parity_str_trim_start);
    register_test("test_parity_str_trim_end", test_parity_str_trim_end);
    register_test("test_parity_str_reverse", test_parity_str_reverse);
    register_test("test_parity_str_count", test_parity_str_count);
    register_test("test_parity_str_is_empty", test_parity_str_is_empty);
    register_test("test_parity_str_capitalize", test_parity_str_capitalize);
    register_test("test_parity_str_title_case", test_parity_str_title_case);
    register_test("test_parity_str_snake_case", test_parity_str_snake_case);
    register_test("test_parity_str_camel_case", test_parity_str_camel_case);
    register_test("test_parity_str_kebab_case", test_parity_str_kebab_case);

    /* Cross-backend parity: Map methods */
    register_test("test_parity_map_keys_values", test_parity_map_keys_values);
    register_test("test_parity_map_has", test_parity_map_has);
    register_test("test_parity_map_remove", test_parity_map_remove);
    register_test("test_parity_map_merge", test_parity_map_merge);
    register_test("test_parity_map_entries", test_parity_map_entries);
    register_test("test_parity_map_for_each", test_parity_map_for_each);
    register_test("test_parity_map_filter", test_parity_map_filter);

    /* Cross-backend parity: Set methods */
    register_test("test_parity_set_add_has_remove", test_parity_set_add_has_remove);
    register_test("test_parity_set_union", test_parity_set_union);
    register_test("test_parity_set_intersection", test_parity_set_intersection);
    register_test("test_parity_set_difference", test_parity_set_difference);
    register_test("test_parity_set_symmetric_difference", test_parity_set_symmetric_difference);
    register_test("test_parity_set_subset_superset", test_parity_set_subset_superset);
    register_test("test_parity_set_to_array", test_parity_set_to_array);

    /* Cross-backend parity: Buffer methods */
    register_test("test_parity_buffer_write_read_u8", test_parity_buffer_write_read_u8);
    register_test("test_parity_buffer_write_read_u16", test_parity_buffer_write_read_u16);
    register_test("test_parity_buffer_write_read_u32", test_parity_buffer_write_read_u32);
    register_test("test_parity_buffer_push", test_parity_buffer_push);
    register_test("test_parity_buffer_slice", test_parity_buffer_slice);
    register_test("test_parity_buffer_to_array", test_parity_buffer_to_array);
    register_test("test_parity_buffer_clear_fill", test_parity_buffer_clear_fill);
    register_test("test_parity_buffer_to_hex", test_parity_buffer_to_hex);
    register_test("test_parity_buffer_read_i8", test_parity_buffer_read_i8);

    /* Cross-backend parity: Nested / edge cases */
    register_test("test_parity_nested_array_methods", test_parity_nested_array_methods);
    register_test("test_parity_str_split_join_roundtrip", test_parity_str_split_join_roundtrip);
    register_test("test_parity_array_empty_operations", test_parity_array_empty_operations);
    register_test("test_parity_map_empty_operations", test_parity_map_empty_operations);
    register_test("test_parity_str_methods_chain", test_parity_str_methods_chain);
    register_test("test_parity_array_map_with_index", test_parity_array_map_with_index);

    /* Cross-backend parity: Arithmetic */
    register_test("test_parity_int_add", test_parity_int_add);
    register_test("test_parity_int_sub", test_parity_int_sub);
    register_test("test_parity_int_mul", test_parity_int_mul);
    register_test("test_parity_int_div", test_parity_int_div);
    register_test("test_parity_int_mod", test_parity_int_mod);
    register_test("test_parity_int_neg", test_parity_int_neg);
    register_test("test_parity_int_precedence", test_parity_int_precedence);
    register_test("test_parity_int_parens", test_parity_int_parens);
    register_test("test_parity_float_add", test_parity_float_add);
    register_test("test_parity_float_div", test_parity_float_div);

    /* Cross-backend parity: Comparison */
    register_test("test_parity_cmp_lt", test_parity_cmp_lt);
    register_test("test_parity_cmp_gt", test_parity_cmp_gt);
    register_test("test_parity_cmp_lteq", test_parity_cmp_lteq);
    register_test("test_parity_cmp_gteq", test_parity_cmp_gteq);
    register_test("test_parity_cmp_eq", test_parity_cmp_eq);
    register_test("test_parity_cmp_neq", test_parity_cmp_neq);
    register_test("test_parity_logic_and", test_parity_logic_and);
    register_test("test_parity_logic_or", test_parity_logic_or);
    register_test("test_parity_logic_not", test_parity_logic_not);

    /* Cross-backend parity: Bitwise */
    register_test("test_parity_bit_and", test_parity_bit_and);
    register_test("test_parity_bit_or", test_parity_bit_or);
    register_test("test_parity_bit_xor", test_parity_bit_xor);
    register_test("test_parity_bit_not", test_parity_bit_not);
    register_test("test_parity_bit_lshift", test_parity_bit_lshift);
    register_test("test_parity_bit_rshift", test_parity_bit_rshift);

    /* Cross-backend parity: String basics */
    register_test("test_parity_str_concat_basic", test_parity_str_concat_basic);
    register_test("test_parity_str_len_basic", test_parity_str_len_basic);
    register_test("test_parity_str_upper", test_parity_str_upper);
    register_test("test_parity_str_lower", test_parity_str_lower);
    register_test("test_parity_str_trim_basic", test_parity_str_trim_basic);
    register_test("test_parity_str_contains_basic", test_parity_str_contains_basic);
    register_test("test_parity_str_starts_with", test_parity_str_starts_with);
    register_test("test_parity_str_ends_with", test_parity_str_ends_with);
    register_test("test_parity_str_interpolation", test_parity_str_interpolation);
    register_test("test_parity_str_interp_expr", test_parity_str_interp_expr);

    /* Cross-backend parity: Variables */
    register_test("test_parity_let_var", test_parity_let_var);
    register_test("test_parity_flux_var", test_parity_flux_var);

    /* Cross-backend parity: Structs */
    register_test("test_parity_struct_basic", test_parity_struct_basic);
    register_test("test_parity_struct_method", test_parity_struct_method);

    /* Cross-backend parity: Functions */
    register_test("test_parity_fn_basic", test_parity_fn_basic);
    register_test("test_parity_fn_string", test_parity_fn_string);
    register_test("test_parity_fn_recursive", test_parity_fn_recursive);

    /* Cross-backend parity: Closures */
    register_test("test_parity_closure_capture", test_parity_closure_capture);
    register_test("test_parity_closure_higher_order", test_parity_closure_higher_order);

    /* Cross-backend parity: Control flow */
    register_test("test_parity_if_true", test_parity_if_true);
    register_test("test_parity_if_false", test_parity_if_false);
    register_test("test_parity_match_basic", test_parity_match_basic);

    /* Cross-backend parity: Loops */
    register_test("test_parity_while_loop", test_parity_while_loop);
    register_test("test_parity_for_loop", test_parity_for_loop);
    register_test("test_parity_for_strings", test_parity_for_strings);
    register_test("test_parity_loop_break", test_parity_loop_break);
    register_test("test_parity_for_range", test_parity_for_range);
    register_test("test_parity_nested_for", test_parity_nested_for);

    /* Cross-backend parity: Enums */
    register_test("test_parity_enum_basic", test_parity_enum_basic);
    register_test("test_parity_enum_tag", test_parity_enum_tag);
    register_test("test_parity_enum_name", test_parity_enum_name);
    register_test("test_parity_enum_payload", test_parity_enum_payload);

    /* Cross-backend parity: Exception handling */
    register_test("test_parity_try_catch", test_parity_try_catch);
    register_test("test_parity_throw_catch", test_parity_throw_catch);

    /* Cross-backend parity: Nil coalescing */
    register_test("test_parity_nil_coalesce", test_parity_nil_coalesce);
    register_test("test_parity_nil_coalesce_non_nil", test_parity_nil_coalesce_non_nil);

    /* Cross-backend parity: Tuples */
    register_test("test_parity_tuple_basic", test_parity_tuple_basic);
    register_test("test_parity_tuple_len", test_parity_tuple_len);

    /* Cross-backend parity: Defer */
    register_test("test_parity_defer_basic", test_parity_defer_basic);
    register_test("test_parity_defer_lifo", test_parity_defer_lifo);

    /* Cross-backend parity: Destructuring */
    register_test("test_parity_destructure_array", test_parity_destructure_array);

    /* Cross-backend parity: Phases */
    register_test("test_parity_flux_phase", test_parity_flux_phase);
    register_test("test_parity_fix_phase", test_parity_fix_phase);
    register_test("test_parity_clone_value", test_parity_clone_value);

    /* Cross-backend parity: Index assignment */
    register_test("test_parity_index_assign_local", test_parity_index_assign_local);
    register_test("test_parity_index_assign_global", test_parity_index_assign_global);

    /* Cross-backend parity: Mixed / pipeline */
    register_test("test_parity_complex_pipeline", test_parity_complex_pipeline);
    register_test("test_parity_fn_return_array", test_parity_fn_return_array);
    register_test("test_parity_global_push", test_parity_global_push);

    /* Cross-backend parity: Map basics */
    register_test("test_parity_map_basic", test_parity_map_basic);
    register_test("test_parity_map_len", test_parity_map_len);

    /* Cross-backend parity: Array literal / index */
    register_test("test_parity_array_literal", test_parity_array_literal);
    register_test("test_parity_array_index", test_parity_array_index);

    /* Error message diagnostics */
    register_test("test_err_undefined_var_suggestion", test_err_undefined_var_suggestion);
    register_test("test_err_undefined_var_no_suggestion", test_err_undefined_var_no_suggestion);
    register_test("test_err_method_suggestion_array", test_err_method_suggestion_array);
    register_test("test_err_method_suggestion_string", test_err_method_suggestion_string);
    register_test("test_err_method_no_suggestion", test_err_method_no_suggestion);
    register_test("test_err_phase_violation_hint", test_err_phase_violation_hint);

    /* LAT-17: Concurrent scope/spawn edge cases */
    register_test("test_scope_multi_spawn_shared_channels", test_scope_multi_spawn_shared_channels);
    register_test("test_scope_sync_body_with_spawns", test_scope_sync_body_with_spawns);
    register_test("test_scope_nested", test_scope_nested);
    register_test("test_scope_spawn_error_div_zero", test_scope_spawn_error_div_zero);
    register_test("test_scope_as_expression", test_scope_as_expression);
    register_test("test_spawn_captures_outer_variable", test_spawn_captures_outer_variable);
    register_test("test_scope_many_spawns_one_channel", test_scope_many_spawns_one_channel);
    register_test("test_spawn_closure_factory", test_spawn_closure_factory);
    register_test("test_scope_expr_with_spawns", test_scope_expr_with_spawns);

    /* LAT-17: Select statement tests */
    register_test("test_select_default_arm", test_select_default_arm);
    register_test("test_select_timeout_arm", test_select_timeout_arm);
    register_test("test_select_channel_ready", test_select_channel_ready);
    register_test("test_select_multiple_channels", test_select_multiple_channels);
    register_test("test_select_in_loop", test_select_in_loop);
    register_test("test_select_as_expression", test_select_as_expression);

    /* LAT-17: Phase edge cases */
    register_test("test_freeze_nested_array", test_freeze_nested_array);
    register_test("test_thaw_and_remutate", test_thaw_and_remutate);
    register_test("test_phase_across_function", test_phase_across_function);
    register_test("test_sublimate_blocks_push_and_pop", test_sublimate_blocks_push_and_pop);
    register_test("test_pressure_no_grow_on_crystal", test_pressure_no_grow_on_crystal);
    register_test("test_freeze_preserves_content", test_freeze_preserves_content);
    register_test("test_thaw_frozen_map_add_keys", test_thaw_frozen_map_add_keys);

    /* LAT-17: Closure capture edge cases */
    register_test("test_closure_upvalue_nested_mutation", test_closure_upvalue_nested_mutation);
    register_test("test_closure_capture_loop_var", test_closure_capture_loop_var);
    register_test("test_closure_in_spawn", test_closure_in_spawn);
    register_test("test_closure_recursive", test_closure_recursive);
    register_test("test_closure_counter", test_closure_counter);

    /* LAT-17: Error propagation */
    register_test("test_try_catch_in_function", test_try_catch_in_function);
    register_test("test_defer_with_error", test_defer_with_error);
    register_test("test_error_multi_frame", test_error_multi_frame);
    register_test("test_try_catch_conditional_error", test_try_catch_conditional_error);

    /* LAT-22: PIC / method dispatch tests */
    register_test("test_pic_method_chain_array", test_pic_method_chain_array);
    register_test("test_pic_method_chain_string", test_pic_method_chain_string);
    register_test("test_pic_loop_same_method", test_pic_loop_same_method);
    register_test("test_pic_polymorphic_len", test_pic_polymorphic_len);
    register_test("test_pic_struct_method_fallthrough", test_pic_struct_method_fallthrough);
    register_test("test_pic_global_push_loop", test_pic_global_push_loop);
    register_test("test_pic_mixed_chain", test_pic_mixed_chain);

    /* LAT-27: Duration/DateTime/Calendar/Timezone */
    register_test("test_duration_create", test_duration_create);
    register_test("test_duration_from_seconds", test_duration_from_seconds);
    register_test("test_duration_from_millis", test_duration_from_millis);
    register_test("test_duration_add", test_duration_add);
    register_test("test_duration_sub", test_duration_sub);
    register_test("test_duration_to_string", test_duration_to_string);
    register_test("test_duration_to_string_with_millis", test_duration_to_string_with_millis);
    register_test("test_duration_field_accessors", test_duration_field_accessors);
    register_test("test_datetime_from_epoch", test_datetime_from_epoch);
    register_test("test_datetime_to_epoch", test_datetime_to_epoch);
    register_test("test_datetime_from_iso", test_datetime_from_iso);
    register_test("test_datetime_to_iso", test_datetime_to_iso);
    register_test("test_datetime_iso_roundtrip", test_datetime_iso_roundtrip);
    register_test("test_datetime_add_duration", test_datetime_add_duration);
    register_test("test_datetime_sub", test_datetime_sub);
    register_test("test_datetime_format_map", test_datetime_format_map);
    register_test("test_datetime_to_utc", test_datetime_to_utc);
    register_test("test_days_in_month", test_days_in_month);
    register_test("test_day_of_week", test_day_of_week);
    register_test("test_day_of_year", test_day_of_year);
    register_test("test_timezone_offset", test_timezone_offset);
    register_test("test_is_leap_year_extended", test_is_leap_year_extended);

    /* LAT-25: String interning in bytecode VMs */
    register_test("test_intern_string_equality", test_intern_string_equality);
    register_test("test_intern_concat_short", test_intern_concat_short);
    register_test("test_intern_interpolation", test_intern_interpolation);
    register_test("test_intern_concat_loop", test_intern_concat_loop);
    register_test("test_intern_equality_after_concat", test_intern_equality_after_concat);
    register_test("test_intern_not_equal", test_intern_not_equal);
    register_test("test_intern_string_methods", test_intern_string_methods);
    register_test("test_intern_map_string_keys", test_intern_map_string_keys);
    register_test("test_intern_long_string_not_interned", test_intern_long_string_not_interned);
    register_test("test_intern_heavy_string_ops", test_intern_heavy_string_ops);
    /* LAT-26: Type, enum variant, and keyword suggestions */
    register_test("test_err_type_suggestion", test_err_type_suggestion);
    register_test("test_err_return_type_suggestion", test_err_return_type_suggestion);
    register_test("test_err_type_no_false_suggestion", test_err_type_no_false_suggestion);
    register_test("test_err_enum_variant_suggestion", test_err_enum_variant_suggestion);
    register_test("test_err_enum_variant_no_suggestion", test_err_enum_variant_no_suggestion);
    register_test("test_err_keyword_suggestion", test_err_keyword_suggestion);
    register_test("test_err_keyword_no_suggestion", test_err_keyword_no_suggestion);
    register_test("test_err_type_suggestion_bool", test_err_type_suggestion_bool);

    /* LAT-20: Phase system extensions */
    /* Feature 1: borrow */
    register_test("test_borrow_basic", test_borrow_basic);
    register_test("test_borrow_already_fluid", test_borrow_already_fluid);
    register_test("test_borrow_nested", test_borrow_nested);
    register_test("test_borrow_mutation_persists", test_borrow_mutation_persists);
    /* Feature 2: @fluid/@crystal annotations */
    register_test("test_annotation_crystal_binding", test_annotation_crystal_binding);
    register_test("test_annotation_fluid_fn", test_annotation_fluid_fn);
    register_test("test_annotation_parse_error", test_annotation_parse_error);
    /* Feature 3: composite phase constraints */
    register_test("test_composite_fluid_or_crystal", test_composite_fluid_or_crystal);
}
