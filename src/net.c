#include "net.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef __EMSCRIPTEN__

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

/* ── Socket tracking ── */

#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

static bool tracked_sockets[FD_SETSIZE];
static bool sigpipe_suppressed = false;

static void ensure_sigpipe_suppressed(void) {
    if (!sigpipe_suppressed) {
        signal(SIGPIPE, SIG_IGN);
        sigpipe_suppressed = true;
    }
}

static void track_socket(int fd) {
    if (fd >= 0 && fd < FD_SETSIZE)
        tracked_sockets[fd] = true;
}

static void untrack_socket(int fd) {
    if (fd >= 0 && fd < FD_SETSIZE)
        tracked_sockets[fd] = false;
}

static bool is_tracked(int fd) {
    return fd >= 0 && fd < FD_SETSIZE && tracked_sockets[fd];
}

static char *make_err(const char *prefix) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s: %s", prefix, strerror(errno));
    return strdup(buf);
}

/* ── tcp_listen ── */

int net_tcp_listen(const char *host, int port, char **err) {
    ensure_sigpipe_suppressed();

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { *err = make_err("tcp_listen: socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        *err = strdup("tcp_listen: invalid host address");
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        *err = make_err("tcp_listen: bind");
        close(fd);
        return -1;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        *err = make_err("tcp_listen: listen");
        close(fd);
        return -1;
    }

    track_socket(fd);
    return fd;
}

/* ── tcp_accept ── */

int net_tcp_accept(int server_fd, char **err) {
    if (!is_tracked(server_fd)) {
        *err = strdup("tcp_accept: not a tracked socket");
        return -1;
    }

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
        *err = make_err("tcp_accept: accept");
        return -1;
    }

    track_socket(client_fd);
    return client_fd;
}

/* ── tcp_connect ── */

int net_tcp_connect(const char *host, int port, char **err) {
    ensure_sigpipe_suppressed();

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "tcp_connect: %s", gai_strerror(gai));
        *err = strdup(buf);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        *err = make_err("tcp_connect: socket");
        freeaddrinfo(res);
        return -1;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        *err = make_err("tcp_connect: connect");
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    track_socket(fd);
    return fd;
}

/* ── tcp_read ── */

#define TCP_READ_BUF 8192

char *net_tcp_read(int fd, char **err) {
    if (!is_tracked(fd)) {
        *err = strdup("tcp_read: not a tracked socket");
        return NULL;
    }

    char *buf = malloc(TCP_READ_BUF + 1);
    if (!buf) { *err = strdup("tcp_read: out of memory"); return NULL; }

    ssize_t n = recv(fd, buf, TCP_READ_BUF, 0);
    if (n < 0) {
        free(buf);
        *err = make_err("tcp_read: recv");
        return NULL;
    }

    buf[n] = '\0';
    return buf;
}

/* ── tcp_read_bytes ── */

char *net_tcp_read_bytes(int fd, size_t count, char **err) {
    if (!is_tracked(fd)) {
        *err = strdup("tcp_read_bytes: not a tracked socket");
        return NULL;
    }

    char *buf = malloc(count + 1);
    if (!buf) { *err = strdup("tcp_read_bytes: out of memory"); return NULL; }

    size_t total = 0;
    while (total < count) {
        ssize_t n = recv(fd, buf + total, count - total, 0);
        if (n <= 0) break;  /* EOF or error */
        total += (size_t)n;
    }

    buf[total] = '\0';
    return buf;
}

/* ── tcp_write ── */

bool net_tcp_write(int fd, const char *data, size_t len, char **err) {
    if (!is_tracked(fd)) {
        *err = strdup("tcp_write: not a tracked socket");
        return false;
    }

    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, data + total, len - total, 0);
        if (n < 0) {
            *err = make_err("tcp_write: send");
            return false;
        }
        total += (size_t)n;
    }
    return true;
}

/* ── tcp_close ── */

void net_tcp_close(int fd) {
    if (is_tracked(fd)) {
        untrack_socket(fd);
        close(fd);
    }
}

/* ── tcp_peer_addr ── */

char *net_tcp_peer_addr(int fd, char **err) {
    if (!is_tracked(fd)) {
        *err = strdup("tcp_peer_addr: not a tracked socket");
        return NULL;
    }

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &addr_len) < 0) {
        *err = make_err("tcp_peer_addr: getpeername");
        return NULL;
    }

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));

    char buf[64];
    snprintf(buf, sizeof(buf), "%s:%d", ip, ntohs(addr.sin_port));
    return strdup(buf);
}

/* ── tcp_set_timeout ── */

bool net_tcp_set_timeout(int fd, int secs, char **err) {
    if (!is_tracked(fd)) {
        *err = strdup("tcp_set_timeout: not a tracked socket");
        return false;
    }

    struct timeval tv;
    tv.tv_sec = secs;
    tv.tv_usec = 0;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        *err = make_err("tcp_set_timeout: SO_RCVTIMEO");
        return false;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        *err = make_err("tcp_set_timeout: SO_SNDTIMEO");
        return false;
    }
    return true;
}

#else /* __EMSCRIPTEN__ */

/* ── WASM stubs ── */

static char *wasm_err(void) {
    return strdup("networking not available in WASM");
}

int net_tcp_listen(const char *host, int port, char **err) {
    (void)host; (void)port;
    *err = wasm_err();
    return -1;
}

int net_tcp_accept(int server_fd, char **err) {
    (void)server_fd;
    *err = wasm_err();
    return -1;
}

int net_tcp_connect(const char *host, int port, char **err) {
    (void)host; (void)port;
    *err = wasm_err();
    return -1;
}

char *net_tcp_read(int fd, char **err) {
    (void)fd;
    *err = wasm_err();
    return NULL;
}

char *net_tcp_read_bytes(int fd, size_t count, char **err) {
    (void)fd; (void)count;
    *err = wasm_err();
    return NULL;
}

bool net_tcp_write(int fd, const char *data, size_t len, char **err) {
    (void)fd; (void)data; (void)len;
    *err = wasm_err();
    return false;
}

void net_tcp_close(int fd) {
    (void)fd;
}

char *net_tcp_peer_addr(int fd, char **err) {
    (void)fd;
    *err = wasm_err();
    return NULL;
}

bool net_tcp_set_timeout(int fd, int secs, char **err) {
    (void)fd; (void)secs;
    *err = wasm_err();
    return false;
}

#endif /* __EMSCRIPTEN__ */
