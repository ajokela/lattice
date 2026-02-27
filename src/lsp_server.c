#include "lsp.h"
#include "formatter.h"
#include "../vendor/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Keywords for completion ── */

static const char *lattice_keywords[] = {
    "fn",   "let",     "flux",   "fix",    "struct", "enum",  "trait",    "impl",   "if",    "else",
    "for",  "while",   "in",     "match",  "return", "break", "continue", "import", "from",  "as",
    "try",  "catch",   "throw",  "true",   "false",  "nil",   "print",    "scope",  "defer", "select",
    "test", "require", "ensure", "freeze", "thaw",   "clone", "spawn",    NULL};

/* ── Keyword hover documentation ── */

typedef struct {
    const char *keyword;
    const char *doc;
} LspKeywordDoc;

static const LspKeywordDoc keyword_docs[] = {
    {"fn", "```lattice\nfn name(params) { body }\n```\n"
           "Declares a named function."},
    {"let", "```lattice\nlet name = value\n```\n"
            "Declares a variable with inferred phase. The variable's mutability "
            "is determined by usage."},
    {"flux", "```lattice\nflux name = value\n```\n"
             "Declares a mutable variable (fluid phase). The value can be "
             "reassigned and mutated freely."},
    {"fix", "```lattice\nfix name = value\n```\n"
            "Declares an immutable variable (crystal phase). The value cannot be "
            "reassigned or mutated after initialization."},
    {"struct", "```lattice\nstruct Name {\n  field: Type,\n  ...\n}\n```\n"
               "Declares a struct type with named fields."},
    {"enum", "```lattice\nenum Name {\n  Variant,\n  Variant(Type),\n  ...\n}\n"
             "```\n"
             "Declares an enum type with variants. Variants can be simple or "
             "carry tuple data."},
    {"trait", "```lattice\ntrait Name {\n  fn method()\n}\n```\n"
              "Declares a trait (interface) that types can implement."},
    {"impl", "```lattice\nimpl Trait for Type {\n  fn method() { ... }\n}\n```\n"
             "Implements methods or a trait for a type."},
    {"if", "```lattice\nif condition { ... } else { ... }\n```\n"
           "Conditional branching. Evaluates the body if the condition is "
           "truthy."},
    {"else", "```lattice\nif condition { ... } else { ... }\n```\n"
             "The alternative branch of an `if` expression."},
    {"for", "```lattice\nfor item in collection { ... }\n```\n"
            "Iterates over elements of a collection (Array, Map, Set, String, "
            "Range, or Iterator)."},
    {"while", "```lattice\nwhile condition { ... }\n```\n"
              "Loops while the condition is truthy."},
    {"in", "```lattice\nfor item in collection { ... }\n```\n"
           "Used in `for` loops to separate the binding from the collection."},
    {"match", "```lattice\nmatch value {\n  pattern => expr,\n  _ => default\n}\n"
              "```\n"
              "Pattern matching expression. Matches a value against one or more "
              "patterns including literals, enum variants, and wildcards."},
    {"return", "```lattice\nreturn value\n```\n"
               "Returns a value from the current function."},
    {"break", "```lattice\nbreak\n```\n"
              "Exits the current loop."},
    {"continue", "```lattice\ncontinue\n```\n"
                 "Skips to the next iteration of the current loop."},
    {"import", "```lattice\nimport \"module.lat\"\nimport { name } from "
               "\"module.lat\"\n```\n"
               "Imports declarations from another Lattice module."},
    {"from", "```lattice\nimport { name } from \"module.lat\"\n```\n"
             "Specifies the source module in a selective import."},
    {"as", "```lattice\nimport { name as alias } from \"module.lat\"\n```\n"
           "Creates an alias for an imported name."},
    {"try", "```lattice\ntry { ... } catch e { ... }\n```\n"
            "Error handling. Runs the body and catches any thrown errors."},
    {"catch", "```lattice\ntry { ... } catch e { ... }\n```\n"
              "Handles errors thrown in the preceding `try` block."},
    {"throw", "```lattice\nthrow \"error message\"\n```\n"
              "Throws an error value, transferring control to the nearest "
              "`catch`."},
    {"true", "```lattice\ntrue\n```\n"
             "Boolean literal representing a truthy value."},
    {"false", "```lattice\nfalse\n```\n"
              "Boolean literal representing a falsy value."},
    {"nil", "```lattice\nnil\n```\n"
            "The absence of a value. Used as a null/none equivalent."},
    {"print", "```lattice\nprint(args...)\n```\n"
              "Prints values separated by spaces with a trailing newline."},
    {"scope", "```lattice\nscope {\n  spawn { task1() }\n  spawn { task2() }\n}\n"
              "```\n"
              "Creates a structured concurrency scope. All spawned tasks within "
              "the scope must complete before the scope exits."},
    {"defer", "```lattice\ndefer { cleanup() }\n```\n"
              "Defers a block until the enclosing scope exits."},
    {"select", "```lattice\nselect {\n  recv ch -> val => { ... }\n  send ch val "
               "=> { ... }\n}\n```\n"
               "Multiplexes over multiple channel operations. Runs the first arm "
               "that becomes ready."},
    {"test", "```lattice\ntest \"description\" {\n  assert_eq(actual, expected)"
             "\n}\n```\n"
             "Declares a test case. Run tests with `clat --test file.lat`."},
    {"require", "```lattice\nrequire condition, \"message\"\n```\n"
                "Precondition contract. Asserts a condition that must hold at the "
                "start of a function."},
    {"ensure", "```lattice\nensure condition, \"message\"\n```\n"
               "Postcondition contract. Asserts a condition that must hold at the "
               "end of a function."},
    {"freeze", "```lattice\nfreeze(value)\n```\n"
               "Transitions a value to crystal (immutable) phase. Returns the "
               "frozen value. Attempting to mutate a frozen value will produce an "
               "error."},
    {"thaw", "```lattice\nthaw(value)\n```\n"
             "Transitions a value back to fluid (mutable) phase. Returns the "
             "thawed value, allowing mutations again."},
    {"clone", "```lattice\nclone(value)\n```\n"
              "Creates a deep copy of a value. The clone is independent and "
              "mutations do not affect the original."},
    {"spawn", "```lattice\nscope {\n  spawn { task() }\n}\n```\n"
              "Spawns a concurrent task within a `scope` block."},
    {NULL, NULL},
};

/* ── Static builtin documentation ── */
/* Used as fallback when source scanning is unavailable, and also
 * to provide hover documentation for builtin functions. */

typedef struct {
    const char *name;
    const char *signature;
    const char *description;
} LspBuiltinDoc;

static const LspBuiltinDoc builtin_docs[] = {
    {"print", "print(args: Any...) -> Unit", "Print values separated by spaces with a trailing newline."},
    {"print_raw", "print_raw(args: Any...) -> Unit", "Print values separated by spaces without a trailing newline."},
    {"eprint", "eprint(args: Any...) -> Unit", "Print values to stderr with a trailing newline."},
    {"len", "len(val: String|Array|Map) -> Int", "Returns the length of a string, array, map, set, or buffer."},
    {"typeof", "typeof(val: Any) -> String", "Returns the type name of a value as a string."},
    {"to_string", "to_string(val: Any) -> String", "Converts any value to its string representation."},
    {"repr", "repr(val: Any) -> String", "Returns the debug representation of a value."},
    {"parse_int", "parse_int(s: String) -> Int", "Parses a string as an integer."},
    {"parse_float", "parse_float(s: String) -> Float", "Parses a string as a float."},
    {"to_int", "to_int(val: Any) -> Int", "Converts a value to an integer."},
    {"to_float", "to_float(val: Any) -> Float", "Converts a value to a float."},
    {"input", "input(prompt?: String) -> String", "Reads a line of input from stdin."},
    {"error", "error(msg: String) -> String", "Creates an error value with the given message."},
    {"panic", "panic(msg: String) -> Unit", "Terminates the program with an error message."},
    {"is_error", "is_error(val: Any) -> Bool", "Returns true if the value is an error."},
    {"assert", "assert(cond: Any, msg?: String) -> Unit", "Asserts that a condition is truthy."},
    {"assert_eq", "assert_eq(actual: Any, expected: Any) -> Unit", "Asserts that two values are equal."},
    {"assert_ne", "assert_ne(actual: Any, expected: Any) -> Unit", "Asserts that two values are not equal."},
    {"assert_true", "assert_true(val: Any) -> Unit", "Asserts that a value is truthy."},
    {"assert_false", "assert_false(val: Any) -> Unit", "Asserts that a value is falsy."},
    {"assert_nil", "assert_nil(val: Any) -> Unit", "Asserts that a value is nil."},
    {"assert_throws", "assert_throws(fn: Closure) -> String", "Asserts that the closure throws an error."},
    {"assert_contains", "assert_contains(haystack: Any, needle: Any) -> Unit",
     "Asserts that haystack contains needle."},
    {"assert_type", "assert_type(val: Any, type_name: String) -> Unit", "Asserts that a value has the given type."},
    {"debug_assert", "debug_assert(cond: Any, msg?: String) -> Unit", "Debug-only assertion (no-op in release mode)."},
    {"exit", "exit(code?: Int) -> Unit", "Exits the program with the given status code (default 0)."},
    {"version", "version() -> String", "Returns the Lattice version string."},
    {"range", "range(start: Int, end: Int, step?: Int) -> Array",
     "Creates an array of integers from start (inclusive) to end "
     "(exclusive)."},
    {"abs", "abs(x: Int|Float) -> Int|Float", "Returns the absolute value."},
    {"floor", "floor(x: Int|Float) -> Int", "Rounds down to the nearest integer."},
    {"ceil", "ceil(x: Int|Float) -> Int", "Rounds up to the nearest integer."},
    {"round", "round(x: Int|Float) -> Int", "Rounds to the nearest integer."},
    {"sqrt", "sqrt(x: Int|Float) -> Float", "Returns the square root."},
    {"pow", "pow(base: Int|Float, exp: Int|Float) -> Float", "Returns base raised to the power of exp."},
    {"min", "min(a: Int|Float, b: Int|Float) -> Int|Float", "Returns the smaller of two values."},
    {"max", "max(a: Int|Float, b: Int|Float) -> Int|Float", "Returns the larger of two values."},
    {"random", "random() -> Float", "Returns a random float between 0.0 and 1.0."},
    {"random_int", "random_int(min: Int, max: Int) -> Int", "Returns a random integer in the range [min, max]."},
    {"log", "log(x: Int|Float) -> Float", "Returns the natural logarithm."},
    {"sin", "sin(x: Int|Float) -> Float", "Returns the sine of an angle in radians."},
    {"cos", "cos(x: Int|Float) -> Float", "Returns the cosine of an angle in radians."},
    {"tan", "tan(x: Int|Float) -> Float", "Returns the tangent of an angle in radians."},
    {"clamp", "clamp(x: Int|Float, lo: Int|Float, hi: Int|Float) -> Int|Float",
     "Clamps a value to the range [lo, hi]."},
    {"math_pi", "math_pi() -> Float", "Returns the mathematical constant pi."},
    {"math_e", "math_e() -> Float", "Returns Euler's number e."},
    {"ord", "ord(ch: String) -> Int", "Returns the Unicode code point of the first character."},
    {"chr", "chr(code: Int) -> String", "Returns the character for a Unicode code point."},
    {"format", "format(fmt: String, args: Any...) -> String", "Formats a string with placeholders."},
    {"read_file", "read_file(path: String) -> String", "Reads the entire contents of a file as a string."},
    {"write_file", "write_file(path: String, content: String) -> Bool",
     "Writes a string to a file, creating or overwriting it."},
    {"file_exists", "file_exists(path: String) -> Bool", "Returns true if a file or directory exists at the path."},
    {"delete_file", "delete_file(path: String) -> Bool", "Deletes a file at the path."},
    {"list_dir", "list_dir(path: String) -> Array", "Lists the entries in a directory."},
    {"mkdir", "mkdir(path: String) -> Bool", "Creates a directory (including parent directories)."},
    {"json_parse", "json_parse(s: String) -> Any", "Parses a JSON string into a Lattice value."},
    {"json_stringify", "json_stringify(val: Any) -> String", "Serializes a Lattice value to a JSON string."},
    {"env", "env(name: String) -> String|Unit", "Returns the value of an environment variable, or nil if not set."},
    {"env_set", "env_set(name: String, value: String) -> Unit", "Sets an environment variable."},
    {"time", "time() -> Int", "Returns current time in milliseconds since the Unix epoch."},
    {"sleep", "sleep(ms: Int) -> Unit", "Pauses for the given number of milliseconds."},
    {"cwd", "cwd() -> String", "Returns the current working directory."},
    {"args", "args() -> Array", "Returns the command-line arguments as an array of strings."},
    {"platform", "platform() -> String", "Returns the platform name (e.g. \"macos\", \"linux\")."},
    {"pid", "pid() -> Int", "Returns the current process ID."},
    {"http_get", "http_get(url: String) -> Map", "Performs an HTTP GET request. Returns {status, headers, body}."},
    {"http_post", "http_post(url: String, options: Map) -> Map",
     "Performs an HTTP POST request. Returns {status, headers, body}."},
    {"http_request", "http_request(method: String, url: String, options?: Map) -> Map",
     "Performs an HTTP request. Returns {status, headers, body}."},
    {"regex_match", "regex_match(pattern: String, str: String) -> Bool",
     "Returns true if the string matches the regex pattern."},
    {"regex_find_all", "regex_find_all(pattern: String, str: String) -> Array",
     "Returns all matches of the regex pattern in the string."},
    {"regex_replace",
     "regex_replace(pattern: String, str: String, replacement: String) "
     "-> String",
     "Replaces all matches of the regex pattern with the replacement."},
    {"sha256", "sha256(s: String) -> String", "Returns the SHA-256 hash of a string as a hex string."},
    {"base64_encode", "base64_encode(s: String) -> String", "Encodes a string to Base64."},
    {"base64_decode", "base64_decode(s: String) -> String", "Decodes a Base64 string."},
    {"struct_name", "struct_name(val: Struct) -> String", "Returns the type name of a struct instance."},
    {"struct_fields", "struct_fields(val: Struct) -> Array",
     "Returns the field names of a struct as an array of strings."},
    {"struct_to_map", "struct_to_map(val: Struct) -> Map", "Converts a struct to a map of field names to values."},
    {"struct_from_map", "struct_from_map(name: String, map: Map) -> Struct",
     "Creates a struct from a type name and a map of field values."},
    {"phase_of", "phase_of(val: Any) -> String",
     "Returns the phase of a value: \"fluid\", \"crystal\", or "
     "\"unphased\"."},
    {"track", "track(name: String) -> Unit", "Enables phase tracking for a named variable."},
    {"phases", "phases(name: String) -> Array", "Returns the phase history of a tracked variable."},
    {"history", "history(name: String) -> Array", "Returns the value history of a tracked variable."},
    {"identity", "identity(val: Any) -> Any", "Returns its argument unchanged."},
    {"pipe", "pipe(val: Any, fns: Closure...) -> Any", "Pipes a value through a series of functions."},
    {"compose", "compose(f: Closure, g: Closure) -> Closure", "Returns a new function that applies f then g."},
    {"iter", "iter(collection: Any) -> Iterator", "Creates an iterator from a collection."},
    {"range_iter", "range_iter(start: Int, end: Int, step?: Int) -> Iterator", "Creates a lazy range iterator."},
    {"tokenize", "tokenize(source: String) -> Array", "Tokenizes Lattice source code into an array of token maps."},
    {"is_complete", "is_complete(source: String) -> Bool",
     "Returns true if the source string is a complete expression."},
    {"freeze", "freeze(val: Any) -> Any", "Transitions a value to crystal (immutable) phase."},
    {"thaw", "thaw(val: Any) -> Any", "Transitions a value back to fluid (mutable) phase."},
    {"clone", "clone(val: Any) -> Any", "Creates a deep copy of a value."},
    {NULL, NULL, NULL},
};

