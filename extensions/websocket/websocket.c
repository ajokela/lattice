/*
 * Lattice WebSocket Extension
 *
 * Client: connect, connect_auto (auto-reconnect)
 * Server: listen, accept, server_close
 * Shared: send, recv, recv_timeout, send_binary, close, status, ping,
 *         set_timeout
 *
 * Uses raw TCP sockets and OpenSSL for SHA1 hashing during handshakes.
 * Implements RFC 6455 framing with fragmented message reassembly,
 * ping/pong handling, and proper masking (client masks, server does not).
 */

/* Enable POSIX extensions for getaddrinfo, nanosleep on Linux with -std=c11 */
#define _POSIX_C_SOURCE 200809L

#include "lattice_ext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <time.h>

/* Forward declare the init function (exported symbol) */
void lat_ext_init(LatExtContext *ctx);

/* ── Connection table ── */

#define MAX_CONNECTIONS 64
#define MAX_SERVERS     16

/* Maximum single frame payload: 64 MB */
#define MAX_FRAME_PAYLOAD (64 * 1024 * 1024)

/* Maximum reassembled fragmented message: 64 MB */
#define MAX_FRAGMENT_TOTAL (64 * 1024 * 1024)

typedef enum { WS_STATE_CLOSED, WS_STATE_CONNECTED } WsState;

typedef enum { WS_ROLE_CLIENT, WS_ROLE_SERVER } WsRole;

typedef struct {
    int fd;
    WsState state;
    WsRole role;
    int recv_timeout_sec; /* 0 = blocking */
} WsConn;

/* Server listener sockets */
typedef struct {
    int fd;
    int port;
    int active;
} WsServer;

static WsConn connections[MAX_CONNECTIONS];
static int conn_count = 0;
static WsServer servers[MAX_SERVERS];
static int server_count = 0;
static int initialized = 0;
static int sigpipe_suppressed = 0;

static void ensure_init(void) {
    if (!initialized) {
        int i;
        for (i = 0; i < MAX_CONNECTIONS; i++) {
            connections[i].fd = -1;
            connections[i].state = WS_STATE_CLOSED;
            connections[i].role = WS_ROLE_CLIENT;
            connections[i].recv_timeout_sec = 0;
        }
        for (i = 0; i < MAX_SERVERS; i++) {
            servers[i].fd = -1;
            servers[i].port = 0;
            servers[i].active = 0;
        }
        initialized = 1;
        srand((unsigned int)time(NULL));

        if (!sigpipe_suppressed) {
            signal(SIGPIPE, SIG_IGN);
            sigpipe_suppressed = 1;
        }
    }
}

static int conn_alloc(int fd, WsRole role) {
    int i;
    for (i = 0; i < conn_count; i++) {
        if (connections[i].state == WS_STATE_CLOSED && connections[i].fd == -1) {
            connections[i].fd = fd;
            connections[i].state = WS_STATE_CONNECTED;
            connections[i].role = role;
            connections[i].recv_timeout_sec = 0;
            return i;
        }
    }
    if (conn_count >= MAX_CONNECTIONS) return -1;
    connections[conn_count].fd = fd;
    connections[conn_count].state = WS_STATE_CONNECTED;
    connections[conn_count].role = role;
    connections[conn_count].recv_timeout_sec = 0;
    return conn_count++;
}

static WsConn *conn_get(int64_t id) {
    if (id < 0 || id >= conn_count) return NULL;
    if (connections[id].fd == -1) return NULL;
    return &connections[id];
}

static void conn_release(int64_t id) {
    if (id >= 0 && id < conn_count) {
        connections[id].fd = -1;
        connections[id].state = WS_STATE_CLOSED;
    }
}

static int server_alloc(int fd, int port) {
    int i;
    for (i = 0; i < server_count; i++) {
        if (!servers[i].active) {
            servers[i].fd = fd;
            servers[i].port = port;
            servers[i].active = 1;
            return i;
        }
    }
    if (server_count >= MAX_SERVERS) return -1;
    servers[server_count].fd = fd;
    servers[server_count].port = port;
    servers[server_count].active = 1;
    return server_count++;
}

static WsServer *server_get(int64_t id) {
    if (id < 0 || id >= server_count) return NULL;
    if (!servers[id].active) return NULL;
    return &servers[id];
}

static void server_release(int64_t id) {
    if (id >= 0 && id < server_count) {
        servers[id].fd = -1;
        servers[id].active = 0;
    }
}

/* ── Base64 encoding ── */

static char *base64_encode(const unsigned char *input, int length) {
    BIO *bmem, *b64;
    BUF_MEM *bptr;
    char *result;

    b64 = BIO_new(BIO_f_base64());
    bmem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, bmem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, input, length);
    (void)BIO_flush(b64);
    BIO_get_mem_ptr(b64, &bptr);

    result = malloc(bptr->length + 1);
    memcpy(result, bptr->data, bptr->length);
    result[bptr->length] = '\0';

    BIO_free_all(b64);
    return result;
}

