/* Host tests for libs/compress (M12.2, D-140). The zlib fixtures in tests/data/compress/ come from
 * Python's stdlib zlib (tests/data/compress/gen.py): an independent encoder, so decoding them is a
 * real cross-check of inflate.c, not a round trip through our own deflate. */
#include "compress/compress.h"
#include "framework/test.h"

#include <stdbool.h>
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
    uint8_t buf[512];
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

/* An empty output needs no buffer: `out` may be NULL when `outCap` is 0. Zero-length stored
 * blocks (what zlib emits for an empty input at level 0, and for every Z_SYNC_FLUSH) must not do
 * pointer arithmetic or a memcpy on that NULL (UB, which UBSan traps here). */
TEST(compressInflateEmptyOutputWithNullBuffer) {
    static const uint8_t storedEmpty[] = {0x78, 0x01, 0x01, 0x00, 0x00, 0xFF,
                                          0xFF, 0x00, 0x00, 0x00, 0x01};
    static const uint8_t fixedEmpty[] = {0x78, 0x9C, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
    static const uint8_t storedOne[] = {0x01, 0x01, 0x00, 0xFE, 0xFF, 'A'};
    size_t outLen = 99, used = 99;
    ASSERT_EQ(compressZlibInflate(storedEmpty, sizeof(storedEmpty), NULL, 0, &outLen, &used),
              STATUS_OK);
    ASSERT_EQ(outLen, (size_t)0);
    ASSERT_EQ(used, sizeof(storedEmpty));
    ASSERT_EQ(compressZlibInflate(fixedEmpty, sizeof(fixedEmpty), NULL, 0, &outLen, &used),
              STATUS_OK);
    ASSERT_EQ(outLen, (size_t)0);
    ASSERT_EQ(used, sizeof(fixedEmpty));
    ASSERT_EQ(compressInflateRaw(storedOne, sizeof(storedOne), NULL, 0, &outLen, &used),
              STATUS_ERR_NO_MEMORY);
    ASSERT_EQ(compressInflateRaw(NULL, 0, NULL, 0, NULL, NULL), STATUS_ERR_INVALID);
    ASSERT_EQ(compressZlibInflate(NULL, 0, NULL, 0, NULL, NULL), STATUS_ERR_INVALID);
}

/* RFC 1950 §2.2: CINFO (the window size, CMF bits 4-7) above 7 is not allowed; zlib rejects it
 * ("invalid window size"). Every CINFO 0..7 with a valid FCHECK is accepted. */
TEST(compressZlibRejectsCinfoAbove7) {
    size_t zLen = 0;
    uint8_t *z = readFixture("text.z6", &zLen);
    ASSERT_TRUE(z != NULL);
    uint8_t *out = malloc(16384);
    ASSERT_TRUE(out != NULL);
    for (uint32_t cinfo = 0; cinfo < 16; cinfo++) {
        z[0] = (uint8_t)((cinfo << 4) | 8);
        z[1] = 0x80; /* FLEVEL 2, FDICT 0; fix up FCHECK below */
        z[1] = (uint8_t)(z[1] + (31 - ((uint32_t)z[0] * 256u + z[1]) % 31u) % 31u);
        size_t outLen = 0;
        Status st = compressZlibInflate(z, zLen, out, 16384, &outLen, NULL);
        ASSERT_EQ(st, cinfo <= 7 ? STATUS_OK : STATUS_ERR_INVALID);
    }
    free(out);
    free(z);
}

/* ---- Adversarial dynamic-block tests (bug-sweeper, M12.2). Each malformed stream below is built
 * so that it would decode *successfully* (or fail differently) if the guard it targets were
 * removed: rejecting it for some other reason would make the test pass against broken code. ---- */

/* Appends a Huffman code: its bits go MSB-first (RFC 1951 §3.1.1). */
static void bsHuff(BitStream *bs, uint32_t code, int len) {
    for (int i = len - 1; i >= 0; i--) {
        bsPut(bs, (code >> i) & 1u, 1);
    }
}

/* Canonical code assignment (RFC 1951 §3.2.2) for `lengths[0..n)`. */
static void canonCodes(const uint8_t *lengths, int n, uint16_t *codes) {
    uint16_t count[16] = {0}, next[16] = {0};
    for (int i = 0; i < n; i++) {
        count[lengths[i]]++;
    }
    count[0] = 0;
    uint16_t code = 0;
    for (int bits = 1; bits < 16; bits++) {
        code = (uint16_t)((code + count[bits - 1]) << 1);
        next[bits] = code;
    }
    for (int i = 0; i < n; i++) {
        codes[i] = lengths[i] != 0 ? next[lengths[i]]++ : 0;
    }
}

/* RFC 1951 §3.2.7's order of the code-length code lengths in the header. */
static const uint8_t CLC_ORDER_TEST[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                           11, 4,  12, 3, 13, 2, 14, 1, 15};

/* One complete code-length code for every test: symbols 0..12 get length 4, 13..18 length 5
 * (13/16 + 6/32 = 1). */
static const uint8_t CL_LENS[19] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5};

