#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lsp.h"
#include "../vendor/cJSON.h"

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

/* ================================================================
 * URI utility tests
 * ================================================================ */

TEST(lsp_uri_to_path_basic) {
    char *path = lsp_uri_to_path("file:///home/user/test.lat");
    ASSERT(path != NULL);
    ASSERT_EQ_STR(path, "/home/user/test.lat");
    free(path);
}

TEST(lsp_uri_to_path_percent_encoding) {
    char *path = lsp_uri_to_path("file:///home/user/my%20project/test.lat");
    ASSERT(path != NULL);
    ASSERT_EQ_STR(path, "/home/user/my project/test.lat");
    free(path);
}

TEST(lsp_uri_to_path_null) {
    char *path = lsp_uri_to_path(NULL);
    ASSERT(path == NULL);
}

TEST(lsp_uri_to_path_no_scheme) {
    /* When there's no file:// prefix, return the path as-is (decoded) */
    char *path = lsp_uri_to_path("/just/a/path");
    ASSERT(path != NULL);
    ASSERT_EQ_STR(path, "/just/a/path");
    free(path);
}

TEST(lsp_uri_to_path_double_slash) {
    /* file:// with only two slashes (no third) */
    char *path = lsp_uri_to_path("file://home/user/test.lat");
    ASSERT(path != NULL);
    ASSERT_EQ_STR(path, "home/user/test.lat");
    free(path);
}

TEST(lsp_path_to_uri_basic) {
    char *uri = lsp_path_to_uri("/home/user/test.lat");
    ASSERT(uri != NULL);
    ASSERT_EQ_STR(uri, "file:///home/user/test.lat");
    free(uri);
}

TEST(lsp_path_to_uri_spaces) {
    char *uri = lsp_path_to_uri("/home/user/my project/test.lat");
    ASSERT(uri != NULL);
    ASSERT_EQ_STR(uri, "file:///home/user/my%20project/test.lat");
    free(uri);
}

TEST(lsp_path_to_uri_null) {
    char *uri = lsp_path_to_uri(NULL);
    ASSERT(uri == NULL);
}

TEST(lsp_uri_roundtrip) {
    const char *original_path = "/home/user/my project/test file.lat";
    char *uri = lsp_path_to_uri(original_path);
    ASSERT(uri != NULL);
    char *path = lsp_uri_to_path(uri);
    ASSERT(path != NULL);
    ASSERT_EQ_STR(path, original_path);
    free(uri);
    free(path);
}

/* ================================================================
 * JSON-RPC protocol message construction tests
 * ================================================================ */

TEST(lsp_make_response_basic) {
    cJSON *result = cJSON_CreateString("hello");
    cJSON *resp = lsp_make_response(42, result);
    ASSERT(resp != NULL);

    /* Check jsonrpc field */
    cJSON *jsonrpc = cJSON_GetObjectItem(resp, "jsonrpc");
    ASSERT(jsonrpc != NULL);
    ASSERT_EQ_STR(jsonrpc->valuestring, "2.0");

    /* Check id field */
    cJSON *id = cJSON_GetObjectItem(resp, "id");
    ASSERT(id != NULL);
    ASSERT_EQ_INT(id->valueint, 42);

    /* Check result field */
    cJSON *res = cJSON_GetObjectItem(resp, "result");
    ASSERT(res != NULL);
    ASSERT_EQ_STR(res->valuestring, "hello");

    cJSON_Delete(resp);
}

TEST(lsp_make_notification_basic) {
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "key", "value");
    cJSON *notif = lsp_make_notification("textDocument/didSave", params);
    ASSERT(notif != NULL);

    /* Check jsonrpc field */
    cJSON *jsonrpc = cJSON_GetObjectItem(notif, "jsonrpc");
    ASSERT(jsonrpc != NULL);
    ASSERT_EQ_STR(jsonrpc->valuestring, "2.0");

    /* Check method field */
    cJSON *method = cJSON_GetObjectItem(notif, "method");
    ASSERT(method != NULL);
    ASSERT_EQ_STR(method->valuestring, "textDocument/didSave");

    /* Check params field */
    cJSON *p = cJSON_GetObjectItem(notif, "params");
    ASSERT(p != NULL);
    cJSON *val = cJSON_GetObjectItem(p, "key");
    ASSERT(val != NULL);
    ASSERT_EQ_STR(val->valuestring, "value");

    /* Notification should not have an id */
    cJSON *id = cJSON_GetObjectItem(notif, "id");
    ASSERT(id == NULL);

    cJSON_Delete(notif);
}

