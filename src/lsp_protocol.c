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

/* ── URI utilities ── */

/* Convert a file:// URI to a filesystem path.
 * Handles percent-encoding (%20 -> space, etc.).
 * Returns a malloc'd string, or NULL on error. */
char *lsp_uri_to_path(const char *uri) {
    if (!uri) return NULL;

    /* Strip "file://" prefix */
    const char *path_start = uri;
    if (strncmp(uri, "file:///", 8) == 0) {
        path_start = uri + 7;  /* Keep one leading / */
    } else if (strncmp(uri, "file://", 7) == 0) {
        path_start = uri + 7;
    }

    size_t len = strlen(path_start);
    char *path = malloc(len + 1);
    if (!path) return NULL;

    char *out = path;
    const char *p = path_start;
    while (*p) {
        if (*p == '%' && p[1] && p[2]) {
            /* Decode percent-encoded byte */
            char hex[3] = { p[1], p[2], '\0' };
            *out++ = (char)strtol(hex, NULL, 16);
            p += 3;
        } else {
            *out++ = *p++;
        }
    }
    *out = '\0';
    return path;
}

/* Convert a filesystem path to a file:// URI.
 * Percent-encodes characters that are not allowed in URIs.
 * Returns a malloc'd string. */
char *lsp_path_to_uri(const char *path) {
    if (!path) return NULL;

    /* Worst case: every char becomes %XX (3 bytes) + "file://" prefix */
    size_t max_len = 7 + strlen(path) * 3 + 1;
    char *uri = malloc(max_len);
    if (!uri) return NULL;

    char *out = uri;
    memcpy(out, "file://", 7);
    out += 7;

    const char *p = path;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        /* Unreserved characters: A-Z a-z 0-9 - . _ ~ / : */
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~' || c == '/' || c == ':') {
            *out++ = (char)c;
        } else {
            out += sprintf(out, "%%%02X", c);
        }
        p++;
    }
    *out = '\0';
    return uri;
}