/* The dynamic-block header with CL_LENS (HCLEN = 19). `nlit`/`ndist` are the real counts, so a
 * test can write the out-of-range 287/288 and 31/32. */
static void dynHeaderFull(BitStream *bs, int nlit, int ndist) {
    dynHeader(bs, (uint32_t)(nlit - 257), (uint32_t)(ndist - 1), 15);
    for (int i = 0; i < 19; i++) {
        bsPut(bs, CL_LENS[CLC_ORDER_TEST[i]], 3);
    }
}

/* Emits one code-length symbol (plus `extra` in its extra bits for 16/17/18). */
static void bsCl(BitStream *bs, int sym, uint32_t extra) {
    uint16_t codes[19];
    canonCodes(CL_LENS, 19, codes);
    bsHuff(bs, codes[sym], CL_LENS[sym]);
    if (sym == 16) {
        bsPut(bs, extra, 2);
    } else if (sym == 17) {
        bsPut(bs, extra, 3);
    } else if (sym == 18) {
        bsPut(bs, extra, 7);
    }
}

/* Writes a whole dynamic header whose lit/len + distance lengths are `lens[0..nlit+ndist)`, each
 * as a plain code-length symbol (no run codes), and returns the canonical codes of both sets in
 * `litCodes`/`distCodes` for the caller to write the block data with. */
static void dynBlockHeader(BitStream *bs, int nlit, int ndist, const uint8_t *lens,
                           uint16_t *litCodes, uint16_t *distCodes) {
    dynHeaderFull(bs, nlit, ndist);
    for (int i = 0; i < nlit + ndist; i++) {
        bsCl(bs, lens[i], 0);
    }
    canonCodes(lens, nlit, litCodes);
    canonCodes(lens + nlit, ndist, distCodes);
}

typedef struct {
    uint8_t lens[288 + 32];
    uint16_t lit[288];
    uint16_t dist[32];
    BitStream bs;
} DynCase;

static Status dynRun(DynCase *c, uint8_t *out, size_t outCap, size_t *outLen) {
    return compressInflateRaw(c->bs.buf, bsBytes(&c->bs), out, outCap, outLen, NULL);
}

/* zlib's one allowed incomplete code: a distance set holding a single code of length 1. The data
 * is 'A', 'B', then a length-3 match at distance 1 through that code: "ABBBB". Sending the unused
 * bit pattern for the distance instead is an invalid code. */
TEST(compressAcceptsSingleLengthOneDistanceCode) {
    for (int badDist = 0; badDist <= 1; badDist++) {
        DynCase c;
        memset(&c, 0, sizeof(c));
        c.lens['A'] = c.lens['B'] = c.lens[256] = c.lens[257] = 2; /* complete: four 2-bit codes */
        c.lens[258 + 0] = 1;                                       /* distance code 0 only */
        dynBlockHeader(&c.bs, 258, 1, c.lens, c.lit, c.dist);
        bsHuff(&c.bs, c.lit['A'], 2);
        bsHuff(&c.bs, c.lit['B'], 2);
        bsHuff(&c.bs, c.lit[257], 2);
        bsPut(&c.bs, badDist ? 1 : 0, 1); /* code "0" is distance symbol 0; "1" is unassigned */
        bsHuff(&c.bs, c.lit[256], 2);
        uint8_t out[16];
        size_t outLen = 0;
        if (!badDist) {
            ASSERT_EQ(dynRun(&c, out, sizeof(out), &outLen), STATUS_OK);
            ASSERT_EQ(outLen, (size_t)5);
            ASSERT_EQ(memcmp(out, "ABBBB", 5), 0);
        } else {
            ASSERT_EQ(dynRun(&c, out, sizeof(out), &outLen), STATUS_ERR_INVALID);
        }
    }
}

