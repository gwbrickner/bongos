/* Host tests for libs/gfx's PNG/BMP decoders (M12.2, D-146). The main oracle is
 * tests/data/gfx/decode-fixtures.z: files and expected premultiplied pixels produced by an
 * independent Python encoder (tests/data/gfx/gen.py). The rest are malformed-input, truncation,
 * mutation-fuzz, allocation-failure and limit tests, all run under ASan/UBSan/LSan with an
 * allocator that counts what is still live. */
#include "compress/compress.h"
#include "framework/test.h"
#include "gfx/gfx-image.h"
#include "gfx/gfx.h"
#include "gfx_golden.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- an allocator that can fail the Nth allocation and counts live blocks ---------------- */

typedef struct {
    int failAt; /* fail allocation number failAt (0-based); -1 = never */
    int count, live;
    size_t liveBytes, peakBytes;
} CountAlloc;

static void *caAlloc(void *ctx, size_t n) {
    CountAlloc *c = ctx;
    if (c->count++ == c->failAt) {
        return NULL;
    }
    void *p = malloc(n != 0 ? n : 1);
    if (p != NULL) {
        c->live++;
        c->liveBytes += n;
        if (c->liveBytes > c->peakBytes) {
            c->peakBytes = c->liveBytes;
        }
    }
    return p;
}

static void caFree(void *ctx, void *p, size_t n) {
    CountAlloc *c = ctx;
    if (p != NULL) {
        c->live--;
        c->liveBytes -= n;
    }
    free(p);
}

#define CA_INIT(name, fail)                                                                        \
    CountAlloc name##State = {fail, 0, 0, 0, 0};                                                   \
    GfxAllocator name = {caAlloc, caFree, &name##State}

/* ---- fixtures -------------------------------------------------------------------------- */

typedef struct {
    char name[96];
    uint8_t kind; /* 1 PNG, 2 BMP */
    uint32_t w, h;
    const uint8_t *data;
    uint32_t dataLen;
    const uint8_t *expected; /* w*h little-endian u32 */
} Fixture;

typedef struct {
    uint8_t *raw;
    Fixture *fx;
    int n;
} FixtureSet;

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool loadFixtures(FixtureSet *fs) {
    memset(fs, 0, sizeof(*fs));
    FILE *f = fopen("tests/data/gfx/decode-fixtures.z", "rb");
    if (f == NULL) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long zn = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *z = malloc((size_t)zn);
    size_t rawCap = 8u << 20;
    fs->raw = malloc(rawCap);
    if (z == NULL || fs->raw == NULL || fread(z, 1, (size_t)zn, f) != (size_t)zn) {
        fclose(f);
        free(z);
        return false;
    }
    fclose(f);
    size_t rawLen = 0;
    Status st = compressZlibInflate(z, (size_t)zn, fs->raw, rawCap, &rawLen, NULL);
    free(z);
    if (st != STATUS_OK) {
        return false;
    }
    fs->fx = calloc(1024, sizeof(Fixture));
    size_t off = 0;
    while (off < rawLen && fs->n < 1024) {
        Fixture *x = &fs->fx[fs->n];
        uint32_t nameLen = (uint32_t)fs->raw[off] | ((uint32_t)fs->raw[off + 1] << 8);
        off += 2;
        memcpy(x->name, fs->raw + off, nameLen < 95 ? nameLen : 95);
        off += nameLen;
        x->kind = fs->raw[off];
        x->w = le32(fs->raw + off + 1);
        x->h = le32(fs->raw + off + 5);
        x->dataLen = le32(fs->raw + off + 9);
        off += 13;
        x->data = fs->raw + off;
        off += x->dataLen;
        x->expected = fs->raw + off;
        off += (size_t)x->w * x->h * 4;
        fs->n++;
    }
    return off == rawLen;
}

static void freeFixtures(FixtureSet *fs) {
    free(fs->raw);
    free(fs->fx);
}

static const Fixture *findFixture(const FixtureSet *fs, const char *name) {
    for (int i = 0; i < fs->n; i++) {
        if (strcmp(fs->fx[i].name, name) == 0) {
            return &fs->fx[i];
        }
    }
    return NULL;
}

static bool premulOk(uint32_t c) {
    uint32_t a = c >> 24;
    return ((c >> 16) & 0xFF) <= a && ((c >> 8) & 0xFF) <= a && (c & 0xFF) <= a;
}

TEST(gfxDecodeMatchesIndependentFixtures) {
    FixtureSet fs;
    ASSERT_TRUE(loadFixtures(&fs));
    ASSERT_TRUE(fs.n >= 250);
    int png = 0, bmp = 0;
    for (int i = 0; i < fs.n; i++) {
        const Fixture *x = &fs.fx[i];
        CA_INIT(ca, -1);
        GfxImage img;
        Status st = x->kind == 1 ? gfxPngDecode(x->data, x->dataLen, NULL, &ca, &img)
                                 : gfxBmpDecode(x->data, x->dataLen, NULL, &ca, &img);
        if (st != STATUS_OK) {
            fprintf(stderr, "  fixture %s: status %d\n", x->name, (int)st);
        }
        ASSERT_EQ(st, STATUS_OK);
        ASSERT_EQ(img.width, x->w);
        ASSERT_EQ(img.height, x->h);
        for (uint32_t p = 0; p < x->w * x->h; p++) {
            uint32_t want = le32(x->expected + (size_t)p * 4);
            if (img.pixels[p] != want) {
                fprintf(stderr, "  fixture %s: pixel %u (x=%u y=%u) got %08X want %08X\n", x->name,
                        p, p % x->w, p / x->w, img.pixels[p], want);
            }
            ASSERT_EQ(img.pixels[p], want);
        }
        /* the sniffing entry point agrees */
        GfxImage viaSniff;
        ASSERT_EQ(gfxImageDecode(x->data, x->dataLen, NULL, &ca, &viaSniff), STATUS_OK);
        ASSERT_EQ(memcmp(viaSniff.pixels, img.pixels, (size_t)x->w * x->h * 4), 0);
        gfxImageFree(&viaSniff);
        gfxImageFree(&img);
        gfxImageFree(&img); /* idempotent */
        ASSERT_EQ(caState.live, 0);
        png += x->kind == 1;
        bmp += x->kind == 2;
    }
    ASSERT_TRUE(png >= 150 && bmp >= 90);
    freeFixtures(&fs);
}

TEST(gfxImageSniffAndSurface) {
    FixtureSet fs;
    ASSERT_TRUE(loadFixtures(&fs));
    GfxImage img;
    uint8_t junk[16] = {'J', 'F', 'I', 'F'};
    ASSERT_EQ(gfxImageDecode(junk, sizeof(junk), NULL, NULL, &img), STATUS_ERR_UNSUPPORTED);
    ASSERT_TRUE(img.pixels == NULL);
    ASSERT_EQ(gfxImageDecode(junk, 0, NULL, NULL, &img), STATUS_ERR_UNSUPPORTED);
    ASSERT_EQ(gfxPngDecode(junk, sizeof(junk), NULL, NULL, &img), STATUS_ERR_INVALID);
    const Fixture *x = findFixture(&fs, "png_t6_d8_9x9");
    ASSERT_TRUE(x != NULL);
    ASSERT_EQ(gfxImageDecode(x->data, x->dataLen, NULL, NULL, &img), STATUS_OK);
    GfxSurface s = gfxImageSurface(&img);
    ASSERT_EQ(s.width, 9);
    ASSERT_EQ(s.stride, 9);
    ASSERT_TRUE(s.pixels == img.pixels);
    gfxImageFree(&img);
    gfxImageFree(NULL);
    freeFixtures(&fs);
}

/* ---- helpers for crafted files --------------------------------------------------------- */

typedef struct {
    uint8_t *b;
    size_t n, cap;
} Buf;

static void bufPut(Buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->b = realloc(b->b, b->cap);
    }
    if (n != 0) {
        memcpy(b->b + b->n, p, n);
    }
    b->n += n;
}

