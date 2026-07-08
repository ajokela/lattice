#include "http.h"
#include "net.h"
#include "tls.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#ifdef _WIN32
#include "win32_compat.h"
#endif

#ifdef __EMSCRIPTEN__

bool http_parse_url(const char *url, HttpUrl *out, char **err) {
    (void)url;
    (void)out;
    *err = strdup("HTTP not available in WASM");
    return false;
}

void http_url_free(HttpUrl *url) { (void)url; }

HttpResponse *http_execute(const HttpRequest *req, char **err) {
    (void)req;
    *err = strdup("HTTP not available in WASM");
    return NULL;
}

void http_response_free(HttpResponse *resp) { (void)resp; }

#else /* !__EMSCRIPTEN__ */

/* ── URL parsing ── */

bool http_parse_url(const char *url, HttpUrl *out, char **err) {
    memset(out, 0, sizeof(*out));

    /* Parse scheme */
    if (strncmp(url, "https://", 8) == 0) {
        out->scheme = strdup("https");
        url += 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        out->scheme = strdup("http");
        url += 7;
    } else {
        *err = strdup("invalid URL: must start with http:// or https://");
        return false;
    }

    /* Find path separator */
    const char *slash = strchr(url, '/');
    size_t host_len = slash ? (size_t)(slash - url) : strlen(url);

    /* Extract host[:port] */
    char *host_part = strndup(url, host_len);
    const char *colon = strchr(host_part, ':');
    if (colon) {
        out->host = strndup(host_part, (size_t)(colon - host_part));
        out->port = atoi(colon + 1);
        if (out->port <= 0 || out->port > 65535) {
            free(host_part);
            http_url_free(out);
            *err = strdup("invalid URL: bad port number");
            return false;
        }
    } else {
        out->host = strdup(host_part);
        out->port = (strcmp(out->scheme, "https") == 0) ? 443 : 80;
    }
    free(host_part);

    if (strlen(out->host) == 0) {
        http_url_free(out);
        *err = strdup("invalid URL: empty host");
        return false;
    }

    /* Path (default to "/") */
    out->path = strdup(slash ? slash : "/");
    return true;
}

void http_url_free(HttpUrl *url) {
    free(url->scheme);
    free(url->host);
    free(url->path);
    memset(url, 0, sizeof(*url));
}

/* ── Request formatting ── */

/* Reject values that get interpolated into the request line or a header but
 * contain CR, LF, or other control characters. Without this an attacker-
 * supplied URL or header value containing "\r\n" could inject extra headers or
 * split the request entirely (HTTP request splitting / smuggling). These are
 * NUL-terminated C strings, so an embedded NUL already truncates the string;
 * the remaining danger is CR/LF and other control bytes, all of which are
 * below 0x20 (plus DEL, 0x7f). */
static bool http_field_is_safe(const char *s) {
    if (!s) return true;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return false;
    }
    return true;
}