/* ── URL parsing ── */

typedef struct {
    char host[256];
    int port;
    char path[1024];
} WsUrl;

static int parse_ws_url(const char *url, WsUrl *out) {
    const char *p = url;
    const char *host_start;
    const char *host_end;
    const char *port_start;
    size_t host_len;

    /* Skip scheme */
    if (strncmp(p, "ws://", 5) == 0) {
        p += 5;
    } else if (strncmp(p, "WS://", 5) == 0) {
        p += 5;
    } else {
        return -1;
    }

    /* Parse host */
    host_start = p;
    host_end = NULL;
    port_start = NULL;
    out->port = 80; /* default */

    while (*p && *p != '/' && *p != ':') p++;

    host_end = p;
    host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) return -1;
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    /* Parse port */
    if (*p == ':') {
        p++;
        port_start = p;
        out->port = 0;
        while (*p >= '0' && *p <= '9') {
            out->port = out->port * 10 + (*p - '0');
            p++;
        }
        if (p == port_start || out->port <= 0 || out->port > 65535) return -1;
    }

    /* Parse path */
    if (*p == '/') {
        size_t path_len = strlen(p);
        if (path_len >= sizeof(out->path)) return -1;
        memcpy(out->path, p, path_len + 1);
    } else {
        out->path[0] = '/';
        out->path[1] = '\0';
    }

    return 0;
}

/* ── TCP connect ── */

static int tcp_connect(const char *host, int port) {
    struct addrinfo hints, *res, *rp;
    char port_str[16];
    int fd;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) { return -1; }

    fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) continue;

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) { break; /* success */ }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

/* ── TCP listen ── */