/* ── Hover documentation lookup (public, for testing) ── */

const char *lsp_lookup_keyword_doc(const char *keyword) {
    for (int i = 0; keyword_docs[i].keyword; i++) {
        if (strcmp(keyword_docs[i].keyword, keyword) == 0) return keyword_docs[i].doc;
    }
    return NULL;
}

const char *lsp_lookup_builtin_doc(const char *name, const char **out_sig) {
    for (int i = 0; builtin_docs[i].name; i++) {
        if (strcmp(builtin_docs[i].name, name) == 0) {
            if (out_sig) *out_sig = builtin_docs[i].signature;
            return builtin_docs[i].description;
        }
    }
    return NULL;
}

/* ── Identifier character helpers ── */

static int is_ident_char(char c) {
    return (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
}

/* Extract the identifier word at a given line:col in the document text.
 * Returns a malloc'd string, or NULL if no identifier found.
 * Optionally outputs the start column of the word. */
static char *extract_word_at(const char *text, int line, int col, int *out_col) {
    const char *p = text;
    int cur_line = 0;
    while (cur_line < line && *p) {
        if (*p == '\n') cur_line++;
        p++;
    }
    const char *line_start = p;
    int cur_col = 0;
    while (cur_col < col && *p && *p != '\n') {
        cur_col++;
        p++;
    }

    const char *ws = p;
    while (ws > line_start && is_ident_char(ws[-1])) ws--;

    const char *we = p;
    while (*we && is_ident_char(*we)) we++;

    if (we <= ws) return NULL;

    size_t wlen = (size_t)(we - ws);
    char *word = malloc(wlen + 1);
    if (!word) return NULL;
    memcpy(word, ws, wlen);
    word[wlen] = '\0';
    if (out_col) *out_col = (int)(ws - line_start);
    return word;
}

/* ── Document management ── */

static LspDocument *find_document(LspServer *srv, const char *uri) {
    for (size_t i = 0; i < srv->doc_count; i++) {
        if (strcmp(srv->documents[i]->uri, uri) == 0) return srv->documents[i];
    }
    return NULL;
}

static LspDocument *add_document(LspServer *srv, const char *uri, const char *text, int version) {
    LspDocument *doc = calloc(1, sizeof(LspDocument));
    if (!doc) return NULL;
    doc->uri = strdup(uri);
    doc->text = strdup(text);
    doc->version = version;

    if (srv->doc_count >= srv->doc_cap) {
        srv->doc_cap = srv->doc_cap ? srv->doc_cap * 2 : 16;
        srv->documents = realloc(srv->documents, srv->doc_cap * sizeof(LspDocument *));
    }
    srv->documents[srv->doc_count++] = doc;
    return doc;
}

static void remove_document(LspServer *srv, const char *uri) {
    for (size_t i = 0; i < srv->doc_count; i++) {
        if (strcmp(srv->documents[i]->uri, uri) == 0) {
            lsp_document_free(srv->documents[i]);
            srv->documents[i] = srv->documents[--srv->doc_count];
            return;
        }
    }
}

/* ── Publish diagnostics ── */

static void publish_diagnostics(LspServer *srv, LspDocument *doc) {
    (void)srv;
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "uri", doc->uri);

    cJSON *diags = cJSON_CreateArray();
    for (size_t i = 0; i < doc->diag_count; i++) {
        LspDiagnostic *d = &doc->diagnostics[i];
        cJSON *diag = cJSON_CreateObject();

        /* Range */
        cJSON *range = cJSON_CreateObject();
        cJSON *start = cJSON_CreateObject();
        cJSON_AddNumberToObject(start, "line", d->line);
        cJSON_AddNumberToObject(start, "character", d->col);
        cJSON *end = cJSON_CreateObject();
        cJSON_AddNumberToObject(end, "line", d->line);
        cJSON_AddNumberToObject(end, "character", d->col + 1);
        cJSON_AddItemToObject(range, "start", start);
        cJSON_AddItemToObject(range, "end", end);
        cJSON_AddItemToObject(diag, "range", range);

        cJSON_AddNumberToObject(diag, "severity", d->severity);
        cJSON_AddStringToObject(diag, "source", "lattice");
        cJSON_AddStringToObject(diag, "message", d->message);

        cJSON_AddItemToArray(diags, diag);
    }

    cJSON_AddItemToObject(params, "diagnostics", diags);

    cJSON *notif = lsp_make_notification("textDocument/publishDiagnostics", params);
    lsp_write_response(notif, stdout);
    cJSON_Delete(notif);
}

/* ── Handler: initialize ── */

static void handle_initialize(LspServer *srv, int id) {
    srv->initialized = true;

    cJSON *result = cJSON_CreateObject();

    /* Server capabilities */
    cJSON *caps = cJSON_CreateObject();

    /* Full text sync */
    cJSON *textDocSync = cJSON_CreateObject();
    cJSON_AddNumberToObject(textDocSync, "openClose", 1);
    cJSON_AddNumberToObject(textDocSync, "change", 1); /* Full sync */
    cJSON_AddItemToObject(caps, "textDocumentSync", textDocSync);

    /* Completion */
    cJSON *compOpts = cJSON_CreateObject();
    cJSON *triggers = cJSON_CreateArray();
    cJSON_AddItemToArray(triggers, cJSON_CreateString("."));
    cJSON_AddItemToArray(triggers, cJSON_CreateString(":"));
    cJSON_AddItemToObject(compOpts, "triggerCharacters", triggers);
    cJSON_AddItemToObject(caps, "completionProvider", compOpts);

    /* Hover */
    cJSON_AddBoolToObject(caps, "hoverProvider", 1);

    /* Definition */
    cJSON_AddBoolToObject(caps, "definitionProvider", 1);

    /* Document symbols (outline / breadcrumbs) */
    cJSON_AddBoolToObject(caps, "documentSymbolProvider", 1);

    /* Signature help */
    cJSON *sigHelpOpts = cJSON_CreateObject();
    cJSON *sigTriggers = cJSON_CreateArray();
    cJSON_AddItemToArray(sigTriggers, cJSON_CreateString("("));
    cJSON_AddItemToArray(sigTriggers, cJSON_CreateString(","));
    cJSON_AddItemToObject(sigHelpOpts, "triggerCharacters", sigTriggers);
    cJSON_AddItemToObject(caps, "signatureHelpProvider", sigHelpOpts);

    /* References */
    cJSON_AddBoolToObject(caps, "referencesProvider", 1);

    /* Rename (with prepare support) */
    cJSON *renameOpts = cJSON_CreateObject();
    cJSON_AddBoolToObject(renameOpts, "prepareProvider", 1);
    cJSON_AddItemToObject(caps, "renameProvider", renameOpts);

    /* Formatting */
    cJSON_AddBoolToObject(caps, "documentFormattingProvider", 1);

    /* Code actions */
    cJSON_AddBoolToObject(caps, "codeActionProvider", 1);

    cJSON_AddItemToObject(result, "capabilities", caps);

    /* Server info */
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", "clat-lsp");
    cJSON_AddStringToObject(info, "version", "0.1.0");
    cJSON_AddItemToObject(result, "serverInfo", info);

    cJSON *resp = lsp_make_response(id, result);
    lsp_write_response(resp, stdout);
    cJSON_Delete(resp);
}

/* ── Handler: textDocument/didOpen ── */

static void handle_did_open(LspServer *srv, cJSON *params) {
    cJSON *td = cJSON_GetObjectItem(params, "textDocument");
    if (!td) return;

    const char *uri = cJSON_GetObjectItem(td, "uri")->valuestring;
    const char *text = cJSON_GetObjectItem(td, "text")->valuestring;
    int version = 0;
    cJSON *ver = cJSON_GetObjectItem(td, "version");
    if (ver) version = ver->valueint;

    LspDocument *doc = add_document(srv, uri, text, version);
    lsp_analyze_document(doc);
    publish_diagnostics(srv, doc);
}

/* ── Handler: textDocument/didChange ── */

