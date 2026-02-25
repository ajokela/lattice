#include "lsp.h"
#include "../vendor/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Keywords for completion ── */

static const char *lattice_keywords[] = {
    "fn", "let", "flux", "fix", "struct", "enum", "trait", "impl",
    "if", "else", "for", "while", "in", "match", "return", "break",
    "continue", "import", "from", "as", "try", "catch", "throw",
    "true", "false", "nil", "print", "scope", "defer", "select",
    "test", "require", "ensure", "freeze", "thaw", "clone",
    NULL
};

/* ── Identifier character helpers ── */

static int is_ident_char(char c) {
    return (c == '_' ||
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9'));
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
    while (ws > line_start && is_ident_char(ws[-1]))
        ws--;

    const char *we = p;
    while (*we && is_ident_char(*we))
        we++;

    if (we <= ws) return NULL;

    size_t wlen = (size_t)(we - ws);
    char *word = malloc(wlen + 1);
    memcpy(word, ws, wlen);
    word[wlen] = '\0';
    if (out_col) *out_col = (int)(ws - line_start);
    return word;
}

/* ── Document management ── */

static LspDocument *find_document(LspServer *srv, const char *uri) {
    for (size_t i = 0; i < srv->doc_count; i++) {
        if (strcmp(srv->documents[i]->uri, uri) == 0)
            return srv->documents[i];
    }
    return NULL;
}

static LspDocument *add_document(LspServer *srv, const char *uri,
                                  const char *text, int version) {
    LspDocument *doc = calloc(1, sizeof(LspDocument));
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
    cJSON_AddNumberToObject(textDocSync, "change", 1);  /* Full sync */
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

    /* Rename */
    cJSON_AddBoolToObject(caps, "renameProvider", 1);

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
    while (start > line_start && is_ident_char(start[-1]))
        start--;
    if (start >= end) return NULL;
    size_t len = (size_t)(end - start);
    char *id = malloc(len + 1);
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
    if (line_start[eff_col - 1] != ':' || line_start[eff_col - 2] != ':')
        return NULL;
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
static char *infer_variable_type(const char *text, const char *var_name,
                                  int use_line, const LspDocument *doc) {
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
            if (strncmp(scan, "let ", 4) == 0) { is_decl = true; after_kw = scan + 4; }
            else if (strncmp(scan, "flux ", 5) == 0) { is_decl = true; after_kw = scan + 5; }
            else if (strncmp(scan, "fix ", 4) == 0) { is_decl = true; after_kw = scan + 4; }

            if (is_decl && after_kw) {
                while (after_kw < p && (*after_kw == ' ' || *after_kw == '\t')) after_kw++;
                size_t vlen = strlen(var_name);
                if (strncmp(after_kw, var_name, vlen) == 0) {
                    char ch = after_kw[vlen];
                    if (ch == ' ' || ch == ':' || ch == '=' || ch == '\t' ||
                        ch == '\n' || ch == '\r' || ch == '\0') {
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
                        while (search < close && (*search == ' ' || *search == ','))
                            search++;
                        if (search + vlen <= close &&
                            strncmp(search, var_name, vlen) == 0) {
                            char nc = search[vlen];
                            if (nc == ':' || nc == ' ' || nc == ',' || nc == ')') {
                                const char *colon = search + vlen;
                                while (colon < close && *colon == ' ') colon++;
                                if (*colon == ':') {
                                    colon++;
                                    while (colon < close && *colon == ' ') colon++;
                                    const char *tstart = colon;
                                    while (colon < close && *colon != ',' && *colon != ')' &&
                                           *colon != ' ')
                                        colon++;
                                    if (colon > tstart) {
                                        size_t tlen = (size_t)(colon - tstart);
                                        char *type = malloc(tlen + 1);
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
                while (after_for < p && (*after_for == ' ' || *after_for == '\t'))
                    after_for++;
                size_t vlen = strlen(var_name);
                if (strncmp(after_for, var_name, vlen) == 0) {
                    char nc = after_for[vlen];
                    if (nc == ' ' || nc == '\t') {
                        /* for-in loop variable - can't easily infer element type */
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
        while (*q && *q != '=' && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r')
            q++;
        if (q > type_start) {
            size_t tlen = (size_t)(q - type_start);
            char *type = malloc(tlen + 1);
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
            while (*num && ((*num >= '0' && *num <= '9') || *num == '.' || *num == '_'))
                num++;
            const char *dot = strchr(q, '.');
            if (dot && dot < num) return strdup("Float");
            return strdup("Int");
        }

        /* Boolean literal */
        if (strncmp(q, "true", 4) == 0 || strncmp(q, "false", 5) == 0)
            return strdup("Bool");

        /* Type::new() constructors */
        if (strncmp(q, "Map::new", 8) == 0) return strdup("Map");
        if (strncmp(q, "Set::new", 8) == 0 || strncmp(q, "Set::from", 9) == 0)
            return strdup("Set");
        if (strncmp(q, "Buffer::new", 11) == 0 ||
            strncmp(q, "Buffer::from", 12) == 0)
            return strdup("Buffer");
        if (strncmp(q, "Channel::new", 12) == 0) return strdup("Channel");
        if (strncmp(q, "Ref::new", 8) == 0) return strdup("Ref");

        /* Struct constructor: StructName { ... } or StructName::new(...) */
        if (*q >= 'A' && *q <= 'Z') {
            const char *id_start = q;
            while (*q && is_ident_char(*q)) q++;
            size_t id_len = (size_t)(q - id_start);
            /* Skip whitespace */
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '{' || (*q == ':' && *(q+1) == ':')) {
                /* Check if this is a known struct name */
                char *name = malloc(id_len + 1);
                memcpy(name, id_start, id_len);
                name[id_len] = '\0';
                if (doc) {
                    for (size_t i = 0; i < doc->struct_def_count; i++) {
                        if (strcmp(doc->struct_defs[i].name, name) == 0)
                            return name;  /* confirmed struct type */
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
static void add_method_completions(cJSON *items, const LspSymbolIndex *idx,
                                    const char *type_name) {
    if (!idx) return;
    for (size_t i = 0; i < idx->method_count; i++) {
        if (idx->methods[i].owner_type &&
            strcmp(idx->methods[i].owner_type, type_name) == 0) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "label", idx->methods[i].name);
            cJSON_AddNumberToObject(item, "kind", 2);  /* Method */
            if (idx->methods[i].signature)
                cJSON_AddStringToObject(item, "detail", idx->methods[i].signature);
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
static void add_struct_field_completions(cJSON *items, const LspDocument *doc,
                                          const char *struct_name) {
    for (size_t i = 0; i < doc->struct_def_count; i++) {
        if (strcmp(doc->struct_defs[i].name, struct_name) == 0) {
            for (size_t j = 0; j < doc->struct_defs[i].field_count; j++) {
                LspFieldInfo *f = &doc->struct_defs[i].fields[j];
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "label", f->name);
                cJSON_AddNumberToObject(item, "kind", 5);  /* Field */
                if (f->type_name)
                    cJSON_AddStringToObject(item, "detail", f->type_name);
                cJSON_AddItemToArray(items, item);
            }
            break;
        }
    }
}

/* Add enum variant completions */
static void add_enum_variant_completions(cJSON *items, const LspDocument *doc,
                                          const char *enum_name) {
    for (size_t i = 0; i < doc->enum_def_count; i++) {
        if (strcmp(doc->enum_defs[i].name, enum_name) == 0) {
            for (size_t j = 0; j < doc->enum_defs[i].variant_count; j++) {
                LspVariantInfo *v = &doc->enum_defs[i].variants[j];
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "label", v->name);
                cJSON_AddNumberToObject(item, "kind", 20);  /* EnumMember */
                if (v->params) {
                    char detail[256];
                    snprintf(detail, sizeof(detail), "%s::%s%s",
                             enum_name, v->name, v->params);
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
        if (strcmp(doc->enum_defs[i].name, name) == 0)
            return true;
    }
    return false;
}

/* Check if a name corresponds to a known struct in the document */
static bool is_struct_name(const LspDocument *doc, const char *name) {
    for (size_t i = 0; i < doc->struct_def_count; i++) {
        if (strcmp(doc->struct_defs[i].name, name) == 0)
            return true;
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
                if (is_struct_name(doc, type)) {
                    add_struct_field_completions(items, doc, type);
                }
                /* Always add methods for the inferred type */
                add_method_completions(items, srv->index, type);
                free(type);
            } else {
                /* Could not infer type — check if the name itself is a builtin type
                 * by looking at the first character (capitalized = could be type) */
                if (dot_obj[0] >= 'A' && dot_obj[0] <= 'Z') {
                    /* Might be a struct variable with capital name */
                    if (is_struct_name(doc, dot_obj)) {
                        add_struct_field_completions(items, doc, dot_obj);
                    }
                }
                /* Fall back: suggest all methods */
                if (srv->index) {
                    for (size_t i = 0; i < srv->index->method_count; i++) {
                        cJSON *item = cJSON_CreateObject();
                        cJSON_AddStringToObject(item, "label",
                            srv->index->methods[i].name);
                        cJSON_AddNumberToObject(item, "kind", 2);  /* Method */
                        if (srv->index->methods[i].signature)
                            cJSON_AddStringToObject(item, "detail",
                                srv->index->methods[i].signature);
                        if (srv->index->methods[i].doc) {
                            cJSON *doc_obj = cJSON_CreateObject();
                            cJSON_AddStringToObject(doc_obj, "kind", "markdown");
                            cJSON_AddStringToObject(doc_obj, "value",
                                srv->index->methods[i].doc);
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
            if (is_enum_name(doc, path_obj)) {
                add_enum_variant_completions(items, doc, path_obj);
            }
            /* Also check builtin constructors: Map::, Set::, Buffer::, etc. */
            if (srv->index) {
                for (size_t i = 0; i < srv->index->builtin_count; i++) {
                    /* Check if signature starts with "Name::" */
                    const char *sig = srv->index->builtins[i].signature;
                    if (sig) {
                        size_t plen = strlen(path_obj);
                        if (strncmp(sig, path_obj, plen) == 0 &&
                            sig[plen] == ':' && sig[plen + 1] == ':') {
                            /* Extract the method name after :: */
                            const char *method_start = sig + plen + 2;
                            const char *paren = strchr(method_start, '(');
                            size_t mlen = paren ? (size_t)(paren - method_start)
                                                : strlen(method_start);
                            char *mname = malloc(mlen + 1);
                            memcpy(mname, method_start, mlen);
                            mname[mlen] = '\0';

                            cJSON *item = cJSON_CreateObject();
                            cJSON_AddStringToObject(item, "label", mname);
                            cJSON_AddNumberToObject(item, "kind", 3); /* Function */
                            cJSON_AddStringToObject(item, "detail", sig);
                            if (srv->index->builtins[i].doc) {
                                cJSON *doc_obj = cJSON_CreateObject();
                                cJSON_AddStringToObject(doc_obj, "kind", "markdown");
                                cJSON_AddStringToObject(doc_obj, "value",
                                    srv->index->builtins[i].doc);
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
            cJSON_AddNumberToObject(item, "kind", 3);  /* Function */
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
                doc->symbols[i].kind == LSP_SYM_FUNCTION ? 3 :
                doc->symbols[i].kind == LSP_SYM_STRUCT   ? 22 :
                doc->symbols[i].kind == LSP_SYM_ENUM     ? 13 : 6);
            if (doc->symbols[i].signature)
                cJSON_AddStringToObject(item, "detail", doc->symbols[i].signature);
            cJSON_AddItemToArray(items, item);
        }
    }

    cJSON *resp = lsp_make_response(id, items);
    lsp_write_response(resp, stdout);
    cJSON_Delete(resp);
}

/* ── Handler: textDocument/hover ── */

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

    /* Find the word at line:col */
    const char *p = doc->text;
    int cur_line = 0;
    while (cur_line < line && *p) {
        if (*p == '\n') cur_line++;
        p++;
    }
    /* p now points to start of target line */
    const char *line_start = p;
    int cur_col = 0;
    while (cur_col < col && *p && *p != '\n') {
        cur_col++;
        p++;
    }

    /* Extract identifier */
    const char *word_start = p;
    while (word_start > line_start &&
           (word_start[-1] == '_' ||
            (word_start[-1] >= 'a' && word_start[-1] <= 'z') ||
            (word_start[-1] >= 'A' && word_start[-1] <= 'Z') ||
            (word_start[-1] >= '0' && word_start[-1] <= '9')))
        word_start--;

    const char *word_end = p;
    while (*word_end && (*word_end == '_' ||
           (*word_end >= 'a' && word_end[0] <= 'z') ||
           (*word_end >= 'A' && word_end[0] <= 'Z') ||
           (*word_end >= '0' && word_end[0] <= '9')))
        word_end++;

    if (word_end <= word_start) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    size_t word_len = (size_t)(word_end - word_start);
    char *word = malloc(word_len + 1);
    memcpy(word, word_start, word_len);
    word[word_len] = '\0';

    const char *hover_text = NULL;
    char *hover_buf = NULL;  /* dynamically built hover, freed at end */

    /* Priority 1: Document symbols (user-defined names take precedence) */
    if (doc) {
        for (size_t i = 0; i < doc->symbol_count; i++) {
            if (strcmp(doc->symbols[i].name, word) == 0) {
                if (doc->symbols[i].kind == LSP_SYM_VARIABLE) {
                    /* For variables, try to include inferred type */
                    char *type = doc->text ?
                        infer_variable_type(doc->text, word, line, doc) : NULL;
                    const char *sig = doc->symbols[i].signature;
                    if (type) {
                        size_t blen = (sig ? strlen(sig) : 0) + strlen(type) + 48;
                        hover_buf = malloc(blen);
                        snprintf(hover_buf, blen, "```lattice\n%s%s%s\n```",
                                 sig ? sig : word,
                                 /* Append type only if sig doesn't already have it */
                                 (sig && strchr(sig, ':')) ? "" : ": ",
                                 (sig && strchr(sig, ':')) ? "" : type);
                        hover_text = hover_buf;
                        free(type);
                    } else {
                        /* No inferred type — show signature as code block */
                        if (sig) {
                            size_t blen = strlen(sig) + 24;
                            hover_buf = malloc(blen);
                            snprintf(hover_buf, blen, "```lattice\n%s\n```", sig);
                            hover_text = hover_buf;
                        }
                    }
                } else {
                    /* Functions, structs, enums, traits: show signature as code block */
                    const char *sig = doc->symbols[i].signature;
                    if (sig) {
                        size_t blen = strlen(sig) + 24;
                        hover_buf = malloc(blen);
                        snprintf(hover_buf, blen, "```lattice\n%s\n```", sig);
                        hover_text = hover_buf;
                    }
                    if (doc->symbols[i].doc)
                        hover_text = doc->symbols[i].doc;
                }
                break;
            }
        }
    }

    /* Priority 2: Builtin functions */
    if (!hover_text && srv->index) {
        for (size_t i = 0; i < srv->index->builtin_count; i++) {
            if (strcmp(srv->index->builtins[i].name, word) == 0) {
                hover_text = srv->index->builtins[i].doc;
                break;
            }
        }
    }

    /* Priority 3: Methods (only if not found as a document symbol) */
    if (!hover_text && srv->index) {
        for (size_t i = 0; i < srv->index->method_count; i++) {
            if (strcmp(srv->index->methods[i].name, word) == 0) {
                hover_text = srv->index->methods[i].doc;
                break;
            }
        }
    }

    /* Priority 4: Try to infer type for identifiers not in symbol table */
    if (!hover_text && doc && doc->text) {
        char *type = infer_variable_type(doc->text, word, line, doc);
        if (type) {
            size_t blen = strlen(word) + strlen(type) + 32;
            hover_buf = malloc(blen);
            snprintf(hover_buf, blen, "```lattice\n%s: %s\n```", word, type);
            hover_text = hover_buf;
            free(type);
        }
    }

    /* Priority 5: Keyword documentation */
    if (!hover_text) {
        for (int i = 0; lattice_keywords[i]; i++) {
            if (strcmp(lattice_keywords[i], word) == 0) {
                size_t blen = strlen(word) + 32;
                hover_buf = malloc(blen);
                snprintf(hover_buf, blen, "*keyword* `%s`", word);
                hover_text = hover_buf;
                break;
            }
        }
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
    while (ws > line_start &&
           (ws[-1] == '_' ||
            (ws[-1] >= 'a' && ws[-1] <= 'z') ||
            (ws[-1] >= 'A' && ws[-1] <= 'Z') ||
            (ws[-1] >= '0' && ws[-1] <= '9')))
        ws--;

    const char *we = p;
    while (*we && (*we == '_' ||
           (*we >= 'a' && *we <= 'z') ||
           (*we >= 'A' && *we <= 'Z') ||
           (*we >= '0' && *we <= '9')))
        we++;

    if (we <= ws) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    size_t wlen = (size_t)(we - ws);
    char *word = malloc(wlen + 1);
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
            cJSON_AddNumberToObject(end_pos, "character",
                doc->symbols[i].col + (int)strlen(doc->symbols[i].name));
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
            if (strncmp(tp, "let ", 4) == 0) { kw = tp; kw_len = 4; }
            else if (strncmp(tp, "flux ", 5) == 0) { kw = tp; kw_len = 5; }
            else if (strncmp(tp, "fix ", 4) == 0) { kw = tp; kw_len = 4; }
            else if (strncmp(tp, "for ", 4) == 0) { kw = tp; kw_len = 4; }

            if (kw) {
                const char *after = kw + kw_len;
                while (*after == ' ' || *after == '\t') after++;
                if (strncmp(after, word, wlen2) == 0) {
                    char nc = after[wlen2];
                    if (nc == ' ' || nc == ':' || nc == '=' || nc == '\t' ||
                        nc == '\n' || nc == '\r' || nc == '\0' || nc == ',') {
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
                        while (search < close && (*search == ' ' || *search == ','))
                            search++;
                        if ((size_t)(close - search) >= wlen2 &&
                            strncmp(search, word, wlen2) == 0) {
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
            if (*tp == '\n') { tp++; scan_line++; }
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
        case LSP_SYM_FUNCTION: return 12;  /* Function */
        case LSP_SYM_STRUCT:   return 23;  /* Struct */
        case LSP_SYM_ENUM:     return 10;  /* Enum */
        case LSP_SYM_VARIABLE: return 13;  /* Variable */
        case LSP_SYM_METHOD:   return 6;   /* Method */
        default:               return 13;  /* Variable as fallback */
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
            cJSON_AddNumberToObject(end, "character",
                (s->col >= 0 ? s->col : 0) + (int)strlen(s->name));
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
            cJSON_AddNumberToObject(sel_end, "character",
                (s->col >= 0 ? s->col : 0) + (int)strlen(s->name));
            cJSON_AddItemToObject(sel_range, "start", sel_start);
            cJSON_AddItemToObject(sel_range, "end", sel_end);
            cJSON_AddItemToObject(sym, "selectionRange", sel_range);

            if (s->signature)
                cJSON_AddStringToObject(sym, "detail", s->signature);

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
        while (trim_end > tok && (trim_end[-1] == ' ' || trim_end[-1] == '\t'))
            trim_end--;

        size_t plen = (size_t)(trim_end - tok);
        if (plen > 0) {
            params[count] = malloc(plen + 1);
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
            if (depth == 0) break;  /* Found the matching open paren */
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
            if (depth == 0) { open_paren = c; break; }
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
    while (name_end > line_start && (name_end[-1] == ' ' || name_end[-1] == '\t'))
        name_end--;
    const char *name_start = name_end;
    while (name_start > line_start && is_ident_char(name_start[-1]))
        name_start--;

    if (name_start >= name_end) {
        cJSON *resp = lsp_make_response(id, cJSON_CreateNull());
        lsp_write_response(resp, stdout);
        cJSON_Delete(resp);
        return;
    }

    size_t name_len = (size_t)(name_end - name_start);
    char *func_name = malloc(name_len + 1);
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
            if (doc->symbols[i].kind == LSP_SYM_FUNCTION &&
                strcmp(doc->symbols[i].name, func_name) == 0) {
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

/* ── Handler: textDocument/references ── */

/* Check if position is inside a string literal (simple heuristic).
 * Counts unescaped quotes from line start to the given column. */
static bool in_string_literal(const char *line_start, int col) {
    bool in_str = false;
    char quote = 0;
    for (int i = 0; i < col && line_start[i] && line_start[i] != '\n'; i++) {
        char c = line_start[i];
        if (in_str) {
            if (c == '\\') { i++; continue; }  /* skip escaped char */
            if (c == quote) in_str = false;
        } else {
            if (c == '"' || c == '\'') { in_str = true; quote = c; }
        }
    }
    return in_str;
}

/* Find all occurrences of a word (as whole identifier) in the document text.
 * Returns a cJSON array of Location objects. */
static cJSON *find_all_references(const char *text, const char *uri,
                                   const char *word) {
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
                    /* Skip if inside a string literal */
                    if (!in_string_literal(line_start, col)) {
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
                continue;  /* p already advanced past the identifier */
            }
            p++;
        }

        if (*p == '\n') { p++; line++; }
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

/* ── Server lifecycle ── */

LspServer *lsp_server_new(void) {
    LspServer *srv = calloc(1, sizeof(LspServer));
    srv->log = stderr;
    return srv;
}

void lsp_server_free(LspServer *srv) {
    if (!srv) return;
    for (size_t i = 0; i < srv->doc_count; i++)
        lsp_document_free(srv->documents[i]);
    free(srv->documents);
    lsp_symbol_index_free(srv->index);
    free(srv);
}

/* Main message loop */
void lsp_server_run(LspServer *srv) {
    while (!srv->shutdown) {
        cJSON *msg = lsp_read_message(stdin);
        if (!msg) break;  /* EOF */

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
        } else if (strcmp(method, "textDocument/rename") == 0) {
            handle_rename(srv, params_node, id);
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
