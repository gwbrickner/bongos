/* Host tests for libs/crypto: ChaCha20, SHA-256 and cryptoWipe (M2.6 step 4). The vectors live in
 * libs/crypto/test/crypto-vectors.h and are shared with the kernel's ktests. */
#include "crypto-vectors.h"
#include "crypto/chacha20.h"
#include "crypto/sha256.h"
#include "crypto/wipe.h"
#include "framework/test.h"

#include <stdlib.h>
#include <string.h>

TEST(chacha20BlockVectors) {
    for (size_t i = 0; i < CHACHA20_BLOCK_VECTOR_COUNT; i++) {
        const ChaCha20BlockVector *v = &chacha20BlockVectors[i];
        uint8_t out[64];
        memset(out, 0xEE, sizeof(out));
        chacha20Block(v->key, v->counter, v->nonce, out);
        if (memcmp(out, v->expected, 64) != 0) {
            fprintf(stderr, "  chacha20Block vector %s differs\n", v->name);
        }
        ASSERT_TRUE(memcmp(out, v->expected, 64) == 0);
    }
}

TEST(chacha20XorVectors) {
    for (size_t i = 0; i < CHACHA20_XOR_VECTOR_COUNT; i++) {
        const ChaCha20XorVector *v = &chacha20XorVectors[i];
        uint8_t *out = malloc(v->len);
        ASSERT_TRUE(out != NULL);
        Status st = chacha20Xor(v->key, v->counter, v->nonce, v->plaintext, out, v->len);
        int ok = st == STATUS_OK && memcmp(out, v->ciphertext, v->len) == 0;
        if (!ok) {
            fprintf(stderr, "  chacha20Xor encrypt vector %s differs\n", v->name);
        }
        /* Decryption is the same operation. */
        st = chacha20Xor(v->key, v->counter, v->nonce, v->ciphertext, out, v->len);
        int okBack = st == STATUS_OK && memcmp(out, v->plaintext, v->len) == 0;
        free(out);
        ASSERT_TRUE(ok);
        ASSERT_TRUE(okBack);
    }
}

TEST(chacha20XorInPlace) {
    for (size_t i = 0; i < CHACHA20_XOR_VECTOR_COUNT; i++) {
        const ChaCha20XorVector *v = &chacha20XorVectors[i];
        uint8_t *buf = malloc(v->len);
        ASSERT_TRUE(buf != NULL);
        memcpy(buf, v->plaintext, v->len);
        ASSERT_EQ(chacha20Xor(v->key, v->counter, v->nonce, buf, buf, v->len), STATUS_OK);
        int ok = memcmp(buf, v->ciphertext, v->len) == 0;
        free(buf);
        ASSERT_TRUE(ok);
    }
}

TEST(chacha20XorZeroLength) {
    uint8_t key[32] = {0};
    uint8_t nonce[12] = {0};
    uint8_t out[4] = {1, 2, 3, 4};
    /* Succeeds for any counter (even the last one) and touches nothing, NULL buffers included. */
    ASSERT_EQ(chacha20Xor(key, 0xFFFFFFFFu, nonce, out, out, 0), STATUS_OK);
    ASSERT_EQ(chacha20Xor(key, 0, nonce, NULL, NULL, 0), STATUS_OK);
    ASSERT_EQ(out[0], 1);
    ASSERT_EQ(out[3], 4);
}