/* Incomplete lit/len (three 2-bit codes) and incomplete distance (two 2-bit codes) sets: without
 * the completeness check both would decode "AB" successfully. */
TEST(compressRejectsIncompleteLitLenAndDistanceCodes) {
    for (int which = 0; which < 2; which++) {
        DynCase c;
        memset(&c, 0, sizeof(c));
        c.lens['A'] = c.lens['B'] = c.lens[256] = 2;
        if (which == 0) {
            c.lens[258 + 0] = 1; /* lit/len incomplete (3 of 4), distance fine */
        } else {
            c.lens[257] = 2;                       /* lit/len complete */
            c.lens[258 + 0] = c.lens[258 + 1] = 2; /* distance incomplete (2 of 4), max len 2 */
        }
        dynBlockHeader(&c.bs, 258, 2, c.lens, c.lit, c.dist);
        bsHuff(&c.bs, c.lit['A'], 2);
        bsHuff(&c.bs, c.lit['B'], 2);
        bsHuff(&c.bs, c.lit[256], 2);
        uint8_t out[16];
        size_t outLen = 0;
        ASSERT_EQ(dynRun(&c, out, sizeof(out), &outLen), STATUS_ERR_INVALID);
    }
}

/* An empty distance set is legal (zlib accepts it) as long as no length code is decoded: a
 * lit/len set of just end-of-block (the single-length-1 case) is an empty block; literals work;
 * a length code then fails at the distance. */
TEST(compressEmptyDistanceSet) {
    uint8_t out[16];
    size_t outLen = 99;
    DynCase c;
    memset(&c, 0, sizeof(c));
    c.lens[256] = 1;
    dynBlockHeader(&c.bs, 257, 1, c.lens, c.lit, c.dist);
    bsHuff(&c.bs, c.lit[256], 1);
    ASSERT_EQ(dynRun(&c, out, sizeof(out), &outLen), STATUS_OK);
    ASSERT_EQ(outLen, (size_t)0);

    memset(&c, 0, sizeof(c));
    c.lens['A'] = c.lens[256] = c.lens[257] = c.lens['B'] = 2;
    dynBlockHeader(&c.bs, 258, 1, c.lens, c.lit, c.dist);
    bsHuff(&c.bs, c.lit['A'], 2);
    bsHuff(&c.bs, c.lit['B'], 2);
    bsHuff(&c.bs, c.lit[256], 2);
    ASSERT_EQ(dynRun(&c, out, sizeof(out), &outLen), STATUS_OK);
    ASSERT_EQ(outLen, (size_t)2);
    ASSERT_EQ(memcmp(out, "AB", 2), 0);

    memset(&c.bs, 0, sizeof(c.bs));
    dynBlockHeader(&c.bs, 258, 1, c.lens, c.lit, c.dist);
    bsHuff(&c.bs, c.lit['A'], 2);
    bsHuff(&c.bs, c.lit[257], 2); /* a match, but there is no distance code */
    bsPut(&c.bs, 0, 16);
    ASSERT_EQ(dynRun(&c, out, sizeof(out), &outLen), STATUS_ERR_INVALID);
}

/* HLIT = 286 and HDIST = 30 are the maxima; 287/288 and 31/32 are rejected even when the extra
 * symbols are never used (each malformed stream here would otherwise decode "AB"). */
