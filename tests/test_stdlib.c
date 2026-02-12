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
#include "json.h"
#include "math_ops.h"
#include "env_ops.h"
#include "time_ops.h"
#include "fs_ops.h"

/* Import test harness from test_main.c */
extern void register_test(const char *name, void (*fn)(void));
extern int test_current_failed;

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        test_current_failed = 1; \
        return; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) != 0) { \
        fprintf(stderr, "  ASSERT_STR_EQ FAILED: \"%s\" != \"%s\" (%s:%d)\n", \
                _a, _b, __FILE__, __LINE__); \
        test_current_failed = 1; \
        return; \
    } \
} while(0)

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
        free(parse_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        fflush(stdout);
        dup2(old_stdout, fileno(stdout));
        close(old_stdout);
        fclose(tmp);
        return strdup("PARSE_ERROR");
    }

    Evaluator *ev = evaluator_new();
    char *eval_err_str = evaluator_run(ev, &prog);

    fflush(stdout);
    dup2(old_stdout, fileno(stdout));
    close(old_stdout);

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

    evaluator_free(ev);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++)
        token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);

    return output;
}

/* Helper macro for testing Lattice source -> expected output */
#define ASSERT_OUTPUT(source, expected) do { \
    char *out = run_capture(source); \
    if (strcmp(out, expected) != 0) { \
        fprintf(stderr, "  ASSERT_OUTPUT FAILED:\n    source:   %s\n    expected: %s\n    actual:   %s\n    at %s:%d\n", \
                source, expected, out, __FILE__, __LINE__); \
        free(out); test_current_failed = 1; return; \
    } \
    free(out); \
} while(0)

/* Helper macro: assert output starts with a prefix (useful for error checks) */
#define ASSERT_OUTPUT_STARTS_WITH(source, prefix) do { \
    char *out = run_capture(source); \
    if (strncmp(out, prefix, strlen(prefix)) != 0) { \
        fprintf(stderr, "  ASSERT_OUTPUT_STARTS_WITH FAILED:\n    source:   %s\n    prefix:   %s\n    actual:   %s\n    at %s:%d\n", \
                source, prefix, out, __FILE__, __LINE__); \
        free(out); test_current_failed = 1; return; \
    } \
    free(out); \
} while(0)


/* ======================================================================
 * String Methods
 * ====================================================================== */

/* 1. test_str_len - "hello".len() -> 5 */
static void test_str_len(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\".len())\n"
        "}\n",
        "5"
    );
    /* Empty string */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"\".len())\n"
        "}\n",
        "0"
    );
}

/* 2. test_str_contains - "hello world".contains("world") -> true */
static void test_str_contains(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello world\".contains(\"world\"))\n"
        "}\n",
        "true"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\".contains(\"xyz\"))\n"
        "}\n",
        "false"
    );
    /* Empty substring always contained */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\".contains(\"\"))\n"
        "}\n",
        "true"
    );
}

/* 3. test_str_starts_with - "hello".starts_with("he") -> true */
static void test_str_starts_with(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\".starts_with(\"he\"))\n"
        "}\n",
        "true"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\".starts_with(\"lo\"))\n"
        "}\n",
        "false"
    );
}

/* 4. test_str_ends_with - "hello".ends_with("lo") -> true */
static void test_str_ends_with(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\".ends_with(\"lo\"))\n"
        "}\n",
        "true"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\".ends_with(\"he\"))\n"
        "}\n",
        "false"
    );
}

/* 5. test_str_trim - "  hello  ".trim() -> "hello" */
static void test_str_trim(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"  hello  \".trim())\n"
        "}\n",
        "hello"
    );
    /* Trim with no whitespace is a no-op */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\".trim())\n"
        "}\n",
        "hello"
    );
}

/* 6. test_str_to_upper - "hello".to_upper() -> "HELLO" */
static void test_str_to_upper(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\".to_upper())\n"
        "}\n",
        "HELLO"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"Hello World\".to_upper())\n"
        "}\n",
        "HELLO WORLD"
    );
}

/* 7. test_str_to_lower - "HELLO".to_lower() -> "hello" */
static void test_str_to_lower(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"HELLO\".to_lower())\n"
        "}\n",
        "hello"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"Hello World\".to_lower())\n"
        "}\n",
        "hello world"
    );
}

/* 8. test_str_replace - "hello world".replace("world", "lattice") -> "hello lattice" */
static void test_str_replace(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello world\".replace(\"world\", \"lattice\"))\n"
        "}\n",
        "hello lattice"
    );
    /* Replace all occurrences */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"aabaa\".replace(\"a\", \"\"))\n"
        "}\n",
        "b"
    );
}

/* 9. test_str_split - "a,b,c".split(",") -> array with 3 elements */
static void test_str_split(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let parts = \"a,b,c\".split(\",\")\n"
        "    print(parts.len())\n"
        "    print(parts[0])\n"
        "    print(parts[1])\n"
        "    print(parts[2])\n"
        "}\n",
        "3\na\nb\nc"
    );
}

/* 10. test_str_index_of - "hello".index_of("ll") -> 2, not found -> -1 */
static void test_str_index_of(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\".index_of(\"ll\"))\n"
        "}\n",
        "2"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\".index_of(\"xyz\"))\n"
        "}\n",
        "-1"
    );
}

/* 11. test_str_substring - "hello".substring(1, 4) -> "ell" */
static void test_str_substring(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\".substring(1, 4))\n"
        "}\n",
        "ell"
    );
    /* Full string */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\".substring(0, 5))\n"
        "}\n",
        "hello"
    );
}

/* 12. test_str_chars - "abc".chars() returns array of single-char strings */
static void test_str_chars(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let cs = \"abc\".chars()\n"
        "    print(cs.len())\n"
        "    print(cs[0])\n"
        "    print(cs[1])\n"
        "    print(cs[2])\n"
        "}\n",
        "3\na\nb\nc"
    );
}

/* 13. test_str_reverse - "hello".reverse() -> "olleh" */
static void test_str_reverse(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\".reverse())\n"
        "}\n",
        "olleh"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"\".reverse())\n"
        "}\n",
        ""
    );
}

/* 14. test_str_repeat - "ab".repeat(3) -> "ababab" */
static void test_str_repeat(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"ab\".repeat(3))\n"
        "}\n",
        "ababab"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"x\".repeat(0))\n"
        "}\n",
        ""
    );
}


/* ======================================================================
 * String Indexing and Concatenation (already working -- verification)
 * ====================================================================== */

/* 15. test_str_index - "hello"[0] -> "h", "hello"[4] -> "o" */
static void test_str_index(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\"[0])\n"
        "}\n",
        "h"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\"[4])\n"
        "}\n",
        "o"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\"[2])\n"
        "}\n",
        "l"
    );
}

/* 16. test_str_concat - "hello" + " " + "world" -> "hello world" */
static void test_str_concat(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\" + \" \" + \"world\")\n"
        "}\n",
        "hello world"
    );
    /* Concat with empty */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"\" + \"abc\")\n"
        "}\n",
        "abc"
    );
}

/* 17. test_str_range_slice - "hello"[1..4] -> "ell" */
static void test_str_range_slice(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\"[1..4])\n"
        "}\n",
        "ell"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"hello\"[0..5])\n"
        "}\n",
        "hello"
    );
}


/* ======================================================================
 * Built-in Functions
 * ====================================================================== */

/* 18. test_typeof - typeof(42) -> "Int", typeof("hi") -> "String", etc. */
static void test_typeof(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(typeof(42))\n"
        "}\n",
        "Int"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(typeof(\"hi\"))\n"
        "}\n",
        "String"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(typeof(true))\n"
        "}\n",
        "Bool"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(typeof(3.14))\n"
        "}\n",
        "Float"
    );
}

/* 19. test_phase_of - phase_of(42) -> "unphased", phase_of(freeze(42)) -> "crystal" */
static void test_phase_of(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(phase_of(42))\n"
        "}\n",
        "unphased"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(phase_of(freeze(42)))\n"
        "}\n",
        "crystal"
    );
}

/* 20. test_to_string - to_string(42) -> "42", to_string(true) -> "true" */
static void test_to_string(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(to_string(42))\n"
        "}\n",
        "42"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(to_string(true))\n"
        "}\n",
        "true"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(to_string(3.14))\n"
        "}\n",
        "3.14"
    );
}