TEST(lsp_make_error_basic) {
    cJSON *resp = lsp_make_error(7, -32601, "Method not found");
    ASSERT(resp != NULL);

    /* Check jsonrpc field */
    cJSON *jsonrpc = cJSON_GetObjectItem(resp, "jsonrpc");
    ASSERT(jsonrpc != NULL);
    ASSERT_EQ_STR(jsonrpc->valuestring, "2.0");

    /* Check id field */
    cJSON *id = cJSON_GetObjectItem(resp, "id");
    ASSERT(id != NULL);
    ASSERT_EQ_INT(id->valueint, 7);

    /* Check error field */
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    ASSERT(err != NULL);

    cJSON *code = cJSON_GetObjectItem(err, "code");
    ASSERT(code != NULL);
    ASSERT_EQ_INT(code->valueint, -32601);

    cJSON *message = cJSON_GetObjectItem(err, "message");
    ASSERT(message != NULL);
    ASSERT_EQ_STR(message->valuestring, "Method not found");

    cJSON_Delete(resp);
}

TEST(lsp_make_response_null_result) {
    cJSON *resp = lsp_make_response(1, cJSON_CreateNull());
    ASSERT(resp != NULL);

    cJSON *res = cJSON_GetObjectItem(resp, "result");
    ASSERT(res != NULL);
    ASSERT(cJSON_IsNull(res));

    cJSON_Delete(resp);
}

/* ================================================================
 * lsp_read_message tests (using tmpfile for simulated stdin)
 * ================================================================ */

TEST(lsp_read_message_valid) {
    const char *body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}";
    size_t body_len = strlen(body);

    FILE *f = tmpfile();
    ASSERT(f != NULL);
    fprintf(f, "Content-Length: %zu\r\n\r\n%s", body_len, body);
    rewind(f);

    cJSON *msg = lsp_read_message(f);
    ASSERT(msg != NULL);

    cJSON *jsonrpc = cJSON_GetObjectItem(msg, "jsonrpc");
    ASSERT(jsonrpc != NULL);
    ASSERT_EQ_STR(jsonrpc->valuestring, "2.0");

    cJSON *id = cJSON_GetObjectItem(msg, "id");
    ASSERT(id != NULL);
    ASSERT_EQ_INT(id->valueint, 1);

    cJSON *method = cJSON_GetObjectItem(msg, "method");
    ASSERT(method != NULL);
    ASSERT_EQ_STR(method->valuestring, "initialize");

    cJSON_Delete(msg);
    fclose(f);
}

TEST(lsp_read_message_no_content_length) {
    FILE *f = tmpfile();
    ASSERT(f != NULL);
    fprintf(f, "Some-Header: value\r\n\r\n{\"test\":true}");
    rewind(f);

    cJSON *msg = lsp_read_message(f);
    ASSERT(msg == NULL);
    fclose(f);
}

TEST(lsp_read_message_empty_input) {
    FILE *f = tmpfile();
    ASSERT(f != NULL);
    /* Write nothing, just close so fgets returns NULL */
    rewind(f);

    cJSON *msg = lsp_read_message(f);
    ASSERT(msg == NULL);
    fclose(f);
}

TEST(lsp_read_message_multiple_headers) {
    const char *body = "{\"id\":2}";
    size_t body_len = strlen(body);

    FILE *f = tmpfile();
    ASSERT(f != NULL);
    fprintf(f, "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s", body_len, body);
    rewind(f);

    cJSON *msg = lsp_read_message(f);
    ASSERT(msg != NULL);

    cJSON *id = cJSON_GetObjectItem(msg, "id");
    ASSERT(id != NULL);
    ASSERT_EQ_INT(id->valueint, 2);

    cJSON_Delete(msg);
    fclose(f);
}

/* ================================================================
 * lsp_write_response tests (capture output to tmpfile)
 * ================================================================ */

