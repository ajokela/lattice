/*
 * Lattice WebSocket Client Extension
 *
 * Provides connect, close, send, recv, send_binary, status, and ping
 * functions for WebSocket client connections using raw TCP sockets.
 * Uses OpenSSL for SHA1 hashing during the WebSocket handshake.
 */

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
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <time.h>

/* Forward declare the init function (exported symbol) */
void lat_ext_init(LatExtContext *ctx);

/* ── Connection table ── */

#define MAX_CONNECTIONS 64

typedef enum {
    WS_STATE_CLOSED,
    WS_STATE_CONNECTED
} WsState;

typedef struct {
    int       fd;
    WsState   state;
} WsConn;

static WsConn connections[MAX_CONNECTIONS];
static int    conn_count = 0;
static int    initialized = 0;

static void ensure_init(void) {
    if (!initialized) {
        int i;
        for (i = 0; i < MAX_CONNECTIONS; i++) {
            connections[i].fd = -1;
            connections[i].state = WS_STATE_CLOSED;
        }
        initialized = 1;
        srand((unsigned int)time(NULL));
    }
}

static int conn_alloc(int fd) {
    int i;
    for (i = 0; i < conn_count; i++) {
        if (connections[i].state == WS_STATE_CLOSED && connections[i].fd == -1) {
            connections[i].fd = fd;
            connections[i].state = WS_STATE_CONNECTED;
            return i;
        }
    }
    if (conn_count >= MAX_CONNECTIONS) return -1;
    connections[conn_count].fd = fd;
    connections[conn_count].state = WS_STATE_CONNECTED;
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
    int  port;
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
    out->port = 80;  /* default */

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

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        return -1;
    }

    fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) continue;

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;  /* success */
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
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

/* ── WebSocket handshake ── */

static const char *WS_MAGIC = "258EAFA5-E914-47DA-95CA-5AB5B11731C5";

static int ws_handshake(int fd, const char *host, int port, const char *path) {
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
    for (i = 0; i < 16; i++) {
        key_bytes[i] = (unsigned char)(rand() & 0xFF);
    }
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
        if (total_read >= 4 &&
            response[total_read - 4] == '\r' &&
            response[total_read - 3] == '\n' &&
            response[total_read - 2] == '\r' &&
            response[total_read - 1] == '\n') {
            break;
        }
    }

    response[total_read] = '\0';

    /* Verify 101 status */
    if (strncmp(response, "HTTP/1.1 101", 12) != 0 &&
        strncmp(response, "HTTP/1.0 101", 12) != 0) {
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

/* ── WebSocket framing ── */

/* Opcodes */
#define WS_OP_CONTINUATION 0x0
#define WS_OP_TEXT         0x1
#define WS_OP_BINARY       0x2
#define WS_OP_CLOSE        0x8
#define WS_OP_PING         0x9
#define WS_OP_PONG         0xA

static int ws_send_frame(int fd, int opcode, const unsigned char *payload, size_t len) {
    unsigned char header[14];
    size_t header_len = 0;
    unsigned char mask_key[4];
    unsigned char *masked;
    size_t i;
    int rc;

    /* FIN bit + opcode */
    header[0] = (unsigned char)(0x80 | (opcode & 0x0F));
    header_len = 1;

    /* Payload length with mask bit set (client must mask) */
    if (len <= 125) {
        header[1] = (unsigned char)(0x80 | len);
        header_len = 2;
    } else if (len <= 65535) {
        header[1] = (unsigned char)(0x80 | 126);
        header[2] = (unsigned char)((len >> 8) & 0xFF);
        header[3] = (unsigned char)(len & 0xFF);
        header_len = 4;
    } else {
        header[1] = (unsigned char)(0x80 | 127);
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

    /* Generate mask key */
    for (i = 0; i < 4; i++) {
        mask_key[i] = (unsigned char)(rand() & 0xFF);
    }

    /* Append mask key to header */
    memcpy(header + header_len, mask_key, 4);
    header_len += 4;

    /* Send header */
    if (send_all(fd, header, header_len) != 0) return -1;

    /* Mask and send payload */
    if (len > 0) {
        masked = malloc(len);
        if (!masked) return -1;
        for (i = 0; i < len; i++) {
            masked[i] = payload[i] ^ mask_key[i % 4];
        }
        rc = send_all(fd, masked, len);
        free(masked);
        if (rc != 0) return -1;
    }

    return 0;
}

typedef struct {
    int           opcode;
    unsigned char *payload;
    size_t        payload_len;
    int           fin;
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
        for (i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | ext[i];
        }
    }

    /* Read mask key if present */
    if (masked) {
        if (recv_all(fd, mask_key, 4) != 0) return -1;
    }

    /* Read payload */
    frame->payload_len = (size_t)payload_len;
    if (payload_len > 0) {
        /* Sanity check: reject frames larger than 16 MB */
        if (payload_len > 16 * 1024 * 1024) return -1;

        frame->payload = malloc((size_t)payload_len);
        if (!frame->payload) return -1;
        if (recv_all(fd, frame->payload, (size_t)payload_len) != 0) {
            free(frame->payload);
            frame->payload = NULL;
            return -1;
        }

        /* Unmask if needed */
        if (masked) {
            for (i = 0; i < (size_t)payload_len; i++) {
                frame->payload[i] ^= mask_key[i % 4];
            }
        }
    }

    return 0;
}

static void ws_frame_free(WsFrame *frame) {
    free(frame->payload);
    frame->payload = NULL;
    frame->payload_len = 0;
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
        snprintf(errbuf, sizeof(errbuf), "ws.connect: failed to connect to %s:%d",
                 url.host, url.port);
        return lat_ext_error(errbuf);
    }

    if (ws_handshake(fd, url.host, url.port, url.path) != 0) {
        close(fd);
        return lat_ext_error("ws.connect: WebSocket handshake failed");
    }

    id = conn_alloc(fd);
    if (id < 0) {
        close(fd);
        return lat_ext_error("ws.connect: too many connections");
    }

    return lat_ext_int(id);
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
    ws_send_frame(conn->fd, WS_OP_CLOSE, NULL, 0);

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

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING) {
        return lat_ext_error("ws.send() expects (handle: Int, message: String)");
    }

    id = lat_ext_as_int(args[0]);
    conn = conn_get(id);
    if (!conn) return lat_ext_error("ws.send: invalid connection handle");
    if (conn->state != WS_STATE_CONNECTED) return lat_ext_error("ws.send: connection is closed");

    msg = lat_ext_as_string(args[1]);
    if (ws_send_frame(conn->fd, WS_OP_TEXT, (const unsigned char *)msg, strlen(msg)) != 0) {
        conn->state = WS_STATE_CLOSED;
        return lat_ext_error("ws.send: failed to send message");
    }

    return lat_ext_nil();
}