/* 21. test_ord - ord("A") -> 65, ord("a") -> 97 */
static void test_ord(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(ord(\"A\"))\n"
        "}\n",
        "65"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(ord(\"a\"))\n"
        "}\n",
        "97"
    );
}

/* 22. test_chr - chr(65) -> "A", chr(97) -> "a" */
static void test_chr(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(chr(65))\n"
        "}\n",
        "A"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(chr(97))\n"
        "}\n",
        "a"
    );
}


/* ======================================================================
 * Try/Catch
 * ====================================================================== */

/* 23. test_try_catch_no_error - try block succeeds, returns try value */
static void test_try_catch_no_error(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let x = try {\n"
        "        42\n"
        "    } catch e {\n"
        "        0\n"
        "    }\n"
        "    print(x)\n"
        "}\n",
        "42"
    );
}

/* 24. test_try_catch_div_zero - catches division by zero */
static void test_try_catch_div_zero(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let result = try {\n"
        "        let x = 1 / 0\n"
        "        x\n"
        "    } catch e {\n"
        "        e\n"
        "    }\n"
        "    print(result)\n"
        "}\n",
        "division by zero"
    );
}

/* 25. test_try_catch_undefined_var - catches undefined variable */
static void test_try_catch_undefined_var(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let result = try {\n"
        "        undefined_var\n"
        "    } catch e {\n"
        "        \"caught\"\n"
        "    }\n"
        "    print(result)\n"
        "}\n",
        "caught"
    );
}

/* 26. test_try_catch_nested - nested try/catch */
static void test_try_catch_nested(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
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
        "inner caught"
    );
    /* Outer catch handles error from inner block */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let result = try {\n"
        "        try {\n"
        "            42\n"
        "        } catch e {\n"
        "            e\n"
        "        }\n"
        "        let x = 1 / 0\n"
        "        x\n"
        "    } catch e {\n"
        "        \"outer: \" + e\n"
        "    }\n"
        "    print(result)\n"
        "}\n",
        "outer: division by zero"
    );
}


/* ======================================================================
 * Lattice Eval and Tokenize Built-in Functions
 * ====================================================================== */

/* 27. test_eval_simple - the Lattice eval() builtin evaluates "1 + 2" -> 3 */
static void test_eval_simple(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let result = lat_eval(\"1 + 2\")\n"
        "    print(result)\n"
        "}\n",
        "3"
    );
}

/* 28. test_eval_string - the Lattice eval() builtin evaluates a string literal */
static void test_eval_string(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let result = lat_eval(\"\\\"hello\\\"\")\n"
        "    print(result)\n"
        "}\n",
        "hello"
    );
}

/* 29. test_tokenize - the Lattice tokenize() builtin returns token array */
static void test_tokenize(void) {
    /* tokenize should return an array; verify it has elements */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let tokens = tokenize(\"let x = 42\")\n"
        "    print(tokens.len() > 0)\n"
        "}\n",
        "true"
    );
}


/* ======================================================================
 * Read/Write File
 * ====================================================================== */

/* 30. test_write_and_read_file - write a temp file and read it back */
static void test_write_and_read_file(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    write_file(\"/tmp/lattice_test_stdlib.txt\", \"hello from lattice\")\n"
        "    let content = read_file(\"/tmp/lattice_test_stdlib.txt\")\n"
        "    print(content)\n"
        "}\n",
        "hello from lattice"
    );
    /* Clean up the temp file */
    (void)remove("/tmp/lattice_test_stdlib.txt");
}


/* ======================================================================
 * Escape Sequences
 * ====================================================================== */

/* 31. test_escape_hex - \x1b produces ESC character (code 27) */
static void test_escape_hex(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(ord(\"\\x1b\"))\n"
        "}\n",
        "27"
    );
    /* \x41 = 'A' */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(\"\\x41\")\n"
        "}\n",
        "A"
    );
}

/* 32. test_escape_carriage_return - \r produces carriage return */
static void test_escape_carriage_return(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(ord(\"\\r\"))\n"
        "}\n",
        "13"
    );
}

/* 33. test_escape_null_byte - \0 produces null byte */
static void test_escape_null_byte(void) {
    /* Null byte in a string - len should be 0 because C string stops at \0 */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let s = \"\\0hello\"\n"
        "    print(s.len())\n"
        "}\n",
        "0"
    );
}

/* 34. test_escape_hex_error - invalid hex escape reports error */
static void test_escape_hex_error(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    print(\"\\xZZ\")\n"
        "}\n",
        "LEX_ERROR"
    );
}


/* ======================================================================
 * Compound Assignment
 * ====================================================================== */

/* 35. test_compound_add_int - x += 5 on int */
static void test_compound_add_int(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux x = 10\n"
        "    x += 5\n"
        "    print(x)\n"
        "}\n",
        "15"
    );
}

/* 36. test_compound_add_string - s += " world" on string */
static void test_compound_add_string(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux s = \"hello\"\n"
        "    s += \" world\"\n"
        "    print(s)\n"
        "}\n",
        "hello world"
    );
}

/* 37. test_compound_sub_mul_div_mod - various compound operators */
static void test_compound_sub_mul_div_mod(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
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
        "15\n45\n9\n1"
    );
}

/* 38. test_compound_field - compound assignment on struct field */
static void test_compound_field(void) {
    ASSERT_OUTPUT(
        "struct Counter { val: Int }\n"
        "fn main() {\n"
        "    flux c = Counter { val: 10 }\n"
        "    c.val += 5\n"
        "    print(c.val)\n"
        "}\n",
        "15"
    );
}

/* 39. test_compound_index - compound assignment on array index */
static void test_compound_index(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux arr = [1, 2, 3]\n"
        "    arr[1] += 10\n"
        "    print(arr[1])\n"
        "}\n",
        "12"
    );
}


/* ======================================================================
 * Array Methods
 * ====================================================================== */

/* 40. test_array_filter - [1,2,3,4,5].filter(|x| x > 3) -> [4, 5] */
static void test_array_filter(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let arr = [1, 2, 3, 4, 5]\n"
        "    let filtered = arr.filter(|x| x > 3)\n"
        "    print(filtered)\n"
        "}\n",
        "[4, 5]"
    );
}

/* 41. test_array_for_each - iterate and print each */
static void test_array_for_each(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let arr = [10, 20, 30]\n"
        "    arr.for_each(|x| print(x))\n"
        "}\n",
        "10\n20\n30"
    );
}

/* 42. test_array_find - find first element matching predicate */
static void test_array_find(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let arr = [1, 2, 3, 4, 5]\n"
        "    let found = arr.find(|x| x > 3)\n"
        "    print(found)\n"
        "}\n",
        "4"
    );
    /* Not found returns unit */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let arr = [1, 2, 3]\n"
        "    let found = arr.find(|x| x > 10)\n"
        "    print(found)\n"
        "}\n",
        "()"
    );
}

/* 43. test_array_contains - check if array contains a value */
static void test_array_contains(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let arr = [1, 2, 3]\n"
        "    print(arr.contains(2))\n"
        "    print(arr.contains(5))\n"
        "}\n",
        "true\nfalse"
    );
}

/* 44. test_array_reverse - reverse an array */
static void test_array_reverse(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let arr = [1, 2, 3]\n"
        "    print(arr.reverse())\n"
        "}\n",
        "[3, 2, 1]"
    );
}

/* 45. test_array_enumerate - enumerate returns [index, value] pairs */
static void test_array_enumerate(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let arr = [\"a\", \"b\", \"c\"]\n"
        "    let pairs = arr.enumerate()\n"
        "    for pair in pairs {\n"
        "        print(pair[0], pair[1])\n"
        "    }\n"
        "}\n",
        "0 a\n1 b\n2 c"
    );
}


/* ======================================================================
 * Parsing & Utility Built-ins
 * ====================================================================== */

