#ifndef TLS_H
#define TLS_H

#include <stdbool.h>
#include <stddef.h>

/*
 * TLS client primitives for Lattice (backed by OpenSSL).
 *
 * Same fd-based API as the TCP layer.  When built without OpenSSL
 * (LATTICE_HAS_TLS not defined) every function returns a stub error.
 */

/* TCP connect + TLS handshake + certificate verification.
 * Returns the connected socket fd, or -1 on error. */
int net_tls_connect(const char *host, int port, char **err);

/* Read available data (up to 8KB) via SSL_read.
 * Returns heap-allocated string, empty on EOF, NULL on error. */
char *net_tls_read(int fd, char **err);

/* Read exactly n bytes via SSL_read.
 * Returns heap-allocated string, NULL on error. */
char *net_tls_read_bytes(int fd, size_t n, char **err);

/* Write all data via SSL_write.
 * Returns true on success, false on error. */
bool net_tls_write(int fd, const char *data, size_t len, char **err);

/* SSL_shutdown + SSL_free + close(fd). */
void net_tls_close(int fd);

/* Returns true if built with OpenSSL support. */
bool net_tls_available(void);

/* Clean up all TLS sessions and the global SSL_CTX.
 * Called from evaluator_free(). */
void net_tls_cleanup(void);

#endif /* TLS_H */