static char *format_request(const HttpRequest *req, const HttpUrl *url, char **err) {
    /* Reject control characters (CR/LF/NUL/...) in anything interpolated into
     * the request line or headers — otherwise the request can be split or
     * extra headers injected (HTTP request splitting / smuggling). */
    if (!http_field_is_safe(req->method) || !http_field_is_safe(url->host) || !http_field_is_safe(url->path)) {
        *err = strdup("invalid request: control characters in method, host, or path");
        return NULL;
    }
    for (size_t i = 0; i < req->header_count; i++) {
        if (!http_field_is_safe(req->header_keys[i]) || !http_field_is_safe(req->header_values[i])) {
            *err = strdup("invalid header: control characters not allowed");
            return NULL;
        }
    }

    /* Calculate buffer size */
    size_t cap = 256;
    cap += strlen(req->method) + strlen(url->path) + strlen(url->host);
    for (size_t i = 0; i < req->header_count; i++)
        cap += strlen(req->header_keys[i]) + strlen(req->header_values[i]) + 8;
    if (req->body) cap += 64 + req->body_len; /* Content-Length header + body */

    char *buf = malloc(cap);
    if (!buf) {
        *err = strdup("out of memory formatting request");
        return NULL;
    }
    size_t pos = 0;

    /* Request line */
    pos += (size_t)snprintf(buf + pos, cap - pos, "%s %s HTTP/1.1\r\n", req->method, url->path);

    /* Host header */
    if ((strcmp(url->scheme, "http") == 0 && url->port != 80) ||
        (strcmp(url->scheme, "https") == 0 && url->port != 443)) {
        pos += (size_t)snprintf(buf + pos, cap - pos, "Host: %s:%d\r\n", url->host, url->port);
    } else {
        pos += (size_t)snprintf(buf + pos, cap - pos, "Host: %s\r\n", url->host);
    }

    /* User headers */
    for (size_t i = 0; i < req->header_count; i++)
        pos += (size_t)snprintf(buf + pos, cap - pos, "%s: %s\r\n", req->header_keys[i], req->header_values[i]);

    /* Content-Length for request body */
    if (req->body && req->body_len > 0)
        pos += (size_t)snprintf(buf + pos, cap - pos, "Content-Length: %zu\r\n", req->body_len);

    /* Connection close */
    pos += (size_t)snprintf(buf + pos, cap - pos, "Connection: close\r\n\r\n");

    /* Body */
    if (req->body && req->body_len > 0) {
        memcpy(buf + pos, req->body, req->body_len);
        pos += req->body_len;
    }
    buf[pos] = '\0';
    return buf;
}

/* ── Response parsing ── */

