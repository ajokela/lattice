/* Unit tests for doc_gen.c — doc comment extraction and rendering */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doc_gen.h"

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

/* ── doc_extract: functions ── */

TEST(doc_extract_fn_basic) {
    DocFile df = doc_extract("/// Adds two integers.\n"
                             "fn add(a: Int, b: Int) -> Int {\n"
                             "    return a + b\n"
                             "}\n",
                             "test.lat");
    ASSERT_EQ_INT(df.item_count, 1);
    ASSERT_EQ_INT(df.items[0].kind, DOC_FUNCTION);
    ASSERT_EQ_STR(df.items[0].name, "add");
    ASSERT(df.items[0].doc != NULL);
    ASSERT_CONTAINS(df.items[0].doc, "Adds two integers.");
    ASSERT_EQ_INT(df.items[0].as.fn.param_count, 2);
    ASSERT_EQ_STR(df.items[0].as.fn.params[0].name, "a");
    ASSERT_EQ_STR(df.items[0].as.fn.params[0].type_name, "Int");
    ASSERT_EQ_STR(df.items[0].as.fn.params[1].name, "b");
    ASSERT_EQ_STR(df.items[0].as.fn.return_type, "Int");
    doc_file_free(&df);
}

TEST(doc_extract_fn_undocumented_included) {
    DocFile df = doc_extract("fn bare(x: Int) { return x }\n", "test.lat");
    ASSERT_EQ_INT(df.item_count, 1);
    ASSERT_EQ_STR(df.items[0].name, "bare");
    ASSERT(df.items[0].doc == NULL);
    doc_file_free(&df);
}

TEST(doc_extract_multiline_doc) {
    DocFile df = doc_extract("/// First line.\n"
                             "/// Second line.\n"
                             "fn f() { }\n",
                             "test.lat");
    ASSERT_EQ_INT(df.item_count, 1);
    ASSERT_CONTAINS(df.items[0].doc, "First line.");
    ASSERT_CONTAINS(df.items[0].doc, "Second line.");
    doc_file_free(&df);
}

TEST(doc_extract_fn_variadic_and_default) {
    DocFile df = doc_extract("fn v(first: Int, ...rest: Array) { }\n"
                             "fn d(x: Int = 1) { }\n",
                             "test.lat");
    ASSERT_EQ_INT(df.item_count, 2);
    ASSERT_EQ_INT(df.items[0].as.fn.param_count, 2);
    ASSERT(!df.items[0].as.fn.params[0].is_variadic);
    ASSERT(df.items[0].as.fn.params[1].is_variadic);
    ASSERT_EQ_INT(df.items[1].as.fn.param_count, 1);
    ASSERT(df.items[1].as.fn.params[0].has_default);
    doc_file_free(&df);
}

TEST(doc_extract_fn_line_numbers) {
    DocFile df = doc_extract("\n\n/// Doc.\nfn late() { }\n", "test.lat");
    ASSERT_EQ_INT(df.item_count, 1);
    ASSERT_EQ_INT(df.items[0].line, 4);
    doc_file_free(&df);
}

TEST(doc_extract_export_fn) {
    DocFile df = doc_extract("/// Exported.\n"
                             "export fn pub_fn() { }\n",
                             "test.lat");
    ASSERT_EQ_INT(df.item_count, 1);
    ASSERT_EQ_INT(df.items[0].kind, DOC_FUNCTION);
    ASSERT_EQ_STR(df.items[0].name, "pub_fn");
    ASSERT_CONTAINS(df.items[0].doc, "Exported.");
    doc_file_free(&df);
}

/* ── doc_extract: structs, enums, traits, impls ── */

TEST(doc_extract_struct_fields) {
    DocFile df = doc_extract("/// A point in 2D space.\n"
                             "struct Point {\n"
                             "    /// Horizontal position\n"
                             "    x: Int,\n"
                             "    y: Int\n"
                             "}\n",
                             "test.lat");
    ASSERT_EQ_INT(df.item_count, 1);
    ASSERT_EQ_INT(df.items[0].kind, DOC_STRUCT);
    ASSERT_EQ_STR(df.items[0].name, "Point");
    ASSERT_CONTAINS(df.items[0].doc, "A point in 2D space.");
    ASSERT_EQ_INT(df.items[0].as.strct.field_count, 2);
    ASSERT_EQ_STR(df.items[0].as.strct.fields[0].name, "x");
    ASSERT_EQ_STR(df.items[0].as.strct.fields[0].type_name, "Int");
    ASSERT(df.items[0].as.strct.fields[0].doc != NULL);
    ASSERT_CONTAINS(df.items[0].as.strct.fields[0].doc, "Horizontal position");
    ASSERT_EQ_STR(df.items[0].as.strct.fields[1].name, "y");
    ASSERT(df.items[0].as.strct.fields[1].doc == NULL);
    doc_file_free(&df);
}