TEST(chacha20XorRejectsCounterWrap) {
    uint8_t key[32] = {0};
    uint8_t nonce[12] = {0};
    uint8_t in[200], out[200];
    memset(in, 0x5A, sizeof(in));

    /* Counter 0xFFFFFFFF has exactly one block left: 1..64 bytes fit, 65 would wrap. */
    memset(out, 0xEE, sizeof(out));
    ASSERT_EQ(chacha20Xor(key, 0xFFFFFFFFu, nonce, in, out, 65), STATUS_ERR_INVALID);
    ASSERT_EQ(out[0], 0xEE); /* nothing written on failure */
    ASSERT_EQ(out[64], 0xEE);
    ASSERT_EQ(chacha20Xor(key, 0xFFFFFFFFu, nonce, in, out, 64), STATUS_OK);
    ASSERT_EQ(chacha20Xor(key, 0xFFFFFFFFu, nonce, in, out, 1), STATUS_OK);

    /* The final block's keystream is the same as chacha20Block's. */
    uint8_t ks[64];
    chacha20Block(key, 0xFFFFFFFFu, nonce, ks);
    ASSERT_EQ(chacha20Xor(key, 0xFFFFFFFFu, nonce, in, out, 64), STATUS_OK);
    for (int i = 0; i < 64; i++) {
        ASSERT_EQ(out[i], (uint8_t)(in[i] ^ ks[i]));
    }

    /* Two blocks needed from 0xFFFFFFFE fit; three do not. */
    ASSERT_EQ(chacha20Xor(key, 0xFFFFFFFEu, nonce, in, out, 128), STATUS_OK);
    ASSERT_EQ(chacha20Xor(key, 0xFFFFFFFEu, nonce, in, out, 129), STATUS_ERR_INVALID);
    ASSERT_EQ(chacha20Xor(key, 0xFFFFFFFDu, nonce, in, out, 129), STATUS_OK);
    /* A huge length is rejected without reading a byte (the pointers are never dereferenced). */
    ASSERT_EQ(chacha20Xor(key, 1, nonce, in, out, (size_t)-1), STATUS_ERR_INVALID);
    ASSERT_EQ(chacha20Xor(key, 0, nonce, in, out, (size_t)1 << 40), STATUS_ERR_INVALID);
}

TEST(chacha20XorRejectsNull) {
    uint8_t key[32] = {0};
    uint8_t nonce[12] = {0};
    uint8_t buf[8] = {0};
    ASSERT_EQ(chacha20Xor(NULL, 0, nonce, buf, buf, 8), STATUS_ERR_INVALID);
    ASSERT_EQ(chacha20Xor(key, 0, NULL, buf, buf, 8), STATUS_ERR_INVALID);
    ASSERT_EQ(chacha20Xor(key, 0, nonce, NULL, buf, 8), STATUS_ERR_INVALID);
    ASSERT_EQ(chacha20Xor(key, 0, nonce, buf, NULL, 8), STATUS_ERR_INVALID);
}

/* Odd lengths straddling block boundaries and every buffer misalignment, checked against the
 * block function directly. Also covers the counter advancing block to block. */
TEST(chacha20XorUnalignedAndPartialBlocks) {
    uint8_t key[32], nonce[12];
    for (int i = 0; i < 32; i++) {
        key[i] = (uint8_t)(i * 7 + 1);
    }
    for (int i = 0; i < 12; i++) {
        nonce[i] = (uint8_t)(0xA0 + i);
    }
    enum { MAXLEN = 200 };
    uint8_t ks[4 * 64];
    for (int b = 0; b < 4; b++) {
        chacha20Block(key, 77u + (uint32_t)b, nonce, ks + 64 * b);
    }
    static const size_t lens[] = {1, 2, 63, 64, 65, 127, 128, 129, 191, MAXLEN};
    for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        for (size_t off = 0; off < 8; off++) {
            uint8_t *inRaw = malloc(MAXLEN + 16);
            uint8_t *outRaw = malloc(MAXLEN + 16);
            ASSERT_TRUE(inRaw != NULL && outRaw != NULL);
            uint8_t *in = inRaw + off, *out = outRaw + (7 - off);
            for (size_t i = 0; i < lens[li]; i++) {
                in[i] = (uint8_t)(i * 3 + 5);
            }
            memset(outRaw, 0xEE, MAXLEN + 16);
            Status st = chacha20Xor(key, 77u, nonce, in, out, lens[li]);
            int ok = st == STATUS_OK;
            for (size_t i = 0; i < lens[li]; i++) {
                ok = ok && out[i] == (uint8_t)(in[i] ^ ks[i]);
            }
            ok = ok && out[lens[li]] == 0xEE; /* one byte past the end untouched */
            free(inRaw);
            free(outRaw);
            ASSERT_TRUE(ok);
        }
    }
}