TEST(lsp_write_response_format) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(json, "id", 1);

    FILE *f = tmpfile();
    ASSERT(f != NULL);
    lsp_write_response(json, f);
    cJSON_Delete(json);

    /* Read back what was written */
    rewind(f);
    char buf[4096];
    size_t nread = fread(buf, 1, sizeof(buf) - 1, f);
    buf[nread] = '\0';
    fclose(f);

    /* Should start with Content-Length header */
    ASSERT(strstr(buf, "Content-Length:") != NULL);
    /* Should have the CRLFCRLF separator */
    ASSERT(strstr(buf, "\r\n\r\n") != NULL);
    /* Body should contain valid JSON */
    const char *body = strstr(buf, "\r\n\r\n");
    ASSERT(body != NULL);
    body += 4;
    cJSON *parsed = cJSON_Parse(body);
    ASSERT(parsed != NULL);
    cJSON *id = cJSON_GetObjectItem(parsed, "id");
    ASSERT(id != NULL);
    ASSERT_EQ_INT(id->valueint, 1);
    cJSON_Delete(parsed);
}

/* ================================================================
 * Symbol index tests
 * ================================================================ */

TEST(lsp_symbol_index_new_null_path) {
    /* Creating an index with a non-existent file should still succeed
     * (just no symbols loaded) */
    LspSymbolIndex *idx = lsp_symbol_index_new("/nonexistent/path/eval.c");
    ASSERT(idx != NULL);
    ASSERT_EQ_INT(idx->builtin_count, 0);
    ASSERT_EQ_INT(idx->method_count, 0);
    lsp_symbol_index_free(idx);
}

TEST(lsp_symbol_index_add_file_null) {
    LspSymbolIndex *idx = lsp_symbol_index_new("/nonexistent/path");
    ASSERT(idx != NULL);
    /* Should not crash on NULL args */
    lsp_symbol_index_add_file(NULL, "/some/path");
    lsp_symbol_index_add_file(idx, NULL);
    lsp_symbol_index_free(idx);
}

TEST(lsp_symbol_index_free_null) {
    /* Should not crash */
    lsp_symbol_index_free(NULL);
}

TEST(lsp_symbol_index_scan_real_eval) {
    /* Test scanning the actual eval.c for builtins if it exists */
    FILE *f = fopen("src/eval.c", "r");
    if (!f) return; /* Skip if not in project root */
    fclose(f);

    LspSymbolIndex *idx = lsp_symbol_index_new("src/eval.c");
    ASSERT(idx != NULL);
    /* eval.c should have at least some @builtin annotations */
    ASSERT(idx->builtin_count > 0);

    /* Verify that known builtins are present (print is a fundamental one) */
    int found_print = 0;
    for (size_t i = 0; i < idx->builtin_count; i++) {
        if (strcmp(idx->builtins[i].name, "print") == 0) {
            found_print = 1;
            ASSERT(idx->builtins[i].signature != NULL);
            ASSERT(idx->builtins[i].doc != NULL);
            ASSERT_EQ_INT(idx->builtins[i].kind, LSP_SYM_FUNCTION);
            break;
        }
    }
    ASSERT(found_print);

    lsp_symbol_index_free(idx);
}

TEST(lsp_symbol_index_scan_methods) {
    /* Test scanning builtin_methods.c for methods */
    FILE *f = fopen("src/builtin_methods.c", "r");
    if (!f) return; /* Skip if not in project root */
    fclose(f);

    LspSymbolIndex *idx = lsp_symbol_index_new("/nonexistent");
    ASSERT(idx != NULL);
    lsp_symbol_index_add_file(idx, "src/builtin_methods.c");

    /* Should find methods with owner types */
    ASSERT(idx->method_count > 0);

    /* Verify at least one method has an owner_type set */
    int found_method_with_owner = 0;
    for (size_t i = 0; i < idx->method_count; i++) {
        if (idx->methods[i].owner_type != NULL) {
            found_method_with_owner = 1;
            ASSERT_EQ_INT(idx->methods[i].kind, LSP_SYM_METHOD);
            break;
        }
    }
    ASSERT(found_method_with_owner);

    lsp_symbol_index_free(idx);
}

/* ================================================================
 * Document analysis tests
 * ================================================================ */

TEST(lsp_analyze_empty_document) {
    LspDocument *doc = calloc(1, sizeof(LspDocument));
    doc->uri = strdup("file:///test.lat");
    doc->text = strdup("");

    lsp_analyze_document(doc);

    /* Empty document should have no diagnostics and no symbols */
    ASSERT_EQ_INT(doc->diag_count, 0);
    ASSERT_EQ_INT(doc->symbol_count, 0);

    lsp_document_free(doc);
}

