/* Host tests for libs/compress (M12.2, D-140). The zlib fixtures in tests/data/compress/ come from
 * Python's stdlib zlib (tests/data/compress/gen.py): an independent encoder, so decoding them is a
 * real cross-check of inflate.c, not a round trip through our own deflate. */
#include "compress/compress.h"
#include "framework/test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reads tests/data/compress/<name> (relative to the repo root, where `make host-tests` runs).
 * The caller frees. NULL on failure. */
static uint8_t *readFixture(const char *name, size_t *len) {
    char path[256];
    snprintf(path, sizeof(path), "tests/data/compress/%s", name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(size > 0 ? (size_t)size : 1);
    if (buf != NULL && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)size;
    return buf;
}

typedef struct {
    uint8_t buf[256];
    size_t bits;
} BitStream;

/* Appends `n` bits of `value`, LSB first (RFC 1951 §3.1.1). */
static void bsPut(BitStream *bs, uint32_t value, int n) {
    for (int i = 0; i < n; i++) {
        if ((bs->bits & 7) == 0) {
            bs->buf[bs->bits >> 3] = 0;
        }
        bs->buf[bs->bits >> 3] |= (uint8_t)(((value >> i) & 1u) << (bs->bits & 7));
        bs->bits++;
    }
}

static size_t bsBytes(const BitStream *bs) {
    return (bs->bits + 7) >> 3;
}

static const char *const FIXTURE_NAMES[] = {"empty", "one", "zeros", "text", "noise", "ramp"};
static const char *const LEVELS[] = {"z0", "z1", "z6", "z9"};

TEST(compressInflatePythonFixtures) {
    int decoded = 0;
    for (size_t n = 0; n < sizeof(FIXTURE_NAMES) / sizeof(FIXTURE_NAMES[0]); n++) {
        char name[64];
        snprintf(name, sizeof(name), "%s.raw", FIXTURE_NAMES[n]);
        size_t rawLen = 0;
        uint8_t *raw = readFixture(name, &rawLen);
        ASSERT_TRUE(raw != NULL);
        for (size_t l = 0; l < sizeof(LEVELS) / sizeof(LEVELS[0]); l++) {
            snprintf(name, sizeof(name), "%s.%s", FIXTURE_NAMES[n], LEVELS[l]);
            size_t zLen = 0;
            uint8_t *z = readFixture(name, &zLen);
            if (z == NULL) {
                continue; /* not every input has every level (gen.py) */
            }
            uint8_t *out = malloc(rawLen + 1);
            ASSERT_TRUE(out != NULL);
            size_t outLen = 0, used = 0;
            Status st = compressZlibInflate(z, zLen, out, rawLen, &outLen, &used);
            ASSERT_EQ(st, STATUS_OK);
            ASSERT_EQ(outLen, rawLen);
            ASSERT_EQ(used, zLen);
            ASSERT_EQ(memcmp(out, raw, rawLen), 0);
            decoded++;
            free(out);
            free(z);
        }
        free(raw);
    }
    ASSERT_TRUE(decoded >= 15);
}

/* Trailing bytes after the zlib stream are left alone, and Adler-32 is read where the DEFLATE
 * stream ends, not at the end of the buffer. */
TEST(compressZlibTrailingBytes) {
    size_t zLen = 0, rawLen = 0;
    uint8_t *z = readFixture("text.z6", &zLen);
    uint8_t *raw = readFixture("text.raw", &rawLen);
    ASSERT_TRUE(z != NULL && raw != NULL);
    uint8_t *padded = malloc(zLen + 5);
    uint8_t *out = malloc(rawLen);
    ASSERT_TRUE(padded != NULL && out != NULL);
    memcpy(padded, z, zLen);
    memset(padded + zLen, 0xAB, 5);
    size_t outLen = 0, used = 0;
    ASSERT_EQ(compressZlibInflate(padded, zLen + 5, out, rawLen, &outLen, &used), STATUS_OK);
    ASSERT_EQ(used, zLen);
    ASSERT_EQ(outLen, rawLen);
    ASSERT_EQ(memcmp(out, raw, rawLen), 0);
    free(padded);
    free(out);
    free(z);
    free(raw);
}

TEST(compressOutputTooSmallIsNoMemory) {
    size_t zLen = 0, rawLen = 0;
    uint8_t *z = readFixture("text.z6", &zLen);
    uint8_t *raw = readFixture("text.raw", &rawLen);
    ASSERT_TRUE(z != NULL && raw != NULL);
    uint8_t *out = malloc(rawLen);
    ASSERT_TRUE(out != NULL);
    size_t outLen = 0;
    ASSERT_EQ(compressZlibInflate(z, zLen, out, rawLen - 1, &outLen, NULL), STATUS_ERR_NO_MEMORY);
    ASSERT_TRUE(outLen <= rawLen - 1);
    ASSERT_EQ(compressZlibInflate(z, zLen, out, 0, &outLen, NULL), STATUS_ERR_NO_MEMORY);
    /* The stored-block path reports the same way. */
    size_t z0Len = 0;
    uint8_t *z0 = readFixture("text.z0", &z0Len);
    ASSERT_TRUE(z0 != NULL);
    ASSERT_EQ(compressZlibInflate(z0, z0Len, out, rawLen - 1, &outLen, NULL), STATUS_ERR_NO_MEMORY);
    free(z0);
    free(out);
    free(z);
    free(raw);
}

/* Every strict prefix of a valid stream must be rejected, never crash (ASan/UBSan are on), for
 * stored, fixed, and dynamic blocks. */
TEST(compressTruncationEveryOffset) {
    const char *const names[] = {"text.z0", "text.z1", "text.z9", "zeros.z9", "ramp.z6"};
    for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        size_t zLen = 0;
        uint8_t *z = readFixture(names[n], &zLen);
        ASSERT_TRUE(z != NULL);
        uint8_t *out = malloc(70000);
        ASSERT_TRUE(out != NULL);
        for (size_t cut = 0; cut < zLen; cut++) {
            size_t outLen = 0;
            ASSERT_TRUE(compressZlibInflate(z, cut, out, 70000, &outLen, NULL) != STATUS_OK);
        }
        free(out);
        free(z);
    }
}