static HttpResponse *parse_response(const char *raw, size_t raw_len, char **err) {
    /* An empty/zero-length response (server closed with no data) leaves the read
     * buffer uninitialized and unterminated; the strstr() scans below would read
     * past the allocation. Reject it cleanly before touching the buffer. */
    if (!raw || raw_len == 0) {
        *err = strdup("invalid HTTP response: empty response");
        return NULL;
    }

    /* Find end of headers */
    const char *hdr_end = strstr(raw, "\r\n\r\n");
    if (!hdr_end) {
        *err = strdup("invalid HTTP response: no header/body separator");
        return NULL;
    }

    HttpResponse *resp = calloc(1, sizeof(HttpResponse));
    if (!resp) return NULL;

    /* Parse status line: HTTP/1.x STATUS REASON */
    const char *line_end = strstr(raw, "\r\n");
    if (!line_end || strncmp(raw, "HTTP/", 5) != 0) {
        *err = strdup("invalid HTTP response: bad status line");
        free(resp);
        return NULL;
    }

    /* Find status code (skip "HTTP/x.x ") */
    const char *p = raw;
    while (p < line_end && *p != ' ') p++;
    if (p < line_end) p++; /* skip space */
    resp->status_code = atoi(p);

    /* Parse headers */
    size_t hdr_cap = 16;
    resp->header_keys = malloc(hdr_cap * sizeof(char *));
    if (!resp->header_keys) return NULL;
    resp->header_values = malloc(hdr_cap * sizeof(char *));
    if (!resp->header_values) return NULL;
    resp->header_count = 0;

    const char *hdr = line_end + 2; /* skip first \r\n */
    while (hdr < hdr_end) {
        const char *next = strstr(hdr, "\r\n");
        if (!next || next == hdr) break;

        const char *colon = memchr(hdr, ':', (size_t)(next - hdr));
        if (colon) {
            if (resp->header_count >= hdr_cap) {
                size_t new_cap = hdr_cap * 2;
                char **nk = realloc(resp->header_keys, new_cap * sizeof(char *));
                char **nv = realloc(resp->header_values, new_cap * sizeof(char *));
                if (!nk || !nv) {
                    free(nk ? nk : resp->header_keys);
                    free(nv ? nv : resp->header_values);
                    resp->header_keys = NULL;
                    resp->header_values = NULL;
                    break; /* out of memory: stop parsing headers */
                }
                resp->header_keys = nk;
                resp->header_values = nv;
                hdr_cap = new_cap;
            }
            /* Key: lowercase for easy lookup */
            size_t klen = (size_t)(colon - hdr);
            char *key = strndup(hdr, klen);
            for (size_t i = 0; i < klen; i++) key[i] = (char)tolower((unsigned char)key[i]);

            /* Value: skip leading whitespace */
            const char *val = colon + 1;
            while (val < next && *val == ' ') val++;
            char *value = strndup(val, (size_t)(next - val));

            resp->header_keys[resp->header_count] = key;
            resp->header_values[resp->header_count] = value;
            resp->header_count++;
        }
        hdr = next + 2;
    }

    /* Body */
    const char *body_start = hdr_end + 4;
    size_t body_len = raw_len - (size_t)(body_start - raw);

    /* Check for chunked transfer encoding */
    bool chunked = false;
    for (size_t i = 0; i < resp->header_count; i++) {
        if (strcmp(resp->header_keys[i], "transfer-encoding") == 0 && strstr(resp->header_values[i], "chunked")) {
            chunked = true;
            break;
        }
    }

    if (chunked) {
        /* Decode chunked body */
        size_t bcap = body_len > 0 ? body_len : 128;
        char *body = malloc(bcap);
        if (!body) return NULL;
        size_t bpos = 0;
        const char *cp = body_start;
        const char *end = raw + raw_len;

        while (cp < end) {
            /* Read chunk size (hex) */
            char *endptr;
            unsigned long chunk_size = strtoul(cp, &endptr, 16);
            if (endptr == cp) break;

            /* Skip to after \r\n */
            cp = strstr(cp, "\r\n");
            if (!cp) break;
            cp += 2;

            if (chunk_size == 0) break; /* Final chunk */

            /* Grow by the bytes actually available to copy, not by the
             * attacker-claimed chunk_size (which can be up to ULONG_MAX and
             * overflow the size arithmetic / force a giant allocation). */
            size_t avail = (size_t)(end - cp);
            size_t to_copy = chunk_size < avail ? chunk_size : avail;
            while (bpos + to_copy + 1 > bcap) {
                size_t newcap = bcap * 2;
                char *nb = (newcap > bcap) ? realloc(body, newcap) : NULL;
                if (!nb) {
                    free(body);
                    body = NULL;
                    break;
                }
                body = nb;
                bcap = newcap;
            }
            if (!body) break;
            memcpy(body + bpos, cp, to_copy);
            bpos += to_copy;
            cp += to_copy;

            /* Skip trailing \r\n */
            if (cp + 2 <= end && cp[0] == '\r' && cp[1] == '\n') cp += 2;
        }
        if (!body) { /* allocation failed mid-decode */
            body = calloc(1, 1);
            bpos = 0;
        }
        if (body) body[bpos] = '\0';
        resp->body = body;
        resp->body_len = bpos;
    } else {
        resp->body = malloc(body_len + 1);
        if (!resp->body) return NULL;
        memcpy(resp->body, body_start, body_len);
        resp->body[body_len] = '\0';
        resp->body_len = body_len;
    }

    return resp;
}

void http_response_free(HttpResponse *resp) {
    if (!resp) return;
    for (size_t i = 0; i < resp->header_count; i++) {
        free(resp->header_keys[i]);
        free(resp->header_values[i]);
    }
    free(resp->header_keys);
    free(resp->header_values);
    free(resp->body);
    free(resp);
}

/* ── Execute request ── */

/* Defined in net.c. Like net_tcp_read() but reports the exact number of bytes
 * read via *out_len, so a binary body containing embedded NUL bytes is not
 * truncated (strlen would stop at the first NUL). */
char *net_tcp_read_buf(int fd, size_t *out_len, char **err);

/* Upper bound on the total accumulated HTTP response. A malicious or runaway
 * server could otherwise stream an unbounded body straight into memory
 * (DoS / OOM). 256 MiB. */