static void put32be(Buf *b, uint32_t v) {
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    bufPut(b, x, 4);
}

static void pngSig(Buf *b) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    bufPut(b, sig, 8);
}

/* A chunk with a correct CRC and a stated length `len` (the payload is what `data` holds). */
static void pngChunk(Buf *b, const char *type, const void *data, size_t n) {
    put32be(b, (uint32_t)n);
    bufPut(b, type, 4);
    bufPut(b, data, n);
    uint32_t crc = compressCrc32(0, type, 4);
    crc = compressCrc32(crc, data, n);
    put32be(b, crc);
}

static void pngIhdr(Buf *b, uint32_t w, uint32_t h, int depth, int ctype, int comp, int filt,
                    int interlace) {
    Buf x = {0};
    put32be(&x, w);
    put32be(&x, h);
    uint8_t t[5] = {(uint8_t)depth, (uint8_t)ctype, (uint8_t)comp, (uint8_t)filt,
                    (uint8_t)interlace};
    bufPut(&x, t, 5);
    pngChunk(b, "IHDR", x.b, x.n);
    free(x.b);
}

/* zlib of `n` bytes (fixed Huffman via our own deflate). */
static void pngIdat(Buf *b, const uint8_t *raw, size_t n) {
    size_t cap = n + n / 4 + 128, len = 0;
    uint8_t *z = malloc(cap);
    Status st = compressZlibDeflate(raw, n, 0, z, cap, &len);
    if (st == STATUS_OK) {
        pngChunk(b, "IDAT", z, len);
    }
    free(z);
}