TEST(lsp_analyze_null_text) {
    LspDocument *doc = calloc(1, sizeof(LspDocument));
    doc->uri = strdup("file:///test.lat");
    doc->text = NULL;

    lsp_analyze_document(doc);

    /* NULL text should be handled gracefully */
    ASSERT_EQ_INT(doc->diag_count, 0);
    ASSERT_EQ_INT(doc->symbol_count, 0);

    lsp_document_free(doc);
}

TEST(lsp_analyze_valid_function) {
    LspDocument *doc = calloc(1, sizeof(LspDocument));
    doc->uri = strdup("file:///test.lat");
    doc->text = strdup("fn greet(name: String) {\n  print(name)\n}\n");

    lsp_analyze_document(doc);

    /* Should find the function symbol with no diagnostics */
    ASSERT_EQ_INT(doc->diag_count, 0);
    ASSERT(doc->symbol_count >= 1);

    /* First symbol should be our function */
    int found_greet = 0;
    for (size_t i = 0; i < doc->symbol_count; i++) {
        if (strcmp(doc->symbols[i].name, "greet") == 0) {
            found_greet = 1;
            ASSERT_EQ_INT(doc->symbols[i].kind, LSP_SYM_FUNCTION);
            ASSERT(doc->symbols[i].signature != NULL);
            /* The function is on line 0 (0-based) */
            ASSERT_EQ_INT(doc->symbols[i].line, 0);
            break;
        }
    }
    ASSERT(found_greet);

    lsp_document_free(doc);
}

TEST(lsp_analyze_struct) {
    LspDocument *doc = calloc(1, sizeof(LspDocument));
    doc->uri = strdup("file:///test.lat");
    doc->text = strdup("struct Point {\n  x: Int,\n  y: Int\n}\n");

    lsp_analyze_document(doc);

    ASSERT_EQ_INT(doc->diag_count, 0);
    ASSERT(doc->symbol_count >= 1);

    /* Check the struct symbol */
    int found_point = 0;
    for (size_t i = 0; i < doc->symbol_count; i++) {
        if (strcmp(doc->symbols[i].name, "Point") == 0) {
            found_point = 1;
            ASSERT_EQ_INT(doc->symbols[i].kind, LSP_SYM_STRUCT);
            break;
        }
    }
    ASSERT(found_point);

    /* Check struct definitions for completion */
    ASSERT_EQ_INT(doc->struct_def_count, 1);
    ASSERT_EQ_STR(doc->struct_defs[0].name, "Point");
    ASSERT_EQ_INT((int)doc->struct_defs[0].field_count, 2);
    ASSERT_EQ_STR(doc->struct_defs[0].fields[0].name, "x");
    ASSERT_EQ_STR(doc->struct_defs[0].fields[1].name, "y");

    lsp_document_free(doc);
}

TEST(lsp_analyze_enum) {
    LspDocument *doc = calloc(1, sizeof(LspDocument));
    doc->uri = strdup("file:///test.lat");
    doc->text = strdup("enum Color {\n  Red,\n  Green,\n  Blue\n}\n");

    lsp_analyze_document(doc);

    ASSERT_EQ_INT(doc->diag_count, 0);
    ASSERT(doc->symbol_count >= 1);

    /* Check the enum symbol */
    int found_color = 0;
    for (size_t i = 0; i < doc->symbol_count; i++) {
        if (strcmp(doc->symbols[i].name, "Color") == 0) {
            found_color = 1;
            ASSERT_EQ_INT(doc->symbols[i].kind, LSP_SYM_ENUM);
            break;
        }
    }
    ASSERT(found_color);

    /* Check enum definitions for completion */
    ASSERT_EQ_INT(doc->enum_def_count, 1);
    ASSERT_EQ_STR(doc->enum_defs[0].name, "Color");
    ASSERT_EQ_INT((int)doc->enum_defs[0].variant_count, 3);
    ASSERT_EQ_STR(doc->enum_defs[0].variants[0].name, "Red");
    ASSERT_EQ_STR(doc->enum_defs[0].variants[1].name, "Green");
    ASSERT_EQ_STR(doc->enum_defs[0].variants[2].name, "Blue");

    lsp_document_free(doc);
}