TEST(doc_extract_enum_variants) {
    DocFile df = doc_extract("/// Shape kinds.\n"
                             "enum Shape {\n"
                             "    Circle(Float),\n"
                             "    /// Width and height\n"
                             "    Rect(Float, Float),\n"
                             "    Empty\n"
                             "}\n",
                             "test.lat");
    ASSERT_EQ_INT(df.item_count, 1);
    ASSERT_EQ_INT(df.items[0].kind, DOC_ENUM);
    ASSERT_EQ_STR(df.items[0].name, "Shape");
    ASSERT_EQ_INT(df.items[0].as.enm.variant_count, 3);
    ASSERT_EQ_STR(df.items[0].as.enm.variants[0].name, "Circle");
    ASSERT(df.items[0].as.enm.variants[0].params != NULL);
    ASSERT_CONTAINS(df.items[0].as.enm.variants[0].params, "Float");
    ASSERT_EQ_STR(df.items[0].as.enm.variants[1].name, "Rect");
    ASSERT_CONTAINS(df.items[0].as.enm.variants[1].doc, "Width and height");
    ASSERT_EQ_STR(df.items[0].as.enm.variants[2].name, "Empty");
    ASSERT(df.items[0].as.enm.variants[2].params == NULL);
    doc_file_free(&df);
}

TEST(doc_extract_trait_methods) {
    DocFile df = doc_extract("/// Things that can speak.\n"
                             "trait Speaker {\n"
                             "    /// Produce a greeting\n"
                             "    fn speak(self: any) -> String\n"
                             "    fn volume(self: any) -> Int\n"
                             "}\n",
                             "test.lat");
    ASSERT_EQ_INT(df.item_count, 1);
    ASSERT_EQ_INT(df.items[0].kind, DOC_TRAIT);
    ASSERT_EQ_STR(df.items[0].name, "Speaker");
    ASSERT_EQ_INT(df.items[0].as.trait.method_count, 2);
    ASSERT_EQ_STR(df.items[0].as.trait.methods[0].name, "speak");
    ASSERT_EQ_STR(df.items[0].as.trait.methods[0].return_type, "String");
    ASSERT_CONTAINS(df.items[0].as.trait.methods[0].doc, "Produce a greeting");
    ASSERT_EQ_STR(df.items[0].as.trait.methods[1].name, "volume");
    doc_file_free(&df);
}

TEST(doc_extract_impl_block) {
    DocFile df = doc_extract("impl Speaker for Dog {\n"
                             "    fn speak(self: any) -> String {\n"
                             "        return \"woof\"\n"
                             "    }\n"
                             "}\n",
                             "test.lat");
    ASSERT_EQ_INT(df.item_count, 1);
    ASSERT_EQ_INT(df.items[0].kind, DOC_IMPL);
    ASSERT_EQ_STR(df.items[0].as.impl.trait_name, "Speaker");
    ASSERT_EQ_STR(df.items[0].as.impl.type_name, "Dog");
    ASSERT_EQ_INT(df.items[0].as.impl.method_count, 1);
    ASSERT_EQ_STR(df.items[0].as.impl.methods[0].name, "speak");
    doc_file_free(&df);
}

/* ── doc_extract: variables and module docs ── */

TEST(doc_extract_variables) {
    DocFile df = doc_extract("/// Mutable counter.\n"
                             "flux count: Int = 0\n"
                             "/// Frozen limit.\n"
                             "fix limit = 100\n",
                             "test.lat");
    ASSERT_EQ_INT(df.item_count, 2);
    ASSERT_EQ_INT(df.items[0].kind, DOC_VARIABLE);
    ASSERT_EQ_STR(df.items[0].name, "count");
    ASSERT_EQ_STR(df.items[0].as.var.phase, "flux");
    ASSERT_EQ_STR(df.items[0].as.var.type_name, "Int");
    ASSERT_EQ_INT(df.items[1].kind, DOC_VARIABLE);
    ASSERT_EQ_STR(df.items[1].name, "limit");
    ASSERT_EQ_STR(df.items[1].as.var.phase, "fix");
    doc_file_free(&df);
}