/* A valid w x h non-interlaced image of `ctype`/`depth` with all-zero filter-0 rows. */
static void pngImage(Buf *b, uint32_t w, uint32_t h, int depth, int ctype, int extra) {
    static const int ch[7] = {1, 0, 3, 1, 2, 0, 4};
    size_t rb = ((size_t)w * (size_t)ch[ctype] * (size_t)depth + 7) / 8;
    size_t n = (rb + 1) * h;
    uint8_t *raw = calloc(n, 1);
    pngSig(b);
    pngIhdr(b, w, h, depth, ctype, 0, 0, 0);
    if (ctype == 3) {
        uint8_t pal[6] = {1, 2, 3, 4, 5, 6};
        pngChunk(b, "PLTE", pal, 6);
    }
    (void)extra;
    pngIdat(b, raw, n);
    pngChunk(b, "IEND", NULL, 0);
    free(raw);
}

/* The image keeps a pointer to its allocator, so the allocator must outlive it: use a static. */
static GfxAllocator gAlloc = {caAlloc, caFree, NULL};

static Status decodeBuf(const Buf *b, const GfxDecodeLimits *lim, CountAlloc *st, GfxImage *img) {
    gAlloc.ctx = st;
    return gfxPngDecode(b->b, b->n, lim, &gAlloc, img);
}

#define EXPECT_PNG(buf, want)                                                                      \
    do {                                                                                           \
        CountAlloc s_ = {-1, 0, 0, 0, 0};                                                          \
        GfxImage im_;                                                                              \
        Status r_ = decodeBuf(&(buf), NULL, &s_, &im_);                                            \
        if (r_ != (want)) {                                                                        \
            fprintf(stderr, "  line %d: got %d want %d\n", __LINE__, (int)r_, (int)(want));        \
        }                                                                                          \
        ASSERT_EQ(r_, (want));                                                                     \
        ASSERT_TRUE((r_ == STATUS_OK) == (im_.pixels != NULL));                                    \
        gfxImageFree(&im_);                                                                        \
        ASSERT_EQ(s_.live, 0);                                                                     \
        free((buf).b);                                                                             \
    } while (0)

TEST(gfxPngMalformedHeaders) {
    Buf b;
    /* a plain valid file first, so the helpers themselves are known good */
    b = (Buf){0};
    pngImage(&b, 3, 2, 8, 6, 0);
    EXPECT_PNG(b, STATUS_OK);
    int badCombos[][2] = {{0, 3}, {0, 5}, {2, 4}, {2, 1}, {3, 16},
                          {4, 4}, {6, 1}, {1, 8}, {5, 8}, {7, 8}};
    for (size_t i = 0; i < sizeof(badCombos) / sizeof(badCombos[0]); i++) {
        b = (Buf){0};
        pngSig(&b);
        pngIhdr(&b, 2, 2, badCombos[i][1], badCombos[i][0], 0, 0, 0);
        pngIdat(&b, (const uint8_t *)"\0\0\0\0\0\0\0\0\0\0\0\0", 12);
        pngChunk(&b, "IEND", NULL, 0);
        EXPECT_PNG(b, STATUS_ERR_INVALID);
    }
    struct {
        uint32_t w, h;
        int comp, filt, il;
    } bad[] = {{0, 4, 0, 0, 0},           {4, 0, 0, 0, 0},  {0x80000000u, 1, 0, 0, 0},
               {1, 0x80000000u, 0, 0, 0}, {2, 2, 1, 0, 0},  {2, 2, 0, 1, 0},
               {2, 2, 0, 0, 2},           {2, 2, 0, 0, 255}};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        b = (Buf){0};
        pngSig(&b);
        pngIhdr(&b, bad[i].w, bad[i].h, 8, 6, bad[i].comp, bad[i].filt, bad[i].il);
        pngIdat(&b, (const uint8_t *)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 18);
        pngChunk(&b, "IEND", NULL, 0);
        EXPECT_PNG(b, STATUS_ERR_INVALID);
    }
    /* IHDR of the wrong length, a duplicate IHDR, and IHDR not first */
    b = (Buf){0};
    pngSig(&b);
    pngChunk(&b, "IHDR", "\0\0\0\1\0\0\0\1\10\6\0\0", 12);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 6, 0, 0, 0);
    pngIhdr(&b, 1, 1, 8, 6, 0, 0, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    b = (Buf){0};
    pngSig(&b);
    pngChunk(&b, "gAMA", "\0\0\0\0", 4);
    pngIhdr(&b, 1, 1, 8, 6, 0, 0, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
}

TEST(gfxPngMalformedChunks) {
    Buf b;
    uint8_t zero[64] = {0};
    /* missing IDAT, missing IEND, IEND with data */
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 6, 0, 0, 0);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 6, 0, 0, 0);
    pngIdat(&b, zero, 5);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 6, 0, 0, 0);
    pngIdat(&b, zero, 5);
    pngChunk(&b, "IEND", zero, 1);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    /* IDAT interleaved with another chunk */
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 6, 0, 0, 0);
    Buf tmp = {0};
    pngIdat(&tmp, zero, 5);
    size_t half = (tmp.n - 12) / 2;
    pngChunk(&b, "IDAT", tmp.b + 8, half);
    pngChunk(&b, "tEXt", "k\0v", 3);
    pngChunk(&b, "IDAT", tmp.b + 8 + half, tmp.n - 12 - half);
    pngChunk(&b, "IEND", NULL, 0);
    free(tmp.b);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    /* chunk length 0xFFFFFFFF and lengths that run past the end */
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 6, 0, 0, 0);
    put32be(&b, 0xFFFFFFFFu);
    bufPut(&b, "IDAT", 4);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 6, 0, 0, 0);
    put32be(&b, 100);
    bufPut(&b, "IDAT", 4);
    bufPut(&b, zero, 10);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    /* bad CRC on an ancillary chunk and on IDAT */
    b = (Buf){0};
    pngImage(&b, 2, 2, 8, 6, 0);
    b.b[8 + 25 + 8 + 2] ^= 0x01; /* inside the first chunk after IHDR (IDAT here) */
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    /* unknown critical chunk: UNSUPPORTED; unknown ancillary chunk: skipped */
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 0, 0, 0, 0);
    pngChunk(&b, "XYZW", zero, 3);
    pngIdat(&b, zero, 2);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_UNSUPPORTED);
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 0, 0, 0, 0);
    pngChunk(&b, "xyZw", zero, 3);
    pngIdat(&b, zero, 2);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_OK);
    /* data after IEND is ignored */
    b = (Buf){0};
    pngImage(&b, 2, 2, 8, 6, 0);
    bufPut(&b, "garbage garbage garbage", 23);
    EXPECT_PNG(b, STATUS_OK);
}

