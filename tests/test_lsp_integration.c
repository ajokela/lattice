#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

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

#define TEST(name)                                                    \
    static void name(void);                                           \
    static void name##_register(void) __attribute__((constructor));   \
    static void name##_register(void) { register_test(#name, name); } \
    static void name(void)

/* Check if clat-lsp binary exists — skip tests gracefully if not */
static int lsp_binary_available(void) { return access("./clat-lsp", X_OK) == 0; }

#define SKIP_IF_NO_LSP()                                     \
    do {                                                     \
        if (!lsp_binary_available()) {                       \
            fprintf(stderr, "  skip: clat-lsp not found\n"); \
            return;                                          \
        }                                                    \
    } while (0)

/* ================================================================
 * LSP subprocess helpers
 * ================================================================ */

typedef struct {
    pid_t pid;
    FILE *to_server;   /* write requests here */
    FILE *from_server; /* read responses here */
} LspProc;

static int lsp_proc_start(LspProc *proc) {
    int to_child[2], from_child[2];
    if (pipe(to_child) != 0 || pipe(from_child) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* Child: exec clat-lsp */
        close(to_child[1]);
        close(from_child[0]);
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]);
        close(from_child[1]);
        /* Redirect stderr to /dev/null to keep test output clean */
        int devnull = open("/dev/null", 0);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("./clat-lsp", "clat-lsp", (char *)NULL);
        _exit(127);
    }

    /* Parent */
    close(to_child[0]);
    close(from_child[1]);
    proc->pid = pid;
    proc->to_server = fdopen(to_child[1], "w");
    proc->from_server = fdopen(from_child[0], "r");
    if (!proc->to_server || !proc->from_server) return -1;
    setvbuf(proc->to_server, NULL, _IONBF, 0);
    return 0;
}

static void lsp_proc_stop(LspProc *proc) {
    if (proc->to_server) {
        fclose(proc->to_server);
        proc->to_server = NULL;
    }
    if (proc->from_server) {
        fclose(proc->from_server);
        proc->from_server = NULL;
    }
    if (proc->pid > 0) {
        kill(proc->pid, SIGTERM);
        int status;
        waitpid(proc->pid, &status, 0);
        proc->pid = 0;
    }
}

/* Send a JSON-RPC message with Content-Length header */
static void lsp_send(LspProc *proc, cJSON *msg) {
    char *body = cJSON_PrintUnformatted(msg);
    fprintf(proc->to_server, "Content-Length: %zu\r\n\r\n%s", strlen(body), body);
    fflush(proc->to_server);
    cJSON_free(body);
}

/* Read a JSON-RPC response (Content-Length header + body).
 * Returns NULL on timeout/error. Caller must cJSON_Delete. */
static cJSON *lsp_recv(LspProc *proc) {
    char header[256];
    int content_length = -1;

    while (fgets(header, sizeof(header), proc->from_server)) {
        if (header[0] == '\r' || header[0] == '\n') break;
        if (strncmp(header, "Content-Length:", 15) == 0) { content_length = atoi(header + 15); }
    }
    if (content_length <= 0) return NULL;

    char *body = malloc((size_t)content_length + 1);
    if (!body) return NULL;
    size_t n = fread(body, 1, (size_t)content_length, proc->from_server);
    body[n] = '\0';
    cJSON *json = cJSON_Parse(body);
    free(body);
    return json;
}

/* Skip notification messages (diagnostics, etc.) until we get a response with the given id */
static cJSON *lsp_recv_response(LspProc *proc, int id) {
    for (int attempts = 0; attempts < 20; attempts++) {
        cJSON *msg = lsp_recv(proc);
        if (!msg) return NULL;
        cJSON *msg_id = cJSON_GetObjectItem(msg, "id");
        if (msg_id && msg_id->valueint == id) return msg;
        /* It's a notification — skip it */
        cJSON_Delete(msg);
    }
    return NULL;
}

/* Build an initialize request */
static cJSON *make_initialize(int id) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", "initialize");

    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "processId", (double)getpid());
    cJSON *caps = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "capabilities", caps);
    cJSON_AddNullToObject(params, "rootUri");
    cJSON_AddItemToObject(req, "params", params);
    return req;
}