TEST(compressHlitHdistBounds) {
    const int nlits[] = {286, 287, 288, 257, 257};
    const int ndists[] = {30, 1, 1, 31, 32};
    for (int t = 0; t < 5; t++) {
        DynCase c;
        memset(&c, 0, sizeof(c));
        int nlit = nlits[t], ndist = ndists[t];
        c.lens['A'] = c.lens['B'] = c.lens[256] = 2;
        c.lens[nlit - 1] = 2; /* the last lit/len symbol completes the set (285, 286 or 287) */
        if (nlit == 257) {
            c.lens[255] = 2;
        }
        c.lens[nlit + 0] = 1;
        c.lens[nlit + ndist - 1] = 1; /* two 1-bit distance codes: 0 and the last (29, 30 or 31) */
        if (ndist == 1) {
            c.lens[nlit + 0] = 1;
        }
        dynBlockHeader(&c.bs, nlit, ndist, c.lens, c.lit, c.dist);
        bsHuff(&c.bs, c.lit['A'], 2);
        bsHuff(&c.bs, c.lit['B'], 2);
        bsHuff(&c.bs, c.lit[256], 2);
        uint8_t out[16];
        size_t outLen = 0;
        bool legal = nlit <= 286 && ndist <= 30;
        ASSERT_EQ(dynRun(&c, out, sizeof(out), &outLen), legal ? STATUS_OK : STATUS_ERR_INVALID);
        if (legal) {
            ASSERT_EQ(outLen, (size_t)2);
            ASSERT_EQ(memcmp(out, "AB", 2), 0);
        }
    }
}

/* A dynamic block with a complete lit/len code but no end-of-block symbol is rejected as INVALID
 * when its tables are read, not left to run into the end of the output buffer (NO_MEMORY). */
TEST(compressRejectsMissingEndOfBlockBeforeDecoding) {
    DynCase c;
    memset(&c, 0, sizeof(c));
    c.lens['A'] = c.lens['B'] = 1;
    c.lens[257 + 0] = 1;
    dynBlockHeader(&c.bs, 257, 1, c.lens, c.lit, c.dist);
    for (int i = 0; i < 4; i++) {
        bsPut(&c.bs, 0, 16); /* 64 x 'A' */
    }
    uint8_t out[4];
    size_t outLen = 0;
    ASSERT_EQ(dynRun(&c, out, sizeof(out), &outLen), STATUS_ERR_INVALID);
    ASSERT_EQ(outLen, (size_t)0);
}

/* Code-length repeat codes: 16 with nothing to repeat is invalid; a repeat (16, 17 or 18) running
 * past HLIT + HDIST is invalid, even though the lengths it would write beyond the end would
 * otherwise just be ignored (each of those streams would decode "A"). */
TEST(compressCodeLengthRepeatBounds) {
    uint8_t out[16];
    size_t outLen = 0;
    BitStream bs;
    memset(&bs, 0, sizeof(bs));
    dynHeaderFull(&bs, 257, 1);
    bsCl(&bs, 16, 0); /* repeat the previous length: there is none */
    bsPut(&bs, 0, 32);
    ASSERT_EQ(compressInflateRaw(bs.buf, bsBytes(&bs), out, sizeof(out), &outLen, NULL),
              STATUS_ERR_INVALID);

    /* HLIT + HDIST = 259; lengths[257] (distance 0) = 1, lengths[258] (distance 1) = 0. Mode 0
     * writes them exactly; modes 1-3 end with a 16 (three 1s), 17 (three 0s) or 18 (eleven 0s)
     * that runs past the end. */
    for (int mode = 0; mode <= 3; mode++) {
        uint8_t lens[257];
        memset(lens, 0, sizeof(lens));
        lens['A'] = lens[256] = 1;
        memset(&bs, 0, sizeof(bs));
        dynHeaderFull(&bs, 257, 2);
        bsCl(&bs, 18, 65 - 11); /* lengths[0..64] = 0 */
        bsCl(&bs, 1, 0);        /* 'A' */
        bsCl(&bs, 18, 138 - 11);
        bsCl(&bs, 18, 52 - 11); /* lengths[66..255] = 0 */
        bsCl(&bs, 1, 0);        /* 256 */
        if (mode == 1) {
            bsCl(&bs, 16, 0); /* [257] = [258] = 1, then one past the end */
        } else {
            bsCl(&bs, 1, 0); /* [257]: distance code 0 */
            if (mode == 0) {
                bsCl(&bs, 0, 0); /* [258] */
            } else {
                bsCl(&bs, mode == 2 ? 17 : 18, 0); /* [258], then 2 or 10 past the end */
            }
        }
        uint16_t lit[257];
        canonCodes(lens, 257, lit);
        bsHuff(&bs, lit['A'], 1);
        bsHuff(&bs, lit[256], 1);
        Status st = compressInflateRaw(bs.buf, bsBytes(&bs), out, sizeof(out), &outLen, NULL);
        ASSERT_EQ(st, mode != 0 ? STATUS_ERR_INVALID : STATUS_OK);
        if (mode == 0) {
            ASSERT_EQ(outLen, (size_t)1);
            ASSERT_EQ(out[0], 'A');
        }
    }
}