TEST(doc_extract_module_doc) {
    /* A leading /// block counts as the module doc only when what follows is
     * NOT a declaration keyword (fn/struct/enum/trait/impl/flux/fix/let/
     * export) — even a blank line doesn't detach it from a declaration.
     * Following it with an import (typical module preamble) works. */
    DocFile df = doc_extract("/// This module does things.\n"
                             "\n"
                             "import \"lib/fn\" as F\n"
                             "\n"
                             "/// A documented fn.\n"
                             "fn f() { }\n",
                             "test.lat");
    ASSERT(df.module_doc != NULL);
    ASSERT_CONTAINS(df.module_doc, "This module does things.");
    doc_file_free(&df);
}

TEST(doc_extract_leading_doc_binds_to_first_decl) {
    /* A /// block immediately followed by a declaration is the decl's doc,
     * not the module doc */
    DocFile df = doc_extract("/// Belongs to f.\n"
                             "fn f() { }\n",
                             "test.lat");
    ASSERT(df.module_doc == NULL);
    ASSERT_EQ_INT(df.item_count, 1);
    ASSERT_CONTAINS(df.items[0].doc, "Belongs to f.");
    doc_file_free(&df);
}

/* ── doc_extract: edge cases ── */

TEST(doc_extract_empty_source) {
    DocFile df = doc_extract("", "empty.lat");
    ASSERT_EQ_INT(df.item_count, 0);
    ASSERT(df.module_doc == NULL);
    doc_file_free(&df);
}

TEST(doc_extract_regular_comments_ignored) {
    DocFile df = doc_extract("// Just a comment, not a doc\n"
                             "/* block comment */\n"
                             "fn f() { }\n",
                             "test.lat");
    ASSERT_EQ_INT(df.item_count, 1);
    ASSERT(df.items[0].doc == NULL);
    doc_file_free(&df);
}

TEST(doc_extract_utf8_doc) {
    DocFile df = doc_extract("/// Grüße — héllo ✓\n"
                             "fn f() { }\n",
                             "test.lat");
    ASSERT_EQ_INT(df.item_count, 1);
    ASSERT_CONTAINS(df.items[0].doc, "Grüße — héllo ✓");
    doc_file_free(&df);
}

/* ── doc_render ── */

static DocFile sample_file(void) {
    return doc_extract("/// Module summary.\n"
                       "\n"
                       "/// Adds two numbers.\n"
                       "fn add(a: Int, b: Int) -> Int { return a + b }\n"
                       "/// A point.\n"
                       "struct Point { x: Int, y: Int }\n",
                       "sample.lat");
}

TEST(doc_render_text_contains_items) {
    DocFile df = sample_file();
    char *out = doc_render(&df, 1, DOC_FMT_TEXT);
    ASSERT(out != NULL);
    ASSERT_CONTAINS(out, "add");
    ASSERT_CONTAINS(out, "Adds two numbers.");
    ASSERT_CONTAINS(out, "Point");
    free(out);
    doc_file_free(&df);
}

TEST(doc_render_markdown_contains_items) {
    DocFile df = sample_file();
    char *out = doc_render(&df, 1, DOC_FMT_MARKDOWN);
    ASSERT(out != NULL);
    ASSERT_CONTAINS(out, "add");
    ASSERT_CONTAINS(out, "Adds two numbers.");
    ASSERT_CONTAINS(out, "Module summary.");
    free(out);
    doc_file_free(&df);
}

TEST(doc_render_json_contains_items) {
    DocFile df = sample_file();
    char *out = doc_render(&df, 1, DOC_FMT_JSON);
    ASSERT(out != NULL);
    ASSERT_CONTAINS(out, "\"add\"");
    ASSERT_CONTAINS(out, "\"module_doc\"");
    free(out);
    doc_file_free(&df);
}

TEST(doc_render_json_escapes_strings) {
    DocFile df = doc_extract("/// Has \"quotes\" and a \\ backslash.\n"
                             "fn f() { }\n",
                             "test.lat");
    char *out = doc_render(&df, 1, DOC_FMT_JSON);
    ASSERT(out != NULL);
    ASSERT_CONTAINS(out, "\\\"quotes\\\"");
    ASSERT_CONTAINS(out, "\\\\");
    free(out);
    doc_file_free(&df);
}

TEST(doc_render_html_escapes_entities) {
    DocFile df = doc_extract("/// Compares a < b && b > c.\n"
                             "fn f() { }\n",
                             "test.lat");
    char *out = doc_render(&df, 1, DOC_FMT_HTML);
    ASSERT(out != NULL);
    ASSERT_CONTAINS(out, "&lt;");
    ASSERT_CONTAINS(out, "&amp;");
    free(out);
    doc_file_free(&df);
}