/* Build an initialized notification */
static cJSON *make_initialized(void) {
    cJSON *notif = cJSON_CreateObject();
    cJSON_AddStringToObject(notif, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notif, "method", "initialized");
    cJSON_AddItemToObject(notif, "params", cJSON_CreateObject());
    return notif;
}

/* Build a textDocument/didOpen notification */
static cJSON *make_did_open(const char *uri, const char *text) {
    cJSON *notif = cJSON_CreateObject();
    cJSON_AddStringToObject(notif, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notif, "method", "textDocument/didOpen");

    cJSON *params = cJSON_CreateObject();
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "uri", uri);
    cJSON_AddStringToObject(doc, "languageId", "lattice");
    cJSON_AddNumberToObject(doc, "version", 1);
    cJSON_AddStringToObject(doc, "text", text);
    cJSON_AddItemToObject(params, "textDocument", doc);
    cJSON_AddItemToObject(notif, "params", params);
    return notif;
}

/* Build a textDocument/completion request */
static cJSON *make_completion(int id, const char *uri, int line, int character) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", "textDocument/completion");

    cJSON *params = cJSON_CreateObject();
    cJSON *td = cJSON_CreateObject();
    cJSON_AddStringToObject(td, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", td);
    cJSON *pos = cJSON_CreateObject();
    cJSON_AddNumberToObject(pos, "line", line);
    cJSON_AddNumberToObject(pos, "character", character);
    cJSON_AddItemToObject(params, "position", pos);
    cJSON_AddItemToObject(req, "params", params);
    return req;
}

/* Build a textDocument/hover request */
static cJSON *make_hover(int id, const char *uri, int line, int character) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", "textDocument/hover");

    cJSON *params = cJSON_CreateObject();
    cJSON *td = cJSON_CreateObject();
    cJSON_AddStringToObject(td, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", td);
    cJSON *pos = cJSON_CreateObject();
    cJSON_AddNumberToObject(pos, "line", line);
    cJSON_AddNumberToObject(pos, "character", character);
    cJSON_AddItemToObject(params, "position", pos);
    cJSON_AddItemToObject(req, "params", params);
    return req;
}

/* Build a textDocument/definition request */
static cJSON *make_definition(int id, const char *uri, int line, int character) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", "textDocument/definition");

    cJSON *params = cJSON_CreateObject();
    cJSON *td = cJSON_CreateObject();
    cJSON_AddStringToObject(td, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", td);
    cJSON *pos = cJSON_CreateObject();
    cJSON_AddNumberToObject(pos, "line", line);
    cJSON_AddNumberToObject(pos, "character", character);
    cJSON_AddItemToObject(params, "position", pos);
    cJSON_AddItemToObject(req, "params", params);
    return req;
}

/* Build a textDocument/documentSymbol request */
static cJSON *make_document_symbol(int id, const char *uri) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", "textDocument/documentSymbol");

    cJSON *params = cJSON_CreateObject();
    cJSON *td = cJSON_CreateObject();
    cJSON_AddStringToObject(td, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", td);
    cJSON_AddItemToObject(req, "params", params);
    return req;
}

/* Build a textDocument/formatting request */
static cJSON *make_formatting(int id, const char *uri) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", "textDocument/formatting");

    cJSON *params = cJSON_CreateObject();
    cJSON *td = cJSON_CreateObject();
    cJSON_AddStringToObject(td, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", td);
    cJSON *opts = cJSON_CreateObject();
    cJSON_AddNumberToObject(opts, "tabSize", 4);
    cJSON_AddBoolToObject(opts, "insertSpaces", 1);
    cJSON_AddItemToObject(params, "options", opts);
    cJSON_AddItemToObject(req, "params", params);
    return req;
}

/* Build a shutdown request */
static cJSON *make_shutdown(int id) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", "shutdown");
    return req;
}

/* Build an exit notification */
static cJSON *make_exit(void) {
    cJSON *notif = cJSON_CreateObject();
    cJSON_AddStringToObject(notif, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notif, "method", "exit");
    return notif;
}

