#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lattice.h"
#include "lexer.h"
#include "parser.h"
#include "eval.h"

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
        "0.1.1"
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
}