TEST(gfxPngMalformedPaletteAndTransparency) {
    Buf b;
    uint8_t zero[64] = {0};
    uint8_t rows[4] = {0, 0, 0, 0};
    uint8_t pal3[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    /* PLTE length not a multiple of 3, empty, > 256 entries, and > 2^depth entries */
    size_t plens[] = {4, 0, 771};
    for (size_t i = 0; i < 3; i++) {
        b = (Buf){0};
        pngSig(&b);
        pngIhdr(&b, 1, 1, 8, 3, 0, 0, 0);
        uint8_t big[771] = {0};
        pngChunk(&b, "PLTE", big, plens[i]);
        pngIdat(&b, rows, 2);
        pngChunk(&b, "IEND", NULL, 0);
        EXPECT_PNG(b, STATUS_ERR_INVALID);
    }
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 1, 3, 0, 0, 0); /* 1-bit palette: at most 2 entries */
    pngChunk(&b, "PLTE", pal3, 9);
    pngIdat(&b, rows, 2);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    /* missing PLTE for a palette image; PLTE for gray / gray+alpha; PLTE after IDAT; duplicate */
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 3, 0, 0, 0);
    pngIdat(&b, rows, 2);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    for (int ct = 0; ct <= 4; ct += 4) {
        b = (Buf){0};
        pngSig(&b);
        pngIhdr(&b, 1, 1, 8, ct, 0, 0, 0);
        pngChunk(&b, "PLTE", pal3, 3);
        pngIdat(&b, zero, ct == 0 ? 2 : 3);
        pngChunk(&b, "IEND", NULL, 0);
        EXPECT_PNG(b, STATUS_ERR_INVALID);
    }
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 3, 0, 0, 0);
    pngIdat(&b, rows, 2);
    pngChunk(&b, "PLTE", pal3, 3);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 3, 0, 0, 0);
    pngChunk(&b, "PLTE", pal3, 3);
    pngChunk(&b, "PLTE", pal3, 3);
    pngIdat(&b, rows, 2);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    /* palette index past the palette (a valid, decodable image otherwise) */
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 3, 0, 0, 0);
    pngChunk(&b, "PLTE", pal3, 6);
    uint8_t badIdx[2] = {0, 2};
    pngIdat(&b, badIdx, 2);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    /* tRNS: longer than PLTE, before PLTE, wrong length for gray/RGB, after IDAT, duplicate */
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 3, 0, 0, 0);
    pngChunk(&b, "PLTE", pal3, 6);
    pngChunk(&b, "tRNS", zero, 3);
    pngIdat(&b, rows, 2);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 3, 0, 0, 0);
    pngChunk(&b, "tRNS", zero, 1);
    pngChunk(&b, "PLTE", pal3, 6);
    pngIdat(&b, rows, 2);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 0, 0, 0, 0);
    pngChunk(&b, "tRNS", zero, 4);
    pngIdat(&b, rows, 2);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 2, 0, 0, 0);
    pngChunk(&b, "tRNS", zero, 2);
    pngIdat(&b, zero, 4);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 0, 0, 0, 0);
    pngIdat(&b, rows, 2);
    pngChunk(&b, "tRNS", zero, 2);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 0, 0, 0, 0);
    pngChunk(&b, "tRNS", zero, 2);
    pngChunk(&b, "tRNS", zero, 2);
    pngIdat(&b, rows, 2);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
}