static void handle_did_change(LspServer *srv, cJSON *params) {
    cJSON *td = cJSON_GetObjectItem(params, "textDocument");
    if (!td) return;

    const char *uri = cJSON_GetObjectItem(td, "uri")->valuestring;
    cJSON *ver = cJSON_GetObjectItem(td, "version");
    int version = ver ? ver->valueint : 0;

    cJSON *changes = cJSON_GetObjectItem(params, "contentChanges");
    if (!changes || cJSON_GetArraySize(changes) == 0) return;

    /* Full sync: take the last content change */
    cJSON *last = cJSON_GetArrayItem(changes, cJSON_GetArraySize(changes) - 1);
    const char *text = cJSON_GetObjectItem(last, "text")->valuestring;

    LspDocument *doc = find_document(srv, uri);
    if (doc) {
        free(doc->text);
        doc->text = strdup(text);
        doc->version = version;
    } else {
        doc = add_document(srv, uri, text, version);
    }

    lsp_analyze_document(doc);
    publish_diagnostics(srv, doc);
}

/* ── Handler: textDocument/didClose ── */

static void handle_did_close(LspServer *srv, cJSON *params) {
    cJSON *td = cJSON_GetObjectItem(params, "textDocument");
    if (!td) return;
    const char *uri = cJSON_GetObjectItem(td, "uri")->valuestring;

    /* Clear diagnostics */
    cJSON *clear_params = cJSON_CreateObject();
    cJSON_AddStringToObject(clear_params, "uri", uri);
    cJSON_AddItemToObject(clear_params, "diagnostics", cJSON_CreateArray());
    cJSON *notif = lsp_make_notification("textDocument/publishDiagnostics", clear_params);
    lsp_write_response(notif, stdout);
    cJSON_Delete(notif);

    remove_document(srv, uri);
}

/* ── Handler: textDocument/completion ── */

/* Navigate to the start of a given 0-based line in text.
 * Returns pointer to the start of that line, or end of text. */
static const char *goto_line(const char *text, int target_line) {
    const char *p = text;
    int line = 0;
    while (line < target_line && *p) {
        if (*p == '\n') line++;
        p++;
    }
    return p;
}

/* Extract the identifier immediately before a given column on a line.
 * Returns a malloc'd string, or NULL. */
static char *extract_preceding_ident(const char *line_start, int col) {
    const char *end = line_start + col;
    const char *start = end;
    while (start > line_start && is_ident_char(start[-1])) start--;
    if (start >= end) return NULL;
    size_t len = (size_t)(end - start);
    char *id = malloc(len + 1);
    if (!id) return NULL;
    memcpy(id, start, len);
    id[len] = '\0';
    return id;
}

/* Check if cursor is immediately after '.' (dot access).
 * If so, extract the identifier before the dot and return it (malloc'd).
 * Returns NULL if not a dot context. */
static char *detect_dot_access(const char *text, int line, int col) {
    const char *line_start = goto_line(text, line);
    if (col < 1) return NULL;
    /* Walk to col position */
    const char *line_end = line_start;
    while (*line_end && *line_end != '\n') line_end++;
    int eff_col = col < (int)(line_end - line_start) ? col : (int)(line_end - line_start);
    if (eff_col < 1) return NULL;
    char ch = line_start[eff_col - 1];
    if (ch != '.') return NULL;
    /* Extract the identifier before the dot */
    return extract_preceding_ident(line_start, eff_col - 1);
}

/* Check if cursor is immediately after '::' (path separator).
 * If so, extract the identifier before '::' and return it (malloc'd).
 * Returns NULL if not a '::' context. */
static char *detect_path_access(const char *text, int line, int col) {
    const char *line_start = goto_line(text, line);
    if (col < 2) return NULL;
    const char *line_end = line_start;
    while (*line_end && *line_end != '\n') line_end++;
    int eff_col = col < (int)(line_end - line_start) ? col : (int)(line_end - line_start);
    if (eff_col < 2) return NULL;
    if (line_start[eff_col - 1] != ':' || line_start[eff_col - 2] != ':') return NULL;
    return extract_preceding_ident(line_start, eff_col - 2);
}

/* Infer the type of a variable by scanning the document text for its declaration.
 * Looks for patterns like:
 *   - let/flux/fix name: Type = ...
 *   - let/flux/fix name = "..."    -> String
 *   - let/flux/fix name = [...]    -> Array
 *   - let/flux/fix name = Map::new() -> Map
 *   - let/flux/fix name = Buffer::new(...) -> Buffer
 *   - let/flux/fix name = Set::new() -> Set
 *   - let/flux/fix name = Channel::new() -> Channel
 *   - let/flux/fix name = Ref::new(...) -> Ref
 *   - let/flux/fix name = StructName { ... } -> StructName
 *   - fn params with type annotations
 * Returns a malloc'd type string, or NULL. */
static char *infer_variable_type(const char *text, const char *var_name, int use_line, const LspDocument *doc) {
    /* Strategy: scan backwards from use_line for declarations of var_name */
    const char *p = text;
    int cur_line = 0;
    const char *best_match = NULL;
    int best_line = -1;

    while (*p) {
        const char *line_start = p;
        /* Find end of line */
        while (*p && *p != '\n') p++;

        if (cur_line <= use_line) {
            /* Look for "let/flux/fix <name>" pattern */
            const char *scan = line_start;
            while (scan < p && (*scan == ' ' || *scan == '\t')) scan++;

            bool is_decl = false;
            const char *after_kw = NULL;
            if (strncmp(scan, "let ", 4) == 0) {
                is_decl = true;
                after_kw = scan + 4;
            } else if (strncmp(scan, "flux ", 5) == 0) {
                is_decl = true;
                after_kw = scan + 5;
            } else if (strncmp(scan, "fix ", 4) == 0) {
                is_decl = true;
                after_kw = scan + 4;
            }

            if (is_decl && after_kw) {
                while (after_kw < p && (*after_kw == ' ' || *after_kw == '\t')) after_kw++;
                size_t vlen = strlen(var_name);
                if (strncmp(after_kw, var_name, vlen) == 0) {
                    char ch = after_kw[vlen];
                    if (ch == ' ' || ch == ':' || ch == '=' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\0') {
                        best_match = after_kw + vlen;
                        best_line = cur_line;
                    }
                }
            }

            /* Also check function parameters: fn name(... var_name: Type ...) */
            if (strncmp(scan, "fn ", 3) == 0) {
                const char *paren = strchr(scan, '(');
                if (paren && paren < p) {
                    const char *close = strchr(paren, ')');
                    if (!close || close > p) close = p;
                    /* Search for var_name within parens */
                    const char *search = paren + 1;
                    size_t vlen = strlen(var_name);
                    while (search < close) {
                        while (search < close && (*search == ' ' || *search == ',')) search++;
                        if (search + vlen <= close && strncmp(search, var_name, vlen) == 0) {
                            char nc = search[vlen];
                            if (nc == ':' || nc == ' ' || nc == ',' || nc == ')') {
                                const char *colon = search + vlen;
                                while (colon < close && *colon == ' ') colon++;
                                if (*colon == ':') {
                                    colon++;
                                    while (colon < close && *colon == ' ') colon++;
                                    const char *tstart = colon;
                                    while (colon < close && *colon != ',' && *colon != ')' && *colon != ' ') colon++;
                                    if (colon > tstart) {
                                        size_t tlen = (size_t)(colon - tstart);
                                        char *type = malloc(tlen + 1);
                                        if (!type) return NULL;
                                        memcpy(type, tstart, tlen);
                                        type[tlen] = '\0';
                                        return type;
                                    }
                                }
                            }
                        }
                        /* Skip to next param */
                        while (search < close && *search != ',') search++;
                        if (search < close) search++;
                    }
                }
            }

            /* Check for-in loop: for var_name in arr_expr */
            if (strncmp(scan, "for ", 4) == 0) {
                const char *after_for = scan + 4;
                while (after_for < p && (*after_for == ' ' || *after_for == '\t')) after_for++;
                size_t vlen = strlen(var_name);
                if (strncmp(after_for, var_name, vlen) == 0) {
                    char nc = after_for[vlen];
                    if (nc == ' ' || nc == '\t') { /* for-in loop variable - can't easily infer element type */
                    }
                }
            }
        }

        if (*p == '\n') p++;
        cur_line++;
    }

    if (!best_match) return NULL;

    /* Now parse what comes after "var_name" in the declaration */
    const char *q = best_match;
    /* Skip whitespace */
    while (*q == ' ' || *q == '\t') q++;

    /* Check for type annotation: "name: Type" */
    if (*q == ':') {
        q++;
        while (*q == ' ' || *q == '\t') q++;
        const char *type_start = q;
        while (*q && *q != '=' && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r') q++;
        if (q > type_start) {
            size_t tlen = (size_t)(q - type_start);
            char *type = malloc(tlen + 1);
            if (!type) return NULL;
            memcpy(type, type_start, tlen);
            type[tlen] = '\0';
            return type;
        }
    }

    /* Check for initializer: "name = expr" */
    if (*q == '=') {
        q++;
        while (*q == ' ' || *q == '\t') q++;

        /* String literal */
        if (*q == '"' || *q == '\'') return strdup("String");

        /* Array literal */
        if (*q == '[') return strdup("Array");

        /* Numeric literal */
        if (*q >= '0' && *q <= '9') {
            /* Check for float */
            const char *num = q;
            while (*num && ((*num >= '0' && *num <= '9') || *num == '.' || *num == '_')) num++;
            const char *dot = strchr(q, '.');
            if (dot && dot < num) return strdup("Float");
            return strdup("Int");
        }

        /* Boolean literal */
        if (strncmp(q, "true", 4) == 0 || strncmp(q, "false", 5) == 0) return strdup("Bool");

        /* Type::new() constructors */
        if (strncmp(q, "Map::new", 8) == 0) return strdup("Map");
        if (strncmp(q, "Set::new", 8) == 0 || strncmp(q, "Set::from", 9) == 0) return strdup("Set");
        if (strncmp(q, "Buffer::new", 11) == 0 || strncmp(q, "Buffer::from", 12) == 0) return strdup("Buffer");
        if (strncmp(q, "Channel::new", 12) == 0) return strdup("Channel");
        if (strncmp(q, "Ref::new", 8) == 0) return strdup("Ref");

        /* Struct constructor: StructName { ... } or StructName::new(...) */
        if (*q >= 'A' && *q <= 'Z') {
            const char *id_start = q;
            while (*q && is_ident_char(*q)) q++;
            size_t id_len = (size_t)(q - id_start);
            /* Skip whitespace */
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '{' || (*q == ':' && *(q + 1) == ':')) {
                /* Check if this is a known struct name */
                char *name = malloc(id_len + 1);
                if (!name) return NULL;
                memcpy(name, id_start, id_len);
                name[id_len] = '\0';
                if (doc) {
                    for (size_t i = 0; i < doc->struct_def_count; i++) {
                        if (strcmp(doc->struct_defs[i].name, name) == 0) return name; /* confirmed struct type */
                    }
                }
                /* Could also be an enum variant assignment, but return the type anyway */
                return name;
            }
        }
    }

    (void)best_line;
    return NULL;
}

/* Add method completions for a given owner type */
static void add_method_completions(cJSON *items, const LspSymbolIndex *idx, const char *type_name) {
    if (!idx) return;
    for (size_t i = 0; i < idx->method_count; i++) {
        if (idx->methods[i].owner_type && strcmp(idx->methods[i].owner_type, type_name) == 0) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "label", idx->methods[i].name);
            cJSON_AddNumberToObject(item, "kind", 2); /* Method */
            if (idx->methods[i].signature) cJSON_AddStringToObject(item, "detail", idx->methods[i].signature);
            if (idx->methods[i].doc) {
                cJSON *doc_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(doc_obj, "kind", "markdown");
                cJSON_AddStringToObject(doc_obj, "value", idx->methods[i].doc);
                cJSON_AddItemToObject(item, "documentation", doc_obj);
            }
            cJSON_AddItemToArray(items, item);
        }
    }
}

/* Add struct field completions */
static void add_struct_field_completions(cJSON *items, const LspDocument *doc, const char *struct_name) {
    for (size_t i = 0; i < doc->struct_def_count; i++) {
        if (strcmp(doc->struct_defs[i].name, struct_name) == 0) {
            for (size_t j = 0; j < doc->struct_defs[i].field_count; j++) {
                LspFieldInfo *f = &doc->struct_defs[i].fields[j];
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "label", f->name);
                cJSON_AddNumberToObject(item, "kind", 5); /* Field */
                if (f->type_name) cJSON_AddStringToObject(item, "detail", f->type_name);
                cJSON_AddItemToArray(items, item);
            }
            break;
        }
    }
}