/* Fixed-Huffman helpers (RFC 1951 §3.2.6). */
static void fixedLit(BitStream *bs, int sym) {
    if (sym <= 143) {
        bsHuff(bs, 0x30u + (uint32_t)sym, 8);
    } else if (sym <= 255) {
        bsHuff(bs, 0x190u + (uint32_t)(sym - 144), 9);
    } else if (sym <= 279) {
        bsHuff(bs, (uint32_t)(sym - 256), 7);
    } else {
        bsHuff(bs, 0xC0u + (uint32_t)(sym - 280), 8);
    }
}

/* Distances: equal to the output so far is fine, one more is "too far back"; a match before any
 * output is invalid. */
TEST(compressDistanceBounds) {
    for (int dsym = 0; dsym <= 2; dsym++) {
        BitStream bs;
        memset(&bs, 0, sizeof(bs));
        bsPut(&bs, 1, 1);
        bsPut(&bs, 1, 2);
        fixedLit(&bs, 'A');
        fixedLit(&bs, 'B');
        fixedLit(&bs, 257);             /* length 3 */
        bsHuff(&bs, (uint32_t)dsym, 5); /* distance dsym + 1 */
        fixedLit(&bs, 256);
        uint8_t out[16];
        size_t outLen = 0;
        Status st = compressInflateRaw(bs.buf, bsBytes(&bs), out, sizeof(out), &outLen, NULL);
        if (dsym == 2) {
            ASSERT_EQ(st, STATUS_ERR_INVALID); /* distance 3 with 2 bytes out */
        } else {
            ASSERT_EQ(st, STATUS_OK);
            ASSERT_EQ(outLen, (size_t)5);
            ASSERT_EQ(memcmp(out, dsym == 0 ? "ABBBB" : "ABABA", 5), 0);
        }
    }
    BitStream bs;
    memset(&bs, 0, sizeof(bs));
    bsPut(&bs, 1, 1);
    bsPut(&bs, 1, 2);
    fixedLit(&bs, 257);
    bsHuff(&bs, 0, 5); /* distance 1 with nothing output yet */
    fixedLit(&bs, 256);
    uint8_t out[16];
    size_t outLen = 0;
    ASSERT_EQ(compressInflateRaw(bs.buf, bsBytes(&bs), out, sizeof(out), &outLen, NULL),
              STATUS_ERR_INVALID);
    /* Fixed-code distance symbols 30 and 31 exist in the code but are invalid. */
    for (uint32_t d = 30; d <= 31; d++) {
        memset(&bs, 0, sizeof(bs));
        bsPut(&bs, 1, 1);
        bsPut(&bs, 1, 2);
        fixedLit(&bs, 'A');
        fixedLit(&bs, 257);
        bsHuff(&bs, d, 5);
        fixedLit(&bs, 256);
        ASSERT_EQ(compressInflateRaw(bs.buf, bsBytes(&bs), out, sizeof(out), &outLen, NULL),
                  STATUS_ERR_INVALID);
    }
}

/* Length 258 (symbol 285, and 284 + 31 extra), and a match that overruns the output buffer by one
 * byte is NO_MEMORY with nothing written past `outCap`. */
