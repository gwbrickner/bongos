/* SHA-256 (FIPS 180-4 section 6.2). EXPERIMENTAL crypto (ARCHITECTURE §17). Big-endian conversion
 * is done with explicit byte operations, so the code is endian- and alignment-independent. The
 * only table is the public round-constant array K (indexed by the round number, never by data).
 * Message-derived working values are wiped before returning. */
#include "crypto/sha256.h"

#include "crypto/wipe.h"

/* First 32 bits of the fractional parts of the cube roots of the first 64 primes (FIPS 180-4
 * 4.2.2). Public constants. */
static const uint32_t sha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

/* First 32 bits of the fractional parts of the square roots of the first 8 primes (FIPS 180-4
 * 5.3.3): the initial chaining value. */
static const uint32_t sha256Init0[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n)))) /* 0 < n < 32 */

static uint32_t load32Be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store32Be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Compresses one 64-byte block into ctx->h. Internal; the working variables and message schedule
 * are wiped on the way out. */
static void sha256Block(Sha256Ctx *ctx, const uint8_t *block) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = load32Be(block + 4 * i);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR32(w[i - 15], 7) ^ ROTR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR32(w[i - 2], 17) ^ ROTR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t bigS1 = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + bigS1 + ch + sha256K[i] + w[i];
        uint32_t bigS0 = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = bigS0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
    ctx->h[5] += f;
    ctx->h[6] += g;
    ctx->h[7] += h;

    cryptoWipe(w, sizeof(w));
    cryptoWipe(&a, sizeof(a));
    cryptoWipe(&b, sizeof(b));
    cryptoWipe(&c, sizeof(c));
    cryptoWipe(&d, sizeof(d));
    cryptoWipe(&e, sizeof(e));
    cryptoWipe(&f, sizeof(f));
    cryptoWipe(&g, sizeof(g));
    cryptoWipe(&h, sizeof(h));
}

/* Contract: see crypto/sha256.h. */
void sha256Init(Sha256Ctx *ctx) {
    for (int i = 0; i < 8; i++) {
        ctx->h[i] = sha256Init0[i];
    }
    ctx->totalBytes = 0;
    for (size_t i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ctx->buf[i] = 0;
    }
    ctx->bufLen = 0;
}

/* Contract: see crypto/sha256.h. */
void sha256Update(Sha256Ctx *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->totalBytes += (uint64_t)len;

    while (len > 0) {
        if (ctx->bufLen == 0 && len >= SHA256_BLOCK_SIZE) {
            sha256Block(ctx, p); /* whole block straight from the caller's buffer */
            p += SHA256_BLOCK_SIZE;
            len -= SHA256_BLOCK_SIZE;
            continue;
        }
        size_t room = SHA256_BLOCK_SIZE - ctx->bufLen;
        size_t n = len < room ? len : room;
        for (size_t i = 0; i < n; i++) {
            ctx->buf[ctx->bufLen + i] = p[i];
        }
        ctx->bufLen += (uint32_t)n;
        p += n;
        len -= n;
        if (ctx->bufLen == SHA256_BLOCK_SIZE) {
            sha256Block(ctx, ctx->buf);
            ctx->bufLen = 0;
        }
    }
}

/* Contract: see crypto/sha256.h. */
void sha256Final(Sha256Ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE]) {
    uint64_t bitLen = ctx->totalBytes << 3; /* wraps mod 2^64 for >= 2^61 bytes, see header */

    /* Padding: 0x80, zeros up to 56 mod 64, then the 64-bit big-endian bit length. */
    ctx->buf[ctx->bufLen++] = 0x80;
    if (ctx->bufLen > 56) {
        while (ctx->bufLen < SHA256_BLOCK_SIZE) {
            ctx->buf[ctx->bufLen++] = 0;
        }
        sha256Block(ctx, ctx->buf);
        ctx->bufLen = 0;
    }
    while (ctx->bufLen < 56) {
        ctx->buf[ctx->bufLen++] = 0;
    }
    store32Be(ctx->buf + 56, (uint32_t)(bitLen >> 32));
    store32Be(ctx->buf + 60, (uint32_t)bitLen);
    sha256Block(ctx, ctx->buf);

    for (int i = 0; i < 8; i++) {
        store32Be(out + 4 * i, ctx->h[i]);
    }
    cryptoWipe(ctx, sizeof(*ctx));
}