/* Add enum variant completions */
static void add_enum_variant_completions(cJSON *items, const LspDocument *doc, const char *enum_name) {
    for (size_t i = 0; i < doc->enum_def_count; i++) {
        if (strcmp(doc->enum_defs[i].name, enum_name) == 0) {
            for (size_t j = 0; j < doc->enum_defs[i].variant_count; j++) {
                LspVariantInfo *v = &doc->enum_defs[i].variants[j];
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "label", v->name);
                cJSON_AddNumberToObject(item, "kind", 20); /* EnumMember */
                if (v->params) {
                    char detail[256];
                    snprintf(detail, sizeof(detail), "%s::%s%s", enum_name, v->name, v->params);
                    cJSON_AddStringToObject(item, "detail", detail);
                } else {
                    char detail[256];
                    snprintf(detail, sizeof(detail), "%s::%s", enum_name, v->name);
                    cJSON_AddStringToObject(item, "detail", detail);
                }
                cJSON_AddItemToArray(items, item);
            }
            break;
        }
    }
}

/* Check if a name corresponds to a known enum in the document */
static bool is_enum_name(const LspDocument *doc, const char *name) {
    for (size_t i = 0; i < doc->enum_def_count; i++) {
        if (strcmp(doc->enum_defs[i].name, name) == 0) return true;
    }
    return false;
}

/* Check if a name corresponds to a known struct in the document */
static bool is_struct_name(const LspDocument *doc, const char *name) {
    for (size_t i = 0; i < doc->struct_def_count; i++) {
        if (strcmp(doc->struct_defs[i].name, name) == 0) return true;
    }
    return false;
}

static void handle_completion(LspServer *srv, cJSON *params, int id) {
    cJSON *td = cJSON_GetObjectItem(params, "textDocument");
    cJSON *pos = cJSON_GetObjectItem(params, "position");
    const char *uri = td ? cJSON_GetObjectItem(td, "uri")->valuestring : NULL;

    int line = 0, col = 0;
    if (pos) {
        line = cJSON_GetObjectItem(pos, "line")->valueint;
        col = cJSON_GetObjectItem(pos, "character")->valueint;
    }

    LspDocument *doc = uri ? find_document(srv, uri) : NULL;
    cJSON *items = cJSON_CreateArray();

    /* ── Check for dot-access completion (obj.) ── */
    if (doc && doc->text) {
        char *dot_obj = detect_dot_access(doc->text, line, col);
        if (dot_obj) {
            /* Try to infer the type of the object before the dot */
            char *type = infer_variable_type(doc->text, dot_obj, line, doc);

            if (type) {
                /* Check if this is a struct type -> suggest fields */
                if (is_struct_name(doc, type)) { add_struct_field_completions(items, doc, type); }
                /* Always add methods for the inferred type */
                add_method_completions(items, srv->index, type);
                free(type);
            } else {
                /* Could not infer type — check if the name itself is a builtin type
                 * by looking at the first character (capitalized = could be type) */
                if (dot_obj[0] >= 'A' && dot_obj[0] <= 'Z') {
                    /* Might be a struct variable with capital name */
                    if (is_struct_name(doc, dot_obj)) { add_struct_field_completions(items, doc, dot_obj); }
                }
                /* Fall back: suggest all methods */
                if (srv->index) {
                    for (size_t i = 0; i < srv->index->method_count; i++) {
                        cJSON *item = cJSON_CreateObject();
                        cJSON_AddStringToObject(item, "label", srv->index->methods[i].name);
                        cJSON_AddNumberToObject(item, "kind", 2); /* Method */
                        if (srv->index->methods[i].signature)
                            cJSON_AddStringToObject(item, "detail", srv->index->methods[i].signature);
                        if (srv->index->methods[i].doc) {
                            cJSON *doc_obj = cJSON_CreateObject();
                            cJSON_AddStringToObject(doc_obj, "kind", "markdown");
                            cJSON_AddStringToObject(doc_obj, "value", srv->index->methods[i].doc);
                            cJSON_AddItemToObject(item, "documentation", doc_obj);
                        }
                        cJSON_AddItemToArray(items, item);
                    }
                }
            }
            free(dot_obj);

            cJSON *resp = lsp_make_response(id, items);
            lsp_write_response(resp, stdout);
            cJSON_Delete(resp);
            return;
        }

        /* ── Check for path-access completion (Name::) ── */
        char *path_obj = detect_path_access(doc->text, line, col);
        if (path_obj) {
            /* Check if this is an enum name */
            if (is_enum_name(doc, path_obj)) { add_enum_variant_completions(items, doc, path_obj); }
            /* Also check builtin constructors: Map::, Set::, Buffer::, etc. */
            if (srv->index) {
                for (size_t i = 0; i < srv->index->builtin_count; i++) {
                    /* Check if signature starts with "Name::" */
                    const char *sig = srv->index->builtins[i].signature;
                    if (sig) {
                        size_t plen = strlen(path_obj);
                        if (strncmp(sig, path_obj, plen) == 0 && sig[plen] == ':' && sig[plen + 1] == ':') {
                            /* Extract the method name after :: */
                            const char *method_start = sig + plen + 2;
                            const char *paren = strchr(method_start, '(');
                            size_t mlen = paren ? (size_t)(paren - method_start) : strlen(method_start);
                            char *mname = malloc(mlen + 1);
                            if (!mname) return;
                            memcpy(mname, method_start, mlen);
                            mname[mlen] = '\0';

                            cJSON *item = cJSON_CreateObject();
                            cJSON_AddStringToObject(item, "label", mname);
                            cJSON_AddNumberToObject(item, "kind", 3); /* Function */
                            cJSON_AddStringToObject(item, "detail", sig);
                            if (srv->index->builtins[i].doc) {
                                cJSON *doc_obj = cJSON_CreateObject();
                                cJSON_AddStringToObject(doc_obj, "kind", "markdown");
                                cJSON_AddStringToObject(doc_obj, "value", srv->index->builtins[i].doc);
                                cJSON_AddItemToObject(item, "documentation", doc_obj);
                            }
                            cJSON_AddItemToArray(items, item);
                            free(mname);
                        }
                    }
                }
            }
            free(path_obj);

            cJSON *resp = lsp_make_response(id, items);
            lsp_write_response(resp, stdout);
            cJSON_Delete(resp);
            return;
        }
    }

    /* ── Default completion: keywords + builtins + document symbols ── */

    /* Keywords */
    for (int i = 0; lattice_keywords[i]; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "label", lattice_keywords[i]);
        cJSON_AddNumberToObject(item, "kind", LSP_SYM_KEYWORD);
        cJSON_AddItemToArray(items, item);
    }

    /* Builtins from symbol index */
    if (srv->index) {
        for (size_t i = 0; i < srv->index->builtin_count; i++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "label", srv->index->builtins[i].name);
            cJSON_AddNumberToObject(item, "kind", 3); /* Function */
            if (srv->index->builtins[i].signature)
                cJSON_AddStringToObject(item, "detail", srv->index->builtins[i].signature);
            if (srv->index->builtins[i].doc) {
                cJSON *doc_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(doc_obj, "kind", "markdown");
                cJSON_AddStringToObject(doc_obj, "value", srv->index->builtins[i].doc);
                cJSON_AddItemToObject(item, "documentation", doc_obj);
            }
            cJSON_AddItemToArray(items, item);
        }
    }

    /* User-defined symbols from current document */
    if (doc) {
        for (size_t i = 0; i < doc->symbol_count; i++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "label", doc->symbols[i].name);
            cJSON_AddNumberToObject(item, "kind",
                                    doc->symbols[i].kind == LSP_SYM_FUNCTION ? 3
                                    : doc->symbols[i].kind == LSP_SYM_STRUCT ? 22
                                    : doc->symbols[i].kind == LSP_SYM_ENUM   ? 13
                                                                             : 6);
            if (doc->symbols[i].signature) cJSON_AddStringToObject(item, "detail", doc->symbols[i].signature);
            cJSON_AddItemToArray(items, item);
        }
    }

    cJSON *resp = lsp_make_response(id, items);
    lsp_write_response(resp, stdout);
    cJSON_Delete(resp);
}

/* ── Handler: textDocument/hover ── */

/* Build markdown hover content for a builtin from the static docs table.
 * Returns a malloc'd string with ```lattice signature``` + description. */
static char *build_builtin_hover(const char *sig, const char *desc) {
    size_t blen = strlen(sig) + strlen(desc) + 32;
    char *buf = malloc(blen);
    if (!buf) return NULL;
    snprintf(buf, blen, "```lattice\n%s\n```\n%s", sig, desc);
    return buf;
}

static void handle_hover(LspServer *srv, cJSON *params, int id) {
    cJSON *td = cJSON_GetObjectItem(params, "textDocument");
    cJSON *pos = cJSON_GetObjectItem(params, "position");
    if (!td || !pos) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    const char *uri = cJSON_GetObjectItem(td, "uri")->valuestring;
    int line = cJSON_GetObjectItem(pos, "line")->valueint;
    int col = cJSON_GetObjectItem(pos, "character")->valueint;

    LspDocument *doc = find_document(srv, uri);
    if (!doc || !doc->text) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    /* Find the word at line:col using the shared helper */
    int word_col = 0;
    char *word = extract_word_at(doc->text, line, col, &word_col);

    if (!word) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    const char *hover_text = NULL;
    char *hover_buf = NULL; /* dynamically built hover, freed at end */

    /* Priority 1: Document symbols (user-defined names take precedence) */
    for (size_t i = 0; i < doc->symbol_count; i++) {
        if (strcmp(doc->symbols[i].name, word) == 0) {
            if (doc->symbols[i].kind == LSP_SYM_VARIABLE) {
                /* For variables, try to include inferred type */
                char *type = infer_variable_type(doc->text, word, line, doc);
                const char *sig = doc->symbols[i].signature;
                if (type) {
                    size_t blen = (sig ? strlen(sig) : 0) + strlen(type) + 48;
                    hover_buf = malloc(blen);
                    if (!hover_buf) {
                        free(type);
                        free(word);
                        return;
                    }
                    snprintf(hover_buf, blen, "```lattice\n%s%s%s\n```", sig ? sig : word,
                             (sig && strchr(sig, ':')) ? "" : ": ", (sig && strchr(sig, ':')) ? "" : type);
                    hover_text = hover_buf;
                    free(type);
                } else if (sig) {
                    size_t blen = strlen(sig) + 24;
                    hover_buf = malloc(blen);
                    if (!hover_buf) {
                        free(word);
                        return;
                    }
                    snprintf(hover_buf, blen, "```lattice\n%s\n```", sig);
                    hover_text = hover_buf;
                }
            } else {
                /* Functions, structs, enums, traits */
                const char *sig = doc->symbols[i].signature;
                const char *sym_doc = doc->symbols[i].doc;
                if (sig && sym_doc) {
                    size_t blen = strlen(sig) + strlen(sym_doc) + 32;
                    hover_buf = malloc(blen);
                    if (!hover_buf) {
                        free(word);
                        return;
                    }
                    snprintf(hover_buf, blen, "```lattice\n%s\n```\n%s", sig, sym_doc);
                    hover_text = hover_buf;
                } else if (sig) {
                    size_t blen = strlen(sig) + 24;
                    hover_buf = malloc(blen);
                    if (!hover_buf) {
                        free(word);
                        return;
                    }
                    snprintf(hover_buf, blen, "```lattice\n%s\n```", sig);
                    hover_text = hover_buf;
                } else if (sym_doc) {
                    hover_text = sym_doc;
                }
            }
            break;
        }
    }

    /* Priority 2: Builtin functions from source-scanned index */
    if (!hover_text && srv->index) {
        for (size_t i = 0; i < srv->index->builtin_count; i++) {
            if (strcmp(srv->index->builtins[i].name, word) == 0) {
                hover_text = srv->index->builtins[i].doc;
                break;
            }
        }
    }

    /* Priority 3: Static builtin documentation fallback */
    if (!hover_text) {
        const char *sig = NULL;
        const char *desc = lsp_lookup_builtin_doc(word, &sig);
        if (desc && sig) {
            hover_buf = build_builtin_hover(sig, desc);
            hover_text = hover_buf;
        }
    }

    /* Priority 4: Methods from source-scanned index */
    if (!hover_text && srv->index) {
        for (size_t i = 0; i < srv->index->method_count; i++) {
            if (strcmp(srv->index->methods[i].name, word) == 0) {
                hover_text = srv->index->methods[i].doc;
                break;
            }
        }
    }

    /* Priority 5: Infer type for identifiers not in symbol table
     * (e.g., local variables inside functions) */
    if (!hover_text && doc->text) {
        char *type = infer_variable_type(doc->text, word, line, doc);
        if (type) {
            size_t blen = strlen(word) + strlen(type) + 32;
            hover_buf = malloc(blen);
            if (hover_buf) {
                snprintf(hover_buf, blen, "```lattice\n%s: %s\n```", word, type);
                hover_text = hover_buf;
            }
            free(type);
        }
    }

    /* Priority 6: Keyword documentation */
    if (!hover_text) {
        const char *kw_doc = lsp_lookup_keyword_doc(word);
        if (kw_doc) hover_text = kw_doc;
    }

    free(word);

    if (hover_text) {
        cJSON *result = cJSON_CreateObject();
        cJSON *contents = cJSON_CreateObject();
        cJSON_AddStringToObject(contents, "kind", "markdown");
        cJSON_AddStringToObject(contents, "value", hover_text);
        cJSON_AddItemToObject(result, "contents", contents);
        cJSON *resp = lsp_make_response(id, result);
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
    } else {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
    }
    free(hover_buf);
}