static void sha256Hex(const void *msg, size_t len, uint8_t out[32]) {
    Sha256Ctx ctx;
    sha256Init(&ctx);
    sha256Update(&ctx, msg, len);
    sha256Final(&ctx, out);
}

TEST(sha256Vectors) {
    for (size_t i = 0; i < SHA256_VECTOR_COUNT; i++) {
        const Sha256Vector *v = &sha256Vectors[i];
        uint8_t d[32];
        sha256Hex(v->msg, v->len, d);
        if (memcmp(d, v->digest, 32) != 0) {
            fprintf(stderr, "  sha256 vector %s differs\n", v->name);
        }
        ASSERT_TRUE(memcmp(d, v->digest, 32) == 0);
    }
}

TEST(sha256OneMillionA) {
    uint8_t chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    Sha256Ctx ctx;
    sha256Init(&ctx);
    for (int i = 0; i < 1000; i++) {
        sha256Update(&ctx, chunk, sizeof(chunk));
    }
    uint8_t d[32];
    sha256Final(&ctx, d);
    ASSERT_TRUE(memcmp(d, sha256MillionADigest, 32) == 0);
}

/* Every split point of every length around the 55/56/63/64/65-byte padding edges and two
 * blocks: the digest must equal the one-shot digest, whichever way the data is chunked. */
TEST(sha256ChunkedUpdateEquivalence) {
    uint8_t msg[200];
    for (size_t i = 0; i < sizeof(msg); i++) {
        msg[i] = (uint8_t)(i * 13 + 7);
    }
    static const size_t lens[] = {0,  1,   54,  55,  56,  57,  63,  64,
                                  65, 119, 120, 127, 128, 129, 191, 200};
    for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        size_t len = lens[li];
        uint8_t want[32];
        sha256Hex(msg, len, want);
        for (size_t split = 0; split <= len; split++) {
            Sha256Ctx ctx;
            uint8_t got[32];
            sha256Init(&ctx);
            sha256Update(&ctx, msg, split);
            sha256Update(&ctx, msg + split, len - split);
            sha256Final(&ctx, got);
            if (memcmp(got, want, 32) != 0) {
                fprintf(stderr, "  sha256 len %zu split %zu differs\n", len, split);
            }
            ASSERT_TRUE(memcmp(got, want, 32) == 0);
        }
        /* Byte at a time. */
        Sha256Ctx ctx;
        uint8_t got[32];
        sha256Init(&ctx);
        for (size_t i = 0; i < len; i++) {
            sha256Update(&ctx, msg + i, 1);
        }
        sha256Final(&ctx, got);
        ASSERT_TRUE(memcmp(got, want, 32) == 0);
    }
}

/* Digests at 55/57/63/64/65/119/120/128 bytes (crypto-vectors.h): pins the padding decision in
 * sha256Final against an independent implementation. The FIPS vectors plus the self-consistency
 * test above could not tell `bufLen > 56` from `bufLen >= 56` (a 55-byte message). */
TEST(sha256PaddingBoundaryVectors) {
    uint8_t msg[SHA256_BOUNDARY_MAX_LEN];
    for (size_t i = 0; i < sizeof(msg); i++) {
        msg[i] = (uint8_t)(i * 13 + 7);
    }
    for (size_t i = 0; i < SHA256_BOUNDARY_VECTOR_COUNT; i++) {
        const Sha256LenVector *v = &sha256BoundaryVectors[i];
        ASSERT_TRUE(v->len <= sizeof(msg));
        uint8_t d[32];
        sha256Hex(msg, v->len, d);
        if (memcmp(d, v->digest, 32) != 0) {
            fprintf(stderr, "  sha256 boundary vector len %zu differs\n", v->len);
        }
        ASSERT_TRUE(memcmp(d, v->digest, 32) == 0);
    }
}