#define HTTP_MAX_RESPONSE_SIZE ((size_t)256 * 1024 * 1024)

HttpResponse *http_execute(const HttpRequest *req, char **err) {
    HttpUrl url;
    if (!http_parse_url(req->url, &url, err)) return NULL;

    bool use_tls = (strcmp(url.scheme, "https") == 0);

    /* Connect */
    int fd;
    if (use_tls) {
        fd = net_tls_connect(url.host, url.port, err);
    } else {
        fd = net_tcp_connect(url.host, url.port, err);
    }
    if (fd < 0) {
        http_url_free(&url);
        return NULL;
    }

    /* Set timeout */
    int timeout_s = (req->timeout_ms > 0) ? (req->timeout_ms / 1000) : 30;
    if (timeout_s < 1) timeout_s = 1;
    net_tcp_set_timeout(fd, timeout_s, err);

    /* Format and send request */
    char *raw_req = format_request(req, &url, err);
    if (!raw_req) {
        if (use_tls) net_tls_close(fd);
        else net_tcp_close(fd);
        http_url_free(&url);
        return NULL;
    }
    bool ok;
    if (use_tls) {
        ok = net_tls_write(fd, raw_req, strlen(raw_req), err);
    } else {
        ok = net_tcp_write(fd, raw_req, strlen(raw_req), err);
    }
    free(raw_req);

    if (!ok) {
        if (use_tls) net_tls_close(fd);
        else net_tcp_close(fd);
        http_url_free(&url);
        return NULL;
    }

    /* Read response */
    size_t resp_cap = 8192;
    size_t resp_len = 0;
    char *resp_buf = malloc(resp_cap);
    if (!resp_buf) {
        if (use_tls) net_tls_close(fd);
        else net_tcp_close(fd);
        http_url_free(&url);
        *err = strdup("out of memory reading response");
        return NULL;
    }

    for (;;) {
        char *chunk;
        size_t clen;
        if (use_tls) {
            chunk = net_tls_read_buf(fd, &clen, err);
        } else {
            chunk = net_tcp_read_buf(fd, &clen, err);
        }
        if (!chunk) {
            /* Read error — but if we already have data, try to parse it */
            if (resp_len > 0) {
                free(*err);
                *err = NULL;
                break;
            }
            free(resp_buf);
            if (use_tls) net_tls_close(fd);
            else net_tcp_close(fd);
            http_url_free(&url);
            return NULL;
        }
        if (clen == 0) {
            free(chunk);
            break;
        } /* EOF */

        /* Bound total response size to avoid unbounded memory growth (DoS).
         * Written as a subtraction to avoid overflow; resp_len never exceeds
         * the cap, so HTTP_MAX_RESPONSE_SIZE - resp_len never underflows. */
        if (clen > HTTP_MAX_RESPONSE_SIZE - resp_len) {
            free(chunk);
            free(resp_buf);
            if (use_tls) net_tls_close(fd);
            else net_tcp_close(fd);
            http_url_free(&url);
            *err = strdup("HTTP response exceeds maximum allowed size");
            return NULL;
        }

        while (resp_len + clen + 1 > resp_cap) {
            size_t newcap = resp_cap * 2;
            char *nb = (newcap > resp_cap) ? realloc(resp_buf, newcap) : NULL;
            if (!nb) {
                free(resp_buf);
                resp_buf = NULL;
                break;
            }
            resp_buf = nb;
            resp_cap = newcap;
        }
        if (!resp_buf) {
            free(chunk);
            break;
        }
        memcpy(resp_buf + resp_len, chunk, clen);
        resp_len += clen;
        resp_buf[resp_len] = '\0';
        free(chunk);
    }

    /* Close connection */
    if (use_tls) net_tls_close(fd);
    else net_tcp_close(fd);
    http_url_free(&url);

    /* Parse response */
    HttpResponse *resp = parse_response(resp_buf, resp_len, err);
    free(resp_buf);
    return resp;
}

#endif /* !__EMSCRIPTEN__ */