/* A corrupted Adler-32 or header is rejected. */
TEST(compressZlibRejectsBadFraming) {
    size_t zLen = 0;
    uint8_t *z = readFixture("text.z6", &zLen);
    ASSERT_TRUE(z != NULL);
    uint8_t out[16384];
    size_t outLen = 0;
    z[zLen - 1] ^= 0x01;
    ASSERT_EQ(compressZlibInflate(z, zLen, out, sizeof(out), &outLen, NULL), STATUS_ERR_INVALID);
    z[zLen - 1] ^= 0x01;
    ASSERT_EQ(compressZlibInflate(z, zLen, out, sizeof(out), &outLen, NULL), STATUS_OK);
    z[0] = 0x79; /* CM=9 */
    ASSERT_EQ(compressZlibInflate(z, zLen, out, sizeof(out), &outLen, NULL), STATUS_ERR_INVALID);
    z[0] = 0x78;
    z[1] |= 0x20; /* FDICT */
    ASSERT_EQ(compressZlibInflate(z, zLen, out, sizeof(out), &outLen, NULL), STATUS_ERR_INVALID);
    free(z);
}

/* Starts a dynamic block header (BFINAL=1, BTYPE=2) with the given field values. */
static void dynHeader(BitStream *bs, uint32_t hlit, uint32_t hdist, uint32_t hclen) {
    bsPut(bs, 1, 1);
    bsPut(bs, 2, 2);
    bsPut(bs, hlit, 5);
    bsPut(bs, hdist, 5);
    bsPut(bs, hclen, 4);
}

TEST(compressRejectsOversubscribedCodeLengthCode) {
    BitStream bs;
    memset(&bs, 0, sizeof(bs));
    dynHeader(&bs, 0, 0, 15); /* 19 code-length code lengths follow */
    for (int i = 0; i < 19; i++) {
        bsPut(&bs, 1, 3); /* nineteen codes of length 1: wildly over-subscribed */
    }
    uint8_t out[64];
    size_t outLen = 0;
    ASSERT_EQ(compressInflateRaw(bs.buf, bsBytes(&bs), out, sizeof(out), &outLen, NULL),
              STATUS_ERR_INVALID);
}

TEST(compressRejectsIncompleteCodeLengthCode) {
    BitStream bs;
    memset(&bs, 0, sizeof(bs));
    dynHeader(&bs, 0, 0, 0); /* 4 code-length code lengths: symbols 16, 17, 18, 0 */
    bsPut(&bs, 0, 3);
    bsPut(&bs, 0, 3);
    bsPut(&bs, 0, 3);
    bsPut(&bs, 1, 3); /* a single length-1 code: incomplete, and never valid for this alphabet */
    bsPut(&bs, 0, 8);
    uint8_t out[64];
    size_t outLen = 0;
    ASSERT_EQ(compressInflateRaw(bs.buf, bsBytes(&bs), out, sizeof(out), &outLen, NULL),
              STATUS_ERR_INVALID);
}

TEST(compressRejectsOversizedHlitHdist) {
    uint8_t out[64];
    size_t outLen = 0;
    BitStream bs;
    memset(&bs, 0, sizeof(bs));
    dynHeader(&bs, 30, 0, 0); /* HLIT = 30 -> 287 lit/len codes (max 286) */
    bsPut(&bs, 0, 32);
    ASSERT_EQ(compressInflateRaw(bs.buf, bsBytes(&bs), out, sizeof(out), &outLen, NULL),
              STATUS_ERR_INVALID);
    memset(&bs, 0, sizeof(bs));
    dynHeader(&bs, 0, 31, 0); /* HDIST = 31 -> 32 distance codes (max 30) */
    bsPut(&bs, 0, 32);
    ASSERT_EQ(compressInflateRaw(bs.buf, bsBytes(&bs), out, sizeof(out), &outLen, NULL),
              STATUS_ERR_INVALID);
}