TEST(compressMaxLengthMatch) {
    for (int form = 0; form < 2; form++) {
        BitStream bs;
        memset(&bs, 0, sizeof(bs));
        bsPut(&bs, 1, 1);
        bsPut(&bs, 1, 2);
        fixedLit(&bs, 'x');
        if (form == 0) {
            fixedLit(&bs, 285);
        } else {
            fixedLit(&bs, 284);
            bsPut(&bs, 31, 5);
        }
        bsHuff(&bs, 0, 5); /* distance 1 */
        fixedLit(&bs, 256);
        uint8_t out[300];
        size_t outLen = 0;
        ASSERT_EQ(compressInflateRaw(bs.buf, bsBytes(&bs), out, 259, &outLen, NULL), STATUS_OK);
        ASSERT_EQ(outLen, (size_t)259);
        for (size_t i = 0; i < 259; i++) {
            ASSERT_EQ(out[i], 'x');
        }
        memset(out, 0, sizeof(out));
        ASSERT_EQ(compressInflateRaw(bs.buf, bsBytes(&bs), out, 258, &outLen, NULL),
                  STATUS_ERR_NO_MEMORY);
        ASSERT_EQ(out[258], 0);
    }
}

/* No single byte is a complete DEFLATE stream (the shortest, an empty fixed block, is 10 bits). */
TEST(compressTinyInputs) {
    uint8_t out[8];
    size_t outLen = 0, used = 99;
    for (int b = 0; b < 256; b++) {
        uint8_t in = (uint8_t)b;
        ASSERT_TRUE(compressInflateRaw(&in, 1, out, sizeof(out), &outLen, &used) != STATUS_OK);
        ASSERT_EQ(used, (size_t)0);
    }
    const uint8_t fixedEmpty[] = {0x03, 0x00};
    ASSERT_EQ(compressInflateRaw(fixedEmpty, 2, out, sizeof(out), &outLen, &used), STATUS_OK);
    ASSERT_EQ(outLen, (size_t)0);
    ASSERT_EQ(used, (size_t)2);
    for (size_t n = 0; n < 8; n++) {
        const uint8_t zEmpty[] = {0x78, 0x9C, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
        ASSERT_EQ(compressZlibInflate(zEmpty, n, out, sizeof(out), &outLen, &used),
                  n == 8 ? STATUS_OK : STATUS_ERR_INVALID);
    }
}

static uint32_t xorshift32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Mutation fuzz: 1-4 byte mutations (bit flips, random bytes, biased toward the header and the
 * dynamic tables at the front) of every zlib fixture, at the full and a short output capacity.
 * Under ASan/UBSan nothing may fault; an OK result must satisfy the API's own postconditions
 * (inUsed within the input, outLen within capacity, and the Adler-32 where inUsed says it is). */
TEST(compressFuzzMutatedFixtures) {
    uint32_t seed = 0x5EED1234u;
    int okCount = 0, runs = 0;
    for (size_t n = 0; n < sizeof(FIXTURE_NAMES) / sizeof(FIXTURE_NAMES[0]); n++) {
        for (size_t l = 0; l < sizeof(LEVELS) / sizeof(LEVELS[0]); l++) {
            char name[64];
            snprintf(name, sizeof(name), "%s.%s", FIXTURE_NAMES[n], LEVELS[l]);
            size_t zLen = 0;
            uint8_t *z = readFixture(name, &zLen);
            if (z == NULL) {
                continue;
            }
            uint8_t *m = malloc(zLen);
            uint8_t *out = malloc(32768);
            ASSERT_TRUE(m != NULL && out != NULL);
            for (int iter = 0; iter < 300; iter++) {
                memcpy(m, z, zLen);
                int k = 1 + (int)(xorshift32(&seed) % 4);
                for (int j = 0; j < k; j++) {
                    uint32_t r = xorshift32(&seed);
                    size_t span = (r & 1) && zLen > 64 ? 64 : zLen;
                    size_t pos = (r >> 1) % span;
                    if (r & 0x80000000u) {
                        m[pos] = (uint8_t)(r >> 8);
                    } else {
                        m[pos] ^= (uint8_t)(1u << ((r >> 8) & 7));
                    }
                }
                size_t cap = (iter & 1) ? 32768 : (xorshift32(&seed) % 32768);
                size_t outLen = 0, used = 0;
                runs++;
                if (compressZlibInflate(m, zLen, out, cap, &outLen, &used) == STATUS_OK) {
                    okCount++;
                    ASSERT_TRUE(used >= 8 && used <= zLen && outLen <= cap);
                    uint32_t stored = ((uint32_t)m[used - 4] << 24) |
                                      ((uint32_t)m[used - 3] << 16) | ((uint32_t)m[used - 2] << 8) |
                                      m[used - 1];
                    ASSERT_EQ(compressAdler32(1, out, outLen), stored);
                }
                if (compressInflateRaw(m + 2, zLen - 2, out, cap, &outLen, &used) == STATUS_OK) {
                    ASSERT_TRUE(used >= 1 && used <= zLen - 2 && outLen <= cap);
                }
            }
            free(out);
            free(m);
            free(z);
        }
    }
    ASSERT_TRUE(runs >= 6000);
    printf("  fuzz: %d runs, %d accepted\n", runs, okCount);
}

/* Fixed-code lit/len symbols 286 and 287 exist in the code (RFC 1951 §3.2.6) but never occur in
 * valid data. */
TEST(compressRejectsFixedLitLen286And287) {
    for (int sym = 286; sym <= 287; sym++) {
        BitStream bs;
        memset(&bs, 0, sizeof(bs));
        bsPut(&bs, 1, 1);
        bsPut(&bs, 1, 2);
        fixedLit(&bs, 'A');
        fixedLit(&bs, sym);
        bsPut(&bs, 0, 16);
        uint8_t out[16];
        size_t outLen = 0;
        ASSERT_EQ(compressInflateRaw(bs.buf, bsBytes(&bs), out, sizeof(out), &outLen, NULL),
                  STATUS_ERR_INVALID);
    }
}

/* The nibble-table CRC-32 against a bit-at-a-time reference (reflected 0xEDB88320) over every
 * byte value and a range of lengths and split points, so every table entry is exercised. */
static uint32_t crc32Reference(const uint8_t *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

TEST(compressCrc32MatchesBitwiseReference) {
    uint8_t data[512];
    uint32_t seed = 0xC0C0A5A5u;
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = i < 256 ? (uint8_t)i : (uint8_t)xorshift32(&seed);
    }
    for (size_t n = 0; n <= sizeof(data); n += 7) {
        ASSERT_EQ(compressCrc32(0, data, n), crc32Reference(data, n));
        size_t split = n / 3;
        ASSERT_EQ(compressCrc32(compressCrc32(0, data, split), data + split, n - split),
                  crc32Reference(data, n));
    }
    ASSERT_EQ(compressCrc32(0, "The quick brown fox jumps over the lazy dog", 43),
              (uint32_t)0x414FA339u);
}

/* compressZlibDeflate round trips across the LZ77 edge cases: empty and 1-2 byte inputs, long runs
 * (length-258 matches and every shorter length at the tail), and row strides from 1 up to the
 * 32768 distance limit and beyond it (ignored), on input longer than the window. */
TEST(compressDeflateRoundTripStrides) {
    enum { N = 40000 };
    uint8_t *data = malloc(N), *z = malloc(2 * N + 64), *out = malloc(N);
    ASSERT_TRUE(data != NULL && z != NULL && out != NULL);
    uint32_t seed = 0xBADC0DEu;
    for (size_t i = 0; i < N; i++) {
        /* runs of 0..600 equal bytes between random ones, then a 33000-periodic tail */
        data[i] = (i >= 33000) ? data[i - 33000] : ((xorshift32(&seed) % 97) == 0 ? 0 : 0x5A);
        if (i < 33000 && (i % 1001) < 5) {
            data[i] = (uint8_t)xorshift32(&seed);
        }
    }
    const uint32_t strides[] = {0, 1, 2, 3, 4, 258, 1000, 32768, 32769, 33000, 0xFFFFFFFFu};
    const size_t sizes[] = {0, 1, 2, 3, 4, 258, 259, 262, 1000, N};
    for (size_t s = 0; s < sizeof(strides) / sizeof(strides[0]); s++) {
        for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
            size_t n = sizes[k];
            size_t zLen = 0, outLen = 0, used = 0;
            ASSERT_EQ(compressZlibDeflate(data, n, strides[s], z, 2 * N + 64, &zLen), STATUS_OK);
            ASSERT_EQ(compressZlibInflate(z, zLen, out, n, &outLen, &used), STATUS_OK);
            ASSERT_EQ(outLen, n);
            ASSERT_EQ(used, zLen);
            ASSERT_EQ(memcmp(out, data, n), 0);
        }
    }
    free(data);
    free(z);
    free(out);
}
