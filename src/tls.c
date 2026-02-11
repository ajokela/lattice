#include "tls.h"
#include "net.h"

#ifdef LATTICE_HAS_TLS

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

/* ── Per-fd SSL session tracking ── */

static SSL *tls_sessions[FD_SETSIZE];

/* ── Lazy-initialised shared context ── */

static SSL_CTX *g_ssl_ctx = NULL;

static SSL_CTX *get_ssl_ctx(char **err) {
    if (g_ssl_ctx) return g_ssl_ctx;

    const SSL_METHOD *method = TLS_client_method();
    g_ssl_ctx = SSL_CTX_new(method);
    if (!g_ssl_ctx) {
        *err = strdup("tls: failed to create SSL_CTX");
        return NULL;
    }

    /* Load system CA certificates and enable peer verification */
    SSL_CTX_set_default_verify_paths(g_ssl_ctx);
    SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);

    return g_ssl_ctx;
}

static char *ssl_error_string(const char *prefix) {
    unsigned long e = ERR_get_error();
    char buf[512];
    if (e) {
        char ssl_buf[256];
        ERR_error_string_n(e, ssl_buf, sizeof(ssl_buf));
        snprintf(buf, sizeof(buf), "%s: %s", prefix, ssl_buf);
    } else {
        snprintf(buf, sizeof(buf), "%s: unknown SSL error", prefix);
    }
    return strdup(buf);
}

/* ── tls_connect ── */

int net_tls_connect(const char *host, int port, char **err) {
    SSL_CTX *ctx = get_ssl_ctx(err);
    if (!ctx) return -1;

    /* Use the existing TCP connect for the raw socket */
    int fd = net_tcp_connect(host, port, err);
    if (fd < 0) return -1;

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        *err = ssl_error_string("tls_connect: SSL_new");
        net_tcp_close(fd);
        return -1;
    }

    /* SNI — required by most HTTPS servers */
    SSL_set_tlsext_host_name(ssl, host);

    /* Enable hostname verification */
    SSL_set1_host(ssl, host);

    SSL_set_fd(ssl, fd);

    if (SSL_connect(ssl) != 1) {
        *err = ssl_error_string("tls_connect: SSL_connect");
        SSL_free(ssl);
        net_tcp_close(fd);
        return -1;
    }

    if (fd >= 0 && fd < FD_SETSIZE)
        tls_sessions[fd] = ssl;

    return fd;
}

/* ── tls_read ── */

#define TLS_READ_BUF 8192

char *net_tls_read(int fd, char **err) {
    if (fd < 0 || fd >= FD_SETSIZE || !tls_sessions[fd]) {
        *err = strdup("tls_read: not a TLS socket");
        return NULL;
    }

    char *buf = malloc(TLS_READ_BUF + 1);
    if (!buf) { *err = strdup("tls_read: out of memory"); return NULL; }

    int n = SSL_read(tls_sessions[fd], buf, TLS_READ_BUF);
    if (n < 0) {
        free(buf);
        *err = ssl_error_string("tls_read: SSL_read");
        return NULL;
    }

    buf[n] = '\0';
    return buf;
}

/* ── tls_read_bytes ── */

char *net_tls_read_bytes(int fd, size_t count, char **err) {
    if (fd < 0 || fd >= FD_SETSIZE || !tls_sessions[fd]) {
        *err = strdup("tls_read_bytes: not a TLS socket");
        return NULL;
    }

    char *buf = malloc(count + 1);
    if (!buf) { *err = strdup("tls_read_bytes: out of memory"); return NULL; }

    size_t total = 0;
    while (total < count) {
        int n = SSL_read(tls_sessions[fd], buf + total, (int)(count - total));
        if (n <= 0) break;  /* EOF or error */
        total += (size_t)n;
    }

    buf[total] = '\0';
    return buf;
}

/* ── tls_write ── */

bool net_tls_write(int fd, const char *data, size_t len, char **err) {
    if (fd < 0 || fd >= FD_SETSIZE || !tls_sessions[fd]) {
        *err = strdup("tls_write: not a TLS socket");
        return false;
    }

    size_t total = 0;
    while (total < len) {
        int n = SSL_write(tls_sessions[fd], data + total, (int)(len - total));
        if (n <= 0) {
            *err = ssl_error_string("tls_write: SSL_write");
            return false;
        }
        total += (size_t)n;
    }
    return true;
}

/* ── tls_close ── */

void net_tls_close(int fd) {
    if (fd >= 0 && fd < FD_SETSIZE && tls_sessions[fd]) {
        SSL_shutdown(tls_sessions[fd]);
        SSL_free(tls_sessions[fd]);
        tls_sessions[fd] = NULL;
    }
    net_tcp_close(fd);
}

/* ── tls_available ── */

bool net_tls_available(void) {
    return true;
}

/* ── tls_cleanup ── */

void net_tls_cleanup(void) {
    for (int i = 0; i < FD_SETSIZE; i++) {
        if (tls_sessions[i]) {
            SSL_shutdown(tls_sessions[i]);
            SSL_free(tls_sessions[i]);
            tls_sessions[i] = NULL;
        }
    }
    if (g_ssl_ctx) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
    }
}

#else /* !LATTICE_HAS_TLS */

/* ── Stubs when built without OpenSSL ── */

#include <stdlib.h>
#include <string.h>

static char *no_tls_err(void) {
    return strdup("TLS not available (built without OpenSSL)");
}

int net_tls_connect(const char *host, int port, char **err) {
    (void)host; (void)port;
    *err = no_tls_err();
    return -1;
}

char *net_tls_read(int fd, char **err) {
    (void)fd;
    *err = no_tls_err();
    return NULL;
}

char *net_tls_read_bytes(int fd, size_t count, char **err) {
    (void)fd; (void)count;
    *err = no_tls_err();
    return NULL;
}

bool net_tls_write(int fd, const char *data, size_t len, char **err) {
    (void)fd; (void)data; (void)len;
    *err = no_tls_err();
    return false;
}

void net_tls_close(int fd) {
    (void)fd;
}

bool net_tls_available(void) {
    return false;
}

void net_tls_cleanup(void) {
    /* nothing to do */
}

#endif /* LATTICE_HAS_TLS */
