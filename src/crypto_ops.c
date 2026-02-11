#include "crypto_ops.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ══════════════════════════════════════════════════════════════════════
 * SHA-256 and MD5 — require OpenSSL (EVP API)
 * ══════════════════════════════════════════════════════════════════════ */

#ifdef LATTICE_HAS_TLS

#include <openssl/evp.h>

static char *hex_encode(const unsigned char *hash, unsigned int len) {
    char *hex = malloc(len * 2 + 1);
    for (unsigned int i = 0; i < len; i++) {
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    hex[len * 2] = '\0';
    return hex;
}

char *crypto_sha256(const char *data, size_t len, char **err) {
    (void)err;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        *err = strdup("sha256: failed to create digest context");
        return NULL;
    }
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);

    return hex_encode(hash, hash_len);
}

char *crypto_md5(const char *data, size_t len, char **err) {
    (void)err;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        *err = strdup("md5: failed to create digest context");
        return NULL;
    }
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);

    return hex_encode(hash, hash_len);
}

#else /* !LATTICE_HAS_TLS */

char *crypto_sha256(const char *data, size_t len, char **err) {
    (void)data; (void)len;
    *err = strdup("sha256: not available (built without OpenSSL)");
    return NULL;
}

char *crypto_md5(const char *data, size_t len, char **err) {
    (void)data; (void)len;
    *err = strdup("md5: not available (built without OpenSSL)");
    return NULL;
}

#endif /* LATTICE_HAS_TLS */


/* ══════════════════════════════════════════════════════════════════════
 * Base64 encode/decode — pure C, always available
 * ══════════════════════════════════════════════════════════════════════ */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *crypto_base64_encode(const char *data, size_t len) {
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = malloc(out_len + 1);
    size_t j = 0;

    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = ((unsigned char)data[i]) << 16;
        if (i + 1 < len) n |= ((unsigned char)data[i + 1]) << 8;
        if (i + 2 < len) n |= ((unsigned char)data[i + 2]);

        out[j++] = b64_table[(n >> 18) & 0x3F];
        out[j++] = b64_table[(n >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? b64_table[(n >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < len) ? b64_table[n & 0x3F] : '=';
    }

    out[j] = '\0';
    return out;
}

/* Decode table: maps ASCII byte -> 6-bit value, or -1 for invalid, -2 for padding */
static int b64_decode_char(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2;
    return -1;
}

char *crypto_base64_decode(const char *data, size_t len, size_t *out_len, char **err) {
    /* Skip trailing whitespace/newlines */
    while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r' ||
                       data[len - 1] == ' '  || data[len - 1] == '\t')) {
        len--;
    }

    if (len == 0) {
        char *out = malloc(1);
        out[0] = '\0';
        *out_len = 0;
        return out;
    }

    if (len % 4 != 0) {
        *err = strdup("base64_decode: invalid input length (must be multiple of 4)");
        return NULL;
    }

    size_t max_out = (len / 4) * 3;
    char *out = malloc(max_out + 1);
    size_t j = 0;

    for (size_t i = 0; i < len; i += 4) {
        int a = b64_decode_char((unsigned char)data[i]);
        int b = b64_decode_char((unsigned char)data[i + 1]);
        int c = b64_decode_char((unsigned char)data[i + 2]);
        int d = b64_decode_char((unsigned char)data[i + 3]);

        /* Check for invalid characters (but not padding) */
        if (a < 0 || b < 0 || (c < 0 && c != -2) || (d < 0 && d != -2)) {
            free(out);
            *err = strdup("base64_decode: invalid character in input");
            return NULL;
        }

        /* Treat padding as 0 for the arithmetic */
        int cv = (c == -2) ? 0 : c;
        int dv = (d == -2) ? 0 : d;

        unsigned int n = ((unsigned int)a << 18) | ((unsigned int)b << 12) |
                         ((unsigned int)cv << 6) | (unsigned int)dv;

        out[j++] = (char)((n >> 16) & 0xFF);
        if (c != -2) out[j++] = (char)((n >> 8) & 0xFF);
        if (d != -2) out[j++] = (char)(n & 0xFF);
    }

    out[j] = '\0';
    *out_len = j;
    return out;
}