TEST(gfxPngMalformedImageData) {
    Buf b;
    /* row filter type 5, and 255 */
    uint8_t f5[2] = {5, 0};
    uint8_t fff[2] = {255, 0};
    uint8_t *cases[2] = {f5, fff};
    for (int i = 0; i < 2; i++) {
        b = (Buf){0};
        pngSig(&b);
        pngIhdr(&b, 1, 1, 8, 0, 0, 0, 0);
        pngIdat(&b, cases[i], 2);
        pngChunk(&b, "IEND", NULL, 0);
        EXPECT_PNG(b, STATUS_ERR_INVALID);
    }
    /* too little / too much inflated data for the IHDR */
    uint8_t few[3] = {0, 0, 0};
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 4, 4, 8, 0, 0, 0, 0); /* needs 4*(1+4) = 20 bytes */
    pngIdat(&b, few, 3);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    uint8_t many[64] = {0};
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 0, 0, 0, 0); /* needs 2 */
    pngIdat(&b, many, 64);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    /* garbage instead of a zlib stream, and a corrupt Adler-32 */
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 0, 0, 0, 0);
    pngChunk(&b, "IDAT", "not a zlib stream at all", 24);
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
    b = (Buf){0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 0, 0, 0, 0);
    {
        uint8_t raw[2] = {0, 0};
        uint8_t z[64];
        size_t zl = 0;
        ASSERT_EQ(compressZlibDeflate(raw, 2, 0, z, sizeof(z), &zl), STATUS_OK);
        z[zl - 1] ^= 0x55;
        pngChunk(&b, "IDAT", z, zl);
    }
    pngChunk(&b, "IEND", NULL, 0);
    EXPECT_PNG(b, STATUS_ERR_INVALID);
}

/* A 1x1 image whose IDAT inflates to megabytes: rejected once it exceeds what IHDR allows,
 * without ever allocating anything near that size. */
TEST(gfxPngDecompressionBomb) {
    size_t n = 10u << 20;
    uint8_t *zeros = calloc(n, 1);
    size_t cap = n / 100 + 4096, zl = 0;
    uint8_t *z = malloc(cap);
    ASSERT_TRUE(zeros != NULL && z != NULL);
    ASSERT_EQ(compressZlibDeflate(zeros, n, 3, z, cap, &zl), STATUS_OK);
    ASSERT_TRUE(zl < (size_t)n / 20);
    Buf b = {0};
    pngSig(&b);
    pngIhdr(&b, 1, 1, 8, 0, 0, 0, 0);
    pngChunk(&b, "IDAT", z, zl);
    pngChunk(&b, "IEND", NULL, 0);
    CountAlloc st = {-1, 0, 0, 0, 0};
    GfxImage im;
    ASSERT_EQ(decodeBuf(&b, NULL, &st, &im), STATUS_ERR_INVALID);
    ASSERT_EQ(st.live, 0);
    ASSERT_TRUE(st.peakBytes < 1u << 20); /* nowhere near the 10 MiB it inflates to */
    free(b.b);
    free(zeros);
    free(z);
}

TEST(gfxPngLimitsRejectBeforeAllocating) {
    Buf b = {0};
    uint8_t zero[8] = {0};
    pngSig(&b);
    pngIhdr(&b, 0x7FFFFFFFu, 0x7FFFFFFFu, 8, 6, 0, 0, 0);
    pngIdat(&b, zero, 2);
    pngChunk(&b, "IEND", NULL, 0);
    CountAlloc st = {-1, 0, 0, 0, 0};
    GfxImage im;
    ASSERT_EQ(decodeBuf(&b, NULL, &st, &im), STATUS_ERR_UNSUPPORTED);
    ASSERT_TRUE(st.peakBytes < 1u << 14); /* only the small header state was ever allocated */
    ASSERT_EQ(st.live, 0);
    free(b.b);

    Buf ok = {0};
    pngImage(&ok, 100, 50, 8, 6, 0);
    GfxDecodeLimits lim = {99, 1000, 1u << 20, 1u << 20};
    CountAlloc s2 = {-1, 0, 0, 0, 0};
    ASSERT_EQ(decodeBuf(&ok, &lim, &s2, &im), STATUS_ERR_UNSUPPORTED); /* width */
    lim = (GfxDecodeLimits){1000, 49, 1u << 20, 1u << 20};
    ASSERT_EQ(decodeBuf(&ok, &lim, &s2, &im), STATUS_ERR_UNSUPPORTED); /* height */
    lim = (GfxDecodeLimits){1000, 1000, 4999, 1u << 20};
    ASSERT_EQ(decodeBuf(&ok, &lim, &s2, &im), STATUS_ERR_UNSUPPORTED); /* pixels */
    lim = (GfxDecodeLimits){1000, 1000, 5000, 5000};
    ASSERT_EQ(decodeBuf(&ok, &lim, &s2, &im), STATUS_ERR_UNSUPPORTED); /* byte budget */
    lim = (GfxDecodeLimits){100, 50, 5000, 1u << 20};
    ASSERT_EQ(decodeBuf(&ok, &lim, &s2, &im), STATUS_OK); /* exactly at every limit */
    gfxImageFree(&im);
    ASSERT_EQ(s2.live, 0);
    free(ok.b);
}