static int tcp_listen(int port) {
    int fd;
    int opt = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/* ── Socket I/O helpers ── */

static int send_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = send(fd, p, remaining, 0);
        if (n <= 0) return -1;
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len) {
    unsigned char *p = (unsigned char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = recv(fd, p, remaining, 0);
        if (n <= 0) return -1;
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/* ── WebSocket handshake (client-side) ── */

static const char *WS_MAGIC = "258EAFA5-E914-47DA-95CA-5AB5B11731C5";

static int ws_client_handshake(int fd, const char *host, int port, const char *path) {
    unsigned char key_bytes[16];
    char *key_b64;
    char request[2048];
    char response[4096];
    char expected_accept[512];
    unsigned char sha1_hash[SHA_DIGEST_LENGTH];
    char *accept_b64;
    ssize_t n;
    int total_read;
    int i;

    /* Generate random 16-byte key */
    for (i = 0; i < 16; i++) { key_bytes[i] = (unsigned char)(rand() & 0xFF); }
    key_b64 = base64_encode(key_bytes, 16);

    /* Compute expected Sec-WebSocket-Accept: SHA1(key + magic), base64 encoded */
    snprintf(expected_accept, sizeof(expected_accept), "%s%s", key_b64, WS_MAGIC);
    SHA1((const unsigned char *)expected_accept, strlen(expected_accept), sha1_hash);
    accept_b64 = base64_encode(sha1_hash, SHA_DIGEST_LENGTH);

    /* Build HTTP upgrade request */
    if (port == 80) {
        snprintf(request, sizeof(request),
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Key: %s\r\n"
                 "Sec-WebSocket-Version: 13\r\n"
                 "\r\n",
                 path, host, key_b64);
    } else {
        snprintf(request, sizeof(request),
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s:%d\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Key: %s\r\n"
                 "Sec-WebSocket-Version: 13\r\n"
                 "\r\n",
                 path, host, port, key_b64);
    }

    free(key_b64);

    /* Send request */
    if (send_all(fd, request, strlen(request)) != 0) {
        free(accept_b64);
        return -1;
    }

    /* Read response (look for end of HTTP headers) */
    total_read = 0;
    memset(response, 0, sizeof(response));

    while (total_read < (int)(sizeof(response) - 1)) {
        n = recv(fd, response + total_read, 1, 0);
        if (n <= 0) {
            free(accept_b64);
            return -1;
        }
        total_read += (int)n;

        /* Check for \r\n\r\n end of headers */
        if (total_read >= 4 && response[total_read - 4] == '\r' && response[total_read - 3] == '\n' &&
            response[total_read - 2] == '\r' && response[total_read - 1] == '\n') {
            break;
        }
    }

    response[total_read] = '\0';

    /* Verify 101 status */
    if (strncmp(response, "HTTP/1.1 101", 12) != 0 && strncmp(response, "HTTP/1.0 101", 12) != 0) {
        free(accept_b64);
        return -1;
    }

    /* Verify Sec-WebSocket-Accept header */
    {
        const char *accept_hdr = strstr(response, "Sec-WebSocket-Accept: ");
        if (!accept_hdr) accept_hdr = strstr(response, "sec-websocket-accept: ");
        if (accept_hdr) {
            accept_hdr += strlen("Sec-WebSocket-Accept: ");
            if (strncmp(accept_hdr, accept_b64, strlen(accept_b64)) != 0) {
                free(accept_b64);
                return -1;
            }
        }
    }

    free(accept_b64);
    return 0;
}

/* ── WebSocket server-side handshake ── */

static int ws_server_handshake(int fd) {
    char request[4096];
    ssize_t n;
    int total_read = 0;
    const char *key_start;
    const char *key_end;
    char client_key[256];
    char concat[512];
    unsigned char sha1_hash[SHA_DIGEST_LENGTH];
    char *accept_b64;
    char response[1024];

    memset(request, 0, sizeof(request));

    /* Read the HTTP upgrade request byte-by-byte until \r\n\r\n */
    while (total_read < (int)(sizeof(request) - 1)) {
        n = recv(fd, request + total_read, 1, 0);
        if (n <= 0) return -1;
        total_read += (int)n;

        if (total_read >= 4 && request[total_read - 4] == '\r' && request[total_read - 3] == '\n' &&
            request[total_read - 2] == '\r' && request[total_read - 1] == '\n') {
            break;
        }
    }
    request[total_read] = '\0';

    /* Verify it looks like a WebSocket upgrade request */
    if (strstr(request, "Upgrade: websocket") == NULL && strstr(request, "Upgrade: WebSocket") == NULL &&
        strstr(request, "upgrade: websocket") == NULL) {
        return -1;
    }

    /* Extract Sec-WebSocket-Key */
    key_start = strstr(request, "Sec-WebSocket-Key: ");
    if (!key_start) key_start = strstr(request, "sec-websocket-key: ");
    if (!key_start) return -1;

    key_start += strlen("Sec-WebSocket-Key: ");
    key_end = strstr(key_start, "\r\n");
    if (!key_end) return -1;

    {
        size_t key_len = (size_t)(key_end - key_start);
        if (key_len == 0 || key_len >= sizeof(client_key)) return -1;
        memcpy(client_key, key_start, key_len);
        client_key[key_len] = '\0';
    }

    /* Compute Sec-WebSocket-Accept: SHA1(key + magic), base64 */
    snprintf(concat, sizeof(concat), "%s%s", client_key, WS_MAGIC);
    SHA1((const unsigned char *)concat, strlen(concat), sha1_hash);
    accept_b64 = base64_encode(sha1_hash, SHA_DIGEST_LENGTH);

    /* Send HTTP 101 response */
    snprintf(response, sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n"
             "\r\n",
             accept_b64);

    free(accept_b64);

    if (send_all(fd, response, strlen(response)) != 0) { return -1; }

    return 0;
}

/* ── WebSocket framing ── */

/* Opcodes */
#define WS_OP_CONTINUATION 0x0
#define WS_OP_TEXT         0x1
#define WS_OP_BINARY       0x2
#define WS_OP_CLOSE        0x8
#define WS_OP_PING         0x9
#define WS_OP_PONG         0xA

/*
 * Send a WebSocket frame. Per RFC 6455:
 *   - Client-to-server frames MUST be masked
 *   - Server-to-client frames MUST NOT be masked
 */
static int ws_send_frame_ex(int fd, int opcode, const unsigned char *payload, size_t len, int use_mask) {
    unsigned char header[14];
    size_t header_len = 0;
    unsigned char mask_key[4];

    /* FIN bit + opcode */
    header[0] = (unsigned char)(0x80 | (opcode & 0x0F));
    header_len = 1;

    /* Payload length, with mask bit set only if masking */
    {
        unsigned char mask_bit = use_mask ? 0x80 : 0x00;
        if (len <= 125) {
            header[1] = (unsigned char)(mask_bit | len);
            header_len = 2;
        } else if (len <= 65535) {
            header[1] = (unsigned char)(mask_bit | 126);
            header[2] = (unsigned char)((len >> 8) & 0xFF);
            header[3] = (unsigned char)(len & 0xFF);
            header_len = 4;
        } else {
            header[1] = (unsigned char)(mask_bit | 127);
            header[2] = 0;
            header[3] = 0;
            header[4] = 0;
            header[5] = 0;
            header[6] = (unsigned char)((len >> 24) & 0xFF);
            header[7] = (unsigned char)((len >> 16) & 0xFF);
            header[8] = (unsigned char)((len >> 8) & 0xFF);
            header[9] = (unsigned char)(len & 0xFF);
            header_len = 10;
        }
    }

    if (use_mask) {
        size_t i;
        /* Generate mask key */
        for (i = 0; i < 4; i++) { mask_key[i] = (unsigned char)(rand() & 0xFF); }

        /* Append mask key to header */
        memcpy(header + header_len, mask_key, 4);
        header_len += 4;

        /* Send header */
        if (send_all(fd, header, header_len) != 0) return -1;

        /* Mask and send payload */
        if (len > 0) {
            unsigned char *masked = malloc(len);
            int rc;
            if (!masked) return -1;
            for (i = 0; i < len; i++) { masked[i] = payload[i] ^ mask_key[i % 4]; }
            rc = send_all(fd, masked, len);
            free(masked);
            if (rc != 0) return -1;
        }
    } else {
        /* Server: send header then unmasked payload */
        if (send_all(fd, header, header_len) != 0) return -1;
        if (len > 0) {
            if (send_all(fd, payload, len) != 0) return -1;
        }
    }

    return 0;
}

/* Send using the correct masking for the connection role */
static int ws_send_frame_conn(WsConn *conn, int opcode, const unsigned char *payload, size_t len) {
    int use_mask = (conn->role == WS_ROLE_CLIENT) ? 1 : 0;
    return ws_send_frame_ex(conn->fd, opcode, payload, len, use_mask);
}

typedef struct {
    int opcode;
    unsigned char *payload;
    size_t payload_len;
    int fin;
} WsFrame;

static int ws_recv_frame(int fd, WsFrame *frame) {
    unsigned char hdr[2];
    uint64_t payload_len;
    unsigned char mask_key[4];
    int masked;
    size_t i;

    memset(frame, 0, sizeof(*frame));

    /* Read first 2 bytes */
    if (recv_all(fd, hdr, 2) != 0) return -1;

    frame->fin = (hdr[0] >> 7) & 1;
    frame->opcode = hdr[0] & 0x0F;
    masked = (hdr[1] >> 7) & 1;
    payload_len = hdr[1] & 0x7F;

    /* Extended payload length */
    if (payload_len == 126) {
        unsigned char ext[2];
        if (recv_all(fd, ext, 2) != 0) return -1;
        payload_len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        unsigned char ext[8];
        if (recv_all(fd, ext, 8) != 0) return -1;
        payload_len = 0;
        for (i = 0; i < 8; i++) { payload_len = (payload_len << 8) | ext[i]; }
    }

    /* Read mask key if present */
    if (masked) {
        if (recv_all(fd, mask_key, 4) != 0) return -1;
    }

    /* Read payload */
    frame->payload_len = (size_t)payload_len;
    if (payload_len > 0) {
        /* Sanity check */
        if (payload_len > MAX_FRAME_PAYLOAD) return -1;

        frame->payload = malloc((size_t)payload_len);
        if (!frame->payload) return -1;
        if (recv_all(fd, frame->payload, (size_t)payload_len) != 0) {
            free(frame->payload);
            frame->payload = NULL;
            return -1;
        }

        /* Unmask if needed */
        if (masked) {
            for (i = 0; i < (size_t)payload_len; i++) { frame->payload[i] ^= mask_key[i % 4]; }
        }
    }

    return 0;
}

static void ws_frame_free(WsFrame *frame) {
    free(frame->payload);
    frame->payload = NULL;
    frame->payload_len = 0;
}

/*
 * Receive a complete WebSocket message with fragment reassembly and
 * automatic ping/pong handling.
 *
 * Handles:
 * - Fragmented messages (continuation frames)
 * - Interleaved control frames (ping/pong/close) during fragments
 * - Automatic pong replies to pings
 * - Close frame detection
 *
 * Returns: heap-allocated message string (caller frees), or NULL on close/error.
 * Sets *out_opcode to the message opcode (text/binary).
 * Sets *out_len to the message byte length.
 */
static unsigned char *ws_recv_message(WsConn *conn, int *out_opcode, size_t *out_len) {
    unsigned char *frag_buf = NULL;
    size_t frag_len = 0;
    size_t frag_cap = 0;
    int msg_opcode = -1;

    for (;;) {
        WsFrame frame;
        if (ws_recv_frame(conn->fd, &frame) != 0) {
            free(frag_buf);
            conn->state = WS_STATE_CLOSED;
            return NULL;
        }

        /* Handle control frames (can arrive between data fragments) */
        if (frame.opcode >= 0x8) {
            switch (frame.opcode) {
                case WS_OP_PING:
                    /* Reply with pong echoing the payload */
                    ws_send_frame_conn(conn, WS_OP_PONG, frame.payload, frame.payload_len);
                    ws_frame_free(&frame);
                    continue;

                case WS_OP_PONG:
                    /* Ignore unsolicited pongs */
                    ws_frame_free(&frame);
                    continue;

                case WS_OP_CLOSE:
                    /* Send close frame back if we haven't already */
                    ws_send_frame_conn(conn, WS_OP_CLOSE, frame.payload, frame.payload_len > 2 ? 2 : frame.payload_len);
                    ws_frame_free(&frame);
                    free(frag_buf);
                    conn->state = WS_STATE_CLOSED;
                    return NULL;

                default: ws_frame_free(&frame); continue;
            }
        }

        /* Data frame handling */
        if (frame.opcode == WS_OP_CONTINUATION) {
            /* Continuation of a fragmented message */
            if (msg_opcode < 0) {
                /* Got continuation without an initial frame -- protocol error */
                ws_frame_free(&frame);
                free(frag_buf);
                conn->state = WS_STATE_CLOSED;
                return NULL;
            }
        } else {
            /* New data frame (text or binary) */
            if (msg_opcode >= 0) {
                /* New data frame while fragments still pending -- protocol error */
                ws_frame_free(&frame);
                free(frag_buf);
                conn->state = WS_STATE_CLOSED;
                return NULL;
            }
            msg_opcode = frame.opcode;

            /* If FIN is set, this is a non-fragmented message */
            if (frame.fin) {
                *out_opcode = msg_opcode;
                *out_len = frame.payload_len;
                if (frame.payload_len == 0) {
                    ws_frame_free(&frame);
                    unsigned char *empty = malloc(1);
                    if (empty) empty[0] = '\0';
                    return empty;
                }
                /* Transfer ownership of frame payload */
                unsigned char *result = frame.payload;
                frame.payload = NULL;
                frame.payload_len = 0;
                return result;
            }
        }

        /* Accumulate fragment data */
        if (frag_len + frame.payload_len > MAX_FRAGMENT_TOTAL) {
            ws_frame_free(&frame);
            free(frag_buf);
            conn->state = WS_STATE_CLOSED;
            return NULL;
        }

        if (frag_len + frame.payload_len > frag_cap) {
            size_t new_cap = frag_cap ? frag_cap * 2 : 4096;
            while (new_cap < frag_len + frame.payload_len) new_cap *= 2;
            if (new_cap > MAX_FRAGMENT_TOTAL) new_cap = MAX_FRAGMENT_TOTAL;
            unsigned char *tmp = realloc(frag_buf, new_cap);
            if (!tmp) {
                ws_frame_free(&frame);
                free(frag_buf);
                conn->state = WS_STATE_CLOSED;
                return NULL;
            }
            frag_buf = tmp;
            frag_cap = new_cap;
        }

        if (frame.payload_len > 0) {
            memcpy(frag_buf + frag_len, frame.payload, frame.payload_len);
            frag_len += frame.payload_len;
        }
        ws_frame_free(&frame);

        /* If FIN, fragmented message is complete */
        if (frame.fin) {
            *out_opcode = msg_opcode;
            *out_len = frag_len;
            return frag_buf;
        }
    }
}

/* ── Extension functions ── */

/* ws.connect(url) -> Int (handle) */
static LatExtValue *ws_connect(LatExtValue **args, size_t argc) {
    WsUrl url;
    int fd, id;

    ensure_init();

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_STRING) {
        return lat_ext_error("ws.connect() expects a URL string (e.g. \"ws://host:port/path\")");
    }

    if (parse_ws_url(lat_ext_as_string(args[0]), &url) != 0) {
        return lat_ext_error("ws.connect: invalid WebSocket URL (expected ws://host[:port][/path])");
    }

    fd = tcp_connect(url.host, url.port);
    if (fd < 0) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "ws.connect: failed to connect to %s:%d", url.host, url.port);
        return lat_ext_error(errbuf);
    }

    if (ws_client_handshake(fd, url.host, url.port, url.path) != 0) {
        close(fd);
        return lat_ext_error("ws.connect: WebSocket handshake failed");
    }

    id = conn_alloc(fd, WS_ROLE_CLIENT);
    if (id < 0) {
        close(fd);
        return lat_ext_error("ws.connect: too many connections");
    }

    return lat_ext_int(id);
}

