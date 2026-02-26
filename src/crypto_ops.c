#include "crypto_ops.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef __EMSCRIPTEN__
#include <unistd.h>  /* getentropy() for WASM random bytes */
#endif

/* ══════════════════════════════════════════════════════════════════════
 * Hex encoding — shared by both OpenSSL and pure-C paths
 * ══════════════════════════════════════════════════════════════════════ */

static char *hex_encode(const unsigned char *hash, unsigned int len) {
    char *hex = malloc(len * 2 + 1);
    if (!hex) return NULL;
    for (unsigned int i = 0; i < len; i++) {
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    hex[len * 2] = '\0';
    return hex;
}

/* ══════════════════════════════════════════════════════════════════════
 * SHA-256, MD5, SHA-512, HMAC-SHA256, random_bytes
 * ══════════════════════════════════════════════════════════════════════ */

#ifdef LATTICE_HAS_TLS

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

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

char *crypto_sha512(const char *data, size_t len, char **err) {
    (void)err;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        *err = strdup("sha512: failed to create digest context");
        return NULL;
    }
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    EVP_DigestInit_ex(ctx, EVP_sha512(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);

    return hex_encode(hash, hash_len);
}

char *crypto_hmac_sha256(const char *key, size_t key_len,
                         const char *data, size_t data_len, char **err) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len = 0;
    unsigned char *r = HMAC(EVP_sha256(),
                            key, (int)key_len,
                            (const unsigned char *)data, data_len,
                            result, &result_len);
    if (!r) {
        *err = strdup("hmac_sha256: HMAC computation failed");
        return NULL;
    }
    return hex_encode(result, result_len);
}

uint8_t *crypto_random_bytes(size_t n, char **err) {
    uint8_t *buf = malloc(n);
    if (!buf) { *err = strdup("random_bytes: allocation failed"); return NULL; }
    if (RAND_bytes(buf, (int)n) != 1) {
        free(buf);
        *err = strdup("random_bytes: RAND_bytes failed");
        return NULL;
    }
    return buf;
}

#else /* !LATTICE_HAS_TLS — pure-C implementations */

/* ── Pure-C SHA-256 (RFC 6234) ──────────────────────────────────────── */

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t  buffer[64];
    uint32_t buflen;
} pc_sha256_ctx;

