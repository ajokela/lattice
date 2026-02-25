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

static void handle_completion(LspServer *srv, cJSON *params, int id) {
    cJSON *td = cJSON_GetObjectItem(params, "textDocument");
    const char *uri = td ? cJSON_GetObjectItem(td, "uri")->valuestring : NULL;

    cJSON *items = cJSON_CreateArray();

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
            cJSON_AddItemToArray(items, item);
        }
    }

    /* User-defined symbols from current document */
    if (uri) {
        LspDocument *doc = find_document(srv, uri);
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

    /* Search builtins */
    const char *hover_text = NULL;
    if (srv->index) {
        for (size_t i = 0; i < srv->index->builtin_count; i++) {
            if (strcmp(srv->index->builtins[i].name, word) == 0) {
                hover_text = srv->index->builtins[i].doc;
                break;
            }
        }
        /* Search methods */
        if (!hover_text) {
            for (size_t i = 0; i < srv->index->method_count; i++) {
                if (strcmp(srv->index->methods[i].name, word) == 0) {
                    hover_text = srv->index->methods[i].doc;
                    break;
                }
            }
        }
    }

    /* Search document symbols */
    if (!hover_text && doc) {
        for (size_t i = 0; i < doc->symbol_count; i++) {
            if (strcmp(doc->symbols[i].name, word) == 0) {
                hover_text = doc->symbols[i].signature;
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

    /* Search document symbols */
    for (size_t i = 0; i < doc->symbol_count; i++) {
        if (strcmp(doc->symbols[i].name, word) == 0 && doc->symbols[i].line >= 0) {
            cJSON *result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "uri", uri);
            cJSON *range = cJSON_CreateObject();
            cJSON *start = cJSON_CreateObject();
            cJSON_AddNumberToObject(start, "line", doc->symbols[i].line);
            cJSON_AddNumberToObject(start, "character", doc->symbols[i].col);
            cJSON_AddItemToObject(range, "start", start);
            cJSON *end = cJSON_CreateObject();
            cJSON_AddNumberToObject(end, "line", doc->symbols[i].line);
            cJSON_AddNumberToObject(end, "character", doc->symbols[i].col);
            cJSON_AddItemToObject(range, "end", end);
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
