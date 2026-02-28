#include "tls.h"
#include "net.h"

#ifdef LATTICE_HAS_TLS

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifndef _WIN32
#include <unistd.h>
#endif

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

    if (fd >= 0 && fd < FD_SETSIZE) tls_sessions[fd] = ssl;

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
    if (!buf) {
        *err = strdup("tls_read: out of memory");
        return NULL;
    }

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
    if (!buf) {
        *err = strdup("tls_read_bytes: out of memory");
        return NULL;
    }

    size_t total = 0;
    while (total < count) {
        int n = SSL_read(tls_sessions[fd], buf + total, (int)(count - total));
        if (n <= 0) break; /* EOF or error */
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

bool net_tls_available(void) { return true; }

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

#elif defined(LATTICE_TLS_SCHANNEL)

/* ══════════════════════════════════════════════════════════════════════════
 * Windows Schannel TLS backend
 * Uses SSPI/Schannel — Windows native TLS with OS certificate store.
 * ══════════════════════════════════════════════════════════════════════════ */

#define SECURITY_WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <security.h>
#include <schannel.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "win32_compat.h"

/* ── Per-fd Schannel session tracking ── */

#define SCHAN_MAX_FDS 1024

typedef struct {
    CtxtHandle ctx;
    CredHandle cred;
    int active;
    /* Decrypted leftover buffer (partial reads from DecryptMessage) */
    char *extra_buf;
    size_t extra_len;
    /* Stream sizes for EncryptMessage */
    SecPkgContext_StreamSizes sizes;
} SchanSession;

static SchanSession schan_sessions[SCHAN_MAX_FDS];

/* ── Helpers ── */

static char *schan_error(const char *prefix, SECURITY_STATUS ss) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s: SSPI error 0x%08lX", prefix, (unsigned long)ss);
    return strdup(buf);
}

