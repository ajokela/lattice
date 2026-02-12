#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <stdbool.h>

/* HTTP response */
typedef struct {
    int status_code;
    char **header_keys;
    char **header_values;
    size_t header_count;
    char *body;
    size_t body_len;
} HttpResponse;

/* HTTP request options */
typedef struct {
    const char *method;
    const char *url;
    char **header_keys;
    char **header_values;
    size_t header_count;
    const char *body;
    size_t body_len;
    int timeout_ms;   /* 0 = default (30s) */
} HttpRequest;

/* Parsed URL */
typedef struct {
    char *scheme;   /* "http" or "https" */
    char *host;
    int port;
    char *path;     /* includes query string, starts with '/' */
} HttpUrl;

/* Parse URL into components. Returns true on success. */
bool http_parse_url(const char *url, HttpUrl *out, char **err);

/* Free parsed URL */
void http_url_free(HttpUrl *url);

/* Execute HTTP request. Returns response on success, NULL on error. */
HttpResponse *http_execute(const HttpRequest *req, char **err);

/* Free HTTP response */
void http_response_free(HttpResponse *resp);

#endif /* HTTP_H */
