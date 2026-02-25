#ifndef LATTICE_LSP_H
#define LATTICE_LSP_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

/* Forward declarations */
struct cJSON;

/* ── Diagnostic ── */

typedef enum {
    LSP_DIAG_ERROR   = 1,
    LSP_DIAG_WARNING = 2,
    LSP_DIAG_INFO    = 3,
    LSP_DIAG_HINT    = 4
} LspDiagSeverity;

typedef struct {
    char *message;
    int line;       /* 0-based (LSP convention) */
    int col;        /* 0-based */
    LspDiagSeverity severity;
} LspDiagnostic;

/* ── Symbol ── */

typedef enum {
    LSP_SYM_FUNCTION = 12,
    LSP_SYM_STRUCT   = 23,
    LSP_SYM_ENUM     = 10,
    LSP_SYM_VARIABLE = 13,
    LSP_SYM_KEYWORD  = 14,
    LSP_SYM_METHOD   = 2
} LspSymbolKind;

typedef struct {
    char *name;
    char *signature;
    char *doc;
    LspSymbolKind kind;
    int line;   /* 0-based, for user-defined symbols */
    int col;
} LspSymbol;

/* ── Document ── */

typedef struct {
    char *uri;
    char *text;
    int version;
    /* Cached analysis */
    LspDiagnostic *diagnostics;
    size_t diag_count;
    LspSymbol *symbols;
    size_t symbol_count;
} LspDocument;

/* ── Symbol Index (builtins + methods) ── */

typedef struct {
    LspSymbol *builtins;
    size_t builtin_count;
    size_t builtin_cap;
    LspSymbol *methods;
    size_t method_count;
    size_t method_cap;
} LspSymbolIndex;

/* ── Server ── */

typedef struct {
    LspDocument **documents;
    size_t doc_count;
    size_t doc_cap;
    LspSymbolIndex *index;
    bool initialized;
    bool shutdown;
    FILE *log;  /* optional debug log (stderr) */
} LspServer;

/* ── Protocol ── */

struct cJSON *lsp_read_message(FILE *in);
void lsp_write_response(struct cJSON *json, FILE *out);
struct cJSON *lsp_make_response(int id, struct cJSON *result);
struct cJSON *lsp_make_notification(const char *method, struct cJSON *params);
struct cJSON *lsp_make_error(int id, int code, const char *message);

/* ── Server ── */

LspServer *lsp_server_new(void);
void lsp_server_free(LspServer *srv);
void lsp_server_run(LspServer *srv);

/* ── Analysis ── */

void lsp_analyze_document(LspDocument *doc);
void lsp_document_free(LspDocument *doc);

/* ── Symbol index ── */

LspSymbolIndex *lsp_symbol_index_new(const char *eval_path);
void lsp_symbol_index_add_file(LspSymbolIndex *idx, const char *path);
void lsp_symbol_index_free(LspSymbolIndex *idx);

/* ── Utilities ── */

char *lsp_uri_to_path(const char *uri);
char *lsp_path_to_uri(const char *path);

#endif /* LATTICE_LSP_H */