/* A dynamic block whose lit/len lengths give symbol 256 length 0 can never terminate. Code-length
 * alphabet: symbols 0 and 1 with length 1 each (complete). Lit/len lengths: 257 zeros then, as the
 * distance code, one zero: every symbol, including 256, is length 0. */
TEST(compressRejectsDynamicBlockWithoutEndOfBlock) {
    BitStream bs;
    memset(&bs, 0, sizeof(bs));
    dynHeader(&bs, 0, 0, 1); /* HLIT=257, HDIST=1, 5 code-length code lengths */
    /* order 16,17,18,0,8: give 18 and 0 length 1 */
    bsPut(&bs, 0, 3);
    bsPut(&bs, 0, 3);
    bsPut(&bs, 1, 3);
    bsPut(&bs, 1, 3);
    bsPut(&bs, 0, 3);
    /* 258 zero lengths: code "1" (symbol 18? canonical: 0 -> '0', 18 -> '1') repeated. Symbol 0
     * is the lower value so gets code 0; symbol 18 gets code 1 followed by 7 bits (rep-11). */
    bsPut(&bs, 1, 1);   /* 18 */
    bsPut(&bs, 127, 7); /* 138 zeros */
    bsPut(&bs, 1, 1);
    bsPut(&bs, 108, 7); /* 119 zeros: 257 total */
    bsPut(&bs, 0, 1);   /* one more zero (the distance length) */
    uint8_t out[64];
    size_t outLen = 0;
    ASSERT_EQ(compressInflateRaw(bs.buf, bsBytes(&bs), out, sizeof(out), &outLen, NULL),
              STATUS_ERR_INVALID);
}

TEST(compressRejectsReservedBlockType) {
    uint8_t in[] = {0x07, 0x00}; /* BFINAL=1, BTYPE=3 */
    uint8_t out[8];
    size_t outLen = 0;
    ASSERT_EQ(compressInflateRaw(in, sizeof(in), out, sizeof(out), &outLen, NULL),
              STATUS_ERR_INVALID);
}

TEST(compressStoredBlockLenMismatch) {
    uint8_t in[] = {0x01, 0x03, 0x00, 0xFC, 0xFF, 'a', 'b', 'c'}; /* valid: LEN=3 NLEN=~3 */
    uint8_t out[8];
    size_t outLen = 0, used = 0;
    ASSERT_EQ(compressInflateRaw(in, sizeof(in), out, sizeof(out), &outLen, &used), STATUS_OK);
    ASSERT_EQ(outLen, (size_t)3);
    ASSERT_EQ(used, sizeof(in));
    in[3] = 0xFD; /* NLEN no longer the complement */
    ASSERT_EQ(compressInflateRaw(in, sizeof(in), out, sizeof(out), &outLen, &used),
              STATUS_ERR_INVALID);
}

TEST(compressDeflateRoundTrip) {
    uint8_t data[6000];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(((i * 37) ^ (i >> 3)) & ((i % 400) < 200 ? 0xFF : 0x0F));
    }
    for (uint32_t stride = 0; stride <= 96; stride += 96) {
        uint8_t z[16384], out[sizeof(data)];
        size_t zLen = 0, outLen = 0, used = 0;
        ASSERT_EQ(compressZlibDeflate(data, sizeof(data), stride, z, sizeof(z), &zLen), STATUS_OK);
        ASSERT_EQ(compressZlibInflate(z, zLen, out, sizeof(out), &outLen, &used), STATUS_OK);
        ASSERT_EQ(outLen, sizeof(data));
        ASSERT_EQ(used, zLen);
        ASSERT_EQ(memcmp(out, data, sizeof(data)), 0);
    }
    /* An output buffer that is too small is NO_MEMORY, not a silent truncation. */
    uint8_t small[32];
    size_t n = 0;
    ASSERT_EQ(compressZlibDeflate(data, sizeof(data), 0, small, sizeof(small), &n),
              STATUS_ERR_NO_MEMORY);
}

TEST(compressChecksumVectors) {
    ASSERT_EQ(compressCrc32(0, "123456789", 9), (uint32_t)0xCBF43926u);
    ASSERT_EQ(compressCrc32(0, "", 0), (uint32_t)0);
    /* Running form: feeding in two pieces equals one shot. */
    uint32_t part = compressCrc32(0, "1234", 4);
    ASSERT_EQ(compressCrc32(part, "56789", 5), (uint32_t)0xCBF43926u);
    const uint8_t wiki[] = "Wikipedia";
    ASSERT_EQ(compressAdler32(1, wiki, 9), (uint32_t)0x11E60398u);
    ASSERT_EQ(compressAdler32(1, wiki, 0), (uint32_t)1);
    uint32_t a = compressAdler32(1, wiki, 4);
    ASSERT_EQ(compressAdler32(a, wiki + 4, 5), (uint32_t)0x11E60398u);
}
