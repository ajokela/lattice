#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * TCP networking primitives for Lattice.
 *
 * All functions use plain int file descriptors for socket handles.
 * A static tracking array validates that an fd is a real socket.
 * Each function takes a `char **err` out-parameter: on failure it sets
 * *err to a heap-allocated error string and returns a sentinel value.
 */

/* Bind + listen on a TCP socket (SO_REUSEADDR).
 * Returns the server socket fd, or -1 on error. */
int net_tcp_listen(const char *host, int port, char **err);

/* Block until a connection arrives on server_fd.
 * Returns the client socket fd, or -1 on error. */
int net_tcp_accept(int server_fd, char **err);

/* Connect to a remote TCP server.
 * Returns the connected socket fd, or -1 on error. */
int net_tcp_connect(const char *host, int port, char **err);

/* Read available data (up to 8KB). Returns heap-allocated string.
 * Returns empty string on EOF, NULL on error. */
char *net_tcp_read(int fd, char **err);

/* Read exactly n bytes (or until EOF). Returns heap-allocated string.
 * Returns NULL on error. */
char *net_tcp_read_bytes(int fd, size_t n, char **err);

/* Write all data, handles partial writes.
 * Returns true on success, false on error. */
bool net_tcp_write(int fd, const char *data, size_t len, char **err);

/* Close socket, deregister from tracking. */
void net_tcp_close(int fd);

/* Get remote "ip:port" string. Returns heap-allocated string, or NULL on error. */
char *net_tcp_peer_addr(int fd, char **err);

/* Set SO_RCVTIMEO/SO_SNDTIMEO. Returns true on success. */
bool net_tcp_set_timeout(int fd, int secs, char **err);

#endif /* NET_H */