static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define RR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z)  (((x)&(y))^((~(x))&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define S256_S0(x) (RR32(x,2)^RR32(x,13)^RR32(x,22))
#define S256_S1(x) (RR32(x,6)^RR32(x,11)^RR32(x,25))
#define S256_s0(x) (RR32(x,7)^RR32(x,18)^((x)>>3))
#define S256_s1(x) (RR32(x,17)^RR32(x,19)^((x)>>10))

static void pc_sha256_init(pc_sha256_ctx *c) {
    c->state[0] = 0x6a09e667; c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372; c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f; c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab; c->state[7] = 0x5be0cd19;
    c->bitcount = 0;
    c->buflen = 0;
}

static void pc_sha256_compress(pc_sha256_ctx *c, const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|
               ((uint32_t)block[i*4+2]<<8)|((uint32_t)block[i*4+3]);
    for (int i = 16; i < 64; i++)
        w[i] = S256_s1(w[i-2]) + w[i-7] + S256_s0(w[i-15]) + w[i-16];

    uint32_t a=c->state[0], b=c->state[1], cc=c->state[2], d=c->state[3],
             e=c->state[4], f=c->state[5], g=c->state[6], h=c->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + S256_S1(e) + CH(e,f,g) + sha256_k[i] + w[i];
        uint32_t t2 = S256_S0(a) + MAJ(a,b,cc);
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->state[0]+=a; c->state[1]+=b; c->state[2]+=cc; c->state[3]+=d;
    c->state[4]+=e; c->state[5]+=f; c->state[6]+=g;  c->state[7]+=h;
}

static void pc_sha256_update(pc_sha256_ctx *c, const uint8_t *data, size_t len) {
    c->bitcount += (uint64_t)len * 8;
    while (len > 0) {
        uint32_t space = 64 - c->buflen;
        uint32_t take = (len < space) ? (uint32_t)len : space;
        memcpy(c->buffer + c->buflen, data, take);
        c->buflen += take;
        data += take;
        len -= take;
        if (c->buflen == 64) {
            pc_sha256_compress(c, c->buffer);
            c->buflen = 0;
        }
    }
}

static void pc_sha256_final(pc_sha256_ctx *c, uint8_t hash[32]) {
    c->buffer[c->buflen++] = 0x80;
    if (c->buflen > 56) {
        memset(c->buffer + c->buflen, 0, 64 - c->buflen);
        pc_sha256_compress(c, c->buffer);
        c->buflen = 0;
    }
    memset(c->buffer + c->buflen, 0, 56 - c->buflen);
    for (int i = 0; i < 8; i++)
        c->buffer[56 + i] = (uint8_t)(c->bitcount >> (56 - i * 8));
    pc_sha256_compress(c, c->buffer);
    for (int i = 0; i < 8; i++) {
        hash[i*4]   = (uint8_t)(c->state[i] >> 24);
        hash[i*4+1] = (uint8_t)(c->state[i] >> 16);
        hash[i*4+2] = (uint8_t)(c->state[i] >> 8);
        hash[i*4+3] = (uint8_t)(c->state[i]);
    }
}

char *crypto_sha256(const char *data, size_t len, char **err) {
    (void)err;
    pc_sha256_ctx ctx;
    uint8_t hash[32];
    pc_sha256_init(&ctx);
    pc_sha256_update(&ctx, (const uint8_t *)data, len);
    pc_sha256_final(&ctx, hash);
    return hex_encode(hash, 32);
}

/* ── Pure-C MD5 (RFC 1321) ──────────────────────────────────────────── */

typedef struct {
    uint32_t state[4];
    uint64_t bitcount;
    uint8_t  buffer[64];
    uint32_t buflen;
} pc_md5_ctx;

#define RL32(x,n) (((x)<<(n))|((x)>>(32-(n))))
#define MD5_F(x,y,z) (((x)&(y))|((~(x))&(z)))
#define MD5_G(x,y,z) (((x)&(z))|((y)&(~(z))))
#define MD5_H(x,y,z) ((x)^(y)^(z))
#define MD5_I(x,y,z) ((y)^((x)|(~(z))))

static const uint32_t md5_t[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const int md5_s[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};

static void pc_md5_init(pc_md5_ctx *c) {
    c->state[0] = 0x67452301; c->state[1] = 0xefcdab89;
    c->state[2] = 0x98badcfe; c->state[3] = 0x10325476;
    c->bitcount = 0;
    c->buflen = 0;
}

static void pc_md5_compress(pc_md5_ctx *c, const uint8_t block[64]) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
        m[i] = ((uint32_t)block[i*4])|((uint32_t)block[i*4+1]<<8)|
               ((uint32_t)block[i*4+2]<<16)|((uint32_t)block[i*4+3]<<24);

    uint32_t a=c->state[0], b=c->state[1], cc=c->state[2], d=c->state[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16)      { f = MD5_F(b,cc,d); g = i; }
        else if (i < 32) { f = MD5_G(b,cc,d); g = (5*i+1)%16; }
        else if (i < 48) { f = MD5_H(b,cc,d); g = (3*i+5)%16; }
        else              { f = MD5_I(b,cc,d); g = (7*i)%16; }
        f = f + a + md5_t[i] + m[g];
        a = d; d = cc; cc = b; b = b + RL32(f, md5_s[i]);
    }
    c->state[0]+=a; c->state[1]+=b; c->state[2]+=cc; c->state[3]+=d;
}

static void pc_md5_update(pc_md5_ctx *c, const uint8_t *data, size_t len) {
    c->bitcount += (uint64_t)len * 8;
    while (len > 0) {
        uint32_t space = 64 - c->buflen;
        uint32_t take = (len < space) ? (uint32_t)len : space;
        memcpy(c->buffer + c->buflen, data, take);
        c->buflen += take;
        data += take;
        len -= take;
        if (c->buflen == 64) {
            pc_md5_compress(c, c->buffer);
            c->buflen = 0;
        }
    }
}

static void pc_md5_final(pc_md5_ctx *c, uint8_t hash[16]) {
    c->buffer[c->buflen++] = 0x80;
    if (c->buflen > 56) {
        memset(c->buffer + c->buflen, 0, 64 - c->buflen);
        pc_md5_compress(c, c->buffer);
        c->buflen = 0;
    }
    memset(c->buffer + c->buflen, 0, 56 - c->buflen);
    /* MD5 uses little-endian bit count */
    for (int i = 0; i < 8; i++)
        c->buffer[56 + i] = (uint8_t)(c->bitcount >> (i * 8));
    pc_md5_compress(c, c->buffer);
    for (int i = 0; i < 4; i++) {
        hash[i*4]   = (uint8_t)(c->state[i]);
        hash[i*4+1] = (uint8_t)(c->state[i] >> 8);
        hash[i*4+2] = (uint8_t)(c->state[i] >> 16);
        hash[i*4+3] = (uint8_t)(c->state[i] >> 24);
    }
}

char *crypto_md5(const char *data, size_t len, char **err) {
    (void)err;
    pc_md5_ctx ctx;
    uint8_t hash[16];
    pc_md5_init(&ctx);
    pc_md5_update(&ctx, (const uint8_t *)data, len);
    pc_md5_final(&ctx, hash);
    return hex_encode(hash, 16);
}

/* ── Pure-C SHA-512 (RFC 6234) ──────────────────────────────────────── */

typedef struct {
    uint64_t state[8];
    uint64_t bitcount[2]; /* [low, high] */
    uint8_t  buffer[128];
    uint32_t buflen;
} pc_sha512_ctx;

static const uint64_t sha512_k[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};

#define RR64(x,n) (((x)>>(n))|((x)<<(64-(n))))
#define S512_S0(x) (RR64(x,28)^RR64(x,34)^RR64(x,39))
#define S512_S1(x) (RR64(x,14)^RR64(x,18)^RR64(x,41))
#define S512_s0(x) (RR64(x,1)^RR64(x,8)^((x)>>7))
#define S512_s1(x) (RR64(x,19)^RR64(x,61)^((x)>>6))
#define CH64(x,y,z)  (((x)&(y))^((~(x))&(z)))
#define MAJ64(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))

static void pc_sha512_init(pc_sha512_ctx *c) {
    c->state[0] = 0x6a09e667f3bcc908ULL; c->state[1] = 0xbb67ae8584caa73bULL;
    c->state[2] = 0x3c6ef372fe94f82bULL; c->state[3] = 0xa54ff53a5f1d36f1ULL;
    c->state[4] = 0x510e527fade682d1ULL; c->state[5] = 0x9b05688c2b3e6c1fULL;
    c->state[6] = 0x1f83d9abfb41bd6bULL; c->state[7] = 0x5be0cd19137e2179ULL;
    c->bitcount[0] = c->bitcount[1] = 0;
    c->buflen = 0;
}

static void pc_sha512_compress(pc_sha512_ctx *c, const uint8_t block[128]) {
    uint64_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint64_t)block[i*8]<<56)|((uint64_t)block[i*8+1]<<48)|
               ((uint64_t)block[i*8+2]<<40)|((uint64_t)block[i*8+3]<<32)|
               ((uint64_t)block[i*8+4]<<24)|((uint64_t)block[i*8+5]<<16)|
               ((uint64_t)block[i*8+6]<<8)|((uint64_t)block[i*8+7]);
    for (int i = 16; i < 80; i++)
        w[i] = S512_s1(w[i-2]) + w[i-7] + S512_s0(w[i-15]) + w[i-16];

    uint64_t a=c->state[0], b=c->state[1], cv=c->state[2], d=c->state[3],
             e=c->state[4], f=c->state[5], g=c->state[6], h=c->state[7];
    for (int i = 0; i < 80; i++) {
        uint64_t t1 = h + S512_S1(e) + CH64(e,f,g) + sha512_k[i] + w[i];
        uint64_t t2 = S512_S0(a) + MAJ64(a,b,cv);
        h=g; g=f; f=e; e=d+t1; d=cv; cv=b; b=a; a=t1+t2;
    }
    c->state[0]+=a; c->state[1]+=b; c->state[2]+=cv; c->state[3]+=d;
    c->state[4]+=e; c->state[5]+=f; c->state[6]+=g;  c->state[7]+=h;
}