/* ================================================================
 * Helper: full handshake (initialize + initialized + didOpen)
 * ================================================================ */

static int lsp_handshake(LspProc *proc, const char *uri, const char *text) {
    /* 1. Initialize */
    cJSON *init = make_initialize(1);
    lsp_send(proc, init);
    cJSON_Delete(init);

    cJSON *resp = lsp_recv_response(proc, 1);
    if (!resp) return -1;

    cJSON *result = cJSON_GetObjectItem(resp, "result");
    if (!result) {
        cJSON_Delete(resp);
        return -1;
    }
    cJSON_Delete(resp);

    /* 2. Initialized notification */
    cJSON *inited = make_initialized();
    lsp_send(proc, inited);
    cJSON_Delete(inited);

    /* 3. Open document */
    cJSON *open = make_did_open(uri, text);
    lsp_send(proc, open);
    cJSON_Delete(open);

    return 0;
}

/* ================================================================
 * Integration tests
 * ================================================================ */

TEST(lsp_integration_initialize) {
    SKIP_IF_NO_LSP();
    LspProc proc;
    ASSERT(lsp_proc_start(&proc) == 0);

    cJSON *init = make_initialize(1);
    lsp_send(&proc, init);
    cJSON_Delete(init);

    cJSON *resp = lsp_recv_response(&proc, 1);
    ASSERT(resp != NULL);

    /* Verify we got a result with capabilities */
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    ASSERT(result != NULL);
    cJSON *caps = cJSON_GetObjectItem(result, "capabilities");
    ASSERT(caps != NULL);

    /* Check key capabilities are present */
    ASSERT(cJSON_GetObjectItem(caps, "completionProvider") != NULL);
    ASSERT(cJSON_GetObjectItem(caps, "hoverProvider") != NULL);
    ASSERT(cJSON_GetObjectItem(caps, "definitionProvider") != NULL);
    ASSERT(cJSON_GetObjectItem(caps, "documentSymbolProvider") != NULL);
    ASSERT(cJSON_GetObjectItem(caps, "referencesProvider") != NULL);
    ASSERT(cJSON_GetObjectItem(caps, "renameProvider") != NULL);
    ASSERT(cJSON_GetObjectItem(caps, "documentFormattingProvider") != NULL);
    ASSERT(cJSON_GetObjectItem(caps, "codeActionProvider") != NULL);
    ASSERT(cJSON_GetObjectItem(caps, "foldingRangeProvider") != NULL);
    ASSERT(cJSON_GetObjectItem(caps, "semanticTokensProvider") != NULL);
    ASSERT(cJSON_GetObjectItem(caps, "inlayHintProvider") != NULL);
    ASSERT(cJSON_GetObjectItem(caps, "signatureHelpProvider") != NULL);
    ASSERT(cJSON_GetObjectItem(caps, "workspaceSymbolProvider") != NULL);

    /* Check serverInfo */
    cJSON *info = cJSON_GetObjectItem(result, "serverInfo");
    ASSERT(info != NULL);
    cJSON *name = cJSON_GetObjectItem(info, "name");
    ASSERT(name != NULL && strcmp(name->valuestring, "clat-lsp") == 0);

    cJSON_Delete(resp);
    lsp_proc_stop(&proc);
}

TEST(lsp_integration_completion) {
    SKIP_IF_NO_LSP();
    LspProc proc;
    ASSERT(lsp_proc_start(&proc) == 0);

    const char *uri = "file:///test_completion.lat";
    const char *src = "fn greet(name: String) {\n  print(name)\n}\nlet x = 42\n";
    ASSERT(lsp_handshake(&proc, uri, src) == 0);

    /* Request completion at start of line 4 (should include keywords, builtins, user symbols) */
    cJSON *comp = make_completion(2, uri, 3, 0);
    lsp_send(&proc, comp);
    cJSON_Delete(comp);

    cJSON *resp = lsp_recv_response(&proc, 2);
    ASSERT(resp != NULL);

    cJSON *result = cJSON_GetObjectItem(resp, "result");
    ASSERT(result != NULL);

    /* Result can be an array or a CompletionList with items */
    cJSON *items = result;
    if (!cJSON_IsArray(result)) { items = cJSON_GetObjectItem(result, "items"); }
    ASSERT(items != NULL);
    ASSERT(cJSON_GetArraySize(items) > 0);

    /* Check that at least one item has a label */
    cJSON *first = cJSON_GetArrayItem(items, 0);
    ASSERT(first != NULL);
    ASSERT(cJSON_GetObjectItem(first, "label") != NULL);

    cJSON_Delete(resp);
    lsp_proc_stop(&proc);
}