TEST(lsp_analyze_syntax_error) {
    LspDocument *doc = calloc(1, sizeof(LspDocument));
    doc->uri = strdup("file:///test.lat");
    doc->text = strdup("fn broken( {\n}\n");

    lsp_analyze_document(doc);

    /* Should produce at least one diagnostic */
    ASSERT(doc->diag_count >= 1);
    ASSERT(doc->diagnostics[0].message != NULL);
    ASSERT_EQ_INT(doc->diagnostics[0].severity, LSP_DIAG_ERROR);

    lsp_document_free(doc);
}

TEST(lsp_analyze_variable_binding) {
    LspDocument *doc = calloc(1, sizeof(LspDocument));
    doc->uri = strdup("file:///test.lat");
    doc->text = strdup("let x = 42\nflux y = \"hello\"\n");

    lsp_analyze_document(doc);

    ASSERT_EQ_INT(doc->diag_count, 0);
    ASSERT(doc->symbol_count >= 2);

    /* Look for both variables */
    int found_x = 0, found_y = 0;
    for (size_t i = 0; i < doc->symbol_count; i++) {
        if (strcmp(doc->symbols[i].name, "x") == 0) {
            found_x = 1;
            ASSERT_EQ_INT(doc->symbols[i].kind, LSP_SYM_VARIABLE);
        }
        if (strcmp(doc->symbols[i].name, "y") == 0) {
            found_y = 1;
            ASSERT_EQ_INT(doc->symbols[i].kind, LSP_SYM_VARIABLE);
        }
    }
    ASSERT(found_x);
    ASSERT(found_y);

    lsp_document_free(doc);
}

TEST(lsp_analyze_multiple_functions) {
    LspDocument *doc = calloc(1, sizeof(LspDocument));
    doc->uri = strdup("file:///test.lat");
    doc->text = strdup("fn add(a: Int, b: Int) {\n  return a + b\n}\n"
                       "fn multiply(a: Int, b: Int) {\n  return a * b\n}\n");

    lsp_analyze_document(doc);

    ASSERT_EQ_INT(doc->diag_count, 0);
    ASSERT(doc->symbol_count >= 2);

    int found_add = 0, found_multiply = 0;
    for (size_t i = 0; i < doc->symbol_count; i++) {
        if (strcmp(doc->symbols[i].name, "add") == 0) {
            found_add = 1;
            ASSERT_EQ_INT(doc->symbols[i].kind, LSP_SYM_FUNCTION);
            /* Signature should contain parameter names */
            ASSERT(strstr(doc->symbols[i].signature, "a") != NULL);
            ASSERT(strstr(doc->symbols[i].signature, "b") != NULL);
        }
        if (strcmp(doc->symbols[i].name, "multiply") == 0) {
            found_multiply = 1;
            ASSERT_EQ_INT(doc->symbols[i].kind, LSP_SYM_FUNCTION);
        }
    }
    ASSERT(found_add);
    ASSERT(found_multiply);

    lsp_document_free(doc);
}

TEST(lsp_analyze_reanalyze_clears_previous) {
    LspDocument *doc = calloc(1, sizeof(LspDocument));
    doc->uri = strdup("file:///test.lat");
    doc->text = strdup("fn foo() {}\n");

    /* First analysis */
    lsp_analyze_document(doc);
    ASSERT_EQ_INT(doc->diag_count, 0);
    ASSERT(doc->symbol_count >= 1);

    /* Update text and re-analyze */
    free(doc->text);
    doc->text = strdup("fn bar() {}\n");
    lsp_analyze_document(doc);

    ASSERT_EQ_INT(doc->diag_count, 0);
    ASSERT(doc->symbol_count >= 1);

    /* Should find bar, not foo */
    int found_foo = 0, found_bar = 0;
    for (size_t i = 0; i < doc->symbol_count; i++) {
        if (strcmp(doc->symbols[i].name, "foo") == 0) found_foo = 1;
        if (strcmp(doc->symbols[i].name, "bar") == 0) found_bar = 1;
    }
    ASSERT(!found_foo);
    ASSERT(found_bar);

    lsp_document_free(doc);
}

/* ================================================================
 * Server lifecycle tests
 * ================================================================ */

TEST(lsp_server_new_free) {
    LspServer *srv = lsp_server_new();
    ASSERT(srv != NULL);
    ASSERT_EQ_INT(srv->initialized, 0);
    ASSERT_EQ_INT(srv->shutdown, 0);
    ASSERT_EQ_INT((int)srv->doc_count, 0);
    lsp_server_free(srv);
}

TEST(lsp_server_free_null) {
    /* Should not crash */
    lsp_server_free(NULL);
}