/*
 * ws.connect_auto(url, max_retries, retry_delay_ms) -> Int (handle)
 *
 * Auto-reconnection client connect. Attempts up to max_retries times
 * with retry_delay_ms milliseconds between attempts.
 */
static LatExtValue *ws_connect_auto(LatExtValue **args, size_t argc) {
    WsUrl url;
    int fd, id;
    int64_t max_retries = 3;
    int64_t retry_delay_ms = 1000;
    int attempt;

    ensure_init();

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_STRING) {
        return lat_ext_error("ws.connect_auto() expects (url: String [, max_retries: Int, retry_delay_ms: Int])");
    }

    if (argc >= 2 && lat_ext_type(args[1]) == LAT_EXT_INT) {
        max_retries = lat_ext_as_int(args[1]);
        if (max_retries < 0) max_retries = 0;
        if (max_retries > 100) max_retries = 100;
    }
    if (argc >= 3 && lat_ext_type(args[2]) == LAT_EXT_INT) {
        retry_delay_ms = lat_ext_as_int(args[2]);
        if (retry_delay_ms < 0) retry_delay_ms = 0;
        if (retry_delay_ms > 60000) retry_delay_ms = 60000;
    }

    if (parse_ws_url(lat_ext_as_string(args[0]), &url) != 0) {
        return lat_ext_error("ws.connect_auto: invalid WebSocket URL");
    }

    for (attempt = 0; attempt <= (int)max_retries; attempt++) {
        if (attempt > 0 && retry_delay_ms > 0) {
            /* Sleep before retry */
            struct timespec ts;
            ts.tv_sec = (time_t)(retry_delay_ms / 1000);
            ts.tv_nsec = (long)((retry_delay_ms % 1000) * 1000000L);
            nanosleep(&ts, NULL);
        }

        fd = tcp_connect(url.host, url.port);
        if (fd < 0) continue;

        if (ws_client_handshake(fd, url.host, url.port, url.path) != 0) {
            close(fd);
            continue;
        }

        /* Success */
        id = conn_alloc(fd, WS_ROLE_CLIENT);
        if (id < 0) {
            close(fd);
            return lat_ext_error("ws.connect_auto: too many connections");
        }
        return lat_ext_int(id);
    }

    {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "ws.connect_auto: failed to connect to %s:%d after %d attempts", url.host,
                 url.port, (int)max_retries + 1);
        return lat_ext_error(errbuf);
    }
}

