/* ktests for libs/crypto (M2.6 step 4): the primitives, built into the kernel, reproduce the
 * RFC 8439 / FIPS 180-4 vectors of libs/crypto/test/crypto-vectors.h (the same header the host
 * tests use). The CSPRNG (step 5) is only as good as these. */
#include "crypto-vectors.h"
#include "crypto/chacha20.h"
#include "crypto/sha256.h"
#include "ktest.h"

static bool bytesEqual(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

/* RFC 8439 2.3.2 (the first vector) and Appendix A.1 #1..#5. */
KTEST(chacha20_rfc8439_block) {
    for (size_t i = 0; i < CHACHA20_BLOCK_VECTOR_COUNT; i++) {
        const ChaCha20BlockVector *v = &chacha20BlockVectors[i];
        uint8_t out[CHACHA20_BLOCK_SIZE];
        chacha20Block(v->key, v->counter, v->nonce, out);
        if (!bytesEqual(out, v->expected, sizeof(out))) {
            ktestFail(ktestCtx, __FILE__, __LINE__, "block vector %s differs", v->name);
            return;
        }
    }
}

/* RFC 8439 2.4.2 ("sunscreen") and Appendix A.2 #1..#3, encrypting and decrypting, plus the
 * 32-bit counter-wrap guard. */
KTEST(chacha20_rfc8439_encrypt) {
    static uint8_t out[512];
    for (size_t i = 0; i < CHACHA20_XOR_VECTOR_COUNT; i++) {
        const ChaCha20XorVector *v = &chacha20XorVectors[i];
        /* The tail after `len` (up to a whole block) must stay untouched: a final partial block
         * written out in full would otherwise go unnoticed in this oversized buffer. */
        KTEST_ASSERT(v->len + CHACHA20_BLOCK_SIZE <= sizeof(out));
        for (size_t j = 0; j < sizeof(out); j++) {
            out[j] = 0xEE;
        }
        KTEST_ASSERT(chacha20Xor(v->key, v->counter, v->nonce, v->plaintext, out, v->len) ==
                     STATUS_OK);
        if (!bytesEqual(out, v->ciphertext, v->len)) {
            ktestFail(ktestCtx, __FILE__, __LINE__, "encrypt vector %s differs", v->name);
            return;
        }
        KTEST_ASSERT(chacha20Xor(v->key, v->counter, v->nonce, out, out, v->len) == STATUS_OK);
        if (!bytesEqual(out, v->plaintext, v->len)) {
            ktestFail(ktestCtx, __FILE__, __LINE__, "in-place decrypt %s differs", v->name);
            return;
        }
        for (size_t j = v->len; j < v->len + CHACHA20_BLOCK_SIZE; j++) {
            if (out[j] != 0xEE) {
                ktestFail(ktestCtx, __FILE__, __LINE__, "vector %s: byte %u past the end written",
                          v->name, (unsigned)(j - v->len));
                return;
            }
        }
    }
    const ChaCha20XorVector *v = &chacha20XorVectors[0];
    KTEST_ASSERT(chacha20Xor(v->key, 0xFFFFFFFFu, v->nonce, v->plaintext, out, 65) ==
                 STATUS_ERR_INVALID);
    KTEST_ASSERT(chacha20Xor(v->key, 0xFFFFFFFFu, v->nonce, v->plaintext, out, 64) == STATUS_OK);
}

/* FIPS 180-4 examples: "", "abc", the 448-bit message, 1,000,000 x 'a' (streamed), plus the
 * padding-boundary digests of crypto-vectors.h, each also split into two updates at every offset.
 */
KTEST(sha256_fips180_vectors) {
    uint8_t d[SHA256_DIGEST_SIZE];
    Sha256Ctx ctx;
    for (size_t i = 0; i < SHA256_VECTOR_COUNT; i++) {
        const Sha256Vector *v = &sha256Vectors[i];
        sha256Init(&ctx);
        sha256Update(&ctx, v->msg, v->len);
        sha256Final(&ctx, d);
        if (!bytesEqual(d, v->digest, sizeof(d))) {
            ktestFail(ktestCtx, __FILE__, __LINE__, "sha256 vector %s differs", v->name);
            return;
        }
        /* Split at every offset of the message (the longest is 56 bytes, so this exercises the
         * buffered partial-block path; the 1000-byte chunks below cross block edges). */
        for (size_t s = 0; s <= v->len; s++) {
            sha256Init(&ctx);
            sha256Update(&ctx, v->msg, s);
            sha256Update(&ctx, v->msg + s, v->len - s);
            sha256Final(&ctx, d);
            KTEST_ASSERT(bytesEqual(d, v->digest, sizeof(d)));
        }
    }

    /* Padding-boundary lengths (55 = exactly one block with padding, 56..63 need a second
     * block, ...): none of the FIPS messages has them (crypto-vectors.h). */
    static uint8_t msg[SHA256_BOUNDARY_MAX_LEN];
    for (size_t i = 0; i < sizeof(msg); i++) {
        msg[i] = (uint8_t)(i * 13 + 7);
    }
    for (size_t i = 0; i < SHA256_BOUNDARY_VECTOR_COUNT; i++) {
        const Sha256LenVector *v = &sha256BoundaryVectors[i];
        KTEST_ASSERT(v->len <= sizeof(msg));
        sha256Init(&ctx);
        sha256Update(&ctx, msg, v->len);
        sha256Final(&ctx, d);
        if (!bytesEqual(d, v->digest, sizeof(d))) {
            ktestFail(ktestCtx, __FILE__, __LINE__, "sha256 boundary len %u differs",
                      (unsigned)v->len);
            return;
        }
        /* Every split point: a partial first update leaves bytes buffered, so a second update
         * of >= 64 bytes must go through the buffer, not compress the caller's data directly.
         * The million-'a' message can't catch that (every alignment of it looks the same), and
         * the FIPS messages above are too short to reach it. */
        for (size_t s = 0; s <= v->len; s++) {
            sha256Init(&ctx);
            sha256Update(&ctx, msg, s);
            sha256Update(&ctx, msg + s, v->len - s);
            sha256Final(&ctx, d);
            if (!bytesEqual(d, v->digest, sizeof(d))) {
                ktestFail(ktestCtx, __FILE__, __LINE__, "sha256 boundary len %u split %u differs",
                          (unsigned)v->len, (unsigned)s);
                return;
            }
        }
    }

    uint8_t chunk[1000];
    for (size_t i = 0; i < sizeof(chunk); i++) {
        chunk[i] = 'a';
    }
    sha256Init(&ctx);
    for (int i = 0; i < 1000; i++) {
        sha256Update(&ctx, chunk, sizeof(chunk));
    }
    sha256Final(&ctx, d);
    KTEST_ASSERT(bytesEqual(d, sha256MillionADigest, sizeof(d)));
}