static void pc_sha512_update(pc_sha512_ctx *c, const uint8_t *data, size_t len) {
    uint64_t bits = (uint64_t)len * 8;
    c->bitcount[0] += bits;
    if (c->bitcount[0] < bits) c->bitcount[1]++;
    while (len > 0) {
        uint32_t space = 128 - c->buflen;
        uint32_t take = (len < space) ? (uint32_t)len : space;
        memcpy(c->buffer + c->buflen, data, take);
        c->buflen += take;
        data += take;
        len -= take;
        if (c->buflen == 128) {
            pc_sha512_compress(c, c->buffer);
            c->buflen = 0;
        }
    }
}

static void pc_sha512_final(pc_sha512_ctx *c, uint8_t hash[64]) {
    c->buffer[c->buflen++] = 0x80;
    if (c->buflen > 112) {
        memset(c->buffer + c->buflen, 0, 128 - c->buflen);
        pc_sha512_compress(c, c->buffer);
        c->buflen = 0;
    }
    memset(c->buffer + c->buflen, 0, 112 - c->buflen);
    /* Big-endian 128-bit length: high word then low word */
    for (int i = 0; i < 8; i++)
        c->buffer[112 + i] = (uint8_t)(c->bitcount[1] >> (56 - i * 8));
    for (int i = 0; i < 8; i++)
        c->buffer[120 + i] = (uint8_t)(c->bitcount[0] >> (56 - i * 8));
    pc_sha512_compress(c, c->buffer);
    for (int i = 0; i < 8; i++) {
        hash[i*8]   = (uint8_t)(c->state[i] >> 56);
        hash[i*8+1] = (uint8_t)(c->state[i] >> 48);
        hash[i*8+2] = (uint8_t)(c->state[i] >> 40);
        hash[i*8+3] = (uint8_t)(c->state[i] >> 32);
        hash[i*8+4] = (uint8_t)(c->state[i] >> 24);
        hash[i*8+5] = (uint8_t)(c->state[i] >> 16);
        hash[i*8+6] = (uint8_t)(c->state[i] >> 8);
        hash[i*8+7] = (uint8_t)(c->state[i]);
    }
}