/* ── Handler: textDocument/definition ── */

static void handle_definition(LspServer *srv, cJSON *params, int id) {
    cJSON *td = cJSON_GetObjectItem(params, "textDocument");
    cJSON *pos = cJSON_GetObjectItem(params, "position");
    if (!td || !pos) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    const char *uri = cJSON_GetObjectItem(td, "uri")->valuestring;
    int line = cJSON_GetObjectItem(pos, "line")->valueint;
    int col = cJSON_GetObjectItem(pos, "character")->valueint;

    LspDocument *doc = find_document(srv, uri);
    if (!doc || !doc->text) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    /* Find word at cursor (same logic as hover) */
    const char *p = doc->text;
    int cur_line = 0;
    while (cur_line < line && *p) {
        if (*p == '\n') cur_line++;
        p++;
    }
    const char *line_start = p;
    int cur_col = 0;
    while (cur_col < col && *p && *p != '\n') {
        cur_col++;
        p++;
    }

    const char *ws = p;
    while (ws > line_start && (ws[-1] == '_' || (ws[-1] >= 'a' && ws[-1] <= 'z') || (ws[-1] >= 'A' && ws[-1] <= 'Z') ||
                               (ws[-1] >= '0' && ws[-1] <= '9')))
        ws--;

    const char *we = p;
    while (*we &&
           (*we == '_' || (*we >= 'a' && *we <= 'z') || (*we >= 'A' && *we <= 'Z') || (*we >= '0' && *we <= '9')))
        we++;

    if (we <= ws) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    size_t wlen = (size_t)(we - ws);
    char *word = malloc(wlen + 1);
    if (!word) return;
    memcpy(word, ws, wlen);
    word[wlen] = '\0';

    /* Search document symbols (fn, struct, enum, trait, impl, variables) */
    for (size_t i = 0; i < doc->symbol_count; i++) {
        if (strcmp(doc->symbols[i].name, word) == 0 && doc->symbols[i].line >= 0) {
            cJSON *result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "uri", uri);
            cJSON *range = cJSON_CreateObject();
            cJSON *start_pos = cJSON_CreateObject();
            cJSON_AddNumberToObject(start_pos, "line", doc->symbols[i].line);
            cJSON_AddNumberToObject(start_pos, "character", doc->symbols[i].col);
            cJSON_AddItemToObject(range, "start", start_pos);
            cJSON *end_pos = cJSON_CreateObject();
            cJSON_AddNumberToObject(end_pos, "line", doc->symbols[i].line);
            cJSON_AddNumberToObject(end_pos, "character", doc->symbols[i].col + (int)strlen(doc->symbols[i].name));
            cJSON_AddItemToObject(range, "end", end_pos);
            cJSON_AddItemToObject(result, "range", range);

            free(word);
            cJSON *resp = lsp_make_response(id, result);
            lsp_write_response(resp, stdout);
            cJSON_Delete(resp);
            return;
        }
    }

    /* Fallback: scan document text for variable declarations (let/flux/fix/for)
     * that the symbol extractor may have missed (e.g., locals inside functions) */
    if (doc->text) {
        const char *text = doc->text;
        const char *tp = text;
        int def_line = -1, def_col = -1;
        int scan_line = 0;
        size_t wlen2 = strlen(word);

        while (*tp) {
            const char *ls = tp;
            /* Skip leading whitespace */
            while (*tp == ' ' || *tp == '\t') tp++;

            /* Check for let/flux/fix/for keywords */
            const char *kw = NULL;
            size_t kw_len = 0;
            if (strncmp(tp, "let ", 4) == 0) {
                kw = tp;
                kw_len = 4;
            } else if (strncmp(tp, "flux ", 5) == 0) {
                kw = tp;
                kw_len = 5;
            } else if (strncmp(tp, "fix ", 4) == 0) {
                kw = tp;
                kw_len = 4;
            } else if (strncmp(tp, "for ", 4) == 0) {
                kw = tp;
                kw_len = 4;
            }

            if (kw) {
                const char *after = kw + kw_len;
                while (*after == ' ' || *after == '\t') after++;
                if (strncmp(after, word, wlen2) == 0) {
                    char nc = after[wlen2];
                    if (nc == ' ' || nc == ':' || nc == '=' || nc == '\t' || nc == '\n' || nc == '\r' || nc == '\0' ||
                        nc == ',') {
                        /* Found a declaration — only use if before the cursor */
                        if (scan_line <= line) {
                            def_line = scan_line;
                            def_col = (int)(after - ls);
                        }
                    }
                }
            }

            /* Check function parameters: fn name(... word ...) */
            if (strncmp(tp, "fn ", 3) == 0) {
                const char *paren = tp;
                while (*paren && *paren != '(' && *paren != '\n') paren++;
                if (*paren == '(') {
                    const char *close = paren;
                    while (*close && *close != ')' && *close != '\n') close++;
                    /* Search for word as a parameter name */
                    const char *search = paren + 1;
                    while (search < close) {
                        while (search < close && (*search == ' ' || *search == ',')) search++;
                        if ((size_t)(close - search) >= wlen2 && strncmp(search, word, wlen2) == 0) {
                            char nc = search[wlen2];
                            if (nc == ':' || nc == ' ' || nc == ',' || nc == ')') {
                                if (scan_line <= line) {
                                    def_line = scan_line;
                                    def_col = (int)(search - ls);
                                }
                                break;
                            }
                        }
                        while (search < close && *search != ',') search++;
                        if (search < close) search++;
                    }
                }
            }

            /* Advance to next line */
            while (*tp && *tp != '\n') tp++;
            if (*tp == '\n') {
                tp++;
                scan_line++;
            }
        }

        if (def_line >= 0) {
            cJSON *result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "uri", uri);
            cJSON *range = cJSON_CreateObject();
            cJSON *start_pos = cJSON_CreateObject();
            cJSON_AddNumberToObject(start_pos, "line", def_line);
            cJSON_AddNumberToObject(start_pos, "character", def_col);
            cJSON_AddItemToObject(range, "start", start_pos);
            cJSON *end_pos = cJSON_CreateObject();
            cJSON_AddNumberToObject(end_pos, "line", def_line);
            cJSON_AddNumberToObject(end_pos, "character", def_col + (int)wlen2);
            cJSON_AddItemToObject(range, "end", end_pos);
            cJSON_AddItemToObject(result, "range", range);

            free(word);
            cJSON *resp = lsp_make_response(id, result);
            lsp_write_response(resp, stdout);
            cJSON_Delete(resp);
            return;
        }
    }

    free(word);
    cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
    lsp_write_response(resp, stdout);
    cJSON_Delete(resp);
}

/* ── Handler: textDocument/documentSymbol ── */

static int lsp_symbol_kind(LspSymbolKind kind) {
    switch (kind) {
        case LSP_SYM_FUNCTION: return 12; /* Function */
        case LSP_SYM_STRUCT: return 23;   /* Struct */
        case LSP_SYM_ENUM: return 10;     /* Enum */
        case LSP_SYM_VARIABLE: return 13; /* Variable */
        case LSP_SYM_METHOD: return 6;    /* Method */
        default: return 13;               /* Variable as fallback */
    }
}

static void handle_document_symbol(LspServer *srv, cJSON *params, int id) {
    cJSON *td = cJSON_GetObjectItem(params, "textDocument");
    if (!td) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateArray());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    const char *uri = cJSON_GetObjectItem(td, "uri")->valuestring;
    LspDocument *doc = find_document(srv, uri);

    cJSON *symbols = cJSON_CreateArray();

    if (doc) {
        for (size_t i = 0; i < doc->symbol_count; i++) {
            LspSymbol *s = &doc->symbols[i];
            cJSON *sym = cJSON_CreateObject();

            cJSON_AddStringToObject(sym, "name", s->name);
            cJSON_AddNumberToObject(sym, "kind", lsp_symbol_kind(s->kind));

            /* range: full extent of the symbol (use line/col as start) */
            cJSON *range = cJSON_CreateObject();
            cJSON *start = cJSON_CreateObject();
            cJSON_AddNumberToObject(start, "line", s->line >= 0 ? s->line : 0);
            cJSON_AddNumberToObject(start, "character", s->col >= 0 ? s->col : 0);
            cJSON *end = cJSON_CreateObject();
            cJSON_AddNumberToObject(end, "line", s->line >= 0 ? s->line : 0);
            cJSON_AddNumberToObject(end, "character", (s->col >= 0 ? s->col : 0) + (int)strlen(s->name));
            cJSON_AddItemToObject(range, "start", start);
            cJSON_AddItemToObject(range, "end", end);
            cJSON_AddItemToObject(sym, "range", range);

            /* selectionRange: just the name */
            cJSON *sel_range = cJSON_CreateObject();
            cJSON *sel_start = cJSON_CreateObject();
            cJSON_AddNumberToObject(sel_start, "line", s->line >= 0 ? s->line : 0);
            cJSON_AddNumberToObject(sel_start, "character", s->col >= 0 ? s->col : 0);
            cJSON *sel_end = cJSON_CreateObject();
            cJSON_AddNumberToObject(sel_end, "line", s->line >= 0 ? s->line : 0);
            cJSON_AddNumberToObject(sel_end, "character", (s->col >= 0 ? s->col : 0) + (int)strlen(s->name));
            cJSON_AddItemToObject(sel_range, "start", sel_start);
            cJSON_AddItemToObject(sel_range, "end", sel_end);
            cJSON_AddItemToObject(sym, "selectionRange", sel_range);

            if (s->signature) cJSON_AddStringToObject(sym, "detail", s->signature);

            cJSON_AddItemToArray(symbols, sym);
        }
    }

    cJSON *resp = lsp_make_response(id, symbols);
    lsp_write_response(resp, stdout);
    cJSON_Delete(resp);
}

/* ── Handler: textDocument/signatureHelp ── */

/* Parse a signature string like "fn name(a: Int, b: String)" or "name(a: Int, b: String) -> Ret"
 * and extract individual parameter label strings.
 * Returns number of params extracted. params[] is filled with malloc'd strings. */
