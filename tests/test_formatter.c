/* Correctness tests for formatter.c — golden output, idempotence,
 * comment/string preservation. (fuzz_formatter only checks for crashes.) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "formatter.h"

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

#define ASSERT_EQ_STR(a, b)                                                                            \
    do {                                                                                               \
        const char *_a = (a), *_b = (b);                                                               \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                                                       \
            fprintf(stderr, "  FAIL: %s:%d:\n--- got ---\n%s\n--- want ---\n%s\n", __FILE__, __LINE__, \
                    _a ? _a : "(null)", _b ? _b : "(null)");                                           \
            test_current_failed = 1;                                                                   \
            return;                                                                                    \
        }                                                                                              \
    } while (0)

#define ASSERT_CONTAINS(haystack, needle)                                                           \
    do {                                                                                            \
        const char *_h = (haystack), *_n = (needle);                                                \
        if (!_h || !strstr(_h, _n)) {                                                               \
            fprintf(stderr, "  FAIL: %s:%d: \"%s\" not found in output\n", __FILE__, __LINE__, _n); \
            test_current_failed = 1;                                                                \
            return;                                                                                 \
        }                                                                                           \
    } while (0)

#define TEST(name)                                                    \
    static void name(void);                                           \
    static void name##_register(void) __attribute__((constructor));   \
    static void name##_register(void) { register_test(#name, name); } \
    static void name(void)

/* Assert that formatting `src` yields exactly `want` */
static int golden(const char *src, const char *want, const char *file, int line) {
    char *err = NULL;
    char *out = lat_format(src, 0, &err);
    if (!out) {
        fprintf(stderr, "  FAIL: %s:%d: lat_format returned NULL (%s)\n", file, line, err ? err : "no error");
        free(err);
        return 0;
    }
    if (strcmp(out, want) != 0) {
        fprintf(stderr, "  FAIL: %s:%d:\n--- got ---\n%s\n--- want ---\n%s\n", file, line, out, want);
        free(out);
        return 0;
    }
    free(out);
    return 1;
}
#define GOLDEN(src, want)                             \
    do {                                              \
        if (!golden(src, want, __FILE__, __LINE__)) { \
            test_current_failed = 1;                  \
            return;                                   \
        }                                             \
    } while (0)

/* Assert format(format(src)) == format(src) */
static int idempotent(const char *src, const char *file, int line) {
    char *err = NULL;
    char *once = lat_format(src, 0, &err);
    if (!once) {
        fprintf(stderr, "  FAIL: %s:%d: first format returned NULL (%s)\n", file, line, err ? err : "no error");
        free(err);
        return 0;
    }
    char *twice = lat_format(once, 0, &err);
    if (!twice) {
        fprintf(stderr, "  FAIL: %s:%d: second format returned NULL (%s)\n", file, line, err ? err : "no error");
        free(once);
        free(err);
        return 0;
    }
    int ok = strcmp(once, twice) == 0;
    if (!ok) {
        fprintf(stderr, "  FAIL: %s:%d: not idempotent\n--- once ---\n%s\n--- twice ---\n%s\n", file, line, once,
                twice);
    }
    free(once);
    free(twice);
    return ok;
}
#define IDEMPOTENT(src)                             \
    do {                                            \
        if (!idempotent(src, __FILE__, __LINE__)) { \
            test_current_failed = 1;                \
            return;                                 \
        }                                           \
    } while (0)

/* ── Golden tests: canonical style ── */

TEST(fmt_function_signature_spacing) {
    GOLDEN("fn add(a:Int,b:Int)->Int{return a+b}\n", "fn add(a: Int, b: Int) -> Int {\n"
                                                     "    return a + b\n"
                                                     "}\n");
}

TEST(fmt_let_and_array_spacing) { GOLDEN("let x=[1,2,3]\n", "let x = [1, 2, 3]\n"); }

TEST(fmt_scope_spawn_blocks) {
    GOLDEN("scope{spawn{print(\"a\")}}\n", "scope {\n"
                                           "    spawn {\n"
                                           "        print(\"a\")\n"
                                           "    }\n"
                                           "}\n");
}

TEST(fmt_already_formatted_unchanged) {
    const char *src = "fn add(a: Int, b: Int) -> Int {\n"
                      "    return a + b\n"
                      "}\n";
    GOLDEN(src, src);
}