/* ---- BMP crafted cases ---------------------------------------------------------------- */

static void put32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static Status bmpDecode(const uint8_t *d, size_t n, const GfxDecodeLimits *lim, CountAlloc *st,
                        GfxImage *img) {
    gAlloc.ctx = st;
    Status r = gfxBmpDecode(d, n, lim, &gAlloc, img);
    if (r != STATUS_OK) {
        if (img->pixels != NULL) {
            return -100;
        }
    }
    gfxImageFree(img);
    return r;
}

#define EXPECT_BMP(bytes, n, want)                                                                 \
    do {                                                                                           \
        CountAlloc s_ = {-1, 0, 0, 0, 0};                                                          \
        GfxImage im_;                                                                              \
        Status r_ = bmpDecode((bytes), (n), NULL, &s_, &im_);                                      \
        if (r_ != (want)) {                                                                        \
            fprintf(stderr, "  line %d: got %d want %d\n", __LINE__, (int)r_, (int)(want));        \
        }                                                                                          \
        ASSERT_EQ(r_, (want));                                                                     \
        ASSERT_EQ(s_.live, 0);                                                                     \
    } while (0)

TEST(gfxBmpMalformedHeaders) {
    FixtureSet fs;
    ASSERT_TRUE(loadFixtures(&fs));
    const Fixture *x24 = findFixture(&fs, "bmp_b24_3x5");
    const Fixture *x32 = findFixture(&fs, "bmp_b32_bf888_3x5");
    const Fixture *p8 = findFixture(&fs, "bmp_p8_c0_3x5");
    const Fixture *p8s = findFixture(&fs, "bmp_p8_c200_3x5");
    ASSERT_TRUE(x24 != NULL && x32 != NULL && p8 != NULL && p8s != NULL);
    uint8_t buf[4096];
    ASSERT_TRUE(x24->dataLen < sizeof(buf) && x32->dataLen < sizeof(buf) &&
                p8->dataLen < sizeof(buf));

    /* offsets: 10 offBits, 14 dibSize, 18 width, 22 height, 26 planes, 28 bpp, 30 comp, 46 clrUsed
     */
#define FROM(fx)                                                                                   \
    memcpy(buf, (fx)->data, (fx)->dataLen);                                                        \
    const size_t len = (fx)->dataLen;                                                              \
    (void)len
    {
        FROM(x24);
        EXPECT_BMP(buf, len, STATUS_OK);
        put32le(buf + 18, 0); /* width 0 */
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
    }
    {
        FROM(x24);
        put32le(buf + 18, (uint32_t)-3); /* negative width */
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
    }
    {
        FROM(x24);
        put32le(buf + 22, 0);
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
        put32le(buf + 22, 0x80000000u); /* INT32_MIN height */
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
    }
    {
        FROM(x24);
        buf[26] = 2; /* planes != 1 */
        EXPECT_BMP(buf, len, STATUS_ERR_UNSUPPORTED);
    }
    {
        FROM(x24);
        buf[28] = 2; /* bpp 2 */
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
        buf[28] = 15;
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
        buf[28] = 64;
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
    }
    {
        FROM(p8);
        buf[30] = 1; /* RLE8 */
        EXPECT_BMP(buf, len, STATUS_ERR_UNSUPPORTED);
        buf[30] = 2; /* RLE4 */
        EXPECT_BMP(buf, len, STATUS_ERR_UNSUPPORTED);
        buf[30] = 4; /* embedded JPEG */
        EXPECT_BMP(buf, len, STATUS_ERR_UNSUPPORTED);
        buf[30] = 5; /* embedded PNG */
        EXPECT_BMP(buf, len, STATUS_ERR_UNSUPPORTED);
        buf[30] = 7; /* unknown */
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
        buf[30] = 3; /* BITFIELDS on a paletted image */
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
    }
    {
        FROM(x24);
        put32le(buf + 14, 12); /* OS/2 core header */
        EXPECT_BMP(buf, len, STATUS_ERR_UNSUPPORTED);
        put32le(buf + 14, 64); /* OS/2 v2 header */
        EXPECT_BMP(buf, len, STATUS_ERR_UNSUPPORTED);
        put32le(buf + 14, 41);
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
        put32le(buf + 14, 0xFFFFFFFFu);
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
    }
    {
        FROM(x24); /* pixel data truncated by one byte, and a bogus offBits */
        EXPECT_BMP(buf, len - 1, STATUS_ERR_INVALID);
        put32le(buf + 10, 0xFFFFFFF0u);
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
        put32le(buf + 10, 20); /* inside the header */
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
    }
    {
        FROM(p8);
        put32le(buf + 46, 257); /* clrUsed > 256 */
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
        EXPECT_BMP(buf, 14 + 40 + 100, STATUS_ERR_INVALID); /* palette runs past the file */
    }
    {
        FROM(p8s); /* clrUsed = 200 but a pixel index >= 200 */
        EXPECT_BMP(buf, len, STATUS_OK);
        uint32_t off = (uint32_t)buf[10] | ((uint32_t)buf[11] << 8);
        buf[off] = 200;
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
    }
    {
        FROM(x24); /* huge dimensions must be refused before the pixel array is even looked at */
        put32le(buf + 18, 0x7FFFFFFF);
        put32le(buf + 22, 0x7FFFFFFF);
        CountAlloc st = {-1, 0, 0, 0, 0};
        GfxImage im;
        ASSERT_EQ(bmpDecode(buf, len, NULL, &st, &im), STATUS_ERR_UNSUPPORTED);
        ASSERT_EQ(st.count, 0);
    }
    {
        /* bit-field masks: 0xFFFFFFFF, overlapping, holes, all-zero color, and a wide 16-bit mask
         */
        FROM(x32);
        uint32_t base = 14 + 40; /* masks follow the 40-byte header */
        put32le(buf + base, 0xFFFFFFFFu);
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID); /* g and b masks are then overlapping/zero... */
        put32le(buf + base, 0x00FF0000u);
        put32le(buf + base + 4, 0x00FF0000u); /* overlap */
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
        put32le(buf + base + 4, 0x0000FF00u);
        put32le(buf + base + 8, 0x000000A5u); /* holes: 1010 0101 */
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID);
        put32le(buf + base + 8, 0);
        EXPECT_BMP(buf, len, STATUS_ERR_INVALID); /* an absent colour channel */
        put32le(buf + base + 8, 0x000000FFu);
        EXPECT_BMP(buf, len, STATUS_OK);
    }
    freeFixtures(&fs);
}