static int parse_signature_params(const char *sig, char **params, int max_params) {
    const char *open = strchr(sig, '(');
    if (!open) return 0;
    const char *close = strchr(open, ')');
    if (!close) return 0;

    /* Extract the content between parens */
    size_t inner_len = (size_t)(close - open - 1);
    if (inner_len == 0) return 0;

    char *inner = malloc(inner_len + 1);
    if (!inner) return 0;
    memcpy(inner, open + 1, inner_len);
    inner[inner_len] = '\0';

    int count = 0;
    char *tok = inner;
    while (*tok && count < max_params) {
        /* Skip leading whitespace */
        while (*tok == ' ' || *tok == '\t') tok++;
        if (!*tok) break;

        /* Find next comma or end */
        char *comma = strchr(tok, ',');
        char *end = comma ? comma : inner + inner_len;

        /* Trim trailing whitespace */
        char *trim_end = end;
        while (trim_end > tok && (trim_end[-1] == ' ' || trim_end[-1] == '\t')) trim_end--;

        size_t plen = (size_t)(trim_end - tok);
        if (plen > 0) {
            params[count] = malloc(plen + 1);
            if (!params[count]) continue;
            memcpy(params[count], tok, plen);
            params[count][plen] = '\0';
            count++;
        }

        if (!comma) break;
        tok = comma + 1;
    }

    free(inner);
    return count;
}

/* Count commas before the cursor position on the current line, starting from
 * the opening paren of the function call. This determines the active parameter. */
static int count_active_param(const char *text, int line, int col) {
    const char *p = text;
    int cur_line = 0;
    while (cur_line < line && *p) {
        if (*p == '\n') cur_line++;
        p++;
    }
    /* p points to start of target line. Walk to col. */
    const char *line_end = p;
    while (*line_end && *line_end != '\n') line_end++;

    /* Scan backwards from col to find the opening '(' */
    int depth = 0;
    int commas = 0;
    const char *cursor = p + (col < (int)(line_end - p) ? col : (int)(line_end - p));

    for (const char *c = cursor - 1; c >= p; c--) {
        if (*c == ')') depth++;
        else if (*c == '(') {
            if (depth == 0) break; /* Found the matching open paren */
            depth--;
        } else if (*c == ',' && depth == 0) {
            commas++;
        }
    }
    return commas;
}

static void handle_signature_help(LspServer *srv, cJSON *params, int id) {
    cJSON *td = cJSON_GetObjectItem(params, "textDocument");
    cJSON *pos = cJSON_GetObjectItem(params, "position");
    if (!td || !pos) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    const char *uri = cJSON_GetObjectItem(td, "uri")->valuestring;
    int line = cJSON_GetObjectItem(pos, "line")->valueint;
    int col = cJSON_GetObjectItem(pos, "character")->valueint;

    LspDocument *doc = find_document(srv, uri);
    if (!doc || !doc->text) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    /* Walk backwards from cursor to find the function name before '(' */
    const char *p = doc->text;
    int cur_line = 0;
    while (cur_line < line && *p) {
        if (*p == '\n') cur_line++;
        p++;
    }
    const char *line_start = p;
    const char *line_end = p;
    while (*line_end && *line_end != '\n') line_end++;

    /* Find the opening paren by scanning back from cursor */
    int eff_col = col < (int)(line_end - line_start) ? col : (int)(line_end - line_start);
    const char *cursor = line_start + eff_col;

    /* Scan backwards to find '(' (handling nested parens) */
    int depth = 0;
    const char *open_paren = NULL;
    for (const char *c = cursor - 1; c >= line_start; c--) {
        if (*c == ')') depth++;
        else if (*c == '(') {
            if (depth == 0) {
                open_paren = c;
                break;
            }
            depth--;
        }
    }

    if (!open_paren) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    /* Extract function name just before the open paren */
    const char *name_end = open_paren;
    while (name_end > line_start && (name_end[-1] == ' ' || name_end[-1] == '\t')) name_end--;
    const char *name_start = name_end;
    while (name_start > line_start && is_ident_char(name_start[-1])) name_start--;

    if (name_start >= name_end) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    size_t name_len = (size_t)(name_end - name_start);
    char *func_name = malloc(name_len + 1);
    if (!func_name) return;
    memcpy(func_name, name_start, name_len);
    func_name[name_len] = '\0';

    /* Look up the function signature */
    const char *sig = NULL;

    /* Search builtins */
    if (srv->index) {
        for (size_t i = 0; i < srv->index->builtin_count; i++) {
            if (strcmp(srv->index->builtins[i].name, func_name) == 0) {
                sig = srv->index->builtins[i].signature;
                break;
            }
        }
        if (!sig) {
            for (size_t i = 0; i < srv->index->method_count; i++) {
                if (strcmp(srv->index->methods[i].name, func_name) == 0) {
                    sig = srv->index->methods[i].signature;
                    break;
                }
            }
        }
    }

    /* Search document symbols */
    if (!sig && doc) {
        for (size_t i = 0; i < doc->symbol_count; i++) {
            if (doc->symbols[i].kind == LSP_SYM_FUNCTION && strcmp(doc->symbols[i].name, func_name) == 0) {
                sig = doc->symbols[i].signature;
                break;
            }
        }
    }

    free(func_name);

    if (!sig) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    /* Parse parameters from signature */
    char *param_labels[32];
    int param_count = parse_signature_params(sig, param_labels, 32);

    /* Determine active parameter from comma count */
    int active_param = count_active_param(doc->text, line, col);
    if (active_param >= param_count) active_param = param_count - 1;
    if (active_param < 0) active_param = 0;

    /* Build SignatureHelp response */
    cJSON *result = cJSON_CreateObject();
    cJSON *signatures = cJSON_CreateArray();
    cJSON *sig_info = cJSON_CreateObject();

    cJSON_AddStringToObject(sig_info, "label", sig);

    /* Parameters */
    cJSON *json_params = cJSON_CreateArray();
    for (int i = 0; i < param_count; i++) {
        cJSON *pi = cJSON_CreateObject();
        cJSON_AddStringToObject(pi, "label", param_labels[i]);
        cJSON_AddItemToArray(json_params, pi);
        free(param_labels[i]);
    }
    cJSON_AddItemToObject(sig_info, "parameters", json_params);

    cJSON_AddItemToArray(signatures, sig_info);
    cJSON_AddItemToObject(result, "signatures", signatures);
    cJSON_AddNumberToObject(result, "activeSignature", 0);
    cJSON_AddNumberToObject(result, "activeParameter", active_param);

    cJSON *resp = lsp_make_response(id, result);
    lsp_write_response(resp, stdout);
    cJSON_Delete(resp);
}

/* ── Rename validation helpers ── */

/* Check if a word is a language keyword (not renameable) */
static bool is_keyword(const char *word) {
    for (int i = 0; lattice_keywords[i]; i++) {
        if (strcmp(lattice_keywords[i], word) == 0) return true;
    }
    return false;
}

/* Check if a word is a builtin function (not renameable) */
static bool is_builtin_name(const char *word) {
    for (int i = 0; builtin_docs[i].name; i++) {
        if (strcmp(builtin_docs[i].name, word) == 0) return true;
    }
    return false;
}

/* ── Handler: textDocument/references ── */

/* Check if position is inside a string literal or comment (simple heuristic).
 * Counts unescaped quotes and detects // line comments from line start to the
 * given column. */
static bool in_string_or_comment(const char *line_start, int col) {
    bool in_str = false;
    char quote = 0;
    for (int i = 0; i < col && line_start[i] && line_start[i] != '\n'; i++) {
        char c = line_start[i];
        if (in_str) {
            if (c == '\\') {
                i++;
                continue;
            } /* skip escaped char */
            if (c == quote) in_str = false;
        } else {
            if (c == '"' || c == '\'') {
                in_str = true;
                quote = c;
            } else if (c == '/' && line_start[i + 1] == '/') {
                return true; /* rest of line is a comment */
            }
        }
    }
    return in_str;
}

/* Find all occurrences of a word (as whole identifier) in the document text.
 * Returns a cJSON array of Location objects. */
static cJSON *find_all_references(const char *text, const char *uri, const char *word) {
    cJSON *locations = cJSON_CreateArray();
    size_t word_len = strlen(word);
    if (word_len == 0) return locations;

    const char *p = text;
    int line = 0;

    while (*p) {
        const char *line_start = p;

        /* Scan this line for occurrences */
        while (*p && *p != '\n') {
            /* Check for word boundary match */
            if (is_ident_char(*p)) {
                const char *start = p;
                while (*p && is_ident_char(*p)) p++;
                size_t ident_len = (size_t)(p - start);

                if (ident_len == word_len && memcmp(start, word, word_len) == 0) {
                    int col = (int)(start - line_start);
                    /* Skip if inside a string literal or comment */
                    if (!in_string_or_comment(line_start, col)) {
                        cJSON *loc = cJSON_CreateObject();
                        cJSON_AddStringToObject(loc, "uri", uri);
                        cJSON *range = cJSON_CreateObject();
                        cJSON *s = cJSON_CreateObject();
                        cJSON_AddNumberToObject(s, "line", line);
                        cJSON_AddNumberToObject(s, "character", col);
                        cJSON *e = cJSON_CreateObject();
                        cJSON_AddNumberToObject(e, "line", line);
                        cJSON_AddNumberToObject(e, "character", col + (int)word_len);
                        cJSON_AddItemToObject(range, "start", s);
                        cJSON_AddItemToObject(range, "end", e);
                        cJSON_AddItemToObject(loc, "range", range);
                        cJSON_AddItemToArray(locations, loc);
                    }
                }
                continue; /* p already advanced past the identifier */
            }
            p++;
        }

        if (*p == '\n') {
            p++;
            line++;
        }
    }

    return locations;
}

static void handle_references(LspServer *srv, cJSON *params, int id) {
    cJSON *td = cJSON_GetObjectItem(params, "textDocument");
    cJSON *pos = cJSON_GetObjectItem(params, "position");
    if (!td || !pos) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateArray());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    const char *uri = cJSON_GetObjectItem(td, "uri")->valuestring;
    int line = cJSON_GetObjectItem(pos, "line")->valueint;
    int col = cJSON_GetObjectItem(pos, "character")->valueint;

    LspDocument *doc = find_document(srv, uri);
    if (!doc || !doc->text) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateArray());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    char *word = extract_word_at(doc->text, line, col, NULL);
    if (!word) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateArray());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    cJSON *locations = find_all_references(doc->text, uri, word);
    free(word);

    cJSON *resp = lsp_make_response(id, locations);
    lsp_write_response(resp, stdout);
    cJSON_Delete(resp);
}

/* ── Handler: textDocument/prepareRename ── */