TEST(lsp_integration_hover) {
    SKIP_IF_NO_LSP();
    LspProc proc;
    ASSERT(lsp_proc_start(&proc) == 0);

    const char *uri = "file:///test_hover.lat";
    const char *src = "fn hello() {\n  print(\"world\")\n}\n";
    ASSERT(lsp_handshake(&proc, uri, src) == 0);

    /* Hover over "print" at line 1, col 2 */
    cJSON *hover = make_hover(2, uri, 1, 2);
    lsp_send(&proc, hover);
    cJSON_Delete(hover);

    cJSON *resp = lsp_recv_response(&proc, 2);
    ASSERT(resp != NULL);

    /* Result may be null if hover isn't found — that's OK, we just want no crash/error */
    ASSERT(cJSON_GetObjectItem(resp, "error") == NULL);

    cJSON_Delete(resp);
    lsp_proc_stop(&proc);
}

TEST(lsp_integration_definition) {
    SKIP_IF_NO_LSP();
    LspProc proc;
    ASSERT(lsp_proc_start(&proc) == 0);

    const char *uri = "file:///test_definition.lat";
    const char *src = "fn add(a: Int, b: Int) -> Int {\n  return a + b\n}\nlet result = add(1, 2)\n";
    ASSERT(lsp_handshake(&proc, uri, src) == 0);

    /* Go to definition of "add" on line 3 (0-indexed), col 13 */
    cJSON *def = make_definition(2, uri, 3, 13);
    lsp_send(&proc, def);
    cJSON_Delete(def);

    cJSON *resp = lsp_recv_response(&proc, 2);
    ASSERT(resp != NULL);
    ASSERT(cJSON_GetObjectItem(resp, "error") == NULL);

    cJSON *result = cJSON_GetObjectItem(resp, "result");
    /* Should resolve to line 0 where add is defined */
    if (result && !cJSON_IsNull(result)) {
        /* Could be a Location or Location[] */
        cJSON *loc = cJSON_IsArray(result) ? cJSON_GetArrayItem(result, 0) : result;
        if (loc) {
            cJSON *range = cJSON_GetObjectItem(loc, "range");
            if (range) {
                cJSON *start = cJSON_GetObjectItem(range, "start");
                if (start) {
                    cJSON *line = cJSON_GetObjectItem(start, "line");
                    ASSERT(line != NULL);
                    ASSERT(line->valueint == 0); /* defined on line 0 */
                }
            }
        }
    }

    cJSON_Delete(resp);
    lsp_proc_stop(&proc);
}

TEST(lsp_integration_document_symbols) {
    SKIP_IF_NO_LSP();
    LspProc proc;
    ASSERT(lsp_proc_start(&proc) == 0);

    const char *uri = "file:///test_symbols.lat";
    const char *src = "fn foo() { return 1 }\n"
                      "fn bar(x: Int) { return x + 1 }\n"
                      "struct Point { x: Int, y: Int }\n"
                      "enum Color { Red, Green, Blue }\n";
    ASSERT(lsp_handshake(&proc, uri, src) == 0);

    cJSON *syms = make_document_symbol(2, uri);
    lsp_send(&proc, syms);
    cJSON_Delete(syms);

    cJSON *resp = lsp_recv_response(&proc, 2);
    ASSERT(resp != NULL);
    ASSERT(cJSON_GetObjectItem(resp, "error") == NULL);

    cJSON *result = cJSON_GetObjectItem(resp, "result");
    ASSERT(result != NULL);
    ASSERT(cJSON_IsArray(result));
    /* Should find at least foo, bar, Point, Color */
    ASSERT(cJSON_GetArraySize(result) >= 4);

    cJSON_Delete(resp);
    lsp_proc_stop(&proc);
}