TEST(lsp_document_free_null) {
    /* Should not crash */
    lsp_document_free(NULL);
}

/* ================================================================
 * LspDiagnostic severity enum values
 * ================================================================ */

TEST(lsp_diagnostic_severity_values) {
    /* Verify LSP-standard severity values */
    ASSERT_EQ_INT(LSP_DIAG_ERROR, 1);
    ASSERT_EQ_INT(LSP_DIAG_WARNING, 2);
    ASSERT_EQ_INT(LSP_DIAG_INFO, 3);
    ASSERT_EQ_INT(LSP_DIAG_HINT, 4);
}

/* ================================================================
 * LspSymbolKind enum values
 * ================================================================ */

TEST(lsp_symbol_kind_values) {
    /* Verify LSP-standard symbol kinds */
    ASSERT_EQ_INT(LSP_SYM_METHOD, 2);
    ASSERT_EQ_INT(LSP_SYM_ENUM, 10);
    ASSERT_EQ_INT(LSP_SYM_FUNCTION, 12);
    ASSERT_EQ_INT(LSP_SYM_VARIABLE, 13);
    ASSERT_EQ_INT(LSP_SYM_KEYWORD, 14);
    ASSERT_EQ_INT(LSP_SYM_STRUCT, 23);
}

/* ================================================================
 * Complex analysis scenarios
 * ================================================================ */

TEST(lsp_analyze_mixed_declarations) {
    LspDocument *doc = calloc(1, sizeof(LspDocument));
    doc->uri = strdup("file:///test.lat");
    doc->text = strdup("struct Person {\n  name: String,\n  age: Int\n}\n"
                       "enum Status {\n  Active,\n  Inactive\n}\n"
                       "fn greet(p: Person) {\n  print(p.name)\n}\n"
                       "let count = 0\n");

    lsp_analyze_document(doc);

    ASSERT_EQ_INT(doc->diag_count, 0);

    /* Should find all four top-level declarations */
    int found_person = 0, found_status = 0, found_greet = 0, found_count = 0;
    for (size_t i = 0; i < doc->symbol_count; i++) {
        if (strcmp(doc->symbols[i].name, "Person") == 0) {
            found_person = 1;
            ASSERT_EQ_INT(doc->symbols[i].kind, LSP_SYM_STRUCT);
        }
        if (strcmp(doc->symbols[i].name, "Status") == 0) {
            found_status = 1;
            ASSERT_EQ_INT(doc->symbols[i].kind, LSP_SYM_ENUM);
        }
        if (strcmp(doc->symbols[i].name, "greet") == 0) {
            found_greet = 1;
            ASSERT_EQ_INT(doc->symbols[i].kind, LSP_SYM_FUNCTION);
        }
        if (strcmp(doc->symbols[i].name, "count") == 0) {
            found_count = 1;
            ASSERT_EQ_INT(doc->symbols[i].kind, LSP_SYM_VARIABLE);
        }
    }
    ASSERT(found_person);
    ASSERT(found_status);
    ASSERT(found_greet);
    ASSERT(found_count);

    /* Check struct and enum definitions */
    ASSERT_EQ_INT(doc->struct_def_count, 1);
    ASSERT_EQ_INT(doc->enum_def_count, 1);

    lsp_document_free(doc);
}

TEST(lsp_analyze_enum_with_tuple_variants) {
    LspDocument *doc = calloc(1, sizeof(LspDocument));
    doc->uri = strdup("file:///test.lat");
    doc->text = strdup("enum Shape {\n"
                       "  Circle(Float),\n"
                       "  Rectangle(Float, Float)\n"
                       "}\n");

    lsp_analyze_document(doc);

    ASSERT_EQ_INT(doc->diag_count, 0);
    ASSERT_EQ_INT(doc->enum_def_count, 1);
    ASSERT_EQ_STR(doc->enum_defs[0].name, "Shape");
    ASSERT_EQ_INT((int)doc->enum_defs[0].variant_count, 2);
    ASSERT_EQ_STR(doc->enum_defs[0].variants[0].name, "Circle");
    ASSERT(doc->enum_defs[0].variants[0].params != NULL);
    ASSERT_EQ_STR(doc->enum_defs[0].variants[1].name, "Rectangle");
    ASSERT(doc->enum_defs[0].variants[1].params != NULL);

    lsp_document_free(doc);
}