/* 46. test_parse_int - parse_int("42") -> 42 */
static void test_parse_int(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(parse_int(\"42\"))\n"
        "}\n",
        "42"
    );
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(parse_int(\"-7\"))\n"
        "}\n",
        "-7"
    );
}

/* 47. test_parse_float - parse_float("3.14") -> 3.14 */
static void test_parse_float(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(parse_float(\"3.14\"))\n"
        "}\n",
        "3.14"
    );
}

/* 48. test_len - generic len() function */
static void test_len(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(len(\"hello\"))\n"
        "    print(len([1, 2, 3]))\n"
        "}\n",
        "5\n3"
    );
}

/* 49. test_print_raw - print without newline */
static void test_print_raw(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print_raw(\"hello\")\n"
        "    print_raw(\" world\")\n"
        "    print(\"!\")\n"
        "}\n",
        "hello world!"
    );
}

/* 50. test_eprint - print to stderr (won't show in captured stdout) */
static void test_eprint(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    eprint(\"error message\")\n"
        "    print(\"ok\")\n"
        "}\n",
        "ok"
    );
}


/* ======================================================================
 * HashMap
 * ====================================================================== */

/* 51. test_map_new - create an empty map */
static void test_map_new(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let m = Map::new()\n"
        "    print(typeof(m))\n"
        "}\n",
        "Map"
    );
}

/* 52. test_map_set_get - set and get values */
static void test_map_set_get(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    m.set(\"key\", 42)\n"
        "    print(m.get(\"key\"))\n"
        "}\n",
        "42"
    );
    /* Get nonexistent key returns unit */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    print(m.get(\"nope\"))\n"
        "}\n",
        "()"
    );
}

/* 53. test_map_has - check key existence */
static void test_map_has(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    m.set(\"x\", 1)\n"
        "    print(m.has(\"x\"))\n"
        "    print(m.has(\"y\"))\n"
        "}\n",
        "true\nfalse"
    );
}

/* 54. test_map_remove - remove a key */
static void test_map_remove(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    m.set(\"a\", 1)\n"
        "    m.set(\"b\", 2)\n"
        "    m.remove(\"a\")\n"
        "    print(m.has(\"a\"))\n"
        "    print(m.get(\"b\"))\n"
        "}\n",
        "false\n2"
    );
}

/* 55. test_map_keys_values - get keys and values arrays */
static void test_map_keys_values(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    m.set(\"a\", 1)\n"
        "    let ks = m.keys()\n"
        "    let vs = m.values()\n"
        "    print(ks.len())\n"
        "    print(vs[0])\n"
        "}\n",
        "1\n1"
    );
}

/* 56. test_map_len - map length */
static void test_map_len(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    print(m.len())\n"
        "    m.set(\"x\", 1)\n"
        "    m.set(\"y\", 2)\n"
        "    print(m.len())\n"
        "}\n",
        "0\n2"
    );
}

/* 57. test_map_index_read_write - map["key"] read and write */
static void test_map_index_read_write(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    m[\"x\"] = 42\n"
        "    print(m[\"x\"])\n"
        "    m[\"x\"] = 99\n"
        "    print(m[\"x\"])\n"
        "}\n",
        "42\n99"
    );
}

/* 58. test_map_for_in - iterate map keys */
static void test_map_for_in(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    m.set(\"hello\", 1)\n"
        "    flux count = 0\n"
        "    for key in m {\n"
        "        count += 1\n"
        "    }\n"
        "    print(count)\n"
        "}\n",
        "1"
    );
}

/* 59. test_map_display - map display format */
static void test_map_display(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    m.set(\"x\", 1)\n"
        "    let s = to_string(m)\n"
        "    // Should contain \"x\": 1\n"
        "    print(s.contains(\"x\"))\n"
        "}\n",
        "true"
    );
}

/* 60. test_map_freeze_thaw - freeze and thaw a map */
static void test_map_freeze_thaw(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    m.set(\"a\", 1)\n"
        "    fix frozen = freeze(m)\n"
        "    print(phase_of(frozen))\n"
        "    flux thawed = thaw(frozen)\n"
        "    print(phase_of(thawed))\n"
        "    thawed.set(\"b\", 2)\n"
        "    print(thawed.len())\n"
        "}\n",
        "crystal\nfluid\n2"
    );
}

/* 61. test_map_len_builtin - len() built-in works on maps */
static void test_map_len_builtin(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    m.set(\"a\", 1)\n"
        "    m.set(\"b\", 2)\n"
        "    print(len(m))\n"
        "}\n",
        "2"
    );
}


/* 62. test_callable_struct_field - closure in struct field called as method */
static void test_callable_struct_field(void) {
    ASSERT_OUTPUT(
        "struct Greeter { name: String, greet: Fn }\n"
        "fn main() {\n"
        "    let g = Greeter { name: \"World\", greet: |self| print(\"Hello, \" + self.name) }\n"
        "    g.greet()\n"
        "}\n",
        "Hello, World"
    );
}

/* 63. test_callable_struct_field_with_args - closure field with extra args */
static void test_callable_struct_field_with_args(void) {
    ASSERT_OUTPUT(
        "struct Calc { value: Int, add: Fn }\n"
        "fn main() {\n"
        "    let c = Calc { value: 10, add: |self, n| print(self.value + n) }\n"
        "    c.add(5)\n"
        "}\n",
        "15"
    );
}

/* 64. test_callable_struct_field_returns - closure field returns a value */
static void test_callable_struct_field_returns(void) {
    ASSERT_OUTPUT(
        "struct Counter { val: Int, next: Fn }\n"
        "fn main() {\n"
        "    let c = Counter { val: 42, next: |self| self.val + 1 }\n"
        "    print(c.next())\n"
        "}\n",
        "43"
    );
}

/* 65. test_callable_struct_non_closure_field - regular field access still works */
static void test_callable_struct_non_closure_field(void) {
    ASSERT_OUTPUT(
        "struct Point { x: Int, y: Int }\n"
        "fn main() {\n"
        "    let p = Point { x: 3, y: 4 }\n"
        "    print(p.x + p.y)\n"
        "}\n",
        "7"
    );
}

/* ======================================================================
 * Block Closures and Block Expressions
 * ====================================================================== */

/* 66. test_block_closure_basic - |x| { let y = x + 1; y } */
static void test_block_closure_basic(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let f = |x| { let y = x + 1; y }\n"
        "    print(f(5))\n"
        "}\n",
        "6"
    );
}

/* 67. test_block_closure_multi_stmt - multiple statements, last is return value */
static void test_block_closure_multi_stmt(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let f = |x| {\n"
        "        let a = x * 2\n"
        "        let b = a + 3\n"
        "        b\n"
        "    }\n"
        "    print(f(10))\n"
        "}\n",
        "23"
    );
}

/* 68. test_block_closure_in_map - arr.map(|x| { let sq = x * x; sq }) */
static void test_block_closure_in_map(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let arr = [1, 2, 3]\n"
        "    let result = arr.map(|x| { let sq = x * x; sq })\n"
        "    print(result)\n"
        "}\n",
        "[1, 4, 9]"
    );
}

/* 69. test_block_expr_standalone - let x = { let a = 1; a + 2 } */
static void test_block_expr_standalone(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let x = { let a = 1; a + 2 }\n"
        "    print(x)\n"
        "}\n",
        "3"
    );
}

/* 70. test_callable_field_block_body - struct field closure with block body */
static void test_callable_field_block_body(void) {
    ASSERT_OUTPUT(
        "struct Doubler { factor: Int, compute: Fn }\n"
        "fn main() {\n"
        "    let d = Doubler { factor: 3, compute: |self, x| {\n"
        "        let result = self.factor * x\n"
        "        result\n"
        "    }}\n"
        "    print(d.compute(7))\n"
        "}\n",
        "21"
    );
}


/* ======================================================================
 * is_complete Builtin
 * ====================================================================== */

/* 71. test_is_complete_true - complete expression returns true */
static void test_is_complete_true(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(is_complete(\"print(1)\"))\n"
        "}\n",
        "true"
    );
}

/* 72. test_is_complete_unclosed_brace - "fn main() {" returns false */
static void test_is_complete_unclosed_brace(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(is_complete(\"fn main() {\"))\n"
        "}\n",
        "false"
    );
}

/* 73. test_is_complete_unclosed_paren - "print(" returns false */
static void test_is_complete_unclosed_paren(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(is_complete(\"print(\"))\n"
        "}\n",
        "false"
    );
}

/* 74. test_is_complete_balanced - complete but invalid code returns true */
static void test_is_complete_balanced(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(is_complete(\"let x = }\"))\n"
        "}\n",
        "true"
    );
}


/* ======================================================================
 * lat_eval Persistence (REPL support)
 * ====================================================================== */

/* 75. test_lat_eval_var_persistence - variables persist across lat_eval calls */
static void test_lat_eval_var_persistence(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    lat_eval(\"let x = 42\")\n"
        "    let result = lat_eval(\"x + 10\")\n"
        "    print(result)\n"
        "}\n",
        "52"
    );
}

/* 76. test_lat_eval_fn_persistence - functions persist across lat_eval calls */
static void test_lat_eval_fn_persistence(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    lat_eval(\"fn add(a: Int, b: Int) -> Int { return a + b }\")\n"
        "    let result = lat_eval(\"add(3, 4)\")\n"
        "    print(result)\n"
        "}\n",
        "7"
    );
}

/* 77. test_lat_eval_struct_persistence - structs persist across lat_eval calls */
static void test_lat_eval_struct_persistence(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    lat_eval(\"struct Point { x: Int, y: Int }\")\n"
        "    lat_eval(\"let p = Point { x: 3, y: 4 }\")\n"
        "    let result = lat_eval(\"p.x + p.y\")\n"
        "    print(result)\n"
        "}\n",
        "7"
    );
}

/* 78. test_lat_eval_mutable_var - mutable variables can be updated across calls */
static void test_lat_eval_mutable_var(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    lat_eval(\"flux counter = 0\")\n"
        "    lat_eval(\"counter += 1\")\n"
        "    lat_eval(\"counter += 1\")\n"
        "    let result = lat_eval(\"counter\")\n"
        "    print(result)\n"
        "}\n",
        "2"
    );
}

/* 79. test_lat_eval_version - version() returns a string */
static void test_lat_eval_version(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(version())\n"
        "}\n",
        "0.1.5"
    );
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
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let server = tcp_listen(\"127.0.0.1\", 0)\n"
        "    print(server >= 0)\n"
        "    tcp_close(server)\n"
        "    print(\"done\")\n"
        "}\n",
        "true\ndone"
    );
}

/* 86. test_tcp_error_handling - bad args produce eval errors */
static void test_tcp_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    tcp_listen(123, 80)\n"
        "}\n",
        "EVAL_ERROR:"
    );
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    tcp_read(\"bad\")\n"
        "}\n",
        "EVAL_ERROR:"
    );
}


/* ======================================================================
 * require()
 * ====================================================================== */

/* 87. test_require_basic - require a file and call its function */
static void test_require_basic(void) {
    /* Write a library file */
    builtin_write_file("/tmp/lattice_test_lib.lat",
        "fn helper() -> Int { return 42 }\n");

    ASSERT_OUTPUT(
        "fn main() {\n"
        "    require(\"/tmp/lattice_test_lib\")\n"
        "    print(helper())\n"
        "}\n",
        "42"
    );
    (void)remove("/tmp/lattice_test_lib.lat");
}

/* 88. test_require_with_extension - require with .lat extension works */
static void test_require_with_extension(void) {
    builtin_write_file("/tmp/lattice_test_lib2.lat",
        "fn helper2() -> Int { return 99 }\n");

    ASSERT_OUTPUT(
        "fn main() {\n"
        "    require(\"/tmp/lattice_test_lib2.lat\")\n"
        "    print(helper2())\n"
        "}\n",
        "99"
    );
    (void)remove("/tmp/lattice_test_lib2.lat");
}

/* 89. test_require_dedup - requiring same file twice is a no-op */
static void test_require_dedup(void) {
    builtin_write_file("/tmp/lattice_test_dedup.lat",
        "fn dedup_fn() -> Int { return 7 }\n");

    ASSERT_OUTPUT(
        "fn main() {\n"
        "    require(\"/tmp/lattice_test_dedup\")\n"
        "    require(\"/tmp/lattice_test_dedup\")\n"
        "    require(\"/tmp/lattice_test_dedup.lat\")\n"
        "    print(dedup_fn())\n"
        "}\n",
        "7"
    );
    (void)remove("/tmp/lattice_test_dedup.lat");
}

/* 90. test_require_structs - require a file that defines structs */
static void test_require_structs(void) {
    builtin_write_file("/tmp/lattice_test_structs.lat",
        "struct Pair { a: Int, b: Int }\n"
        "fn make_pair(x: Int, y: Int) -> Pair {\n"
        "    return Pair { a: x, b: y }\n"
        "}\n");

    ASSERT_OUTPUT(
        "fn main() {\n"
        "    require(\"/tmp/lattice_test_structs\")\n"
        "    let p = make_pair(3, 4)\n"
        "    print(p.a + p.b)\n"
        "}\n",
        "7"
    );
    (void)remove("/tmp/lattice_test_structs.lat");
}

/* 91. test_require_missing - require a nonexistent file produces error */
static void test_require_missing(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    require(\"/tmp/lattice_no_such_file_xyz\")\n"
        "}\n",
        "EVAL_ERROR:require: cannot find"
    );
}

/* 92. test_require_nested - transitive require */
static void test_require_nested(void) {
    builtin_write_file("/tmp/lattice_test_base.lat",
        "fn base_fn() -> Int { return 10 }\n");
    builtin_write_file("/tmp/lattice_test_mid.lat",
        "require(\"/tmp/lattice_test_base\")\n"
        "fn mid_fn() -> Int { return base_fn() + 5 }\n");

    ASSERT_OUTPUT(
        "fn main() {\n"
        "    require(\"/tmp/lattice_test_mid\")\n"
        "    print(mid_fn())\n"
        "}\n",
        "15"
    );
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
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(tls_available())\n"
        "}\n",
        "true"
    );
#else
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(tls_available())\n"
        "}\n",
        "false"
    );