/* Raw socket send/recv wrappers */
static int schan_send_raw(SOCKET s, const void *data, int len) {
    int total = 0;
    while (total < len) {
        int n = send(s, (const char *)data + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return total;
}

static int schan_recv_raw(SOCKET s, void *buf, int len) { return recv(s, (char *)buf, len, 0); }

/* ── tls_connect ── */

int net_tls_connect(const char *host, int port, char **err) {
    win32_net_init();

    /* TCP connect using the existing net layer */
    int fd = net_tcp_connect(host, port, err);
    if (fd < 0) return -1;

    if (fd >= SCHAN_MAX_FDS) {
        *err = strdup("tls_connect: fd exceeds session table");
        net_tcp_close(fd);
        return -1;
    }

    SOCKET sock = (SOCKET)fd;
    SECURITY_STATUS ss;

    /* Acquire Schannel credentials */
    SCHANNEL_CRED scred;
    memset(&scred, 0, sizeof(scred));
    scred.dwVersion = SCHANNEL_CRED_VERSION;
    scred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT;
    scred.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;

    CredHandle cred;
    TimeStamp ts;
    ss = AcquireCredentialsHandleA(NULL, (SEC_CHAR *)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &scred, NULL, NULL,
                                   &cred, &ts);
    if (ss != SEC_E_OK) {
        *err = schan_error("tls_connect: AcquireCredentialsHandle", ss);
        net_tcp_close(fd);
        return -1;
    }

    /* TLS handshake loop */
    CtxtHandle ctx;
    int have_ctx = 0;
    DWORD ctx_req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                    ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

    SecBuffer out_buf = {0, SECBUFFER_TOKEN, NULL};
    SecBufferDesc out_desc = {SECBUFFER_VERSION, 1, &out_buf};

    /* Initial call with empty input */
    ss = InitializeSecurityContextA(&cred, NULL, (SEC_CHAR *)host, ctx_req, 0, 0, NULL, 0, &ctx, &out_desc, &ctx_req,
                                    &ts);

    if (ss != SEC_I_CONTINUE_NEEDED && ss != SEC_E_OK) {
        *err = schan_error("tls_connect: initial ISC", ss);
        FreeCredentialsHandle(&cred);
        net_tcp_close(fd);
        return -1;
    }
    have_ctx = 1;

    /* Send initial token */
    if (out_buf.cbBuffer > 0 && out_buf.pvBuffer) {
        if (schan_send_raw(sock, out_buf.pvBuffer, (int)out_buf.cbBuffer) < 0) {
            *err = strdup("tls_connect: send failed");
            FreeContextBuffer(out_buf.pvBuffer);
            DeleteSecurityContext(&ctx);
            FreeCredentialsHandle(&cred);
            net_tcp_close(fd);
            return -1;
        }
        FreeContextBuffer(out_buf.pvBuffer);
    }

    /* Handshake read/write loop */
    char *hs_buf = malloc(65536);
    int hs_len = 0;

    while (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_INCOMPLETE_MESSAGE) {
        if (ss != SEC_E_INCOMPLETE_MESSAGE) hs_len = 0;

        int n = schan_recv_raw(sock, hs_buf + hs_len, 65536 - hs_len);
        if (n <= 0) {
            *err = strdup("tls_connect: recv failed during handshake");
            free(hs_buf);
            DeleteSecurityContext(&ctx);
            FreeCredentialsHandle(&cred);
            net_tcp_close(fd);
            return -1;
        }
        hs_len += n;

        SecBuffer in_bufs[2];
        in_bufs[0].BufferType = SECBUFFER_TOKEN;
        in_bufs[0].cbBuffer = (unsigned long)hs_len;
        in_bufs[0].pvBuffer = hs_buf;
        in_bufs[1].BufferType = SECBUFFER_EMPTY;
        in_bufs[1].cbBuffer = 0;
        in_bufs[1].pvBuffer = NULL;
        SecBufferDesc in_desc = {SECBUFFER_VERSION, 2, in_bufs};

        out_buf.cbBuffer = 0;
        out_buf.pvBuffer = NULL;
        out_buf.BufferType = SECBUFFER_TOKEN;
        out_desc.cBuffers = 1;
        out_desc.pBuffers = &out_buf;

        ss = InitializeSecurityContextA(&cred, &ctx, (SEC_CHAR *)host, ctx_req, 0, 0, &in_desc, 0, NULL, &out_desc,
                                        &ctx_req, &ts);

        if (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED) {
            /* Send any output token */
            if (out_buf.cbBuffer > 0 && out_buf.pvBuffer) {
                if (schan_send_raw(sock, out_buf.pvBuffer, (int)out_buf.cbBuffer) < 0) {
                    *err = strdup("tls_connect: send failed during handshake");
                    FreeContextBuffer(out_buf.pvBuffer);
                    free(hs_buf);
                    DeleteSecurityContext(&ctx);
                    FreeCredentialsHandle(&cred);
                    net_tcp_close(fd);
                    return -1;
                }
                FreeContextBuffer(out_buf.pvBuffer);
            }
            /* Handle extra data from the server */
            if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer > 0) {
                memmove(hs_buf, hs_buf + hs_len - in_bufs[1].cbBuffer, in_bufs[1].cbBuffer);
                hs_len = (int)in_bufs[1].cbBuffer;
            } else {
                hs_len = 0;
            }
        } else if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            /* Need more data — continue reading */
        } else {
            if (out_buf.pvBuffer) FreeContextBuffer(out_buf.pvBuffer);
            *err = schan_error("tls_connect: handshake failed", ss);
            free(hs_buf);
            DeleteSecurityContext(&ctx);
            FreeCredentialsHandle(&cred);
            net_tcp_close(fd);
            return -1;
        }
    }

    /* Store session, including any leftover handshake data */
    SchanSession *sess = &schan_sessions[fd];
    memset(sess, 0, sizeof(*sess));
    sess->ctx = ctx;
    sess->cred = cred;
    sess->active = 1;

    /* Query stream sizes for EncryptMessage */
    QueryContextAttributes(&ctx, SECPKG_ATTR_STREAM_SIZES, &sess->sizes);

    /* Save any extra data after handshake */
    if (hs_len > 0) {
        sess->extra_buf = malloc((size_t)hs_len);
        memcpy(sess->extra_buf, hs_buf, (size_t)hs_len);
        sess->extra_len = (size_t)hs_len;
    }

    free(hs_buf);
    return fd;
}

/* ── tls_read ── */

#define SCHAN_READ_BUF 65536