/* ws.close(handle) -> Nil */
static LatExtValue *ws_close(LatExtValue **args, size_t argc) {
    int64_t id;
    WsConn *conn;

    ensure_init();

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("ws.close() expects a connection handle (Int)");
    }

    id = lat_ext_as_int(args[0]);
    conn = conn_get(id);
    if (!conn) return lat_ext_error("ws.close: invalid connection handle");

    /* Send close frame (best effort) */
    ws_send_frame_conn(conn, WS_OP_CLOSE, NULL, 0);

    shutdown(conn->fd, SHUT_RDWR);
    close(conn->fd);
    conn_release(id);

    return lat_ext_nil();
}

/* ws.send(handle, message) -> Nil */
static LatExtValue *ws_send(LatExtValue **args, size_t argc) {
    int64_t id;
    WsConn *conn;
    const char *msg;

    ensure_init();

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_INT || lat_ext_type(args[1]) != LAT_EXT_STRING) {
        return lat_ext_error("ws.send() expects (handle: Int, message: String)");
    }

    id = lat_ext_as_int(args[0]);
    conn = conn_get(id);
    if (!conn) return lat_ext_error("ws.send: invalid connection handle");
    if (conn->state != WS_STATE_CONNECTED) return lat_ext_error("ws.send: connection is closed");

    msg = lat_ext_as_string(args[1]);
    if (ws_send_frame_conn(conn, WS_OP_TEXT, (const unsigned char *)msg, strlen(msg)) != 0) {
        conn->state = WS_STATE_CLOSED;
        return lat_ext_error("ws.send: failed to send message");
    }

    return lat_ext_nil();
}