/* ── Comment preservation ── */

TEST(fmt_preserves_line_comments) {
    const char *src = "let x = 1 // inline comment\n"
                      "// standalone comment\n"
                      "let y = 2\n";
    char *err = NULL;
    char *out = lat_format(src, 0, &err);
    ASSERT(out != NULL);
    ASSERT_CONTAINS(out, "// inline comment");
    ASSERT_CONTAINS(out, "// standalone comment");
    free(out);
}

TEST(fmt_preserves_doc_comments) {
    /* /// must stay /// — doc_gen depends on the triple-slash prefix */
    const char *src = "/// Adds two numbers.\n"
                      "/// Second line.\n"
                      "fn add(a: Int, b: Int) -> Int {\n"
                      "    return a + b\n"
                      "}\n";
    char *err = NULL;
    char *out = lat_format(src, 0, &err);
    ASSERT(out != NULL);
    ASSERT_CONTAINS(out, "/// Adds two numbers.");
    ASSERT_CONTAINS(out, "/// Second line.");
    free(out);
}

TEST(fmt_preserves_block_comments) {
    const char *src = "/* keep me\n   exactly */\n"
                      "let x = 1\n";
    char *err = NULL;
    char *out = lat_format(src, 0, &err);
    ASSERT(out != NULL);
    ASSERT_CONTAINS(out, "/* keep me\n   exactly */");
    free(out);
}

/* ── String preservation ── */

TEST(fmt_strings_not_reformatted) {
    /* Operators and commas inside string literals must not gain spaces */
    const char *src = "let s = \"a+b,c:d\"\n"
                      "let t = \"hi ${x}\"\n";
    char *err = NULL;
    char *out = lat_format(src, 0, &err);
    ASSERT(out != NULL);
    ASSERT_CONTAINS(out, "\"a+b,c:d\"");
    ASSERT_CONTAINS(out, "\"hi ${x}\"");
    free(out);
}

/* ── Idempotence across language constructs ── */

TEST(fmt_idempotent_functions_and_closures) {
    IDEMPOTENT("fn f(a:Int)->Int{let g=|n:Int|{return n*2}\nreturn g(a)}\n");
}

TEST(fmt_idempotent_structs_enums_traits) {
    IDEMPOTENT("struct Point{x:Int,y:Int}\n"
               "enum Shape{Circle(Float),Rect(Float,Float),Empty}\n"
               "trait Speaker{fn speak(self:any)->String}\n"
               "impl Speaker for Point{fn speak(self:any)->String{return \"p\"}}\n");
}

TEST(fmt_idempotent_match) { IDEMPOTENT("match x{1=>print(\"one\"),_=>print(\"other\")}\n"); }

TEST(fmt_idempotent_concurrency) { IDEMPOTENT("scope{spawn{ch.send(1)}\nselect{v=ch=>{print(v)}}}\n"); }

TEST(fmt_idempotent_phase_keywords) {
    IDEMPOTENT("flux a=1\nfix b=2\nfreeze(a)\nthaw(a)\nlet c=clone(b)\n"
               "freeze var a\nsublimate var b\n");
}

TEST(fmt_idempotent_error_handling) { IDEMPOTENT("try{risky()?}catch e{print(e?.message)}\ndefer{cleanup()}\n"); }

TEST(fmt_idempotent_contracts) { IDEMPOTENT("fn safe_div(a:Int,b:Int)->Int require b!=0,\"nonzero\" {return a/b}\n"); }

TEST(fmt_idempotent_doc_comments) { IDEMPOTENT("/// Doc line.\nfn f() { }\n"); }

/* ── Edge cases ── */

TEST(fmt_empty_input) {
    char *err = NULL;
    char *out = lat_format("", 0, &err);
    ASSERT(out != NULL);
    free(out);
}

TEST(fmt_whitespace_only_input) {
    char *err = NULL;
    char *out = lat_format("\n\n   \n", 0, &err);
    ASSERT(out != NULL);
    free(out);
}

/* ── lat_format_check ── */

TEST(fmt_check_detects_formatted_and_unformatted) {
    char *err = NULL;
    ASSERT(lat_format_check("let x = 1\n", 0, &err));
    ASSERT(!lat_format_check("let x=1\n", 0, &err));
}