/* ws.recv(handle) -> String or Nil */
static LatExtValue *ws_recv(LatExtValue **args, size_t argc) {
    int64_t id;
    WsConn *conn;
    WsFrame frame;
    LatExtValue *result;

    ensure_init();

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("ws.recv() expects a connection handle (Int)");
    }

    id = lat_ext_as_int(args[0]);
    conn = conn_get(id);
    if (!conn) return lat_ext_error("ws.recv: invalid connection handle");
    if (conn->state != WS_STATE_CONNECTED) return lat_ext_nil();

    for (;;) {
        if (ws_recv_frame(conn->fd, &frame) != 0) {
            conn->state = WS_STATE_CLOSED;
            return lat_ext_nil();
        }

        switch (frame.opcode) {
            case WS_OP_TEXT:
            case WS_OP_BINARY: {
                /* Return payload as string */
                char *str = malloc(frame.payload_len + 1);
                if (!str) {
                    ws_frame_free(&frame);
                    return lat_ext_error("ws.recv: out of memory");
                }
                if (frame.payload_len > 0) {
                    memcpy(str, frame.payload, frame.payload_len);
                }
                str[frame.payload_len] = '\0';
                result = lat_ext_string(str);
                free(str);
                ws_frame_free(&frame);
                return result;
            }

            case WS_OP_CLOSE:
                /* Connection closed by server */
                ws_frame_free(&frame);
                conn->state = WS_STATE_CLOSED;
                return lat_ext_nil();

            case WS_OP_PING:
                /* Respond with pong (same payload) */
                ws_send_frame(conn->fd, WS_OP_PONG, frame.payload, frame.payload_len);
                ws_frame_free(&frame);
                /* Continue reading next frame */
                break;

            case WS_OP_PONG:
                /* Ignore unsolicited pongs */
                ws_frame_free(&frame);
                break;

            default:
                ws_frame_free(&frame);
                break;
        }
    }
}

/* ws.send_binary(handle, data) -> Nil */
static LatExtValue *ws_send_binary(LatExtValue **args, size_t argc) {
    int64_t id;
    WsConn *conn;
    const char *data;

    ensure_init();

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING) {
        return lat_ext_error("ws.send_binary() expects (handle: Int, data: String)");
    }

    id = lat_ext_as_int(args[0]);
    conn = conn_get(id);
    if (!conn) return lat_ext_error("ws.send_binary: invalid connection handle");
    if (conn->state != WS_STATE_CONNECTED) return lat_ext_error("ws.send_binary: connection is closed");

    data = lat_ext_as_string(args[1]);
    if (ws_send_frame(conn->fd, WS_OP_BINARY, (const unsigned char *)data, strlen(data)) != 0) {
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
        if (ws_send_frame(conn->fd, WS_OP_PING, ping_data, 4) != 0) {
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

        /* If we get a data frame while waiting for pong, just discard it */
        ws_frame_free(&frame);

        /* Check if more data is available */
        FD_ZERO(&readfds);
        FD_SET(conn->fd, &readfds);
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        ready = select(conn->fd + 1, &readfds, NULL, NULL, &tv);
        if (ready <= 0) {
            return lat_ext_bool(false);
        }
    }
}

/* ── Extension init ── */

void lat_ext_init(LatExtContext *ctx) {
    lat_ext_register(ctx, "connect",     ws_connect);
    lat_ext_register(ctx, "close",       ws_close);
    lat_ext_register(ctx, "send",        ws_send);
    lat_ext_register(ctx, "recv",        ws_recv);
    lat_ext_register(ctx, "send_binary", ws_send_binary);
    lat_ext_register(ctx, "status",      ws_status);
    lat_ext_register(ctx, "ping",        ws_ping);
}