/*
 * ws.recv(handle) -> String or Nil
 *
 * Blocks until a complete message (possibly reassembled from fragments) is
 * received. Automatically replies to pings. Returns Nil on connection close.
 */
static LatExtValue *ws_recv(LatExtValue **args, size_t argc) {
    int64_t id;
    WsConn *conn;
    int msg_opcode;
    size_t msg_len;
    unsigned char *msg_data;
    LatExtValue *result;

    ensure_init();

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("ws.recv() expects a connection handle (Int)");
    }

    id = lat_ext_as_int(args[0]);
    conn = conn_get(id);
    if (!conn) return lat_ext_error("ws.recv: invalid connection handle");
    if (conn->state != WS_STATE_CONNECTED) return lat_ext_nil();

    msg_data = ws_recv_message(conn, &msg_opcode, &msg_len);
    if (!msg_data) { return lat_ext_nil(); }

    /* Convert to null-terminated string for Lattice */
    {
        char *str = malloc(msg_len + 1);
        if (!str) {
            free(msg_data);
            return lat_ext_error("ws.recv: out of memory");
        }
        if (msg_len > 0) memcpy(str, msg_data, msg_len);
        str[msg_len] = '\0';
        free(msg_data);
        result = lat_ext_string(str);
        free(str);
    }
    return result;
}

/*
 * ws.recv_timeout(handle, timeout_ms) -> String or Nil
 *
 * Like recv, but returns Nil if no data arrives within timeout_ms milliseconds.
 */