TEST(lsp_analyze_fix_binding) {
    LspDocument *doc = calloc(1, sizeof(LspDocument));
    doc->uri = strdup("file:///test.lat");
    doc->text = strdup("fix PI = 3.14159\n");

    lsp_analyze_document(doc);

    ASSERT_EQ_INT(doc->diag_count, 0);
    ASSERT(doc->symbol_count >= 1);

    int found_pi = 0;
    for (size_t i = 0; i < doc->symbol_count; i++) {
        if (strcmp(doc->symbols[i].name, "PI") == 0) {
            found_pi = 1;
            ASSERT_EQ_INT(doc->symbols[i].kind, LSP_SYM_VARIABLE);
            /* Signature should include "fix" */
            ASSERT(strstr(doc->symbols[i].signature, "fix") != NULL);
            break;
        }
    }
    ASSERT(found_pi);

    lsp_document_free(doc);
}

/* ================================================================
 * Hover documentation tests
 * ================================================================ */

TEST(lsp_hover_keyword_flux) {
    const char *doc = lsp_lookup_keyword_doc("flux");
    ASSERT(doc != NULL);
    ASSERT(strstr(doc, "mutable") != NULL);
    ASSERT(strstr(doc, "fluid") != NULL);
}

TEST(lsp_hover_keyword_fix) {
    const char *doc = lsp_lookup_keyword_doc("fix");
    ASSERT(doc != NULL);
    ASSERT(strstr(doc, "immutable") != NULL);
    ASSERT(strstr(doc, "crystal") != NULL);
}

TEST(lsp_hover_keyword_let) {
    const char *doc = lsp_lookup_keyword_doc("let");
    ASSERT(doc != NULL);
    ASSERT(strstr(doc, "inferred phase") != NULL);
}

TEST(lsp_hover_keyword_freeze) {
    const char *doc = lsp_lookup_keyword_doc("freeze");
    ASSERT(doc != NULL);
    ASSERT(strstr(doc, "crystal") != NULL);
    ASSERT(strstr(doc, "immutable") != NULL);
}

TEST(lsp_hover_keyword_thaw) {
    const char *doc = lsp_lookup_keyword_doc("thaw");
    ASSERT(doc != NULL);
    ASSERT(strstr(doc, "fluid") != NULL);
    ASSERT(strstr(doc, "mutable") != NULL);
}

TEST(lsp_hover_keyword_scope) {
    const char *doc = lsp_lookup_keyword_doc("scope");
    ASSERT(doc != NULL);
    ASSERT(strstr(doc, "concurrency") != NULL);
}

TEST(lsp_hover_keyword_spawn) {
    const char *doc = lsp_lookup_keyword_doc("spawn");
    ASSERT(doc != NULL);
    ASSERT(strstr(doc, "concurrent") != NULL || strstr(doc, "task") != NULL);
}

TEST(lsp_hover_keyword_match) {
    const char *doc = lsp_lookup_keyword_doc("match");
    ASSERT(doc != NULL);
    ASSERT(strstr(doc, "pattern") != NULL);
}

TEST(lsp_hover_keyword_struct) {
    const char *doc = lsp_lookup_keyword_doc("struct");
    ASSERT(doc != NULL);
    ASSERT(strstr(doc, "struct") != NULL);
}

TEST(lsp_hover_keyword_enum) {
    const char *doc = lsp_lookup_keyword_doc("enum");
    ASSERT(doc != NULL);
    ASSERT(strstr(doc, "enum") != NULL);
    ASSERT(strstr(doc, "variant") != NULL);
}

TEST(lsp_hover_keyword_fn) {
    const char *doc = lsp_lookup_keyword_doc("fn");
    ASSERT(doc != NULL);
    ASSERT(strstr(doc, "function") != NULL);
}

TEST(lsp_hover_keyword_nil_lookup) {
    /* "nil" should have documentation */
    const char *doc = lsp_lookup_keyword_doc("nil");
    ASSERT(doc != NULL);
    ASSERT(strstr(doc, "absence") != NULL);
}

TEST(lsp_hover_keyword_not_found) {
    const char *doc = lsp_lookup_keyword_doc("nonexistent_keyword");
    ASSERT(doc == NULL);
}

TEST(lsp_hover_keyword_select) {
    const char *doc = lsp_lookup_keyword_doc("select");
    ASSERT(doc != NULL);
    ASSERT(strstr(doc, "channel") != NULL);
}

