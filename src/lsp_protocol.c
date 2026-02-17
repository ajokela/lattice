#include "lsp.h"
#include "../vendor/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Read one JSON-RPC message from stdin (Content-Length header + body) */
cJSON *lsp_read_message(FILE *in) {
    char header[256];
    int content_length = -1;

    /* Read headers until empty line */
    while (fgets(header, sizeof(header), in)) {
        if (header[0] == '\r' || header[0] == '\n') break;

        if (strncmp(header, "Content-Length:", 15) == 0) {
            content_length = atoi(header + 15);
        }
    }

    if (content_length <= 0) return NULL;

    /* Read body */
    char *body = malloc((size_t)content_length + 1);
    if (!body) return NULL;

    size_t read = fread(body, 1, (size_t)content_length, in);
    if ((int)read != content_length) {
        free(body);
        return NULL;
    }
    body[content_length] = '\0';

    cJSON *json = cJSON_Parse(body);
    free(body);
    return json;
}

/* Write a JSON-RPC message to stdout with Content-Length header */
void lsp_write_response(cJSON *json, FILE *out) {
    char *body = cJSON_PrintUnformatted(json);
    if (!body) return;

    fprintf(out, "Content-Length: %zu\r\n\r\n%s", strlen(body), body);
    fflush(out);
    cJSON_free(body);
}

/* Create a JSON-RPC response */
cJSON *lsp_make_response(int id, cJSON *result) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(resp, "id", id);
    cJSON_AddItemToObject(resp, "result", result);
    return resp;
}

/* Create a JSON-RPC notification */
cJSON *lsp_make_notification(const char *method, cJSON *params) {
    cJSON *notif = cJSON_CreateObject();
    cJSON_AddStringToObject(notif, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notif, "method", method);
    cJSON_AddItemToObject(notif, "params", params);
    return notif;
}

/* Create a JSON-RPC error response */
cJSON *lsp_make_error(int id, int code, const char *message) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(resp, "id", id);
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(resp, "error", err);
    return resp;
}
