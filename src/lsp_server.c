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