/* ---- truncation, mutation fuzz, allocation failure ------------------------------------ */

static const char *const SAMPLE_NAMES[] = {
    "png_t6_d8_33x7_i",     "png_t3_d4_9x9_i_trns", "png_t0_d16_9x9",  "png_t2_d16_3x5_i",
    "png_t4_d8_9x9",        "png_t0_d1_33x7",       "png_t3_d8_3x5",   "png_t0_d4_9x9_trns",
    "bmp_b24_9x9",          "bmp_p4_c11_33x7",      "bmp_b32_abf_9x9", "bmp_b16_565_v4_3x5_td",
    "bmp_b32_v4_10102_3x5", "bmp_p1_c0_9x9",
};

TEST(gfxDecodeTruncationEveryOffset) {
    FixtureSet fs;
    ASSERT_TRUE(loadFixtures(&fs));
    for (size_t s = 0; s < sizeof(SAMPLE_NAMES) / sizeof(SAMPLE_NAMES[0]); s++) {
        const Fixture *x = findFixture(&fs, SAMPLE_NAMES[s]);
        ASSERT_TRUE(x != NULL);
        for (size_t cut = 0; cut < x->dataLen; cut++) {
            CA_INIT(ca, -1);
            GfxImage img;
            Status st = gfxImageDecode(x->data, cut, NULL, &ca, &img);
            if (st == STATUS_OK) {
                fprintf(stderr, "  %s decoded from %zu of %u bytes\n", x->name, cut, x->dataLen);
            }
            ASSERT_TRUE(st != STATUS_OK);
            ASSERT_TRUE(img.pixels == NULL);
            ASSERT_EQ(caState.live, 0);
        }
    }
    freeFixtures(&fs);
}

/* Recomputes every PNG chunk CRC of a (possibly damaged) file, so the mutation reaches inflate and
 * the unfilter/convert code instead of being stopped at the CRC check. */
static void fixCrcs(uint8_t *d, size_t n) {
    size_t off = 8;
    while (off + 12 <= n) {
        uint32_t len = ((uint32_t)d[off] << 24) | ((uint32_t)d[off + 1] << 16) |
                       ((uint32_t)d[off + 2] << 8) | d[off + 3];
        if (len > n - off - 12) {
            return;
        }
        uint32_t crc = compressCrc32(0, d + off + 4, (size_t)len + 4);
        d[off + 8 + len] = (uint8_t)(crc >> 24);
        d[off + 9 + len] = (uint8_t)(crc >> 16);
        d[off + 10 + len] = (uint8_t)(crc >> 8);
        d[off + 11 + len] = (uint8_t)crc;
        off += 12 + (size_t)len;
    }
}

static uint32_t rngState = 1;

static void rngSeed(uint32_t s) {
    rngState = s;
}

static uint32_t rngNext(void) {
    uint32_t x = rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rngState = x;
    return x;
}

