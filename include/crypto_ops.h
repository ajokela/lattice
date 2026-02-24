#ifndef CRYPTO_OPS_H
#define CRYPTO_OPS_H

#include <stddef.h>
#include <stdint.h>

/* SHA-256 hash. Returns heap-allocated hex string (64 chars). Sets *err if unavailable. */
char *crypto_sha256(const char *data, size_t len, char **err);

/* MD5 hash. Returns heap-allocated hex string (32 chars). Sets *err if unavailable. */
char *crypto_md5(const char *data, size_t len, char **err);

/* Base64 encode. Returns heap-allocated base64 string. */
char *crypto_base64_encode(const char *data, size_t len);

/* Base64 decode. Returns heap-allocated decoded string, sets *out_len. Sets *err on invalid input. */
char *crypto_base64_decode(const char *data, size_t len, size_t *out_len, char **err);

/* SHA-512 hash. Returns heap-allocated hex string (128 chars). Sets *err if unavailable. */
char *crypto_sha512(const char *data, size_t len, char **err);

/* HMAC-SHA256. Returns heap-allocated hex string. Sets *err if unavailable. */
char *crypto_hmac_sha256(const char *key, size_t key_len,
                         const char *data, size_t data_len, char **err);

/* Random bytes. Returns malloc'd buffer of n bytes. Sets *err on failure. */
uint8_t *crypto_random_bytes(size_t n, char **err);

#endif
