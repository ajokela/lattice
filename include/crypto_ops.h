#ifndef CRYPTO_OPS_H
#define CRYPTO_OPS_H

#include <stddef.h>

/* SHA-256 hash. Returns heap-allocated hex string (64 chars). Sets *err if unavailable. */
char *crypto_sha256(const char *data, size_t len, char **err);

/* MD5 hash. Returns heap-allocated hex string (32 chars). Sets *err if unavailable. */
char *crypto_md5(const char *data, size_t len, char **err);

/* Base64 encode. Returns heap-allocated base64 string. */
char *crypto_base64_encode(const char *data, size_t len);

/* Base64 decode. Returns heap-allocated decoded string, sets *out_len. Sets *err on invalid input. */
char *crypto_base64_decode(const char *data, size_t len, size_t *out_len, char **err);

#endif