TEST(gfxDecodeMutationFuzz) {
    FixtureSet fs;
    ASSERT_TRUE(loadFixtures(&fs));
    rngSeed(0x5EEDu);
    int ok = 0, bad = 0;
    for (size_t s = 0; s < sizeof(SAMPLE_NAMES) / sizeof(SAMPLE_NAMES[0]); s++) {
        const Fixture *x = findFixture(&fs, SAMPLE_NAMES[s]);
        ASSERT_TRUE(x != NULL);
        uint8_t *m = malloc(x->dataLen);
        ASSERT_TRUE(m != NULL);
        for (int iter = 0; iter < 2000; iter++) {
            memcpy(m, x->data, x->dataLen);
            int nm = 1 + (int)(rngNext() % 4);
            for (int k = 0; k < nm; k++) {
                m[rngNext() % x->dataLen] = (uint8_t)rngNext();
            }
            if (x->kind == 1) {
                fixCrcs(m, x->dataLen);
            }
            CA_INIT(ca, -1);
            GfxImage img;
            GfxDecodeLimits lim = {4096, 4096, 1u << 22, 64u << 20}; /* keep mutated dims cheap */
            Status st = gfxImageDecode(m, x->dataLen, &lim, &ca, &img);
            if (st == STATUS_OK) {
                ok++;
                ASSERT_TRUE(img.width > 0 && img.height > 0 && img.pixels != NULL);
                ASSERT_TRUE(img.width <= 4096 && img.height <= 4096);
                for (uint32_t p = 0; p < img.width * img.height; p++) {
                    ASSERT_TRUE(premulOk(img.pixels[p]));
                }
            } else {
                bad++;
                ASSERT_TRUE(img.pixels == NULL);
            }
            gfxImageFree(&img);
            ASSERT_EQ(caState.live, 0);
        }
        free(m);
    }
    ASSERT_TRUE(ok > 0 && bad > 0); /* the fuzz reaches both outcomes */
    freeFixtures(&fs);
}

TEST(gfxDecodeAllocationFailureSweep) {
    FixtureSet fs;
    ASSERT_TRUE(loadFixtures(&fs));
    for (size_t s = 0; s < sizeof(SAMPLE_NAMES) / sizeof(SAMPLE_NAMES[0]); s++) {
        const Fixture *x = findFixture(&fs, SAMPLE_NAMES[s]);
        ASSERT_TRUE(x != NULL);
        bool succeeded = false;
        for (int failAt = 0; failAt < 12 && !succeeded; failAt++) {
            CA_INIT(ca, failAt);
            GfxImage img;
            Status st = gfxImageDecode(x->data, x->dataLen, NULL, &ca, &img);
            if (st == STATUS_OK) {
                succeeded = true;
                ASSERT_TRUE(caState.count <= failAt); /* it simply needed fewer allocations */
            } else {
                ASSERT_EQ(st, STATUS_ERR_NO_MEMORY);
                ASSERT_TRUE(img.pixels == NULL);
            }
            gfxImageFree(&img);
            ASSERT_EQ(caState.live, 0);
        }
        ASSERT_TRUE(succeeded);
    }
    freeFixtures(&fs);
}

/* Decode real images and composite them over a checkerboard: exercises decode -> premultiplied
 * pixels -> gfxBlit end to end, with translucent pixels (RGBA, tRNS, gray+alpha, BMP alpha). */
TEST(gfxGoldenDecodedImagesOverChecker) {
    FixtureSet fs;
    ASSERT_TRUE(loadFixtures(&fs));
    enum { W = 96, H = 56 };
    uint32_t px[W * H];
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            px[y * W + x] = (((x / 8) + (y / 8)) & 1) ? 0xFFB8B8B8u : 0xFFF0F0F0u;
        }
    }
    GfxSurface surf = {px, W, H, W};
    GfxCanvas c;
    ASSERT_EQ(gfxCanvasInit(&c, surf, NULL), STATUS_OK);
    struct {
        const char *name;
        int32_t x, y;
        uint8_t alpha;
    } items[] = {
        {"png_t6_d8_33x7", 2, 2, 255},         {"png_t3_d8_33x7_i_trns", 40, 2, 255},
        {"png_t4_d16_9x9", 78, 2, 255},        {"bmp_b32_abf_33x7", 2, 16, 255},
        {"png_t6_d16_33x7_i", 40, 16, 128},    {"png_t2_d8_33x7_i_trns", 2, 30, 255},
        {"bmp_b32_v4_10102_3x5", 40, 30, 255}, {"png_t0_d8_9x9_trns", 78, 20, 255},
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        const Fixture *x = findFixture(&fs, items[i].name);
        if (x == NULL) {
            fprintf(stderr, "  missing fixture %s\n", items[i].name);
        }
        ASSERT_TRUE(x != NULL);
        GfxImage img;
        ASSERT_EQ(gfxImageDecode(x->data, x->dataLen, NULL, NULL, &img), STATUS_OK);
        GfxSurface src = gfxImageSurface(&img);
        gfxBlit(&c, items[i].x, items[i].y, &src, (GfxRect){0, 0, src.width, src.height},
                GFX_OP_SRC_OVER, items[i].alpha);
        gfxImageFree(&img);
    }
    gfxCanvasDestroy(&c);
    ASSERT_TRUE(goldenCheck("gfx_decoded_images", &surf));
    freeFixtures(&fs);
}