TEST(lsp_integration_formatting) {
    SKIP_IF_NO_LSP();
    LspProc proc;
    ASSERT(lsp_proc_start(&proc) == 0);

    const char *uri = "file:///test_format.lat";
    /* Intentionally messy formatting */
    const char *src = "fn   foo(  )   {\nreturn   1\n}\n";
    ASSERT(lsp_handshake(&proc, uri, src) == 0);

    cJSON *fmt = make_formatting(2, uri);
    lsp_send(&proc, fmt);
    cJSON_Delete(fmt);

    cJSON *resp = lsp_recv_response(&proc, 2);
    ASSERT(resp != NULL);
    ASSERT(cJSON_GetObjectItem(resp, "error") == NULL);

    cJSON *result = cJSON_GetObjectItem(resp, "result");
    /* Result is an array of TextEdits (possibly empty if already formatted) */
    ASSERT(result != NULL);
    ASSERT(cJSON_IsArray(result));

    cJSON_Delete(resp);
    lsp_proc_stop(&proc);
}

TEST(lsp_integration_shutdown_exit) {
    SKIP_IF_NO_LSP();
    LspProc proc;
    ASSERT(lsp_proc_start(&proc) == 0);

    const char *uri = "file:///test_shutdown.lat";
    ASSERT(lsp_handshake(&proc, uri, "let x = 1\n") == 0);

    /* Shutdown */
    cJSON *sd = make_shutdown(99);
    lsp_send(&proc, sd);
    cJSON_Delete(sd);

    cJSON *resp = lsp_recv_response(&proc, 99);
    ASSERT(resp != NULL);
    ASSERT(cJSON_GetObjectItem(resp, "error") == NULL);
    cJSON_Delete(resp);

    /* Exit */
    cJSON *ex = make_exit();
    lsp_send(&proc, ex);
    cJSON_Delete(ex);

    /* Wait for clean exit */
    int status;
    int r = waitpid(proc.pid, &status, 0);
    ASSERT(r == proc.pid);
    ASSERT(WIFEXITED(status));
    ASSERT(WEXITSTATUS(status) == 0);

    /* Clean up without killing (already exited) */
    proc.pid = 0;
    if (proc.to_server) {
        fclose(proc.to_server);
        proc.to_server = NULL;
    }
    if (proc.from_server) {
        fclose(proc.from_server);
        proc.from_server = NULL;
    }
}

TEST(lsp_integration_diagnostics_on_open) {
    SKIP_IF_NO_LSP();
    LspProc proc;
    ASSERT(lsp_proc_start(&proc) == 0);

    const char *uri = "file:///test_diag.lat";
    /* Source with a syntax error */
    const char *src = "fn broken( {\n}\n";
    ASSERT(lsp_handshake(&proc, uri, src) == 0);

    /* After didOpen, the server should push a textDocument/publishDiagnostics notification.
     * Read messages until we find it (skip non-matching ones). */
    int found_diag = 0;
    for (int i = 0; i < 10; i++) {
        cJSON *msg = lsp_recv(&proc);
        if (!msg) break;
        cJSON *method = cJSON_GetObjectItem(msg, "method");
        if (method && strcmp(method->valuestring, "textDocument/publishDiagnostics") == 0) {
            cJSON *params = cJSON_GetObjectItem(msg, "params");
            ASSERT(params != NULL);
            cJSON *diag_uri = cJSON_GetObjectItem(params, "uri");
            ASSERT(diag_uri != NULL);
            ASSERT(strcmp(diag_uri->valuestring, uri) == 0);
            cJSON *diagnostics = cJSON_GetObjectItem(params, "diagnostics");
            ASSERT(diagnostics != NULL);
            ASSERT(cJSON_IsArray(diagnostics));
            /* Should have at least one diagnostic for the syntax error */
            ASSERT(cJSON_GetArraySize(diagnostics) >= 1);
            found_diag = 1;
            cJSON_Delete(msg);
            break;
        }
        cJSON_Delete(msg);
    }
    ASSERT(found_diag);

    lsp_proc_stop(&proc);
}

#endif /* !_WIN32 */