#endif
}

/* 97. test_tls_error_handling - bad arg types produce eval errors */
static void test_tls_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    tls_connect(123, 443)\n"
        "}\n",
        "EVAL_ERROR:"
    );
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    tls_read(\"bad\")\n"
        "}\n",
        "EVAL_ERROR:"
    );
}


/* ======================================================================
 * JSON Tests
 * ====================================================================== */

static void test_json_parse_object(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let obj = json_parse(\"{\\\"name\\\": \\\"Alice\\\", \\\"age\\\": 30}\")\n"
        "    print(obj[\"name\"])\n"
        "    print(to_string(obj[\"age\"]))\n"
        "}\n",
        "Alice\n30"
    );
}

static void test_json_parse_array(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let arr = json_parse(\"[1, 2, 3]\")\n"
        "    print(to_string(len(arr)))\n"
        "    print(to_string(arr[0]))\n"
        "    print(to_string(arr[2]))\n"
        "}\n",
        "3\n1\n3"
    );
}

static void test_json_parse_nested(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let data = json_parse(\"{\\\"items\\\": [1, 2], \\\"ok\\\": true}\")\n"
        "    print(to_string(data[\"ok\"]))\n"
        "    print(to_string(len(data[\"items\"])))\n"
        "}\n",
        "true\n2"
    );
}

static void test_json_parse_primitives(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(to_string(json_parse(\"42\")))\n"
        "    print(to_string(json_parse(\"3.14\")))\n"
        "    print(to_string(json_parse(\"true\")))\n"
        "    print(to_string(json_parse(\"false\")))\n"
        "    print(to_string(json_parse(\"null\")))\n"
        "}\n",
        "42\n3.14\ntrue\nfalse\n()"
    );
}

static void test_json_stringify_basic(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(json_stringify(42))\n"
        "    print(json_stringify(\"hello\"))\n"
        "    print(json_stringify(true))\n"
        "    print(json_stringify(false))\n"
        "}\n",
        "42\n\"hello\"\ntrue\nfalse"
    );
}

static void test_json_stringify_array(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(json_stringify([1, 2, 3]))\n"
        "}\n",
        "[1,2,3]"
    );
}

static void test_json_roundtrip(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let json = \"{\\\"a\\\": 1}\"\n"
        "    let obj = json_parse(json)\n"
        "    let back = json_stringify(obj)\n"
        "    let obj2 = json_parse(back)\n"
        "    print(to_string(obj2[\"a\"]))\n"
        "}\n",
        "1"
    );
}

static void test_json_parse_error(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    json_parse(\"{bad json}\")\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

static void test_json_stringify_error(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    json_stringify(123, 456)\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

/* ======================================================================
 * Math Tests
 * ====================================================================== */

static void test_math_abs(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(to_string(abs(-5)))\n"
        "    print(to_string(abs(5)))\n"
        "    print(to_string(abs(-3.14)))\n"
        "}\n",
        "5\n5\n3.14"
    );
}

static void test_math_floor_ceil_round(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(to_string(floor(3.7)))\n"
        "    print(to_string(ceil(3.2)))\n"
        "    print(to_string(round(3.5)))\n"
        "    print(to_string(round(3.4)))\n"
        "}\n",
        "3\n4\n4\n3"
    );
}

static void test_math_sqrt(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(to_string(sqrt(9)))\n"
        "    print(to_string(sqrt(4)))\n"
        "}\n",
        "3\n2"
    );
}

static void test_math_sqrt_error(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    sqrt(-1)\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

static void test_math_pow(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(to_string(pow(2, 10)))\n"
        "    print(to_string(pow(3, 0)))\n"
        "}\n",
        "1024\n1"
    );
}

static void test_math_min_max(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(to_string(min(3, 7)))\n"
        "    print(to_string(max(3, 7)))\n"
        "    print(to_string(min(1.5, 2.5)))\n"
        "    print(to_string(max(1.5, 2.5)))\n"
        "}\n",
        "3\n7\n1.5\n2.5"
    );
}

static void test_math_random(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let r = random()\n"
        "    if r >= 0.0 {\n"
        "        if r < 1.0 {\n"
        "            print(\"ok\")\n"
        "        }\n"
        "    }\n"
        "}\n",
        "ok"
    );
}

static void test_math_random_int(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let r = random_int(1, 10)\n"
        "    if r >= 1 {\n"
        "        if r <= 10 {\n"
        "            print(\"ok\")\n"
        "        }\n"
        "    }\n"
        "}\n",
        "ok"
    );
}

/* ======================================================================
 * Environment Variable Tests
 * ====================================================================== */

static void test_env_get(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let path = env(\"PATH\")\n"
        "    if len(path) > 0 {\n"
        "        print(\"ok\")\n"
        "    }\n"
        "}\n",
        "ok"
    );
}

static void test_env_get_missing(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let val = env(\"LATTICE_NONEXISTENT_VAR_12345\")\n"
        "    print(to_string(val))\n"
        "}\n",
        "()"
    );
}

static void test_env_set_get(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    env_set(\"LATTICE_TEST_VAR\", \"hello\")\n"
        "    print(env(\"LATTICE_TEST_VAR\"))\n"
        "}\n",
        "hello"
    );
}

static void test_env_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    env(123)\n"
        "}\n",
        "EVAL_ERROR:"
    );
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    env_set(123, \"val\")\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

/* ======================================================================
 * Time Tests
 * ====================================================================== */

static void test_time_now(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let t = time()\n"
        "    if t > 0 {\n"
        "        print(\"ok\")\n"
        "    }\n"
        "}\n",
        "ok"
    );
}

static void test_time_sleep(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let before = time()\n"
        "    sleep(50)\n"
        "    let after = time()\n"
        "    if after - before >= 40 {\n"
        "        print(\"ok\")\n"
        "    }\n"
        "}\n",
        "ok"
    );
}

static void test_time_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    sleep(\"bad\")\n"
        "}\n",
        "EVAL_ERROR:"
    );
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    time(1)\n"
        "}\n",
        "EVAL_ERROR:"
    );
}


/* ======================================================================
 * Filesystem Operations
 * ====================================================================== */

/* test_file_exists - file_exists returns true for existing file, false otherwise */
static void test_file_exists(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    write_file(\"/tmp/lattice_test_exists.txt\", \"hi\")\n"
        "    print(file_exists(\"/tmp/lattice_test_exists.txt\"))\n"
        "    print(file_exists(\"/tmp/lattice_test_no_such_file_xyz.txt\"))\n"
        "}\n",
        "true\nfalse"
    );
    (void)remove("/tmp/lattice_test_exists.txt");
}

/* test_delete_file - delete_file removes an existing file */
static void test_delete_file(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    write_file(\"/tmp/lattice_test_del.txt\", \"bye\")\n"
        "    print(file_exists(\"/tmp/lattice_test_del.txt\"))\n"
        "    delete_file(\"/tmp/lattice_test_del.txt\")\n"
        "    print(file_exists(\"/tmp/lattice_test_del.txt\"))\n"
        "}\n",
        "true\nfalse"
    );
}

/* test_delete_file_error - deleting nonexistent file produces error */
static void test_delete_file_error(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    delete_file(\"/tmp/lattice_test_no_such_file_xyz.txt\")\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

/* test_list_dir - list_dir returns array of filenames */
static void test_list_dir(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
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
        "Array\ntrue\ntrue"
    );
    (void)remove("/tmp/lattice_test_listdir_a.txt");
    (void)remove("/tmp/lattice_test_listdir_b.txt");
}

/* test_list_dir_error - listing nonexistent directory produces error */
static void test_list_dir_error(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    list_dir(\"/tmp/lattice_no_such_dir_xyz\")\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

/* test_append_file - append_file adds data to existing file */
static void test_append_file(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    write_file(\"/tmp/lattice_test_append.txt\", \"hello\")\n"
        "    append_file(\"/tmp/lattice_test_append.txt\", \" world\")\n"
        "    let content = read_file(\"/tmp/lattice_test_append.txt\")\n"
        "    print(content)\n"
        "}\n",
        "hello world"
    );
    (void)remove("/tmp/lattice_test_append.txt");
}

/* test_append_file_creates - append_file creates file if it doesn't exist */
static void test_append_file_creates(void) {
    (void)remove("/tmp/lattice_test_append_new.txt");
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    append_file(\"/tmp/lattice_test_append_new.txt\", \"new content\")\n"
        "    let content = read_file(\"/tmp/lattice_test_append_new.txt\")\n"
        "    print(content)\n"
        "}\n",
        "new content"
    );
    (void)remove("/tmp/lattice_test_append_new.txt");
}

/* test_fs_error_handling - bad arg types produce eval errors */
static void test_fs_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    file_exists(123)\n"
        "}\n",
        "EVAL_ERROR:"
    );
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    delete_file(123)\n"
        "}\n",
        "EVAL_ERROR:"
    );
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    list_dir(123)\n"
        "}\n",
        "EVAL_ERROR:"
    );
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    append_file(123, \"data\")\n"
        "}\n",
        "EVAL_ERROR:"
    );
}