char *net_tls_read(int fd, char **err) {
    if (fd < 0 || fd >= SCHAN_MAX_FDS || !schan_sessions[fd].active) {
        *err = strdup("tls_read: not a TLS socket");
        return NULL;
    }

    SchanSession *sess = &schan_sessions[fd];
    SOCKET sock = (SOCKET)fd;

    /* If we have leftover decrypted data from a previous read, return that */
    if (sess->extra_buf && sess->extra_len > 0) { /* Try to decrypt the extra buffer first */
    }

    char *iobuf = malloc(SCHAN_READ_BUF);
    int iolen = 0;

    /* Copy any extra (undecrypted) data */
    if (sess->extra_buf && sess->extra_len > 0) {
        memcpy(iobuf, sess->extra_buf, sess->extra_len);
        iolen = (int)sess->extra_len;
        free(sess->extra_buf);
        sess->extra_buf = NULL;
        sess->extra_len = 0;
    }

    for (;;) {
        if (iolen == 0 || 1) {
            /* Only recv if we need more data (or always try on first pass) */
            if (iolen == 0) {
                int n = schan_recv_raw(sock, iobuf + iolen, SCHAN_READ_BUF - iolen);
                if (n <= 0) {
                    /* EOF */
                    free(iobuf);
                    char *empty = malloc(1);
                    empty[0] = '\0';
                    return empty;
                }
                iolen += n;
            }
        }

        SecBuffer bufs[4];
        bufs[0].BufferType = SECBUFFER_DATA;
        bufs[0].cbBuffer = (unsigned long)iolen;
        bufs[0].pvBuffer = iobuf;
        bufs[1].BufferType = SECBUFFER_EMPTY;
        bufs[1].cbBuffer = 0;
        bufs[1].pvBuffer = NULL;
        bufs[2].BufferType = SECBUFFER_EMPTY;
        bufs[2].cbBuffer = 0;
        bufs[2].pvBuffer = NULL;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        bufs[3].cbBuffer = 0;
        bufs[3].pvBuffer = NULL;
        SecBufferDesc desc = {SECBUFFER_VERSION, 4, bufs};

        SECURITY_STATUS ss = DecryptMessage(&sess->ctx, &desc, 0, NULL);

        if (ss == SEC_E_OK) {
            /* Find the DATA and EXTRA buffers */
            char *data = NULL;
            int data_len = 0;
            for (int i = 0; i < 4; i++) {
                if (bufs[i].BufferType == SECBUFFER_DATA) {
                    data = (char *)bufs[i].pvBuffer;
                    data_len = (int)bufs[i].cbBuffer;
                } else if (bufs[i].BufferType == SECBUFFER_EXTRA && bufs[i].cbBuffer > 0) {
                    sess->extra_buf = malloc(bufs[i].cbBuffer);
                    memcpy(sess->extra_buf, bufs[i].pvBuffer, bufs[i].cbBuffer);
                    sess->extra_len = bufs[i].cbBuffer;
                }
            }
            char *result = malloc((size_t)data_len + 1);
            if (data && data_len > 0) memcpy(result, data, (size_t)data_len);
            result[data_len] = '\0';
            free(iobuf);
            return result;
        } else if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            /* Need more data */
            int n = schan_recv_raw(sock, iobuf + iolen, SCHAN_READ_BUF - iolen);
            if (n <= 0) {
                free(iobuf);
                char *empty = malloc(1);
                empty[0] = '\0';
                return empty;
            }
            iolen += n;
            continue;
        } else if (ss == SEC_I_CONTEXT_EXPIRED) {
            /* Server closed TLS */
            free(iobuf);
            char *empty = malloc(1);
            empty[0] = '\0';
            return empty;
        } else {
            free(iobuf);
            *err = schan_error("tls_read: DecryptMessage", ss);
            return NULL;
        }
    }
}

/* ── tls_read_bytes ── */

char *net_tls_read_bytes(int fd, size_t count, char **err) {
    char *result = malloc(count + 1);
    if (!result) {
        *err = strdup("tls_read_bytes: out of memory");
        return NULL;
    }
    size_t total = 0;
    while (total < count) {
        char *chunk = net_tls_read(fd, err);
        if (!chunk) {
            free(result);
            return NULL;
        }
        size_t clen = strlen(chunk);
        if (clen == 0) {
            free(chunk);
            break; /* EOF */
        }
        size_t take = (total + clen > count) ? count - total : clen;
        memcpy(result + total, chunk, take);
        total += take;

        /* If we read more than needed, push extra back */
        if (take < clen) {
            SchanSession *sess = &schan_sessions[fd];
            size_t leftover = clen - take;
            char *newextra = malloc(sess->extra_len + leftover);
            memcpy(newextra, chunk + take, leftover);
            if (sess->extra_buf) {
                memcpy(newextra + leftover, sess->extra_buf, sess->extra_len);
                free(sess->extra_buf);
            }
            sess->extra_buf = newextra;
            sess->extra_len += leftover;
        }
        free(chunk);
    }
    result[total] = '\0';
    return result;
}

/* ── tls_write ── */