static LatExtValue *ws_recv_timeout(LatExtValue **args, size_t argc) {
    int64_t id;
    WsConn *conn;
    int64_t timeout_ms;
    fd_set readfds;
    struct timeval tv;
    int ready;

    ensure_init();

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_INT || lat_ext_type(args[1]) != LAT_EXT_INT) {
        return lat_ext_error("ws.recv_timeout() expects (handle: Int, timeout_ms: Int)");
    }

    id = lat_ext_as_int(args[0]);
    conn = conn_get(id);
    if (!conn) return lat_ext_error("ws.recv_timeout: invalid connection handle");
    if (conn->state != WS_STATE_CONNECTED) return lat_ext_nil();

    timeout_ms = lat_ext_as_int(args[1]);
    if (timeout_ms < 0) timeout_ms = 0;

    /* Use select to wait for data with timeout */
    FD_ZERO(&readfds);
    FD_SET(conn->fd, &readfds);
    tv.tv_sec = (long)(timeout_ms / 1000);
    tv.tv_usec = (int)((timeout_ms % 1000) * 1000);

    ready = select(conn->fd + 1, &readfds, NULL, NULL, &tv);
    if (ready <= 0) {
        /* Timeout or error -- return Nil without closing */
        return lat_ext_nil();
    }

    /* Data available, delegate to full recv logic */
    {
        int msg_opcode;
        size_t msg_len;
        unsigned char *msg_data = ws_recv_message(conn, &msg_opcode, &msg_len);
        LatExtValue *result;

        if (!msg_data) return lat_ext_nil();

        {
            char *str = malloc(msg_len + 1);
            if (!str) {
                free(msg_data);
                return lat_ext_error("ws.recv_timeout: out of memory");
            }
            if (msg_len > 0) memcpy(str, msg_data, msg_len);
            str[msg_len] = '\0';
            free(msg_data);
            result = lat_ext_string(str);
            free(str);
        }
        return result;
    }
}

/* ws.send_binary(handle, data) -> Nil */
static LatExtValue *ws_send_binary(LatExtValue **args, size_t argc) {
    int64_t id;
    WsConn *conn;
    const char *data;

    ensure_init();

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_INT || lat_ext_type(args[1]) != LAT_EXT_STRING) {
        return lat_ext_error("ws.send_binary() expects (handle: Int, data: String)");
    }

    id = lat_ext_as_int(args[0]);
    conn = conn_get(id);
    if (!conn) return lat_ext_error("ws.send_binary: invalid connection handle");
    if (conn->state != WS_STATE_CONNECTED) return lat_ext_error("ws.send_binary: connection is closed");

    data = lat_ext_as_string(args[1]);
    if (ws_send_frame_conn(conn, WS_OP_BINARY, (const unsigned char *)data, strlen(data)) != 0) {
        conn->state = WS_STATE_CLOSED;
        return lat_ext_error("ws.send_binary: failed to send data");
    }

    return lat_ext_nil();
}

/* ws.status(handle) -> String */
static LatExtValue *ws_status(LatExtValue **args, size_t argc) {
    int64_t id;
    WsConn *conn;

    ensure_init();

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("ws.status() expects a connection handle (Int)");
    }

    id = lat_ext_as_int(args[0]);
    conn = conn_get(id);
    if (!conn) return lat_ext_string("closed");
    if (conn->state == WS_STATE_CONNECTED) return lat_ext_string("connected");
    return lat_ext_string("closed");
}

/* ws.ping(handle) -> Bool */
static LatExtValue *ws_ping(LatExtValue **args, size_t argc) {
    int64_t id;
    WsConn *conn;
    WsFrame frame;
    fd_set readfds;
    struct timeval tv;
    int ready;

    ensure_init();

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("ws.ping() expects a connection handle (Int)");
    }

    id = lat_ext_as_int(args[0]);
    conn = conn_get(id);
    if (!conn) return lat_ext_bool(false);
    if (conn->state != WS_STATE_CONNECTED) return lat_ext_bool(false);

    /* Send ping frame with small payload */
    {
        const unsigned char ping_data[] = "ping";
        if (ws_send_frame_conn(conn, WS_OP_PING, ping_data, 4) != 0) {
            conn->state = WS_STATE_CLOSED;
            return lat_ext_bool(false);
        }
    }

    /* Wait for pong response with 5 second timeout */
    FD_ZERO(&readfds);
    FD_SET(conn->fd, &readfds);
    tv.tv_sec = 5;
    tv.tv_usec = 0;

    ready = select(conn->fd + 1, &readfds, NULL, NULL, &tv);
    if (ready <= 0) {
        /* Timeout or error */
        return lat_ext_bool(false);
    }

    /* Read frames until we get a pong or timeout */
    for (;;) {
        if (ws_recv_frame(conn->fd, &frame) != 0) {
            conn->state = WS_STATE_CLOSED;
            return lat_ext_bool(false);
        }

        if (frame.opcode == WS_OP_PONG) {
            ws_frame_free(&frame);
            return lat_ext_bool(true);
        }

        if (frame.opcode == WS_OP_CLOSE) {
            ws_frame_free(&frame);
            conn->state = WS_STATE_CLOSED;
            return lat_ext_bool(false);
        }

        if (frame.opcode == WS_OP_PING) {
            /* Reply with pong even while waiting for our pong */
            ws_send_frame_conn(conn, WS_OP_PONG, frame.payload, frame.payload_len);
        }

        /* If we get a data frame while waiting for pong, just discard it */
        ws_frame_free(&frame);

        /* Check if more data is available */
        FD_ZERO(&readfds);
        FD_SET(conn->fd, &readfds);
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        ready = select(conn->fd + 1, &readfds, NULL, NULL, &tv);
        if (ready <= 0) { return lat_ext_bool(false); }
    }
}