/* ======================================================================
 * Regex Builtins
 * ====================================================================== */

/* regex_match: pattern matches */
static void test_regex_match_true(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(regex_match(\"[0-9]+\", \"abc123\"))\n"
        "}\n",
        "true"
    );
}

/* regex_match: pattern does not match */
static void test_regex_match_false(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(regex_match(\"^[0-9]+$\", \"abc\"))\n"
        "}\n",
        "false"
    );
}

/* regex_match: full string anchor */
static void test_regex_match_anchored(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(regex_match(\"^hello$\", \"hello\"))\n"
        "}\n",
        "true"
    );
}

/* regex_find_all: multiple matches */
static void test_regex_find_all_basic(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let matches = regex_find_all(\"[0-9]+\", \"a1b22c333\")\n"
        "    print(matches)\n"
        "}\n",
        "[1, 22, 333]"
    );
}

/* regex_find_all: no matches returns empty array */
static void test_regex_find_all_no_match(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let matches = regex_find_all(\"[0-9]+\", \"abc\")\n"
        "    print(len(matches))\n"
        "}\n",
        "0"
    );
}

/* regex_find_all: word matches */
static void test_regex_find_all_words(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let matches = regex_find_all(\"[a-z]+\", \"foo123bar456baz\")\n"
        "    print(matches)\n"
        "}\n",
        "[foo, bar, baz]"
    );
}

/* regex_replace: basic replacement */
static void test_regex_replace_basic(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(regex_replace(\"[0-9]+\", \"a1b2\", \"X\"))\n"
        "}\n",
        "aXbX"
    );
}

/* regex_replace: no match returns original */
static void test_regex_replace_no_match(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(regex_replace(\"[0-9]+\", \"abc\", \"X\"))\n"
        "}\n",
        "abc"
    );
}

/* regex_replace: replace all whitespace */
static void test_regex_replace_whitespace(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(regex_replace(\"[ ]+\", \"hello   world   foo\", \"-\"))\n"
        "}\n",
        "hello-world-foo"
    );
}

/* regex_match: bad pattern returns error */
static void test_regex_match_error(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    print(regex_match(\"[\", \"test\"))\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

/* regex_replace: empty replacement (deletion) */
static void test_regex_replace_delete(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(regex_replace(\"[0-9]\", \"a1b2c3\", \"\"))\n"
        "}\n",
        "abc"
    );
}


/* ======================================================================
 * format() Builtin
 * ====================================================================== */

static void test_format_basic(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(format(\"Hello, {}!\", \"world\"))\n"
        "}\n",
        "Hello, world!"
    );
}

static void test_format_multiple(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(format(\"{} + {} = {}\", 1, 2, 3))\n"
        "}\n",
        "1 + 2 = 3"
    );
}

static void test_format_no_placeholders(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(format(\"empty\"))\n"
        "}\n",
        "empty"
    );
}

static void test_format_escaped_braces(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(format(\"{{literal}}\"))\n"
        "}\n",
        "{literal}"
    );
}

static void test_format_bool(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(format(\"{}\", true))\n"
        "}\n",
        "true"
    );
}

static void test_format_too_few_args(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    format(\"{} {}\")\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

static void test_format_mixed_types(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(format(\"{} is {} and {}\", \"pi\", 3.14, true))\n"
        "}\n",
        "pi is 3.14 and true"
    );
}

static void test_format_error_non_string_fmt(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    format(42)\n"
        "}\n",
        "EVAL_ERROR:"
    );
}


/* ======================================================================
 * Crypto / Base64 Tests
 * ====================================================================== */

#ifdef LATTICE_HAS_TLS
static void test_sha256_empty(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(sha256(\"\"))\n"
        "}\n",
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    );
}

static void test_sha256_hello(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(sha256(\"hello\"))\n"
        "}\n",
        "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
    );
}

static void test_md5_empty(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(md5(\"\"))\n"
        "}\n",
        "d41d8cd98f00b204e9800998ecf8427e"
    );
}

static void test_md5_hello(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(md5(\"hello\"))\n"
        "}\n",
        "5d41402abc4b2a76b9719d911017c592"
    );
}
#endif

static void test_sha256_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    sha256(123)\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

static void test_md5_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    md5(123)\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

static void test_base64_encode_hello(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(base64_encode(\"Hello\"))\n"
        "}\n",
        "SGVsbG8="
    );
}

static void test_base64_encode_empty(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(base64_encode(\"\"))\n"
        "}\n",
        ""
    );
}

static void test_base64_decode_hello(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(base64_decode(\"SGVsbG8=\"))\n"
        "}\n",
        "Hello"
    );
}

static void test_base64_decode_empty(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(base64_decode(\"\"))\n"
        "}\n",
        ""
    );
}

static void test_base64_roundtrip(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(base64_decode(base64_encode(\"test\")))\n"
        "}\n",
        "test"
    );
}

static void test_base64_roundtrip_longer(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(base64_decode(base64_encode(\"Hello, World!\")))\n"
        "}\n",
        "Hello, World!"
    );
}

static void test_base64_encode_padding(void) {
    /* 1 byte -> 4 chars with == padding */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(base64_encode(\"a\"))\n"
        "}\n",
        "YQ=="
    );
    /* 2 bytes -> 4 chars with = padding */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(base64_encode(\"ab\"))\n"
        "}\n",
        "YWI="
    );
    /* 3 bytes -> 4 chars, no padding */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(base64_encode(\"abc\"))\n"
        "}\n",
        "YWJj"
    );
}

static void test_base64_decode_error(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    base64_decode(\"!!!\")\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

static void test_base64_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    base64_encode(123)\n"
        "}\n",
        "EVAL_ERROR:"
    );
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    base64_decode(123)\n"
        "}\n",
        "EVAL_ERROR:"
    );
}


/* ======================================================================
 * Array: sort, flat, reduce, slice
 * ====================================================================== */

static void test_array_sort_int(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([3, 1, 2].sort())\n"
        "}\n",
        "[1, 2, 3]"
    );
}

static void test_array_sort_string(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([\"c\", \"a\", \"b\"].sort())\n"
        "}\n",
        "[a, b, c]"
    );
}

static void test_array_sort_float(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([3.1, 1.5, 2.7].sort())\n"
        "}\n",
        "[1.5, 2.7, 3.1]"
    );
}

static void test_array_sort_empty(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([].sort())\n"
        "}\n",
        "[]"
    );
}

static void test_array_sort_mixed_error(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    [1, \"a\"].sort()\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

static void test_array_flat_basic(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([1, [2, 3], [4]].flat())\n"
        "}\n",
        "[1, 2, 3, 4]"
    );
}

static void test_array_flat_no_nesting(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([1, 2, 3].flat())\n"
        "}\n",
        "[1, 2, 3]"
    );
}

static void test_array_flat_empty(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([].flat())\n"
        "}\n",
        "[]"
    );
}

static void test_array_reduce_sum(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([1, 2, 3].reduce(|acc, x| { acc + x }, 0))\n"
        "}\n",
        "6"
    );
}

static void test_array_reduce_product(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([1, 2, 3, 4].reduce(|acc, x| { acc * x }, 1))\n"
        "}\n",
        "24"
    );
}

static void test_array_reduce_string_concat(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([\"a\", \"b\", \"c\"].reduce(|acc, x| { acc + x }, \"\"))\n"
        "}\n",
        "abc"
    );
}

static void test_array_reduce_empty(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([].reduce(|acc, x| { acc + x }, 42))\n"
        "}\n",
        "42"
    );
}

static void test_array_slice_basic(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([1, 2, 3, 4, 5].slice(1, 3))\n"
        "}\n",
        "[2, 3]"
    );
}

static void test_array_slice_full(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([1, 2, 3].slice(0, 3))\n"
        "}\n",
        "[1, 2, 3]"
    );
}

static void test_array_slice_empty(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([1, 2, 3].slice(1, 1))\n"
        "}\n",
        "[]"
    );
}

static void test_array_slice_clamped(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([1, 2, 3].slice(0, 100))\n"
        "}\n",
        "[1, 2, 3]"
    );
}


/* ======================================================================
 * Date/Time Formatting Tests
 * ====================================================================== */

/* time_parse returns a positive Int for a valid date */
static void test_time_parse_basic(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let ms = time_parse(\"2024-01-15\", \"%Y-%m-%d\")\n"
        "    print(ms > 0)\n"
        "}\n",
        "true"
    );
}

/* time_format produces a non-empty string */
static void test_time_format_basic(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let s = time_format(0, \"%Y\")\n"
        "    print(s.len() == 4)\n"
        "}\n",
        "true"
    );
}

/* Round-trip: format then parse should recover the same timestamp (to second precision) */
static void test_time_roundtrip(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let ms = time_parse(\"2024-06-15 12:30:45\", \"%Y-%m-%d %H:%M:%S\")\n"
        "    let formatted = time_format(ms, \"%Y-%m-%d %H:%M:%S\")\n"
        "    let ms2 = time_parse(formatted, \"%Y-%m-%d %H:%M:%S\")\n"
        "    print(ms == ms2)\n"
        "}\n",
        "true"
    );
}

/* time_format with ISO date format produces expected length (10 chars for YYYY-MM-DD) */
static void test_time_format_iso_date(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let s = time_format(1000000000000, \"%Y-%m-%d\")\n"
        "    print(s.len() == 10)\n"
        "}\n",
        "true"
    );
}

/* time_parse error: invalid date string */
static void test_time_parse_error(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    time_parse(\"not-a-date\", \"%Y-%m-%d\")\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

/* time_format error: wrong arg types */
static void test_time_format_error(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    time_format(\"bad\", \"%Y\")\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

/* time_parse error: wrong arg types */
static void test_time_parse_type_error(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    time_parse(123, \"%Y\")\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

/* time_format with time components */
static void test_time_format_time_components(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let s = time_format(1000000000000, \"%H:%M:%S\")\n"
        "    print(s.len() == 8)\n"
        "}\n",
        "true"
    );
}


/* ======================================================================
 * Path Operations
 * ====================================================================== */

/* test_path_join - join path components */
static void test_path_join(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(path_join(\"foo\", \"bar\", \"baz.txt\"))\n"
        "}\n",
        "foo/bar/baz.txt"
    );
    /* Single argument */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(path_join(\"hello\"))\n"
        "}\n",
        "hello"
    );
    /* Avoid double slashes */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(path_join(\"foo/\", \"/bar\"))\n"
        "}\n",
        "foo/bar"
    );
    /* Absolute path */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(path_join(\"/usr\", \"local\", \"bin\"))\n"
        "}\n",
        "/usr/local/bin"
    );
}

/* test_path_dir - extract directory portion */
static void test_path_dir(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(path_dir(\"/foo/bar.txt\"))\n"
        "}\n",
        "/foo"
    );
    /* No slash returns "." */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(path_dir(\"bar.txt\"))\n"
        "}\n",
        "."
    );
    /* Root path */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(path_dir(\"/\"))\n"
        "}\n",
        "/"
    );
    /* Nested */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(path_dir(\"/a/b/c/d.txt\"))\n"
        "}\n",
        "/a/b/c"
    );
}

/* test_path_base - extract base filename */
static void test_path_base(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(path_base(\"/foo/bar.txt\"))\n"
        "}\n",
        "bar.txt"
    );
    /* No directory */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(path_base(\"file.txt\"))\n"
        "}\n",
        "file.txt"
    );
    /* Trailing slash returns empty */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(path_base(\"/foo/\"))\n"
        "}\n",
        ""
    );
}

/* test_path_ext - extract file extension */
static void test_path_ext(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(path_ext(\"file.tar.gz\"))\n"
        "}\n",
        ".gz"
    );
    /* No extension */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(path_ext(\"Makefile\"))\n"
        "}\n",
        ""
    );
    /* Hidden file (dot-prefixed, no extension) */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(path_ext(\".hidden\"))\n"
        "}\n",
        ""
    );
    /* Simple extension */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(path_ext(\"foo.txt\"))\n"
        "}\n",
        ".txt"
    );
    /* Extension in path with directory */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(path_ext(\"/usr/local/foo.c\"))\n"
        "}\n",
        ".c"
    );
}

/* test_path_error_handling - bad arg types produce eval errors */
static void test_path_error_handling(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    path_join(123)\n"
        "}\n",
        "EVAL_ERROR:"
    );
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    path_dir(123)\n"
        "}\n",
        "EVAL_ERROR:"
    );
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    path_base(123)\n"
        "}\n",
        "EVAL_ERROR:"
    );
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    path_ext(123)\n"
        "}\n",
        "EVAL_ERROR:"
    );
}


/* ======================================================================
 * Channel & Scope Tests
 * ====================================================================== */

static void test_channel_basic_send_recv(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let ch = Channel::new()\n"
        "    ch.send(freeze(42))\n"
        "    let val = ch.recv()\n"
        "    print(val)\n"
        "}\n",
        "42"
    );
}

static void test_scope_two_spawns_channels(void) {
    ASSERT_OUTPUT(
        "fn compute_a() -> Int { return 10 }\n"
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
        "30"
    );
}

static void test_channel_close_recv_unit(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let ch = Channel::new()\n"
        "    ch.send(freeze(1))\n"
        "    ch.close()\n"
        "    let a = ch.recv()\n"
        "    let b = ch.recv()\n"
        "    print(a)\n"
        "    print(typeof(b))\n"
        "}\n",
        "1\nUnit"
    );
}

static void test_channel_crystal_only_send(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    let ch = Channel::new()\n"
        "    flux arr = [1, 2, 3]\n"
        "    ch.send(arr)\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

static void test_scope_no_spawns_sequential(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let x = scope {\n"
        "        let a = 10\n"
        "        let b = 20\n"
        "        a + b\n"
        "    }\n"
        "    print(x)\n"
        "}\n",
        "30"
    );
}

static void test_spawn_outside_scope(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let x = spawn {\n"
        "        let a = 5\n"
        "        let b = 10\n"
        "        return a + b\n"
        "    }\n"
        "    print(x)\n"
        "}\n",
        "15"
    );
}

static void test_channel_multiple_sends_fifo(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let ch = Channel::new()\n"
        "    ch.send(freeze(1))\n"
        "    ch.send(freeze(2))\n"
        "    ch.send(freeze(3))\n"
        "    print(ch.recv())\n"
        "    print(ch.recv())\n"
        "    print(ch.recv())\n"
        "}\n",
        "1\n2\n3"
    );
}

static void test_scope_spawn_error_propagates(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn bad() -> Int {\n"
        "    let x = 1 / 0\n"
        "    return x\n"
        "}\n"
        "fn main() {\n"
        "    scope {\n"
        "        spawn { bad() }\n"
        "    }\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

static void test_cannot_freeze_channel(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() {\n"
        "    let ch = Channel::new()\n"
        "    let frozen = freeze(ch)\n"
        "}\n",
        "EVAL_ERROR:"
    );
}

static void test_channel_typeof(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let ch = Channel::new()\n"
        "    print(typeof(ch))\n"
        "}\n",
        "Channel"
    );
}

/* ── Array method tests ── */

static void test_array_pop(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux arr = [1, 2, 3]\n"
        "    print(arr.pop())\n"
        "    print(arr)\n"
        "}\n",
        "3\n[1, 2]"
    );
}

static void test_array_index_of(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let arr = [10, 20, 30]\n"
        "    print(arr.index_of(20))\n"
        "    print(arr.index_of(99))\n"
        "}\n",
        "1\n-1"
    );
}

static void test_array_any_all(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let arr = [1, 2, 3]\n"
        "    print(arr.any(|x| { x > 2 }))\n"
        "    print(arr.all(|x| { x > 0 }))\n"
        "    print(arr.all(|x| { x > 1 }))\n"
        "    print(arr.any(|x| { x > 10 }))\n"
        "}\n",
        "true\ntrue\nfalse\nfalse"
    );
}

static void test_array_zip(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let a = [1, 2, 3]\n"
        "    let b = [4, 5]\n"
        "    print(a.zip(b))\n"
        "}\n",
        "[[1, 4], [2, 5]]"
    );
}

static void test_array_unique(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([1, 2, 3, 2, 1, 4].unique())\n"
        "}\n",
        "[1, 2, 3, 4]"
    );
}

static void test_array_insert(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux arr = [1, 2, 3]\n"
        "    arr.insert(1, 10)\n"
        "    print(arr)\n"
        "}\n",
        "[1, 10, 2, 3]"
    );
}

static void test_array_remove_at(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux arr = [1, 2, 3]\n"
        "    print(arr.remove_at(1))\n"
        "    print(arr)\n"
        "}\n",
        "2\n[1, 3]"
    );
}

static void test_array_sort_by(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let arr = [3, 1, 4, 1, 5]\n"
        "    print(arr.sort_by(|a, b| { a - b }))\n"
        "    print(arr.sort_by(|a, b| { b - a }))\n"
        "}\n",
        "[1, 1, 3, 4, 5]\n[5, 4, 3, 1, 1]"
    );
}

/* ── Map method tests ── */

static void test_map_entries(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    m.set(\"a\", 1)\n"
        "    let e = m.entries()\n"
        "    print(len(e))\n"
        "    print(e[0][0])\n"
        "    print(e[0][1])\n"
        "}\n",
        "1\na\n1"
    );
}

static void test_map_merge(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m1 = Map::new()\n"
        "    m1.set(\"a\", 1)\n"
        "    flux m2 = Map::new()\n"
        "    m2.set(\"b\", 2)\n"
        "    m1.merge(m2)\n"
        "    print(m1.has(\"b\"))\n"
        "    print(m1.get(\"b\"))\n"
        "}\n",
        "true\n2"
    );
}

static void test_map_for_each(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    m.set(\"x\", 10)\n"
        "    m.for_each(|k, v| { print(format(\"{} -> {}\", k, v)) })\n"
        "}\n",
        "x -> 10"
    );
}

/* ── String method tests ── */

static void test_str_trim_start(void) {
    ASSERT_OUTPUT(
        "fn main() { print(\"  hi  \".trim_start()) }\n",
        "hi  "
    );
}

static void test_str_trim_end(void) {
    ASSERT_OUTPUT(
        "fn main() { print(\"  hi  \".trim_end()) }\n",
        "  hi"
    );
}

static void test_str_pad_left(void) {
    ASSERT_OUTPUT(
        "fn main() { print(\"42\".pad_left(5, \"0\")) }\n",
        "00042"
    );
}

static void test_str_pad_right(void) {
    ASSERT_OUTPUT(
        "fn main() { print(\"hi\".pad_right(5, \".\")) }\n",
        "hi..."
    );
}

/* ── Math function tests ── */

static void test_math_log(void) {
    ASSERT_OUTPUT(
        "fn main() { print(log(math_e())) }\n",
        "1"
    );
}

static void test_math_log2(void) {
    ASSERT_OUTPUT(
        "fn main() { print(log2(8)) }\n",
        "3"
    );
}

static void test_math_log10(void) {
    ASSERT_OUTPUT(
        "fn main() { print(log10(1000)) }\n",
        "3"
    );
}

static void test_math_trig(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(sin(0.0))\n"
        "    print(cos(0.0))\n"
        "    print(tan(0.0))\n"
        "}\n",
        "0\n1\n0"
    );
}

static void test_math_atan2(void) {
    ASSERT_OUTPUT(
        "fn main() { print(atan2(0.0, 1.0)) }\n",
        "0"
    );
}

static void test_math_clamp(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(clamp(5, 1, 10))\n"
        "    print(clamp(-3, 0, 100))\n"
        "    print(clamp(200, 0, 100))\n"
        "}\n",
        "5\n0\n100"
    );
}

static void test_math_pi_e(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(math_pi() > 3.14)\n"
        "    print(math_e() > 2.71)\n"
        "}\n",
        "true\ntrue"
    );
}

static void test_math_inverse_trig(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(format(\"{}\", asin(0.0)))\n"
        "    print(format(\"{}\", acos(1.0)))\n"
        "    print(format(\"{}\", atan(0.0)))\n"
        "}\n",
        "0\n0\n0"
    );
}

static void test_math_exp(void) {
    ASSERT_OUTPUT(
        "fn main() { print(format(\"{}\", exp(0.0))) }\n",
        "1"
    );
}

static void test_math_sign(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(sign(-5))\n"
        "    print(sign(0))\n"
        "    print(sign(42))\n"
        "}\n",
        "-1\n0\n1"
    );
}

static void test_math_gcd_lcm(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(gcd(12, 8))\n"
        "    print(lcm(4, 6))\n"
        "}\n",
        "4\n12"
    );
}

static void test_is_nan_inf(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(is_nan(0.0 / 0.0))\n"
        "    print(is_nan(1.0))\n"
        "    print(is_inf(1.0 / 0.0))\n"
        "    print(is_inf(1.0))\n"
        "}\n",
        "true\nfalse\ntrue\nfalse"
    );
}

/* ── System/FS tests ── */

static void test_cwd_builtin(void) {
    char *out = run_capture("fn main() { print(cwd()) }\n");
    ASSERT(strlen(out) > 0);
    ASSERT(out[0] == '/');
    free(out);
}

static void test_is_dir_file(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(is_dir(\".\"))\n"
        "    print(is_file(\"Makefile\"))\n"
        "    print(is_dir(\"Makefile\"))\n"
        "    print(is_file(\"nonexistent\"))\n"
        "}\n",
        "true\ntrue\nfalse\nfalse"
    );
}

static void test_mkdir_builtin(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let dir = \"/tmp/lattice_test_mkdir_\" + to_string(time())\n"
        "    print(mkdir(dir))\n"
        "    print(is_dir(dir))\n"
        "}\n",
        "true\ntrue"
    );
}

static void test_rename_builtin(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let f1 = \"/tmp/lattice_rename_src_\" + to_string(time())\n"
        "    let f2 = \"/tmp/lattice_rename_dst_\" + to_string(time())\n"
        "    write_file(f1, \"hello\")\n"
        "    print(rename(f1, f2))\n"
        "    print(file_exists(f1))\n"
        "    print(file_exists(f2))\n"
        "    delete_file(f2)\n"
        "}\n",
        "true\nfalse\ntrue"
    );
}

static void test_assert_pass(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    assert(true)\n"
        "    assert(1 + 1 == 2, \"math works\")\n"
        "    print(\"ok\")\n"
        "}\n",
        "ok"
    );
}

static void test_assert_fail(void) {
    char *out = run_capture(
        "fn main() { assert(false, \"should fail\") }\n"
    );
    ASSERT(strstr(out, "EVAL_ERROR") != NULL);
    ASSERT(strstr(out, "should fail") != NULL);
    free(out);
}

static void test_args_builtin(void) {
    char *out = run_capture(
        "fn main() { print(typeof(args())) }\n"
    );
    ASSERT_STR_EQ(out, "Array");
    free(out);
}

/* ── Map .filter() and .map() tests ── */

static void test_map_filter(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    m.set(\"a\", 1)\n"
        "    m.set(\"b\", 2)\n"
        "    m.set(\"c\", 3)\n"
        "    let filtered = m.filter(|k, v| { v > 1 })\n"
        "    print(filtered.len())\n"
        "}\n",
        "2"
    );
    /* Filter that matches nothing */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    m.set(\"x\", 10)\n"
        "    let filtered = m.filter(|k, v| { v > 100 })\n"
        "    print(filtered.len())\n"
        "}\n",
        "0"
    );
}

static void test_map_map(void) {
    /* Single-entry map for deterministic output */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    m.set(\"x\", 5)\n"
        "    let doubled = m.map(|k, v| { v * 2 })\n"
        "    print(doubled.get(\"x\"))\n"
        "}\n",
        "10"
    );
    /* Map preserves all keys */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    flux m = Map::new()\n"
        "    m.set(\"a\", 1)\n"
        "    m.set(\"b\", 2)\n"
        "    m.set(\"c\", 3)\n"
        "    let mapped = m.map(|k, v| { v + 10 })\n"
        "    print(mapped.len())\n"
        "}\n",
        "3"
    );
}

/* ── String .count() and .is_empty() tests ── */

static void test_str_count(void) {
    ASSERT_OUTPUT(
        "fn main() { print(\"hello world hello\".count(\"hello\")) }\n",
        "2"
    );
    /* Zero matches */
    ASSERT_OUTPUT(
        "fn main() { print(\"abcdef\".count(\"xyz\")) }\n",
        "0"
    );
    /* Overlapping: non-overlapping count */
    ASSERT_OUTPUT(
        "fn main() { print(\"aaa\".count(\"aa\")) }\n",
        "1"
    );
}

static void test_str_is_empty(void) {
    ASSERT_OUTPUT(
        "fn main() { print(\"\".is_empty()) }\n",
        "true"
    );
    ASSERT_OUTPUT(
        "fn main() { print(\"hello\".is_empty()) }\n",
        "false"
    );
}

/* ── process exec/shell builtins ── */

static void test_exec_builtin(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let result = exec(\"echo hello\")\n"
        "    print(result.trim())\n"
        "}\n",
        "hello"
    );
}

static void test_shell_builtin(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let r = shell(\"echo hello\")\n"
        "    print(r.get(\"stdout\").trim())\n"
        "    print(r.get(\"exit_code\"))\n"
        "}\n",
        "hello\n0"
    );
}

static void test_shell_stderr(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let r = shell(\"echo err >&2\")\n"
        "    print(r.get(\"stderr\").trim())\n"
        "    print(r.get(\"exit_code\"))\n"
        "}\n",
        "err\n0"
    );
}

static void test_exec_failure(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let r = shell(\"exit 42\")\n"
        "    print(r.get(\"exit_code\"))\n"
        "}\n",
        "42"
    );
}

/* ======================================================================
 * New filesystem builtins: rmdir, glob, stat, copy_file, realpath, tempdir, tempfile
 * ====================================================================== */

static void test_rmdir_builtin(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let dir = \"/tmp/lattice_test_rmdir_\" + to_string(time())\n"
        "    mkdir(dir)\n"
        "    print(rmdir(dir))\n"
        "    print(is_dir(dir))\n"
        "}\n",
        "true\nfalse"
    );
}

static void test_rmdir_error(void) {
    char *out = run_capture(
        "fn main() { rmdir(\"/tmp/nonexistent_lattice_dir_999\") }\n"
    );
    ASSERT(strstr(out, "EVAL_ERROR") != NULL);
    ASSERT(strstr(out, "rmdir") != NULL);
    free(out);
}

static void test_glob_builtin(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
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
        "2"
    );
}

static void test_glob_no_match(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let matches = glob(\"/tmp/lattice_nonexistent_glob_*.xyz\")\n"
        "    print(len(matches))\n"
        "}\n",
        "0"
    );
}

static void test_stat_builtin(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let f = \"/tmp/lattice_test_stat_\" + to_string(time())\n"
        "    write_file(f, \"hello\")\n"
        "    let s = stat(f)\n"
        "    print(s.get(\"size\"))\n"
        "    print(s.get(\"type\"))\n"
        "    print(s.get(\"mtime\") > 0)\n"
        "    print(s.get(\"permissions\") > 0)\n"
        "    delete_file(f)\n"
        "}\n",
        "5\nfile\ntrue\ntrue"
    );
}

static void test_stat_dir(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let s = stat(\".\")\n"
        "    print(s.get(\"type\"))\n"
        "}\n",
        "dir"
    );
}

static void test_stat_error(void) {
    char *out = run_capture(
        "fn main() { stat(\"/tmp/nonexistent_lattice_stat_999\") }\n"
    );
    ASSERT(strstr(out, "EVAL_ERROR") != NULL);
    ASSERT(strstr(out, "stat") != NULL);
    free(out);
}

static void test_copy_file_builtin(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let src = \"/tmp/lattice_test_cp_src_\" + to_string(time())\n"
        "    let dst = \"/tmp/lattice_test_cp_dst_\" + to_string(time())\n"
        "    write_file(src, \"copy me\")\n"
        "    print(copy_file(src, dst))\n"
        "    print(read_file(dst))\n"
        "    delete_file(src)\n"
        "    delete_file(dst)\n"
        "}\n",
        "true\ncopy me"
    );
}

static void test_copy_file_error(void) {
    char *out = run_capture(
        "fn main() { copy_file(\"/tmp/nonexistent_lattice_cp_999\", \"/tmp/out\") }\n"
    );
    ASSERT(strstr(out, "EVAL_ERROR") != NULL);
    ASSERT(strstr(out, "copy_file") != NULL);
    free(out);
}

static void test_realpath_builtin(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let rp = realpath(\".\")\n"
        "    print(rp.starts_with(\"/\"))\n"
        "}\n",
        "true"
    );
}

static void test_realpath_error(void) {
    char *out = run_capture(
        "fn main() { realpath(\"/tmp/nonexistent_lattice_rp_999\") }\n"
    );
    ASSERT(strstr(out, "EVAL_ERROR") != NULL);
    ASSERT(strstr(out, "realpath") != NULL);
    free(out);
}

static void test_tempdir_builtin(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let d = tempdir()\n"
        "    print(is_dir(d))\n"
        "    rmdir(d)\n"
        "}\n",
        "true"
    );
}

static void test_tempfile_builtin(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let f = tempfile()\n"
        "    print(is_file(f))\n"
        "    delete_file(f)\n"
        "}\n",
        "true"
    );
}

/* ======================================================================
 * Array: flat_map, chunk, group_by, sum, min, max, first, last
 * ====================================================================== */

static void test_array_flat_map(void) {
    ASSERT_OUTPUT(
        "fn main() { print([1, 2, 3].flat_map(|x| { [x, x * 10] })) }\n",
        "[1, 10, 2, 20, 3, 30]"
    );
    /* Single values (not arrays) are kept as-is */
    ASSERT_OUTPUT(
        "fn main() { print([1, 2, 3].flat_map(|x| { x + 1 })) }\n",
        "[2, 3, 4]"
    );
    /* Empty array */
    ASSERT_OUTPUT(
        "fn main() { print([].flat_map(|x| { [x] })) }\n",
        "[]"
    );
}

static void test_array_chunk(void) {
    /* Even split */
    ASSERT_OUTPUT(
        "fn main() { print([1, 2, 3, 4].chunk(2)) }\n",
        "[[1, 2], [3, 4]]"
    );
    /* Uneven split */
    ASSERT_OUTPUT(
        "fn main() { print([1, 2, 3, 4, 5].chunk(2)) }\n",
        "[[1, 2], [3, 4], [5]]"
    );
    /* Chunk size larger than array */
    ASSERT_OUTPUT(
        "fn main() { print([1, 2].chunk(5)) }\n",
        "[[1, 2]]"
    );
    /* Empty array */
    ASSERT_OUTPUT(
        "fn main() { print([].chunk(3)) }\n",
        "[]"
    );
}

static void test_array_group_by(void) {
    /* Group by even/odd using map access to avoid order issues */
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let g = [1, 2, 3, 4, 5].group_by(|x| { x % 2 })\n"
        "    print(g.get(\"0\"))\n"
        "    print(g.get(\"1\"))\n"
        "}\n",
        "[2, 4]\n[1, 3, 5]"
    );
}

static void test_array_sum(void) {
    /* Integer sum */
    ASSERT_OUTPUT(
        "fn main() { print([1, 2, 3, 4, 5].sum()) }\n",
        "15"
    );
    /* Float sum */
    ASSERT_OUTPUT(
        "fn main() { print([1.5, 2.5, 3.0].sum()) }\n",
        "7"
    );
    /* Empty array */
    ASSERT_OUTPUT(
        "fn main() { print([].sum()) }\n",
        "0"
    );
}

static void test_array_min_max(void) {
    /* Int min/max */
    ASSERT_OUTPUT(
        "fn main() { print([3, 1, 4, 1, 5].min()) }\n",
        "1"
    );
    ASSERT_OUTPUT(
        "fn main() { print([3, 1, 4, 1, 5].max()) }\n",
        "5"
    );
    /* Float min/max */
    ASSERT_OUTPUT(
        "fn main() { print([3.5, 1.2, 4.8].min()) }\n",
        "1.2"
    );
    ASSERT_OUTPUT(
        "fn main() { print([3.5, 1.2, 4.8].max()) }\n",
        "4.8"
    );
    /* Error on empty array */
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() { print([].min()) }\n",
        "EVAL_ERROR"
    );
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() { print([].max()) }\n",
        "EVAL_ERROR"
    );
}

static void test_array_first_last(void) {
    /* Non-empty */
    ASSERT_OUTPUT(
        "fn main() { print([10, 20, 30].first()) }\n",
        "10"
    );
    ASSERT_OUTPUT(
        "fn main() { print([10, 20, 30].last()) }\n",
        "30"
    );
    /* Empty returns unit */
    ASSERT_OUTPUT(
        "fn main() { print([].first()) }\n",
        "()"
    );
    ASSERT_OUTPUT(
        "fn main() { print([].last()) }\n",
        "()"
    );
}

/* ======================================================================
 * range() builtin
 * ====================================================================== */

static void test_range_basic(void) {
    ASSERT_OUTPUT(
        "fn main() { print(range(0, 5)) }\n",
        "[0, 1, 2, 3, 4]"
    );
    /* Negative direction auto-detects step */
    ASSERT_OUTPUT(
        "fn main() { print(range(5, 0)) }\n",
        "[5, 4, 3, 2, 1]"
    );
}

static void test_range_with_step(void) {
    ASSERT_OUTPUT(
        "fn main() { print(range(0, 10, 3)) }\n",
        "[0, 3, 6, 9]"
    );
    /* Negative step */
    ASSERT_OUTPUT(
        "fn main() { print(range(10, 0, -2)) }\n",
        "[10, 8, 6, 4, 2]"
    );
}

static void test_range_empty(void) {
    /* Wrong direction for step produces empty */
    ASSERT_OUTPUT(
        "fn main() { print(range(0, 5, -1)) }\n",
        "[]"
    );
    /* Same start and end */
    ASSERT_OUTPUT(
        "fn main() { print(range(3, 3)) }\n",
        "[]"
    );
}

static void test_range_step_zero(void) {
    ASSERT_OUTPUT_STARTS_WITH(
        "fn main() { print(range(0, 5, 0)) }\n",
        "EVAL_ERROR"
    );
}

/* ======================================================================
 * String .bytes()
 * ====================================================================== */

static void test_str_bytes(void) {
    ASSERT_OUTPUT(
        "fn main() { print(\"ABC\".bytes()) }\n",
        "[65, 66, 67]"
    );
}

/* ======================================================================
 * Array .take() and .drop()
 * ====================================================================== */

static void test_array_take(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([1, 2, 3, 4, 5].take(3))\n"
        "    print([1, 2].take(5))\n"
        "    print([1, 2, 3].take(0))\n"
        "}\n",
        "[1, 2, 3]\n[1, 2]\n[]"
    );
}

static void test_array_drop(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print([1, 2, 3, 4, 5].drop(2))\n"
        "    print([1, 2].drop(5))\n"
        "    print([1, 2, 3].drop(0))\n"
        "}\n",
        "[3, 4, 5]\n[]\n[1, 2, 3]"
    );
}

/* ======================================================================
 * error() and is_error() builtins
 * ====================================================================== */

static void test_error_builtin(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let e = error(\"something went wrong\")\n"
        "    print(is_error(e))\n"
        "    print(is_error(42))\n"
        "    print(is_error(\"hello\"))\n"
        "}\n",
        "true\nfalse\nfalse"
    );
}

/* ======================================================================
 * System info builtins: platform, hostname, pid
 * ====================================================================== */

static void test_platform_builtin(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let p = platform()\n"
        "    print(len(p) > 0)\n"
        "}\n",
        "true"
    );
}

static void test_hostname_builtin(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let h = hostname()\n"
        "    print(len(h) > 0)\n"
        "}\n",
        "true"
    );
}

static void test_pid_builtin(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    let p = pid()\n"
        "    print(p > 0)\n"
        "}\n",
        "true"
    );
}

/* ======================================================================
 * env_keys builtin
 * ====================================================================== */

static void test_env_keys(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    env_set(\"LATTICE_TEST_KEY\", \"1\")\n"
        "    let keys = env_keys()\n"
        "    print(keys.contains(\"LATTICE_TEST_KEY\"))\n"
        "}\n",
        "true"
    );
}

/* ======================================================================
 * URL encoding builtins
 * ====================================================================== */

static void test_url_encode(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(url_encode(\"hello world\"))\n"
        "    print(url_encode(\"foo=bar&baz=1\"))\n"
        "}\n",
        "hello%20world\nfoo%3Dbar%26baz%3D1"
    );
}

static void test_url_decode(void) {
    ASSERT_OUTPUT(
        "fn main() {\n"
        "    print(url_decode(\"hello%20world\"))\n"
        "    print(url_decode(\"foo+bar\"))\n"
        "}\n",
        "hello world\nfoo bar"
    );
}

/* ======================================================================
 * Test Registration
 * ====================================================================== */

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
#ifdef LATTICE_HAS_TLS
    register_test("test_sha256_empty", test_sha256_empty);
    register_test("test_sha256_hello", test_sha256_hello);
    register_test("test_md5_empty", test_md5_empty);
    register_test("test_md5_hello", test_md5_hello);
#endif
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
}