bool net_tls_write(int fd, const char *data, size_t len, char **err) {
    if (fd < 0 || fd >= SCHAN_MAX_FDS || !schan_sessions[fd].active) {
        *err = strdup("tls_write: not a TLS socket");
        return false;
    }

    SchanSession *sess = &schan_sessions[fd];
    SOCKET sock = (SOCKET)fd;
    size_t max_msg = sess->sizes.cbMaximumMessage;

    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > max_msg) chunk = max_msg;

        size_t total_size = sess->sizes.cbHeader + chunk + sess->sizes.cbTrailer;
        char *msgbuf = malloc(total_size);

        SecBuffer bufs[3];
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].cbBuffer = sess->sizes.cbHeader;
        bufs[0].pvBuffer = msgbuf;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].cbBuffer = (unsigned long)chunk;
        bufs[1].pvBuffer = msgbuf + sess->sizes.cbHeader;
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].cbBuffer = sess->sizes.cbTrailer;
        bufs[2].pvBuffer = msgbuf + sess->sizes.cbHeader + chunk;
        SecBufferDesc desc = {SECBUFFER_VERSION, 3, bufs};

        memcpy(bufs[1].pvBuffer, data + offset, chunk);

        SECURITY_STATUS ss = EncryptMessage(&sess->ctx, 0, &desc, 0);
        if (ss != SEC_E_OK) {
            free(msgbuf);
            *err = schan_error("tls_write: EncryptMessage", ss);
            return false;
        }

        int send_len = (int)(bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer);
        if (schan_send_raw(sock, msgbuf, send_len) < 0) {
            free(msgbuf);
            *err = strdup("tls_write: send failed");
            return false;
        }

        free(msgbuf);
        offset += chunk;
    }
    return true;
}

/* ── tls_close ── */

void net_tls_close(int fd) {
    if (fd >= 0 && fd < SCHAN_MAX_FDS && schan_sessions[fd].active) {
        SchanSession *sess = &schan_sessions[fd];

        /* Send TLS shutdown */
        DWORD type = SCHANNEL_SHUTDOWN;
        SecBuffer shutbuf = {sizeof(type), SECBUFFER_TOKEN, &type};
        SecBufferDesc shutdesc = {SECBUFFER_VERSION, 1, &shutbuf};
        ApplyControlToken(&sess->ctx, &shutdesc);

        SecBuffer out = {0, SECBUFFER_TOKEN, NULL};
        SecBufferDesc outdesc = {SECBUFFER_VERSION, 1, &out};
        DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                      ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
        TimeStamp ts;
        SECURITY_STATUS ss = InitializeSecurityContextA(&sess->cred, &sess->ctx, NULL, flags, 0, 0, NULL, 0, NULL,
                                                        &outdesc, &flags, &ts);
        if (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED) {
            if (out.cbBuffer > 0 && out.pvBuffer) {
                send((SOCKET)fd, (const char *)out.pvBuffer, (int)out.cbBuffer, 0);
                FreeContextBuffer(out.pvBuffer);
            }
        }

        DeleteSecurityContext(&sess->ctx);
        FreeCredentialsHandle(&sess->cred);
        free(sess->extra_buf);
        memset(sess, 0, sizeof(*sess));
    }
    net_tcp_close(fd);
}

/* ── tls_available ── */

bool net_tls_available(void) { return true; }

/* ── tls_cleanup ── */

void net_tls_cleanup(void) {
    for (int i = 0; i < SCHAN_MAX_FDS; i++) {
        if (schan_sessions[i].active) {
            SchanSession *sess = &schan_sessions[i];
            DeleteSecurityContext(&sess->ctx);
            FreeCredentialsHandle(&sess->cred);
            free(sess->extra_buf);
            memset(sess, 0, sizeof(*sess));
        }
    }
}

#else /* !LATTICE_HAS_TLS */

/* ── Stubs when built without OpenSSL ── */

#include <stdlib.h>
#include <string.h>

static char *no_tls_err(void) { return strdup("TLS not available (built without OpenSSL)"); }

int net_tls_connect(const char *host, int port, char **err) {
    (void)host;
    (void)port;
    *err = no_tls_err();
    return -1;
}

char *net_tls_read(int fd, char **err) {
    (void)fd;
    *err = no_tls_err();
    return NULL;
}

char *net_tls_read_bytes(int fd, size_t count, char **err) {
    (void)fd;
    (void)count;
    *err = no_tls_err();
    return NULL;
}

bool net_tls_write(int fd, const char *data, size_t len, char **err) {
    (void)fd;
    (void)data;
    (void)len;
    *err = no_tls_err();
    return false;
}

void net_tls_close(int fd) { (void)fd; }

bool net_tls_available(void) { return false; }

void net_tls_cleanup(void) { /* nothing to do */ }

#endif /* LATTICE_HAS_TLS */