/* The 64-bit length field's high word, which no feasible message reaches (it needs >= 512 MiB):
 * white-box, `totalBytes` is preset as if 2^32 (then 2^61 - 4) bytes had already been absorbed,
 * then "abc" is hashed. Expected digests are the compression of the single padded block with
 * bit length 0x0000000800000018 (0xFFFFFFFFFFFFFFF8, the largest supported), computed by a
 * from-scratch Python SHA-256 that matches hashlib on real messages. Catches a length stored as
 * 32 bits or a dropped/garbled high word. */
TEST(sha256LengthFieldHighWord) {
    static const struct {
        uint64_t preset;
        uint8_t digest[32];
    } cases[] = {
        {0x100000000ULL, {0x82, 0x67, 0xcd, 0x2a, 0xbf, 0xac, 0xc0, 0x72, 0x81, 0x63, 0xd6,
                          0xfb, 0x04, 0x59, 0x3e, 0x11, 0x25, 0x7a, 0x93, 0x5c, 0x59, 0xb4,
                          0x4d, 0x4f, 0x65, 0x71, 0x45, 0xc3, 0xea, 0xef, 0x6f, 0xf1}},
        {0x1FFFFFFFFFFFFFFCULL, {0xd7, 0x2a, 0xad, 0xe5, 0xe7, 0x79, 0x2d, 0xb1, 0xa7, 0xb9, 0x6e,
                                 0x5b, 0x65, 0x2c, 0xab, 0x5b, 0x8e, 0xd0, 0x25, 0x82, 0xf3, 0xa7,
                                 0x92, 0x74, 0xd9, 0x41, 0xa6, 0x84, 0xc5, 0x37, 0xaf, 0xc9}},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Sha256Ctx ctx;
        uint8_t d[32];
        sha256Init(&ctx);
        ctx.totalBytes = cases[i].preset;
        sha256Update(&ctx, "abc", 3);
        sha256Final(&ctx, d);
        ASSERT_TRUE(memcmp(d, cases[i].digest, 32) == 0);
    }
}

TEST(sha256UnalignedInputAndZeroLengthUpdate) {
    uint8_t raw[80 + 8];
    for (size_t i = 0; i < sizeof(raw); i++) {
        raw[i] = (uint8_t)(i + 1);
    }
    uint8_t want[32];
    sha256Hex(raw + 1, 80, want);
    for (size_t off = 0; off < 8; off++) {
        uint8_t copy[80];
        memcpy(copy, raw + 1, 80);
        uint8_t *shifted = malloc(80 + 8);
        ASSERT_TRUE(shifted != NULL);
        memcpy(shifted + off, copy, 80);
        Sha256Ctx ctx;
        uint8_t got[32];
        sha256Init(&ctx);
        sha256Update(&ctx, NULL, 0); /* no-op, NULL allowed */
        sha256Update(&ctx, shifted + off, 80);
        sha256Update(&ctx, shifted, 0);
        sha256Final(&ctx, got);
        free(shifted);
        ASSERT_TRUE(memcmp(got, want, 32) == 0);
    }
}

TEST(sha256FinalWipesContext) {
    Sha256Ctx ctx;
    uint8_t d[32];
    sha256Init(&ctx);
    sha256Update(&ctx, "abc", 3);
    sha256Final(&ctx, d);
    const uint8_t *p = (const uint8_t *)&ctx;
    int nonzero = 0;
    for (size_t i = 0; i < sizeof(ctx); i++) {
        nonzero |= p[i];
    }
    ASSERT_EQ(nonzero, 0);
}

TEST(cryptoWipeZeroesExactlyTheRange) {
    uint8_t buf[32];
    memset(buf, 0xAB, sizeof(buf));
    cryptoWipe(buf + 4, 20);
    for (size_t i = 0; i < sizeof(buf); i++) {
        ASSERT_EQ(buf[i], (i >= 4 && i < 24) ? 0 : 0xAB);
    }
    cryptoWipe(NULL, 0); /* legal no-op */
}