char *crypto_sha512(const char *data, size_t len, char **err) {
    (void)err;
    pc_sha512_ctx ctx;
    uint8_t hash[64];
    pc_sha512_init(&ctx);
    pc_sha512_update(&ctx, (const uint8_t *)data, len);
    pc_sha512_final(&ctx, hash);
    return hex_encode(hash, 64);
}

/* ── HMAC-SHA256 (RFC 2104) using pure-C SHA-256 ────────────────────── */

char *crypto_hmac_sha256(const char *key, size_t key_len,
                         const char *data, size_t data_len, char **err) {
    (void)err;
    uint8_t k_pad[64];
    memset(k_pad, 0, 64);

    /* If key > 64 bytes, hash it first */
    if (key_len > 64) {
        pc_sha256_ctx kctx;
        pc_sha256_init(&kctx);
        pc_sha256_update(&kctx, (const uint8_t *)key, key_len);
        pc_sha256_final(&kctx, k_pad); /* 32 bytes, rest stays 0 */
    } else {
        memcpy(k_pad, key, key_len);
    }

    /* Inner hash: SHA256((k_pad ^ ipad) || data) */
    uint8_t i_pad[64], o_pad[64];
    for (int i = 0; i < 64; i++) {
        i_pad[i] = k_pad[i] ^ 0x36;
        o_pad[i] = k_pad[i] ^ 0x5c;
    }

    pc_sha256_ctx ctx;
    uint8_t inner_hash[32];
    pc_sha256_init(&ctx);
    pc_sha256_update(&ctx, i_pad, 64);
    pc_sha256_update(&ctx, (const uint8_t *)data, data_len);
    pc_sha256_final(&ctx, inner_hash);

    /* Outer hash: SHA256((k_pad ^ opad) || inner_hash) */
    uint8_t final_hash[32];
    pc_sha256_init(&ctx);
    pc_sha256_update(&ctx, o_pad, 64);
    pc_sha256_update(&ctx, inner_hash, 32);
    pc_sha256_final(&ctx, final_hash);

    return hex_encode(final_hash, 32);
}

/* ── random_bytes fallback (already existed) ────────────────────────── */

uint8_t *crypto_random_bytes(size_t n, char **err) {
#if defined(__EMSCRIPTEN__)
    uint8_t *buf = malloc(n);
    if (!buf) { *err = strdup("random_bytes: allocation failed"); return NULL; }
    /* getentropy is limited to 256 bytes per call */
    size_t offset = 0;
    while (offset < n) {
        size_t chunk = n - offset;
        if (chunk > 256) chunk = 256;
        if (getentropy(buf + offset, chunk) != 0) {
            free(buf);
            *err = strdup("random_bytes: getentropy failed");
            return NULL;
        }
        offset += chunk;
    }
    return buf;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    uint8_t *buf = malloc(n);
    if (!buf) { *err = strdup("random_bytes: allocation failed"); return NULL; }
    arc4random_buf(buf, n);
    return buf;
#else
    uint8_t *buf = malloc(n);
    if (!buf) { *err = strdup("random_bytes: allocation failed"); return NULL; }
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) { free(buf); *err = strdup("random_bytes: cannot open /dev/urandom"); return NULL; }
    if (fread(buf, 1, n, f) != n) { fclose(f); free(buf); *err = strdup("random_bytes: short read from /dev/urandom"); return NULL; }
    fclose(f);
    return buf;
#endif
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
    if (!out) return NULL;
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
        if (!out) { *err = strdup("base64_decode: out of memory"); return NULL; }
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
    if (!out) { *err = strdup("base64_decode: out of memory"); return NULL; }
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