static void handle_prepare_rename(LspServer *srv, cJSON *params, int id) {
    cJSON *td = cJSON_GetObjectItem(params, "textDocument");
    cJSON *pos = cJSON_GetObjectItem(params, "position");
    if (!td || !pos) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    const char *uri = cJSON_GetObjectItem(td, "uri")->valuestring;
    int line = cJSON_GetObjectItem(pos, "line")->valueint;
    int col = cJSON_GetObjectItem(pos, "character")->valueint;

    LspDocument *doc = find_document(srv, uri);
    if (!doc || !doc->text) {
        cJSON *resp = lsp_make_error(id, -32602, "Document not found");
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    int word_col = 0;
    char *word = extract_word_at(doc->text, line, col, &word_col);
    if (!word) {
        cJSON *resp = lsp_make_error(id, -32602, "No identifier at position");
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    /* Reject rename of keywords */
    if (is_keyword(word)) {
        free(word);
        cJSON *resp = lsp_make_error(id, -32602, "Cannot rename a keyword");
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    /* Reject rename of builtin functions */
    if (is_builtin_name(word)) {
        free(word);
        cJSON *resp = lsp_make_error(id, -32602, "Cannot rename a built-in function");
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    /* Return the range and placeholder text for the identifier */
    cJSON *result = cJSON_CreateObject();
    cJSON *range = cJSON_CreateObject();
    cJSON *start = cJSON_CreateObject();
    cJSON_AddNumberToObject(start, "line", line);
    cJSON_AddNumberToObject(start, "character", word_col);
    cJSON *end = cJSON_CreateObject();
    cJSON_AddNumberToObject(end, "line", line);
    cJSON_AddNumberToObject(end, "character", word_col + (int)strlen(word));
    cJSON_AddItemToObject(range, "start", start);
    cJSON_AddItemToObject(range, "end", end);
    cJSON_AddItemToObject(result, "range", range);
    cJSON_AddStringToObject(result, "placeholder", word);

    free(word);

    cJSON *resp = lsp_make_response(id, result);
    lsp_write_response(resp, stdout);
    cJSON_Delete(resp);
}

/* ── Handler: textDocument/rename ── */

static void handle_rename(LspServer *srv, cJSON *params, int id) {
    cJSON *td = cJSON_GetObjectItem(params, "textDocument");
    cJSON *pos = cJSON_GetObjectItem(params, "position");
    cJSON *new_name_node = cJSON_GetObjectItem(params, "newName");
    if (!td || !pos || !new_name_node) {
        cJSON *resp = lsp_make_error(id, -32602, "Invalid params");
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    const char *uri = cJSON_GetObjectItem(td, "uri")->valuestring;
    int line = cJSON_GetObjectItem(pos, "line")->valueint;
    int col = cJSON_GetObjectItem(pos, "character")->valueint;
    const char *new_name = new_name_node->valuestring;

    LspDocument *doc = find_document(srv, uri);
    if (!doc || !doc->text) {
        cJSON *resp = lsp_make_error(id, -32602, "Document not found");
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    char *word = extract_word_at(doc->text, line, col, NULL);
    if (!word) {
        cJSON *resp = lsp_make_error(id, -32602, "No identifier at position");
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    /* Reject rename of keywords and builtins */
    if (is_keyword(word) || is_builtin_name(word)) {
        free(word);
        cJSON *resp = lsp_make_error(id, -32602, "Cannot rename keywords or built-in functions");
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    /* Validate the new name is a valid identifier */
    if (!new_name || !*new_name || !is_ident_char(new_name[0]) || (new_name[0] >= '0' && new_name[0] <= '9')) {
        free(word);
        cJSON *resp = lsp_make_error(id, -32602, "Invalid identifier name");
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    /* Reject renaming to a keyword */
    if (is_keyword(new_name)) {
        free(word);
        cJSON *resp = lsp_make_error(id, -32602, "Cannot rename to a keyword");
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    /* Find all references and build TextEdit array */
    cJSON *locations = find_all_references(doc->text, uri, word);
    free(word);

    cJSON *edits = cJSON_CreateArray();
    int loc_count = cJSON_GetArraySize(locations);
    for (int i = 0; i < loc_count; i++) {
        cJSON *loc = cJSON_GetArrayItem(locations, i);
        cJSON *range = cJSON_GetObjectItem(loc, "range");

        cJSON *edit = cJSON_CreateObject();
        cJSON_AddItemToObject(edit, "range", cJSON_Duplicate(range, 1));
        cJSON_AddStringToObject(edit, "newText", new_name);
        cJSON_AddItemToArray(edits, edit);
    }
    cJSON_Delete(locations);

    /* Build WorkspaceEdit */
    cJSON *result = cJSON_CreateObject();
    cJSON *changes = cJSON_CreateObject();
    cJSON_AddItemToObject(changes, uri, edits);
    cJSON_AddItemToObject(result, "changes", changes);

    cJSON *resp = lsp_make_response(id, result);
    lsp_write_response(resp, stdout);
    cJSON_Delete(resp);
}

/* ── Handler: textDocument/formatting ── */

static void handle_formatting(LspServer *srv, cJSON *params, int id) {
    cJSON *td = cJSON_GetObjectItem(params, "textDocument");
    if (!td) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    const char *uri = cJSON_GetObjectItem(td, "uri")->valuestring;
    LspDocument *doc = find_document(srv, uri);
    if (!doc || !doc->text) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    /* Run the formatter on the document text */
    char *fmt_err = NULL;
    char *formatted = lat_format(doc->text, &fmt_err);
    if (!formatted) {
        /* Formatting failed — return null result (no edits) */
        free(fmt_err);
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    /* If already formatted, return empty edits array */
    if (strcmp(doc->text, formatted) == 0) {
        free(formatted);
        cJSON *resp = lsp_make_response(id, cJSON_CreateArray());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    /* Count lines in original document to build the full-document range */
    int last_line = 0;
    int last_char = 0;
    for (const char *p = doc->text; *p; p++) {
        if (*p == '\n') {
            last_line++;
            last_char = 0;
        } else {
            last_char++;
        }
    }

    /* Build a single TextEdit replacing the entire document */
    cJSON *edit = cJSON_CreateObject();

    cJSON *range = cJSON_CreateObject();
    cJSON *start = cJSON_CreateObject();
    cJSON_AddNumberToObject(start, "line", 0);
    cJSON_AddNumberToObject(start, "character", 0);
    cJSON *end = cJSON_CreateObject();
    cJSON_AddNumberToObject(end, "line", last_line);
    cJSON_AddNumberToObject(end, "character", last_char);
    cJSON_AddItemToObject(range, "start", start);
    cJSON_AddItemToObject(range, "end", end);
    cJSON_AddItemToObject(edit, "range", range);

    cJSON_AddStringToObject(edit, "newText", formatted);
    free(formatted);

    cJSON *edits = cJSON_CreateArray();
    cJSON_AddItemToArray(edits, edit);

    cJSON *resp = lsp_make_response(id, edits);
    lsp_write_response(resp, stdout);
    cJSON_Delete(resp);
}

/* ── Handler: textDocument/codeAction ── */

/* Levenshtein distance between two strings (simple O(m*n) implementation) */
static int levenshtein(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la == 0) return (int)lb;
    if (lb == 0) return (int)la;

    /* Use a single-row DP approach to save memory */
    int *row = malloc((lb + 1) * sizeof(int));
    if (!row) return (int)(la > lb ? la : lb);
    for (size_t j = 0; j <= lb; j++) row[j] = (int)j;

    for (size_t i = 1; i <= la; i++) {
        int prev = row[0];
        row[0] = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del = row[j] + 1;
            int ins = row[j - 1] + 1;
            int sub = prev + cost;
            prev = row[j];
            int min_val = del < ins ? del : ins;
            row[j] = min_val < sub ? min_val : sub;
        }
    }
    int result = row[lb];
    free(row);
    return result;
}

/* Collect all identifier-like names from document text.
 * Returns a malloc'd array of malloc'd strings, sets *out_count.
 * Caller must free each string and the array. */
static char **collect_identifiers(const char *text, size_t *out_count) {
    size_t cap = 64, count = 0;
    char **names = malloc(cap * sizeof(char *));
    if (!names) {
        *out_count = 0;
        return NULL;
    }

    const char *p = text;
    while (*p) {
        if (is_ident_char(*p) && !(p > text && is_ident_char(p[-1]))) {
            const char *start = p;
            while (*p && is_ident_char(*p)) p++;
            size_t len = (size_t)(p - start);

            /* Skip very short identifiers and digits-only */
            if (len >= 2 && !(start[0] >= '0' && start[0] <= '9')) {
                /* Check for duplicates (simple linear scan) */
                int dup = 0;
                for (size_t i = 0; i < count; i++) {
                    if (strlen(names[i]) == len && memcmp(names[i], start, len) == 0) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    if (count >= cap) {
                        cap *= 2;
                        names = realloc(names, cap * sizeof(char *));
                        if (!names) {
                            *out_count = 0;
                            return NULL;
                        }
                    }
                    names[count] = malloc(len + 1);
                    if (names[count]) {
                        memcpy(names[count], start, len);
                        names[count][len] = '\0';
                        count++;
                    }
                }
            }
            continue;
        }
        p++;
    }
    *out_count = count;
    return names;
}

/* Find the closest matching identifier to 'name' in the document.
 * Returns a malloc'd string, or NULL if no close match found.
 * Only returns matches with distance <= max_dist. */
static char *find_closest_identifier(const char *text, const char *name, int max_dist) {
    size_t id_count = 0;
    char **ids = collect_identifiers(text, &id_count);
    if (!ids) return NULL;

    char *best = NULL;
    int best_dist = max_dist + 1;

    for (size_t i = 0; i < id_count; i++) {
        if (strcmp(ids[i], name) == 0) { /* skip exact match */
            free(ids[i]);
            continue;
        }
        int d = levenshtein(name, ids[i]);
        if (d < best_dist) {
            best_dist = d;
            free(best);
            best = strdup(ids[i]);
        }
        free(ids[i]);
    }
    free(ids);

    if (best_dist > max_dist) {
        free(best);
        return NULL;
    }
    return best;
}

/* Get the text of a specific line (0-based). Returns pointer into text, sets *line_len. */
static const char *get_line_text(const char *text, int target_line, size_t *line_len) {
    const char *p = text;
    int cur = 0;
    while (cur < target_line && *p) {
        if (*p == '\n') cur++;
        p++;
    }
    const char *start = p;
    while (*p && *p != '\n') p++;
    *line_len = (size_t)(p - start);
    return start;
}

/* Count total number of lines in text (0-based last line index + 1) */
static int count_lines(const char *text) {
    int lines = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') lines++;
    }
    return lines;
}

/* Create a TextEdit JSON object */
static cJSON *make_text_edit(int start_line, int start_col, int end_line, int end_col, const char *new_text) {
    cJSON *edit = cJSON_CreateObject();
    cJSON *range = cJSON_CreateObject();
    cJSON *start = cJSON_CreateObject();
    cJSON_AddNumberToObject(start, "line", start_line);
    cJSON_AddNumberToObject(start, "character", start_col);
    cJSON *end = cJSON_CreateObject();
    cJSON_AddNumberToObject(end, "line", end_line);
    cJSON_AddNumberToObject(end, "character", end_col);
    cJSON_AddItemToObject(range, "start", start);
    cJSON_AddItemToObject(range, "end", end);
    cJSON_AddItemToObject(edit, "range", range);
    cJSON_AddStringToObject(edit, "newText", new_text);
    return edit;
}

/* Create a CodeAction JSON object with a workspace edit */
static cJSON *make_code_action(const char *title, const char *uri, cJSON *diagnostic, cJSON *text_edit) {
    cJSON *action = cJSON_CreateObject();
    cJSON_AddStringToObject(action, "title", title);
    cJSON_AddStringToObject(action, "kind", "quickfix");

    if (diagnostic) {
        cJSON *diags = cJSON_CreateArray();
        cJSON_AddItemToArray(diags, cJSON_Duplicate(diagnostic, 1));
        cJSON_AddItemToObject(action, "diagnostics", diags);
    }

    cJSON *workspace_edit = cJSON_CreateObject();
    cJSON *changes = cJSON_CreateObject();
    cJSON *edits = cJSON_CreateArray();
    cJSON_AddItemToArray(edits, text_edit);
    cJSON_AddItemToObject(changes, uri, edits);
    cJSON_AddItemToObject(workspace_edit, "changes", changes);
    cJSON_AddItemToObject(action, "edit", workspace_edit);

    return action;
}

static void handle_code_action(LspServer *srv, cJSON *params, int id) {
    cJSON *td = cJSON_GetObjectItem(params, "textDocument");
    cJSON *context = cJSON_GetObjectItem(params, "context");
    cJSON *range_node = cJSON_GetObjectItem(params, "range");
    if (!td || !context) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateArray());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    const char *uri = cJSON_GetObjectItem(td, "uri")->valuestring;
    LspDocument *doc = find_document(srv, uri);
    cJSON *actions = cJSON_CreateArray();

    if (!doc || !doc->text) {
        cJSON *resp = lsp_make_response(id, actions);
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    /* Get the range from the request */
    int range_start_line = 0, range_end_line = 0;
    if (range_node) {
        cJSON *rs = cJSON_GetObjectItem(range_node, "start");
        cJSON *re = cJSON_GetObjectItem(range_node, "end");
        if (rs) range_start_line = cJSON_GetObjectItem(rs, "line")->valueint;
        if (re) range_end_line = cJSON_GetObjectItem(re, "line")->valueint;
    }

    cJSON *diagnostics = cJSON_GetObjectItem(context, "diagnostics");
    int diag_count = diagnostics ? cJSON_GetArraySize(diagnostics) : 0;

    for (int i = 0; i < diag_count; i++) {
        cJSON *diag = cJSON_GetArrayItem(diagnostics, i);
        cJSON *msg_node = cJSON_GetObjectItem(diag, "message");
        if (!msg_node || !msg_node->valuestring) continue;
        const char *msg = msg_node->valuestring;

        cJSON *diag_range = cJSON_GetObjectItem(diag, "range");
        int diag_line = 0;
        if (diag_range) {
            cJSON *ds = cJSON_GetObjectItem(diag_range, "start");
            if (ds) diag_line = cJSON_GetObjectItem(ds, "line")->valueint;
        }

        /* ── Code action: phase violation → add thaw() ── */
        if (strstr(msg, "cannot mutate") && strstr(msg, "crystal")) {
            /* Extract the variable name from the diagnostic line */
            size_t line_len = 0;
            const char *line_text = get_line_text(doc->text, diag_line, &line_len);

            /* Find the assignment target on this line: look for identifiers before '=' */
            const char *eq = NULL;
            for (const char *p = line_text; p < line_text + line_len; p++) {
                if (*p == '=' && (p + 1 >= line_text + line_len || p[1] != '=')) {
                    eq = p;
                    break;
                }
            }

            if (eq) {
                /* Extract identifier before '=' */
                const char *id_end = eq;
                while (id_end > line_text && (id_end[-1] == ' ' || id_end[-1] == '\t')) id_end--;
                const char *id_start = id_end;
                while (id_start > line_text && is_ident_char(id_start[-1])) id_start--;

                if (id_start < id_end) {
                    size_t vlen = (size_t)(id_end - id_start);
                    char *var_name = malloc(vlen + 1);
                    if (var_name) {
                        memcpy(var_name, id_start, vlen);
                        var_name[vlen] = '\0';

                        /* Build "thaw(var_name)\n" to insert before the offending line */
                        size_t new_len = strlen(var_name) + 16;
                        char *new_text = malloc(new_len);
                        if (new_text) {
                            /* Calculate indentation of the current line */
                            const char *indent_end = line_text;
                            while (indent_end < line_text + line_len && (*indent_end == ' ' || *indent_end == '\t'))
                                indent_end++;
                            size_t indent_len = (size_t)(indent_end - line_text);
                            char *indent = malloc(indent_len + 1);
                            if (indent) {
                                memcpy(indent, line_text, indent_len);
                                indent[indent_len] = '\0';
                                free(new_text);
                                new_len = indent_len + strlen(var_name) + 32;
                                new_text = malloc(new_len);
                                if (new_text) {
                                    snprintf(new_text, new_len, "%sthaw(%s)\n", indent, var_name);
                                    cJSON *text_edit = make_text_edit(diag_line, 0, diag_line, 0, new_text);
                                    cJSON *action =
                                        make_code_action("Add thaw() to make mutable", uri, diag, text_edit);
                                    cJSON_AddItemToArray(actions, action);
                                }
                                free(indent);
                            }
                            free(new_text);
                        }
                        free(var_name);
                    }
                }
            }
        }

        /* ── Code action: unknown identifier → suggest closest match ── */
        if (strstr(msg, "Undefined variable") || strstr(msg, "undefined variable") ||
            strstr(msg, "Undeclared identifier") || strstr(msg, "undeclared identifier") ||
            strstr(msg, "not defined") || strstr(msg, "Unknown identifier") || strstr(msg, "unknown identifier")) {
            /* Try to extract the identifier name from the error message.
             * Common patterns: "Undefined variable 'foo'" or "Undefined variable `foo`" */
            const char *quote = strchr(msg, '\'');
            if (!quote) quote = strchr(msg, '`');
            if (quote) {
                const char *name_start = quote + 1;
                char end_quote = (*quote == '\'') ? '\'' : '`';
                const char *name_end = strchr(name_start, end_quote);
                if (name_end && name_end > name_start) {
                    size_t nlen = (size_t)(name_end - name_start);
                    char *bad_name = malloc(nlen + 1);
                    if (bad_name) {
                        memcpy(bad_name, name_start, nlen);
                        bad_name[nlen] = '\0';

                        /* Find closest identifier in document */
                        int max_dist = (int)nlen / 3;
                        if (max_dist < 2) max_dist = 2;
                        char *suggestion = find_closest_identifier(doc->text, bad_name, max_dist);
                        if (suggestion) {
                            /* Build title */
                            size_t title_len = strlen(suggestion) + 32;
                            char *title = malloc(title_len);
                            if (title) {
                                snprintf(title, title_len, "Did you mean '%s'?", suggestion);

                                /* Get the range of the bad identifier from the diagnostic */
                                int start_col = 0, end_col = 0;
                                if (diag_range) {
                                    cJSON *ds = cJSON_GetObjectItem(diag_range, "start");
                                    cJSON *de = cJSON_GetObjectItem(diag_range, "end");
                                    if (ds) start_col = cJSON_GetObjectItem(ds, "character")->valueint;
                                    if (de) end_col = cJSON_GetObjectItem(de, "character")->valueint;
                                }
                                /* If range is zero-width, use the bad_name length */
                                if (end_col <= start_col) end_col = start_col + (int)nlen;

                                cJSON *text_edit = make_text_edit(diag_line, start_col, diag_line, end_col, suggestion);
                                cJSON *action = make_code_action(title, uri, diag, text_edit);
                                cJSON_AddItemToArray(actions, action);
                                free(title);
                            }
                            free(suggestion);
                        }
                        free(bad_name);
                    }
                }
            }
        }

        /* ── Code action: wrap in try/catch ── */
        if (strstr(msg, "uncaught") || strstr(msg, "Uncaught") || strstr(msg, "unhandled error") ||
            strstr(msg, "Unhandled error") || strstr(msg, "throw") || strstr(msg, "exception")) {
            /* Wrap the line range in try/catch */
            size_t line_len = 0;
            const char *line_text = get_line_text(doc->text, diag_line, &line_len);

            /* Calculate indentation */
            const char *indent_end = line_text;
            while (indent_end < line_text + line_len && (*indent_end == ' ' || *indent_end == '\t')) indent_end++;
            size_t indent_len = (size_t)(indent_end - line_text);
            char *indent = malloc(indent_len + 1);
            if (!indent) continue;
            memcpy(indent, line_text, indent_len);
            indent[indent_len] = '\0';

            /* Get the original line content (trimmed of leading whitespace) */
            char *original = malloc(line_len - indent_len + 1);
            if (!original) {
                free(indent);
                continue;
            }
            memcpy(original, indent_end, line_len - indent_len);
            original[line_len - indent_len] = '\0';

            /* Build try/catch wrapper */
            size_t new_len = indent_len * 3 + strlen(original) + 64;
            char *new_text = malloc(new_len);
            if (new_text) {
                snprintf(new_text, new_len, "%stry {\n%s  %s\n%s} catch (e) {\n%s  // handle error\n%s}", indent,
                         indent, original, indent, indent, indent);

                /* Determine the end of the line (including newline if present) */
                int end_line = diag_line;
                int end_col = (int)line_len;

                cJSON *text_edit = make_text_edit(diag_line, 0, end_line, end_col, new_text);
                cJSON *action = make_code_action("Wrap in try/catch", uri, diag, text_edit);
                cJSON_AddItemToArray(actions, action);
                free(new_text);
            }
            free(indent);
            free(original);
        }

        /* ── Code action: add missing import ── */
        if (strstr(msg, "module") && (strstr(msg, "not found") || strstr(msg, "could not find"))) {
            /* Try to extract module name from the error */
            const char *quote = strchr(msg, '\'');
            if (!quote) quote = strchr(msg, '`');
            if (quote) {
                const char *name_start = quote + 1;
                char end_quote = (*quote == '\'') ? '\'' : '`';
                const char *name_end = strchr(name_start, end_quote);
                if (name_end && name_end > name_start) {
                    size_t mlen = (size_t)(name_end - name_start);
                    char *mod_name = malloc(mlen + 1);
                    if (mod_name) {
                        memcpy(mod_name, name_start, mlen);
                        mod_name[mlen] = '\0';

                        /* Build import statement to insert at top of file */
                        size_t imp_len = mlen + 32;
                        char *import_text = malloc(imp_len);
                        if (import_text) {
                            snprintf(import_text, imp_len, "import \"%s\"\n", mod_name);

                            size_t title_len = mlen + 32;
                            char *title = malloc(title_len);
                            if (title) {
                                snprintf(title, title_len, "Add import for '%s'", mod_name);

                                cJSON *text_edit = make_text_edit(0, 0, 0, 0, import_text);
                                cJSON *action = make_code_action(title, uri, diag, text_edit);
                                cJSON_AddItemToArray(actions, action);
                                free(title);
                            }
                            free(import_text);
                        }
                        free(mod_name);
                    }
                }
            }
        }
    }

    (void)range_start_line;
    (void)range_end_line;
    (void)count_lines;

    cJSON *resp = lsp_make_response(id, actions);
    lsp_write_response(resp, stdout);
    cJSON_Delete(resp);
}

/* ── Server lifecycle ── */

LspServer *lsp_server_new(void) {
    LspServer *srv = calloc(1, sizeof(LspServer));
    if (!srv) return NULL;
    srv->log = stderr;
    return srv;
}

void lsp_server_free(LspServer *srv) {
    if (!srv) return;
    for (size_t i = 0; i < srv->doc_count; i++) lsp_document_free(srv->documents[i]);
    free(srv->documents);
    lsp_symbol_index_free(srv->index);
    free(srv);
}

/* Main message loop */
void lsp_server_run(LspServer *srv) {
    while (!srv->shutdown) {
        cJSON *msg = lsp_read_message(stdin);
        if (!msg) break; /* EOF */

        cJSON *method_node = cJSON_GetObjectItem(msg, "method");
        cJSON *id_node = cJSON_GetObjectItem(msg, "id");
        cJSON *params_node = cJSON_GetObjectItem(msg, "params");

        const char *method = method_node ? method_node->valuestring : NULL;
        int id = id_node ? id_node->valueint : -1;

        if (!method) {
            cJSON_Delete(msg);
            continue;
        }

        if (strcmp(method, "initialize") == 0) {
            handle_initialize(srv, id);
        } else if (strcmp(method, "initialized") == 0) {
            /* No-op */
        } else if (strcmp(method, "textDocument/didOpen") == 0) {
            handle_did_open(srv, params_node);
        } else if (strcmp(method, "textDocument/didChange") == 0) {
            handle_did_change(srv, params_node);
        } else if (strcmp(method, "textDocument/didClose") == 0) {
            handle_did_close(srv, params_node);
        } else if (strcmp(method, "textDocument/completion") == 0) {
            handle_completion(srv, params_node, id);
        } else if (strcmp(method, "textDocument/hover") == 0) {
            handle_hover(srv, params_node, id);
        } else if (strcmp(method, "textDocument/definition") == 0) {
            handle_definition(srv, params_node, id);
        } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
            handle_document_symbol(srv, params_node, id);
        } else if (strcmp(method, "textDocument/signatureHelp") == 0) {
            handle_signature_help(srv, params_node, id);
        } else if (strcmp(method, "textDocument/references") == 0) {
            handle_references(srv, params_node, id);
        } else if (strcmp(method, "textDocument/prepareRename") == 0) {
            handle_prepare_rename(srv, params_node, id);
        } else if (strcmp(method, "textDocument/rename") == 0) {
            handle_rename(srv, params_node, id);
        } else if (strcmp(method, "textDocument/formatting") == 0) {
            handle_formatting(srv, params_node, id);
        } else if (strcmp(method, "textDocument/codeAction") == 0) {
            handle_code_action(srv, params_node, id);
        } else if (strcmp(method, "shutdown") == 0) {
            cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
            lsp_write_response(resp, stdout);
            cJSON_Delete(resp);
            srv->shutdown = true;
        } else if (strcmp(method, "exit") == 0) {
            cJSON_Delete(msg);
            break;
        } else if (id >= 0) {
            /* Unknown request — return method not found */
            cJSON *resp = lsp_make_error(id, -32601, "Method not found");
            lsp_write_response(resp, stdout);
            cJSON_Delete(resp);
        }

        cJSON_Delete(msg);
    }
}