/* ── Server functions ── */

/*
 * ws.listen(port) -> Int (server handle)
 *
 * Starts a WebSocket server listening on the given port.
 * Returns a server handle used with ws.accept().
 */
static LatExtValue *ws_listen(LatExtValue **args, size_t argc) {
    int64_t port;
    int fd, id;

    ensure_init();

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("ws.listen() expects a port number (Int)");
    }

    port = lat_ext_as_int(args[0]);
    if (port <= 0 || port > 65535) { return lat_ext_error("ws.listen: port must be between 1 and 65535"); }

    fd = tcp_listen((int)port);
    if (fd < 0) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "ws.listen: failed to listen on port %d: %s", (int)port, strerror(errno));
        return lat_ext_error(errbuf);
    }

    id = server_alloc(fd, (int)port);
    if (id < 0) {
        close(fd);
        return lat_ext_error("ws.listen: too many servers");
    }

    return lat_ext_int(id);
}

/*
 * ws.accept(server_handle) -> Int (connection handle)
 *
 * Blocks until a client connects to the server, completes the WebSocket
 * handshake, and returns a connection handle usable with ws.send/ws.recv.
 */
static LatExtValue *ws_accept(LatExtValue **args, size_t argc) {
    int64_t srv_id;
    WsServer *srv;
    struct sockaddr_in client_addr;
    socklen_t addr_len;
    int client_fd;
    int conn_id;

    ensure_init();

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("ws.accept() expects a server handle (Int)");
    }

    srv_id = lat_ext_as_int(args[0]);
    srv = server_get(srv_id);
    if (!srv) return lat_ext_error("ws.accept: invalid server handle");

    addr_len = sizeof(client_addr);
    client_fd = accept(srv->fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "ws.accept: accept failed: %s", strerror(errno));
        return lat_ext_error(errbuf);
    }

    /* Perform server-side WebSocket handshake */
    if (ws_server_handshake(client_fd) != 0) {
        close(client_fd);
        return lat_ext_error("ws.accept: WebSocket handshake failed");
    }

    conn_id = conn_alloc(client_fd, WS_ROLE_SERVER);
    if (conn_id < 0) {
        close(client_fd);
        return lat_ext_error("ws.accept: too many connections");
    }

    return lat_ext_int(conn_id);
}

/*
 * ws.server_close(server_handle) -> Nil
 *
 * Closes a server listener socket. Existing connections remain open.
 */
static LatExtValue *ws_server_close(LatExtValue **args, size_t argc) {
    int64_t id;
    WsServer *srv;

    ensure_init();

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("ws.server_close() expects a server handle (Int)");
    }

    id = lat_ext_as_int(args[0]);
    srv = server_get(id);
    if (!srv) return lat_ext_error("ws.server_close: invalid server handle");

    close(srv->fd);
    server_release(id);

    return lat_ext_nil();
}

/*
 * ws.set_timeout(handle, seconds) -> Nil
 *
 * Sets a recv/send timeout on a connection. 0 = no timeout (blocking).
 */
static LatExtValue *ws_set_timeout(LatExtValue **args, size_t argc) {
    int64_t id, secs;
    WsConn *conn;
    struct timeval tv;

    ensure_init();

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_INT || lat_ext_type(args[1]) != LAT_EXT_INT) {
        return lat_ext_error("ws.set_timeout() expects (handle: Int, seconds: Int)");
    }

    id = lat_ext_as_int(args[0]);
    conn = conn_get(id);
    if (!conn) return lat_ext_error("ws.set_timeout: invalid connection handle");

    secs = lat_ext_as_int(args[1]);
    if (secs < 0) secs = 0;
    conn->recv_timeout_sec = (int)secs;

    tv.tv_sec = (long)secs;
    tv.tv_usec = 0;
    setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(conn->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    return lat_ext_nil();
}

/* ── Extension init ── */

void lat_ext_init(LatExtContext *ctx) {
    /* Client functions */
    lat_ext_register(ctx, "connect", ws_connect);
    lat_ext_register(ctx, "connect_auto", ws_connect_auto);
    lat_ext_register(ctx, "close", ws_close);
    lat_ext_register(ctx, "send", ws_send);
    lat_ext_register(ctx, "recv", ws_recv);
    lat_ext_register(ctx, "recv_timeout", ws_recv_timeout);
    lat_ext_register(ctx, "send_binary", ws_send_binary);
    lat_ext_register(ctx, "status", ws_status);
    lat_ext_register(ctx, "ping", ws_ping);
    lat_ext_register(ctx, "set_timeout", ws_set_timeout);

    /* Server functions */
    lat_ext_register(ctx, "listen", ws_listen);
    lat_ext_register(ctx, "accept", ws_accept);
    lat_ext_register(ctx, "server_close", ws_server_close);
}