TEST(lsp_hover_keyword_test) {
    const char *doc = lsp_lookup_keyword_doc("test");
    ASSERT(doc != NULL);
    ASSERT(strstr(doc, "test case") != NULL);
}

TEST(lsp_hover_keyword_all_have_code_block) {
    /* All keywords with documentation should include markdown code blocks */
    const char *kws[] = {"fn",    "let",     "flux",   "fix",    "struct", "enum",  "trait",    "impl",
                         "if",    "for",     "while",  "match",  "return", "break", "continue", "try",
                         "throw", "true",    "false",  "nil",    "print",  "scope", "defer",    "select",
                         "test",  "require", "ensure", "freeze", "thaw",   "clone", "spawn",    NULL};
    for (int i = 0; kws[i]; i++) {
        const char *doc = lsp_lookup_keyword_doc(kws[i]);
        ASSERT(doc != NULL);
        /* Every keyword doc should have a code block */
        ASSERT(strstr(doc, "```lattice") != NULL);
    }
}

TEST(lsp_hover_builtin_len) {
    const char *sig = NULL;
    const char *desc = lsp_lookup_builtin_doc("len", &sig);
    ASSERT(desc != NULL);
    ASSERT(sig != NULL);
    ASSERT(strstr(sig, "len") != NULL);
    ASSERT(strstr(desc, "length") != NULL);
}

TEST(lsp_hover_builtin_typeof) {
    const char *sig = NULL;
    const char *desc = lsp_lookup_builtin_doc("typeof", &sig);
    ASSERT(desc != NULL);
    ASSERT(sig != NULL);
    ASSERT(strstr(sig, "typeof") != NULL);
    ASSERT(strstr(sig, "Any") != NULL);
    ASSERT(strstr(desc, "type name") != NULL);
}

TEST(lsp_hover_builtin_print) {
    const char *sig = NULL;
    const char *desc = lsp_lookup_builtin_doc("print", &sig);
    ASSERT(desc != NULL);
    ASSERT(sig != NULL);
    ASSERT(strstr(sig, "print") != NULL);
    ASSERT(strstr(sig, "Any...") != NULL);
}

TEST(lsp_hover_builtin_assert_eq) {
    const char *sig = NULL;
    const char *desc = lsp_lookup_builtin_doc("assert_eq", &sig);
    ASSERT(desc != NULL);
    ASSERT(sig != NULL);
    ASSERT(strstr(sig, "actual") != NULL);
    ASSERT(strstr(sig, "expected") != NULL);
}

TEST(lsp_hover_builtin_not_found) {
    const char *sig = NULL;
    const char *desc = lsp_lookup_builtin_doc("nonexistent_builtin", &sig);
    ASSERT(desc == NULL);
    ASSERT(sig == NULL);
}

TEST(lsp_hover_builtin_null_sig_out) {
    /* Passing NULL for out_sig should not crash */
    const char *desc = lsp_lookup_builtin_doc("len", NULL);
    ASSERT(desc != NULL);
}

TEST(lsp_hover_builtin_range) {
    const char *sig = NULL;
    const char *desc = lsp_lookup_builtin_doc("range", &sig);
    ASSERT(desc != NULL);
    ASSERT(sig != NULL);
    ASSERT(strstr(sig, "start") != NULL);
    ASSERT(strstr(sig, "end") != NULL);
}

TEST(lsp_hover_builtin_json_parse) {
    const char *sig = NULL;
    const char *desc = lsp_lookup_builtin_doc("json_parse", &sig);
    ASSERT(desc != NULL);
    ASSERT(sig != NULL);
    ASSERT(strstr(desc, "JSON") != NULL);
}

TEST(lsp_hover_builtin_http_get) {
    const char *sig = NULL;
    const char *desc = lsp_lookup_builtin_doc("http_get", &sig);
    ASSERT(desc != NULL);
    ASSERT(sig != NULL);
    ASSERT(strstr(desc, "HTTP") != NULL || strstr(desc, "GET") != NULL);
}

TEST(lsp_hover_builtin_freeze) {
    const char *sig = NULL;
    const char *desc = lsp_lookup_builtin_doc("freeze", &sig);
    ASSERT(desc != NULL);
    ASSERT(sig != NULL);
    ASSERT(strstr(desc, "crystal") != NULL || strstr(desc, "immutable") != NULL);
}
