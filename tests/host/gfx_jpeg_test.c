/* Host tests for libs/gfx's JPEG decoder (M12.7, D-160..D-165). The main oracle is
 * tests/data/gfx/jpeg-fixtures.z: files and expected pixels produced by an independent Python
 * encoder plus an exact integer model (tests/data/gfx/gen_jpeg.py); the pixels come from the
 * encoder's own coefficients, never from parsing the bitstream. The rest are IDCT/colour
 * accuracy checks, hand-built malformed files, truncation, mutation fuzz, allocation-failure and
 * budget tests, all under ASan/UBSan/LSan with an allocator that counts what is live. */
#include "compress/compress.h"
#include "framework/test.h"
#include "gfx/gfx-image.h"
#include "gfx/gfx.h"
#include "gfx/jpeg-internal.h"
#include "gfx_decode_testutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- fixtures ---------------------------------------------------------------------------- */

typedef struct {
    char name[96];
    int32_t status;
    uint8_t flags; /* bit0: `expected` is the CRC-32 of the pixel bytes */
    uint32_t w, h;
    const uint8_t *data;
    uint32_t dataLen;
    const uint8_t *expected; /* w*h LE u32, or a 4-byte CRC-32; unused unless status == 0 */
} JFix;

typedef struct {
    uint8_t *raw;
    JFix *fx;
    int n;
} JFixSet;

static bool loadJFix(JFixSet *fs) {
    memset(fs, 0, sizeof(*fs));
    size_t rawLen;
    if (!decLoadContainer("tests/data/gfx/jpeg-fixtures.z", &fs->raw, &rawLen)) {
        return false;
    }
    int cap = 4096;
    fs->fx = calloc((size_t)cap, sizeof(JFix));
    size_t off = 0;
    while (off < rawLen && fs->n < cap) {
        JFix *x = &fs->fx[fs->n];
        size_t nameLen = decLe16(fs->raw + off);
        off += 2;
        memcpy(x->name, fs->raw + off, nameLen < 95 ? nameLen : 95);
        off += nameLen;
        x->status = (int32_t)decLe32(fs->raw + off);
        x->flags = fs->raw[off + 4];
        x->w = decLe32(fs->raw + off + 5);
        x->h = decLe32(fs->raw + off + 9);
        x->dataLen = decLe32(fs->raw + off + 13);
        off += 17;
        x->data = fs->raw + off;
        off += x->dataLen;
        x->expected = fs->raw + off;
        if (x->status == STATUS_OK) {
            off += (x->flags & 1) ? 4 : (size_t)x->w * x->h * 4;
        }
        fs->n++;
    }
    return off == rawLen;
}

static void freeJFix(JFixSet *fs) {
    free(fs->raw);
    free(fs->fx);
}

static bool pixelsMatch(const JFix *x, const GfxImage *img) {
    if (img->width != x->w || img->height != x->h || img->pixels == NULL) {
        return false;
    }
    size_t bytes = (size_t)x->w * x->h * 4;
    if (x->flags & 1) {
        return compressCrc32(0, img->pixels, bytes) == decLe32(x->expected);
    }
    for (size_t i = 0; i < (size_t)x->w * x->h; i++) {
        if (img->pixels[i] != decLe32(x->expected + 4 * i)) {
            return false;
        }
    }
    return true;
}

static Status decodeDefault(const uint8_t *d, size_t n, GfxImage *img) {
    return gfxJpegDecode(d, n, NULL, NULL, img);
}

TEST(gfxJpegMatchesIndependentFixtures) {
    JFixSet fs;
    ASSERT_TRUE(loadJFix(&fs));
    ASSERT_TRUE(fs.n >= 150);
    int nOk = 0, nGray = 0, nSub = 0, nRst = 0;
    for (int i = 0; i < fs.n; i++) {
        const JFix *x = &fs.fx[i];
        DecCountAlloc st;
        GfxAllocator al;
        decAllocInit(&st, &al, -1);
        GfxImage img;
        Status s = gfxJpegDecode(x->data, x->dataLen, NULL, &al, &img);
        if (s != x->status) {
            fprintf(stderr, "  %s: status %d, expected %d\n", x->name, (int)s, (int)x->status);
        }
        ASSERT_EQ(s, x->status);
        if (s == STATUS_OK) {
            if (!pixelsMatch(x, &img)) {
                fprintf(stderr, "  %s: pixels differ\n", x->name);
            }
            ASSERT_TRUE(pixelsMatch(x, &img));
            for (size_t p = 0; p < (size_t)img.width * img.height; p++) {
                ASSERT_TRUE(decPremulOk(img.pixels[p]) && (img.pixels[p] >> 24) == 0xFF);
            }
            gfxImageFree(&img);
            nOk++;
            nGray += strstr(x->name, "gray") != NULL;
            nSub += strstr(x->name, "c420") != NULL || strstr(x->name, "c411") != NULL ||
                    strstr(x->name, "frac") != NULL;
            nRst += strstr(x->name, "__ri") != NULL || strstr(x->name, "_ri") != NULL;
            /* the sniffing entry point must agree */
            GfxImage img2;
            ASSERT_EQ(gfxImageDecode(x->data, x->dataLen, NULL, NULL, &img2), STATUS_OK);
            ASSERT_TRUE(pixelsMatch(x, &img2));
            gfxImageFree(&img2);
        } else {
            ASSERT_TRUE(img.pixels == NULL);
        }
        ASSERT_EQ(st.live, 0);
    }
    ASSERT_TRUE(nOk >= 150 && nGray >= 20 && nSub >= 20 && nRst >= 20);
    freeJFix(&fs);
}

/* Every variant of a group encodes the same coefficients, so the decoded pixels must be
 * identical whatever the scan script, tables, restarts or marker noise. */
TEST(gfxJpegSameCoefficientsAnyScript) {
    JFixSet fs;
    ASSERT_TRUE(loadJFix(&fs));
    int groups = 0;
    for (int i = 0; i < fs.n; i++) {
        const JFix *a = &fs.fx[i];
        if (a->status != STATUS_OK || strncmp(a->name, "jg_", 3) != 0 || (a->flags & 1)) {
            continue;
        }
        const char *sep = strstr(a->name, "__");
        ASSERT_TRUE(sep != NULL);
        size_t glen = (size_t)(sep - a->name);
        /* only compare against the first member of the group (later ones compare with it) */
        bool first = true;
        for (int k = 0; k < i; k++) {
            if (strncmp(fs.fx[k].name, a->name, glen) == 0 && fs.fx[k].name[glen] == '_') {
                first = false;
            }
        }
        if (!first) {
            continue;
        }
        GfxImage ia;
        ASSERT_EQ(decodeDefault(a->data, a->dataLen, &ia), STATUS_OK);
        int members = 1;
        for (int k = i + 1; k < fs.n; k++) {
            const JFix *b = &fs.fx[k];
            if (strncmp(b->name, a->name, glen) != 0 || b->name[glen] != '_' || b->status != 0) {
                continue;
            }
            GfxImage ib;
            ASSERT_EQ(decodeDefault(b->data, b->dataLen, &ib), STATUS_OK);
            ASSERT_TRUE(ia.width == ib.width && ia.height == ib.height);
            if (strstr(b->name, "latch") == NULL) {
                ASSERT_TRUE(memcmp(ia.pixels, ib.pixels, (size_t)ia.width * ia.height * 4) == 0);
            }
            gfxImageFree(&ib);
            members++;
        }
        gfxImageFree(&ia);
        groups += members > 1;
    }
    ASSERT_TRUE(groups >= 30);
    freeJFix(&fs);
}

/* ---- IDCT and colour conversion ----------------------------------------------------------- */

static const double COS16[32] = {1.0,
                                 0.9807852804032304,
                                 0.9238795325112867,
                                 0.8314696123025452,
                                 0.7071067811865476,
                                 0.5555702330196023,
                                 0.38268343236508984,
                                 0.19509032201612833,
                                 0.0,
                                 -0.1950903220161282,
                                 -0.3826834323650897,
                                 -0.555570233019602,
                                 -0.7071067811865475,
                                 -0.8314696123025453,
                                 -0.9238795325112867,
                                 -0.9807852804032304,
                                 -1.0,
                                 -0.9807852804032304,
                                 -0.9238795325112868,
                                 -0.8314696123025455,
                                 -0.7071067811865477,
                                 -0.5555702330196022,
                                 -0.38268343236509034,
                                 -0.19509032201612866,
                                 0.0,
                                 0.1950903220161283,
                                 0.38268343236509,
                                 0.5555702330196018,
                                 0.7071067811865474,
                                 0.8314696123025452,
                                 0.9238795325112865,
                                 0.9807852804032303};

/* cos((2x+1) u pi / 16) */
static double basis(int x, int u) {
    return COS16[((2 * x + 1) * u) % 32];
}

static double alpha(int u) {
    return u == 0 ? 0.7071067811865476 : 1.0;
}

static void refFdct(const double *px, double *co) {
    for (int v = 0; v < 8; v++) {
        for (int u = 0; u < 8; u++) {
            double s = 0;
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    s += px[8 * y + x] * basis(x, u) * basis(y, v);
                }
            }
            co[8 * v + u] = 0.25 * alpha(u) * alpha(v) * s;
        }
    }
}

static void refIdct(const double *co, double *px) {
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            double s = 0;
            for (int v = 0; v < 8; v++) {
                for (int u = 0; u < 8; u++) {
                    s += alpha(u) * alpha(v) * co[8 * v + u] * basis(x, u) * basis(y, v);
                }
            }
            px[8 * y + x] = 0.25 * s;
        }
    }
}

static int roundHalfUp(double v) {
    return (int)(v < 0 ? v - 0.5 : v + 0.5);
}

/* IEEE-1180-style accuracy of the integer IDCT against a double one, on random blocks. */
TEST(gfxJpegIdctAccuracy) {
    static const int RANGE[3][2] = {{-128, 127}, {-5, 5}, {-300, 300}};
    uint16_t q[64];
    for (int i = 0; i < 64; i++) {
        q[i] = 1;
    }
    uint32_t seed = 1234567;
    for (int r = 0; r < 3; r++) {
        double sumErr = 0, sumSq = 0, posSum[64] = {0}, posSq[64] = {0};
        int peak = 0;
        const int N = 10000;
        for (int n = 0; n < N; n++) {
            double px[64], co[64], back[64];
            int16_t c[64];
            int span = RANGE[r][1] - RANGE[r][0] + 1;
            for (int i = 0; i < 64; i++) {
                px[i] = RANGE[r][0] + (int)(decRng(&seed) % (uint32_t)span);
            }
            refFdct(px, co);
            for (int i = 0; i < 64; i++) {
                c[i] = (int16_t)roundHalfUp(co[i]);
                co[i] = c[i];
            }
            refIdct(co, back);
            uint8_t out[64];
            jpegIdctIslow(c, q, out, 8);
            for (int i = 0; i < 64; i++) {
                int e = roundHalfUp(back[i]) + 128;
                e = e < 0 ? 0 : e > 255 ? 255 : e;
                int d = (int)out[i] - e;
                if (abs(d) > peak) {
                    peak = abs(d);
                }
                sumErr += d;
                sumSq += (double)d * d;
                posSum[i] += d;
                posSq[i] += (double)d * d;
            }
        }
        double total = 64.0 * N;
        ASSERT_TRUE(peak <= 1);
        ASSERT_TRUE(sumSq / total <= 0.02);
        ASSERT_TRUE(sumErr / total <= 0.0015 && sumErr / total >= -0.0015);
        for (int i = 0; i < 64; i++) {
            ASSERT_TRUE(posSq[i] / N <= 0.06);
            ASSERT_TRUE(posSum[i] / N <= 0.015 && posSum[i] / N >= -0.015);
        }
    }
}

/* A block that is DC only comes out flat, and a known DC maps to the expected sample. */
TEST(gfxJpegIdctDcOnly) {
    uint16_t q[64];
    for (int i = 0; i < 64; i++) {
        q[i] = 1;
    }
    int16_t c[64] = {0};
    uint8_t out[64];
    for (int dc = -1100; dc <= 1100; dc += 7) {
        c[0] = (int16_t)dc;
        jpegIdctIslow(c, q, out, 8);
        int e = roundHalfUp(dc / 8.0) + 128;
        e = e < 0 ? 0 : e > 255 ? 255 : e;
        for (int i = 1; i < 64; i++) {
            ASSERT_EQ(out[i], out[0]);
        }
        ASSERT_TRUE(abs((int)out[0] - e) <= 1);
    }
    /* dequantization saturates at int16 instead of overflowing */
    q[0] = 65535;
    c[0] = 32767;
    jpegIdctIslow(c, q, out, 8);
    ASSERT_EQ(out[0], 255);
    c[0] = -32768;
    jpegIdctIslow(c, q, out, 8);
    ASSERT_EQ(out[0], 0);
}

TEST(gfxJpegColorConversion) {
    uint32_t r, g, b;
    jpegYccToRgb(128, 128, 128, &r, &g, &b);
    ASSERT_TRUE(r == 128 && g == 128 && b == 128);
    jpegYccToRgb(0, 128, 128, &r, &g, &b);
    ASSERT_TRUE(r == 0 && g == 0 && b == 0);
    jpegYccToRgb(255, 128, 128, &r, &g, &b);
    ASSERT_TRUE(r == 255 && g == 255 && b == 255);
    jpegYccToRgb(76, 85, 255, &r, &g, &b); /* red: cr' = 127 */
    ASSERT_TRUE(r == 254 && g == 0 && b == 0);
    /* spot values from the independent Python model (gen_jpeg.py ycc_to_rgb) */
    jpegYccToRgb(150, 44, 21, &r, &g, &b);
    ASSERT_TRUE(r == 0 && g == 255 && b == 1);
    jpegYccToRgb(200, 90, 160, &r, &g, &b);
    ASSERT_TRUE(r == 245 && g == 190 && b == 133);
    /* against the real-valued transform, within one level, for every (y, cb, cr) */
    for (int y = 0; y < 256; y++) {
        for (int cb = 0; cb < 256; cb++) {
            for (int cr = 0; cr < 256; cr++) {
                jpegYccToRgb(y, cb, cr, &r, &g, &b);
                double rr = y + 1.402 * (cr - 128);
                double gg = y - 0.344136 * (cb - 128) - 0.714136 * (cr - 128);
                double bb = y + 1.772 * (cb - 128);
                int er = roundHalfUp(rr), eg = roundHalfUp(gg), eb = roundHalfUp(bb);
                er = er < 0 ? 0 : er > 255 ? 255 : er;
                eg = eg < 0 ? 0 : eg > 255 ? 255 : eg;
                eb = eb < 0 ? 0 : eb > 255 ? 255 : eb;
                ASSERT_TRUE(abs((int)r - er) <= 1 && abs((int)g - eg) <= 1 &&
                            abs((int)b - eb) <= 1);
            }
        }
    }
}

/* ---- Huffman tables and the bit reader ------------------------------------------------------ */

static void bitsFor(uint8_t bits[16], int l, int n) {
    memset(bits, 0, 16);
    bits[l - 1] = (uint8_t)n;
}

TEST(gfxJpegHuffmanTables) {
    JpegHuff t;
    uint8_t bits[16], vals[256];
    for (int i = 0; i < 256; i++) {
        vals[i] = (uint8_t)i;
    }
    bitsFor(bits, 1, 3); /* over-subscribed */
    ASSERT_EQ(jpegHuffBuild(&t, bits, vals), STATUS_ERR_INVALID);
    bitsFor(bits, 1, 2); /* uses the reserved all-ones code */
    ASSERT_EQ(jpegHuffBuild(&t, bits, vals), STATUS_ERR_INVALID);
    bitsFor(bits, 2, 4); /* 00 01 10 11: all-ones again */
    ASSERT_EQ(jpegHuffBuild(&t, bits, vals), STATUS_ERR_INVALID);
    bitsFor(bits, 2, 3);
    ASSERT_EQ(jpegHuffBuild(&t, bits, vals), STATUS_OK);
    memset(bits, 0, 16);
    bits[13] = 255;
    bits[14] = 2; /* 257 symbols */
    ASSERT_EQ(jpegHuffBuild(&t, bits, vals), STATUS_ERR_INVALID);
    memset(bits, 0, 16); /* an empty table is fine, but decoding from it fails */
    ASSERT_EQ(jpegHuffBuild(&t, bits, vals), STATUS_OK);
    static const uint8_t zeros[8] = {0};
    JpegBits b;
    jpegBitsInit(&b, zeros, sizeof(zeros), 0);
    ASSERT_EQ(jpegDecodeSym(&b, &t), -1);
    ASSERT_TRUE(b.err);
    /* lengths 1..15 once each and a single 16-bit code: every code decodes */
    memset(bits, 0, 16);
    for (int i = 0; i < 16; i++) {
        bits[i] = 1;
    }
    uint8_t v16[16];
    for (int i = 0; i < 16; i++) {
        v16[i] = (uint8_t)(0xA0 + i);
    }
    ASSERT_EQ(jpegHuffBuild(&t, bits, v16), STATUS_OK);
    for (int sym = 0; sym < 16; sym++) {
        /* code for symbol `sym`: `sym` ones then a zero (the last one, sym 15, is 15 ones + 0) */
        uint8_t buf[8] = {0};
        int nb = 0;
        for (int i = 0; i < sym; i++) {
            buf[nb / 8] |= (uint8_t)(0x80 >> (nb % 8));
            nb++;
        }
        nb++; /* the terminating zero */
        /* pad the rest with ones as an encoder does, then stuff FF bytes */
        for (int i = nb; i < 24; i++) {
            buf[i / 8] |= (uint8_t)(0x80 >> (i % 8));
        }
        uint8_t stuffed[16];
        size_t sn = 0;
        for (int i = 0; i < 3; i++) {
            stuffed[sn++] = buf[i];
            if (buf[i] == 0xFF) {
                stuffed[sn++] = 0;
            }
        }
        stuffed[sn++] = 0xFF;
        stuffed[sn++] = 0xD9;
        jpegBitsInit(&b, stuffed, sn, 0);
        ASSERT_EQ(jpegDecodeSym(&b, &t), 0xA0 + sym);
        ASSERT_TRUE(!b.err);
    }
    /* a code not in the table: only '0' is defined, the data starts with a 1 */
    bitsFor(bits, 1, 1);
    ASSERT_EQ(jpegHuffBuild(&t, bits, vals), STATUS_OK);
    static const uint8_t ones[4] = {0xFF, 0x00, 0xFF, 0x00};
    jpegBitsInit(&b, ones, sizeof(ones), 0);
    ASSERT_EQ(jpegDecodeSym(&b, &t), -1);
    ASSERT_TRUE(b.err);
}

TEST(gfxJpegBitReaderPadAndMarkers) {
    static const uint8_t d[] = {0xA5, 0xFF, 0x00, 0xFF, 0xFF, 0x00, 0x3C, 0xFF, 0xD9};
    JpegBits b;
    jpegBitsInit(&b, d, sizeof(d), 0);
    ASSERT_EQ(jpegGetBits(&b, 8), 0xA5u);
    ASSERT_EQ(jpegGetBits(&b, 8), 0xFFu); /* FF 00 is data */
    ASSERT_EQ(jpegGetBits(&b, 8), 0xFFu); /* FF FF 00 is one data 0xFF (fill) */
    ASSERT_EQ(jpegGetBits(&b, 8), 0x3Cu);
    ASSERT_TRUE(!b.err);
    ASSERT_TRUE(b.marker && b.markerPos == 7);
    ASSERT_EQ(jpegGetBits(&b, 1), 0u); /* past the marker: pad, not data */
    ASSERT_TRUE(b.err);
    jpegBitsInit(&b, d, sizeof(d), 0);
    ASSERT_EQ(jpegGetBits(&b, 0), 0u);
    ASSERT_TRUE(!b.err);
    jpegBitsInit(&b, d, sizeof(d), 6);
    ASSERT_EQ(jpegBitsFindMarker(&b), 7u);
    jpegBitsInit(&b, d, 6, 0);
    ASSERT_EQ(jpegBitsFindMarker(&b), SIZE_MAX);
}

/* ---- hand-built files ------------------------------------------------------------------- */

/* A minimal valid file: 8x8, DQT of ones, DC and AC tables each with one 1-bit code (category 0 /
 * EOB), one all-zero block per component: flat gray 128. */
typedef struct {
    uint8_t b[600];
    size_t n, dqt, sof, dhtDc, dhtAc, sos, ent, eoi;
} Jb;

static void jbPut(Jb *j, const void *p, size_t n) {
    memcpy(j->b + j->n, p, n);
    j->n += n;
}

static void jbBase(Jb *j, int nf) {
    memset(j, 0, sizeof(*j));
    static const uint8_t soi[] = {0xFF, 0xD8};
    jbPut(j, soi, 2);
    j->dqt = j->n;
    static const uint8_t dqtHead[] = {0xFF, 0xDB, 0x00, 0x43, 0x00};
    jbPut(j, dqtHead, 5);
    uint8_t ones[64];
    memset(ones, 1, 64);
    jbPut(j, ones, 64);
    j->sof = j->n;
    uint8_t sof[] = {0xFF, 0xC0, 0, (uint8_t)(8 + 3 * nf), 8, 0, 8, 0, 8, (uint8_t)nf};
    jbPut(j, sof, sizeof(sof));
    for (int i = 0; i < nf; i++) {
        uint8_t c[3] = {(uint8_t)(i + 1), 0x11, 0};
        jbPut(j, c, 3);
    }
    j->dhtDc = j->n;
    static const uint8_t dht[] = {0xFF, 0xC4, 0x00, 0x14, 0x00, 1, 0, 0, 0, 0, 0,
                                  0,    0,    0,    0,    0,    0, 0, 0, 0, 0, 0x00};
    jbPut(j, dht, sizeof(dht));
    j->dhtAc = j->n;
    jbPut(j, dht, sizeof(dht));
    j->b[j->dhtAc + 4] = 0x10;
    j->sos = j->n;
    uint8_t sos[] = {0xFF, 0xDA, 0, (uint8_t)(6 + 2 * nf), (uint8_t)nf};
    jbPut(j, sos, sizeof(sos));
    for (int i = 0; i < nf; i++) {
        uint8_t c[2] = {(uint8_t)(i + 1), 0x00};
        jbPut(j, c, 2);
    }
    static const uint8_t tail[] = {0, 63, 0};
    jbPut(j, tail, 3);
    j->ent = j->n;
    j->b[j->n++] = nf == 1 ? 0x3F : 0x03; /* nf * "00", then 1-bit padding */
    j->eoi = j->n;
    j->b[j->n++] = 0xFF;
    j->b[j->n++] = 0xD9;
}

static Status jbDecode(const Jb *j) {
    GfxImage img;
    Status s = decodeDefault(j->b, j->n, &img);
    if (s == STATUS_OK) {
        gfxImageFree(&img);
    } else if (img.pixels != NULL) {
        return 1000; /* a failed decode must not hand out pixels */
    }
    return s;
}

static Status jbDecodeMut(const Jb *j, size_t at, uint8_t v) {
    Jb m = *j;
    m.b[at] = v;
    return jbDecode(&m);
}

static Status jbDecode2(const Jb *j, size_t at1, uint8_t v1, size_t at2, uint8_t v2) {
    Jb m = *j;
    m.b[at1] = v1;
    m.b[at2] = v2;
    return jbDecode(&m);
}

TEST(gfxJpegHandBuiltBaseFilesDecode) {
    for (int nf = 1; nf <= 3; nf += 2) {
        Jb j;
        jbBase(&j, nf);
        GfxImage img;
        ASSERT_EQ(decodeDefault(j.b, j.n, &img), STATUS_OK);
        ASSERT_TRUE(img.width == 8 && img.height == 8);
        for (int i = 0; i < 64; i++) {
            ASSERT_EQ(img.pixels[i], 0xFF808080u);
        }
        gfxImageFree(&img);
    }
}

TEST(gfxJpegMalformedMarkers) {
    Jb j, k;
    jbBase(&j, 1);
    jbBase(&k, 3);
    /* SOF: lengths, fields, unsupported features */
    ASSERT_EQ(jbDecodeMut(&j, j.sof + 3, 12), STATUS_ERR_INVALID);     /* length != 8 + 3 Nf */
    ASSERT_EQ(jbDecodeMut(&j, j.sof + 3, 5), STATUS_ERR_INVALID);      /* length too short */
    ASSERT_EQ(jbDecodeMut(&j, j.sof + 4, 12), STATUS_ERR_UNSUPPORTED); /* 12-bit */
    ASSERT_EQ(jbDecodeMut(&j, j.sof + 6, 0), STATUS_ERR_UNSUPPORTED);  /* Y = 0 (DNL) */
    ASSERT_EQ(jbDecodeMut(&j, j.sof + 8, 0), STATUS_ERR_INVALID);      /* X = 0 */
    ASSERT_EQ(jbDecodeMut(&j, j.sof + 11, 0x01), STATUS_ERR_INVALID);  /* H = 0 */
    ASSERT_EQ(jbDecodeMut(&j, j.sof + 11, 0x10), STATUS_ERR_INVALID);  /* V = 0 */
    ASSERT_EQ(jbDecodeMut(&j, j.sof + 11, 0x51), STATUS_ERR_INVALID);  /* H = 5 */
    ASSERT_EQ(jbDecodeMut(&j, j.sof + 11, 0x15), STATUS_ERR_INVALID);  /* V = 5 */
    ASSERT_EQ(jbDecodeMut(&j, j.sof + 12, 4), STATUS_ERR_INVALID);     /* Tq = 4 */
    ASSERT_EQ(jbDecodeMut(&j, j.sof + 12, 1), STATUS_ERR_INVALID);     /* Tq undefined */
    ASSERT_EQ(jbDecodeMut(&k, k.sof + 13, 1), STATUS_ERR_INVALID);     /* duplicate id */
    /* component counts, with a consistent segment length */
    ASSERT_EQ(jbDecode2(&j, j.sof + 3, 8, j.sof + 9, 0), STATUS_ERR_INVALID);      /* Nf = 0 */
    ASSERT_EQ(jbDecode2(&j, j.sof + 3, 14, j.sof + 9, 2), STATUS_ERR_UNSUPPORTED); /* Nf = 2 */
    ASSERT_EQ(jbDecode2(&j, j.sof + 3, 20, j.sof + 9, 4), STATUS_ERR_UNSUPPORTED); /* CMYK */
    static const uint8_t unsupportedSof[] = {0xC3, 0xC5, 0xC6, 0xC7, 0xC9,
                                             0xCA, 0xCB, 0xCD, 0xCE, 0xCF};
    for (size_t i = 0; i < sizeof(unsupportedSof); i++) {
        ASSERT_EQ(jbDecodeMut(&j, j.sof + 1, unsupportedSof[i]), STATUS_ERR_UNSUPPORTED);
    }
    /* unknown, reserved and unsupported markers where a segment is expected */
    static const uint8_t badMarkers[] = {0x02, 0x60, 0xBF, 0xC8, 0xCC, 0xDE, 0xDF, 0xF0, 0xFD};
    for (size_t i = 0; i < sizeof(badMarkers); i++) {
        ASSERT_EQ(jbDecodeMut(&j, j.dqt + 1, badMarkers[i]), STATUS_ERR_UNSUPPORTED);
    }
    /* SOS */
    ASSERT_EQ(jbDecodeMut(&j, j.sos + 3, 10), STATUS_ERR_INVALID);   /* length != 6 + 2 Ns */
    ASSERT_EQ(jbDecodeMut(&j, j.sos + 4, 0), STATUS_ERR_INVALID);    /* Ns = 0 */
    ASSERT_EQ(jbDecodeMut(&j, j.sos + 4, 5), STATUS_ERR_INVALID);    /* Ns > 4 */
    ASSERT_EQ(jbDecodeMut(&j, j.sos + 4, 2), STATUS_ERR_INVALID);    /* Ns > Nf */
    ASSERT_EQ(jbDecodeMut(&j, j.sos + 5, 9), STATUS_ERR_INVALID);    /* component not in frame */
    ASSERT_EQ(jbDecodeMut(&j, j.sos + 6, 0x40), STATUS_ERR_INVALID); /* Td = 4 */
    ASSERT_EQ(jbDecodeMut(&j, j.sos + 6, 0x04), STATUS_ERR_INVALID); /* Ta = 4 */
    ASSERT_EQ(jbDecodeMut(&j, j.sos + 6, 0x10), STATUS_ERR_INVALID); /* DC table 1 undefined */
    ASSERT_EQ(jbDecodeMut(&j, j.sos + 6, 0x01), STATUS_ERR_INVALID); /* AC table 1 undefined */
    ASSERT_EQ(jbDecodeMut(&k, k.sos + 7, 1), STATUS_ERR_INVALID);    /* duplicate component */
    /* structure */
    {
        Jb m = j; /* SOS before SOF */
        memmove(m.b + m.sof, m.b + m.dhtDc, m.n - m.dhtDc);
        m.n -= m.dhtDc - m.sof;
        ASSERT_EQ(jbDecode(&m), STATUS_ERR_INVALID);
    }
    {
        Jb m = j; /* a second SOF */
        memmove(m.b + m.dhtDc + 13, m.b + m.dhtDc, m.n - m.dhtDc);
        memcpy(m.b + m.dhtDc, m.b + m.sof, 13);
        m.n += 13;
        ASSERT_EQ(jbDecode(&m), STATUS_ERR_INVALID);
    }
    {
        Jb m = j; /* EOI before any scan */
        m.b[m.dhtDc + 1] = 0xD9;
        ASSERT_EQ(jbDecode(&m), STATUS_ERR_INVALID);
    }
    {
        Jb m = j; /* the EOI is missing */
        m.n -= 2;
        ASSERT_EQ(jbDecode(&m), STATUS_ERR_INVALID);
        m.n -= 1;
        ASSERT_EQ(jbDecode(&m), STATUS_ERR_INVALID);
    }
    {
        Jb m = j; /* garbage between segments */
        memmove(m.b + m.sof + 1, m.b + m.sof, m.n - m.sof);
        m.b[m.sof] = 0x12;
        m.n++;
        ASSERT_EQ(jbDecode(&m), STATUS_ERR_INVALID);
    }
    {
        Jb m = j; /* the same scan twice: a sequential component coded in two scans */
        memmove(m.b + m.eoi + (m.eoi - m.sos), m.b + m.eoi, 2);
        memcpy(m.b + m.eoi, m.b + m.sos, m.eoi - m.sos);
        m.n += m.eoi - m.sos;
        ASSERT_EQ(jbDecode(&m), STATUS_ERR_INVALID);
    }
    {
        Jb m = k; /* only component 1 is ever coded */
        uint8_t sos1[] = {0xFF, 0xDA, 0, 8, 1, 1, 0, 0, 63, 0, 0x3F};
        m.n = m.sos;
        memcpy(m.b + m.n, sos1, sizeof(sos1));
        m.n += sizeof(sos1);
        m.b[m.n++] = 0xFF;
        m.b[m.n++] = 0xD9;
        ASSERT_EQ(jbDecode(&m), STATUS_ERR_INVALID);
    }
    /* segment lengths */
    ASSERT_EQ(jbDecodeMut(&j, j.dqt + 3, 1), STATUS_ERR_INVALID);
    ASSERT_EQ(jbDecodeMut(&j, j.dqt + 2, 0xFF), STATUS_ERR_INVALID); /* runs past the end */
    ASSERT_EQ(jbDecodeMut(&j, j.dqt + 3, 0x42), STATUS_ERR_INVALID); /* table truncated */
    /* DQT and DHT contents */
    ASSERT_EQ(jbDecodeMut(&j, j.dqt + 4, 0x20), STATUS_ERR_INVALID);   /* Pq = 2 */
    ASSERT_EQ(jbDecodeMut(&j, j.dqt + 4, 0x04), STATUS_ERR_INVALID);   /* Tq = 4 */
    ASSERT_EQ(jbDecodeMut(&j, j.dhtDc + 4, 0x20), STATUS_ERR_INVALID); /* Tc = 2 */
    ASSERT_EQ(jbDecodeMut(&j, j.dhtDc + 4, 0x04), STATUS_ERR_INVALID); /* Th = 4 */
    ASSERT_EQ(jbDecodeMut(&j, j.dhtDc + 5, 3), STATUS_ERR_INVALID);    /* over-subscribed */
    ASSERT_EQ(jbDecodeMut(&j, j.dhtDc + 5, 2), STATUS_ERR_INVALID);    /* all-ones code */
    ASSERT_EQ(jbDecodeMut(&j, j.dhtDc + 3, 0x13), STATUS_ERR_INVALID); /* counts past the segment */
    /* DRI */
    ASSERT_EQ(jbDecodeMut(&j, j.dqt + 1, 0xDD), STATUS_ERR_INVALID); /* DRI of length 0x43 */
    /* things that are tolerated: stray RSTn, TEM, fill bytes, other segments */
    {
        Jb m = j;
        static const uint8_t junk[] = {0xFF, 0xD3, 0xFF, 0x01, 0xFF, 0xFF, 0xFF, 0xFE, 0x00, 0x04,
                                       'h',  'i',  0xFF, 0xE5, 0x00, 0x03, 0x00, 0xFF, 0xEE, 0x00,
                                       0x03, 0x00, 0xFF, 0xDC, 0x00, 0x04, 0x00, 0x08};
        memmove(m.b + m.dqt + sizeof(junk), m.b + m.dqt, m.n - m.dqt);
        memcpy(m.b + m.dqt, junk, sizeof(junk));
        m.n += sizeof(junk);
        ASSERT_EQ(jbDecode(&m), STATUS_OK);
    }
}

/* Builds a file with custom Huffman tables and entropy bytes (8x8 gray). */
static size_t buildCustom(uint8_t *out, const uint8_t dcBits[16], const uint8_t *dcVals, size_t nDc,
                          const uint8_t acBits[16], const uint8_t *acVals, size_t nAc,
                          const uint8_t *ent, size_t nEnt) {
    Jb j;
    jbBase(&j, 1);
    size_t n = j.dhtDc;
    memcpy(out, j.b, n);
    out[n++] = 0xFF;
    out[n++] = 0xC4;
    out[n++] = 0;
    out[n++] = (uint8_t)(19 + nDc);
    out[n++] = 0x00;
    memcpy(out + n, dcBits, 16);
    n += 16;
    memcpy(out + n, dcVals, nDc);
    n += nDc;
    out[n++] = 0xFF;
    out[n++] = 0xC4;
    out[n++] = 0;
    out[n++] = (uint8_t)(19 + nAc);
    out[n++] = 0x10;
    memcpy(out + n, acBits, 16);
    n += 16;
    memcpy(out + n, acVals, nAc);
    n += nAc;
    memcpy(out + n, j.b + j.sos, j.ent - j.sos);
    n += j.ent - j.sos;
    memcpy(out + n, ent, nEnt);
    n += nEnt;
    out[n++] = 0xFF;
    out[n++] = 0xD9;
    return n;
}

TEST(gfxJpegEntropyEdgeCases) {
    uint8_t f[300];
    uint8_t bits2[16] = {0, 2, 0}; /* two 2-bit codes: 00 and 01 */
    uint8_t bits1[16] = {1, 0};    /* one 1-bit code: 0 */
    uint8_t dcCat12[2] = {0, 12}, dcCat1[2] = {0, 1};
    uint8_t acSize11[2] = {0x00, 0x0B}, acZrl[2] = {0x00, 0xF0};
    uint8_t dcOne[1] = {0}, acEob[1] = {0x00};
    GfxImage img;
    /* DC category 12 is out of range (max 11): '01' */
    static const uint8_t e1[] = {0x7F, 0x7F};
    size_t n = buildCustom(f, bits2, dcCat12, 2, bits1, acEob, 1, e1, sizeof(e1));
    ASSERT_EQ(decodeDefault(f, n, &img), STATUS_ERR_INVALID);
    /* AC size 11 is out of range (max 10): DC '00', AC '01' */
    static const uint8_t e2[] = {0x1F, 0x7F};
    n = buildCustom(f, bits2, dcCat1, 2, bits2, acSize11, 2, e2, sizeof(e2));
    ASSERT_EQ(decodeDefault(f, n, &img), STATUS_ERR_INVALID);
    /* ZRL: DC '00', three ZRLs '01' (k = 1 -> 49), then EOB '00' is fine... */
    static const uint8_t e3[] = {0x15, 0x3F};
    n = buildCustom(f, bits2, dcCat1, 2, bits2, acZrl, 2, e3, sizeof(e3));
    ASSERT_EQ(decodeDefault(f, n, &img), STATUS_OK);
    gfxImageFree(&img);
    /* ...but a fourth ZRL runs past coefficient 63 */
    static const uint8_t e4[] = {0x15, 0x7F};
    n = buildCustom(f, bits2, dcCat1, 2, bits2, acZrl, 2, e4, sizeof(e4));
    ASSERT_EQ(decodeDefault(f, n, &img), STATUS_ERR_INVALID);
    /* the data ends (a marker follows) before the block does: pad bits may not be consumed */
    static const uint8_t none[1] = {0};
    n = buildCustom(f, bits1, dcOne, 1, bits1, acEob, 1, none, 0);
    ASSERT_EQ(decodeDefault(f, n, &img), STATUS_ERR_INVALID);
    ASSERT_TRUE(img.pixels == NULL);
}

static const JFix *findJFix(const JFixSet *fs, const char *name) {
    for (int i = 0; i < fs->n; i++) {
        if (strcmp(fs->fx[i].name, name) == 0) {
            return &fs->fx[i];
        }
    }
    return NULL;
}

/* Restarts must be exactly RSTn in order (no resync, D-160); extraneous bytes are skipped; a stray
 * RST after the last MCU is ignored. */
TEST(gfxJpegRestartHandling) {
    JFixSet fs;
    ASSERT_TRUE(loadJFix(&fs));
    static const char *const NAMES[] = {"jg_var_c444__ri2", "jg_var_c420__split_ri1",
                                        "jg_var_c444__prog_default_ri7"};
    for (size_t t = 0; t < sizeof(NAMES) / sizeof(NAMES[0]); t++) {
        const JFix *x = findJFix(&fs, NAMES[t]);
        ASSERT_TRUE(x != NULL);
        size_t sos = 0;
        while (!(x->data[sos] == 0xFF && x->data[sos + 1] == 0xDA)) {
            sos++;
        }
        size_t first = sos + 2, n = x->dataLen;
        while (
            !(x->data[first] == 0xFF && x->data[first + 1] >= 0xD0 && x->data[first + 1] <= 0xD7)) {
            first++;
            ASSERT_TRUE(first + 1 < n);
        }
        uint8_t *m = malloc(n + 8);
        GfxImage img;
        memcpy(m, x->data, n);
        m[first + 1] = (uint8_t)(0xD0 + ((x->data[first + 1] - 0xD0 + 1) & 7)); /* wrong number */
        ASSERT_EQ(decodeDefault(m, n, &img), STATUS_ERR_INVALID);
        ASSERT_TRUE(img.pixels == NULL);
        memcpy(m, x->data, first); /* the RST is missing */
        memcpy(m + first, x->data + first + 2, n - first - 2);
        ASSERT_EQ(decodeDefault(m, n - 2, &img), STATUS_ERR_INVALID);
        memcpy(m, x->data, n); /* a stray RST after the last MCU, before the EOI, is skipped */
        memcpy(m + n - 2, "\xFF\xD5\xFF\xD9", 4);
        ASSERT_EQ(decodeDefault(m, n + 2, &img), STATUS_OK);
        ASSERT_TRUE(pixelsMatch(x, &img));
        gfxImageFree(&img);
        free(m);
    }
    freeJFix(&fs);
}

/* Offset of the next SOS marker at or after `from` (encoder output never has FF DA elsewhere). */
static size_t nextSos(const uint8_t *d, size_t n, size_t from) {
    for (size_t p = from; p + 1 < n; p++) {
        if (d[p] == 0xFF && d[p + 1] == 0xDA) {
            return p;
        }
    }
    return SIZE_MAX;
}

/* SOS field offsets: FF DA len(2) Ns [id sel]*Ns Ss Se AhAl. */
static size_t sosSs(const uint8_t *d, size_t sos) {
    return sos + 5 + 2 * (size_t)d[sos + 4];
}

static Status decodeMutated(const JFix *x, size_t at, uint8_t v) {
    uint8_t *m = malloc(x->dataLen);
    memcpy(m, x->data, x->dataLen);
    m[at] = v;
    GfxImage img;
    Status s = decodeDefault(m, x->dataLen, &img);
    if (s == STATUS_OK) {
        gfxImageFree(&img);
    } else if (img.pixels != NULL) {
        s = 1000;
    }
    free(m);
    return s;
}

/* Every progression rule of T.81 G.1.1.1.1 broken on its own, by editing SOS fields of real
 * progressive files (gray: bands and the default script), plus tolerance of sequential SOS fields.
 */
TEST(gfxJpegProgressionRules) {
    JFixSet fs;
    ASSERT_TRUE(loadJFix(&fs));
    const JFix *bands = findJFix(&fs, "jg_var_gray1__prog_bands");
    const JFix *def = findJFix(&fs, "jg_var_gray1__prog_default");
    const JFix *seq = findJFix(&fs, "jg_var_gray1__inter_opt");
    ASSERT_TRUE(bands != NULL && def != NULL && seq != NULL);
    /* bands: scans are DC(0,0,0,0) AC(1,1) AC(2,9) AC(10,63) */
    size_t s0 = nextSos(bands->data, bands->dataLen, 0);
    size_t s1 = nextSos(bands->data, bands->dataLen, s0 + 2);
    size_t s2 = nextSos(bands->data, bands->dataLen, s1 + 2);
    ASSERT_TRUE(s0 != SIZE_MAX && s1 != SIZE_MAX && s2 != SIZE_MAX);
    const uint8_t *d = bands->data;
    ASSERT_EQ(decodeMutated(bands, 0, d[0]), STATUS_OK); /* unmodified */
    ASSERT_EQ(decodeMutated(bands, sosSs(d, s0) + 1, 1),
              STATUS_ERR_INVALID);                                        /* DC scan with Se = 1 */
    ASSERT_EQ(decodeMutated(bands, sosSs(d, s0), 1), STATUS_ERR_INVALID); /* AC scan before DC */
    ASSERT_EQ(decodeMutated(bands, sosSs(d, s1), 0), STATUS_ERR_INVALID); /* DC scan Ss=0, Se=1 */
    ASSERT_EQ(decodeMutated(bands, sosSs(d, s2), 1), STATUS_ERR_INVALID); /* band overlaps scan 1 */
    ASSERT_EQ(decodeMutated(bands, sosSs(d, s2) + 1, 1), STATUS_ERR_INVALID);    /* Se < Ss */
    ASSERT_EQ(decodeMutated(bands, sosSs(d, s2) + 1, 64), STATUS_ERR_INVALID);   /* Se > 63 */
    ASSERT_EQ(decodeMutated(bands, sosSs(d, s1) + 2, 0xE0), STATUS_ERR_INVALID); /* Ah = 14 */
    ASSERT_EQ(decodeMutated(bands, sosSs(d, s1) + 2, 0x0E), STATUS_ERR_INVALID); /* Al = 14 */
    ASSERT_EQ(decodeMutated(bands, sosSs(d, s1) + 2, 0x10),
              STATUS_ERR_INVALID); /* refine of nothing */
    /* default: DC(al 1) AC(1-5, al 2) AC(6-63, al 2) AC refine(1-63, 2 -> 1) DC refine, AC refine
     */
    size_t r = nextSos(def->data, def->dataLen, 0);
    for (int i = 0; i < 3; i++) {
        r = nextSos(def->data, def->dataLen, r + 2);
    }
    ASSERT_TRUE(r != SIZE_MAX);
    ASSERT_EQ(def->data[sosSs(def->data, r) + 2], 0x21);
    ASSERT_EQ(decodeMutated(def, sosSs(def->data, r) + 2, 0x20),
              STATUS_ERR_INVALID); /* Al != Ah-1 */
    ASSERT_EQ(decodeMutated(def, sosSs(def->data, r) + 2, 0x10), STATUS_ERR_INVALID); /* wrong Ah */
    ASSERT_EQ(decodeMutated(def, sosSs(def->data, r) + 2, 0x32), STATUS_ERR_INVALID); /* wrong Ah */
    /* sequential SOS: Ss/Se/Ah/Al are ignored */
    size_t q = nextSos(seq->data, seq->dataLen, 0);
    ASSERT_TRUE(q != SIZE_MAX);
    size_t f = sosSs(seq->data, q);
    uint8_t *m = malloc(seq->dataLen);
    memcpy(m, seq->data, seq->dataLen);
    m[f] = 5;
    m[f + 1] = 9;
    m[f + 2] = 0x32;
    GfxImage img;
    ASSERT_EQ(decodeDefault(m, seq->dataLen, &img), STATUS_OK);
    ASSERT_TRUE(pixelsMatch(seq, &img));
    gfxImageFree(&img);
    free(m);
    freeJFix(&fs);
}

/* ---- hardening: limits, budget, truncation, fuzz, allocation failure ----------------------------
 */

static const char *const HARDEN_NAMES[] = {
    "jg_nat_gray1_17x13__seq_inter",   "jg_nat_c420_17x13__seq_inter",
    "jg_nat_c444_16x16__prog_default", "jg_nat_c411_17x13__seq_split",
    "jg_var_c420__prog_deep",          "jg_var_c444__prog_default_ri7",
    "jg_rnd_c444_16x16__inter_skew",   "jg_rnd_gray1_17x13__inter_flat_ri3",
    "jg_var_gray1__prog_bands",        "jg_nat_frac_17x13__seq_inter",
};
#define N_HARDEN (sizeof(HARDEN_NAMES) / sizeof(HARDEN_NAMES[0]))

/* An over-limit header is refused after touching only the small decoder state: nothing the size
 * of the image is ever allocated (D-162). */
TEST(gfxJpegLimitsRejectBeforeAllocating) {
    Jb j;
    jbBase(&j, 1);
    j.b[j.sof + 5] = 0xFF; /* 65535 x 65535 */
    j.b[j.sof + 6] = 0xFF;
    j.b[j.sof + 7] = 0xFF;
    j.b[j.sof + 8] = 0xFF;
    DecCountAlloc st;
    GfxAllocator al;
    decAllocInit(&st, &al, -1);
    GfxImage img;
    ASSERT_EQ(gfxJpegDecode(j.b, j.n, NULL, &al, &img), STATUS_ERR_UNSUPPORTED);
    ASSERT_TRUE(img.pixels == NULL);
    ASSERT_TRUE(st.peakBytes <= sizeof(JpegDec) + 1024);
    ASSERT_EQ(st.live, 0);
    /* within the dimension limits, but the coefficients + output exceed a small byte budget */
    static const GfxDecodeLimits lim = {8192, 8192, (uint64_t)1 << 26, (uint64_t)64 << 20};
    j.b[j.sof + 5] = 0x10; /* 4096 x 4096 gray: 33.5 MB coefficients + 67 MB pixels */
    j.b[j.sof + 6] = 0x00;
    j.b[j.sof + 7] = 0x10;
    j.b[j.sof + 8] = 0x00;
    decAllocInit(&st, &al, -1);
    ASSERT_EQ(gfxJpegDecode(j.b, j.n, &lim, &al, &img), STATUS_ERR_UNSUPPORTED);
    ASSERT_TRUE(st.peakBytes <= sizeof(JpegDec) + 1024);
    ASSERT_EQ(st.live, 0);
    /* a limit lower than a legitimate image's size */
    static const GfxDecodeLimits tiny = {4, 4, 16, (uint64_t)1 << 20};
    jbBase(&j, 1); /* 8 x 8 */
    ASSERT_EQ(gfxJpegDecode(j.b, j.n, &tiny, NULL, &img), STATUS_ERR_UNSUPPORTED);
    ASSERT_TRUE(img.pixels == NULL);
}

/* maxTotalBytes bounds the peak of everything live: the measured peak as the budget decodes, one
 * byte less is refused before allocating the coefficients. */
TEST(gfxJpegBudgetIsTheExactPeak) {
    JFixSet fs;
    ASSERT_TRUE(loadJFix(&fs));
    for (size_t i = 0; i < N_HARDEN; i++) {
        const JFix *x = findJFix(&fs, HARDEN_NAMES[i]);
        ASSERT_TRUE(x != NULL);
        DecCountAlloc st;
        GfxAllocator al;
        decAllocInit(&st, &al, -1);
        GfxImage img;
        ASSERT_EQ(gfxJpegDecode(x->data, x->dataLen, NULL, &al, &img), STATUS_OK);
        gfxImageFree(&img);
        GfxDecodeLimits lim = {16384, 16384, (uint64_t)1 << 26, st.peakBytes};
        decAllocInit(&st, &al, -1);
        ASSERT_EQ(gfxJpegDecode(x->data, x->dataLen, &lim, &al, &img), STATUS_OK);
        ASSERT_TRUE(pixelsMatch(x, &img));
        gfxImageFree(&img);
        lim.maxTotalBytes--;
        decAllocInit(&st, &al, -1);
        ASSERT_EQ(gfxJpegDecode(x->data, x->dataLen, &lim, &al, &img), STATUS_ERR_UNSUPPORTED);
        ASSERT_TRUE(img.pixels == NULL);
        ASSERT_EQ(st.live, 0);
        ASSERT_TRUE(st.peakBytes < lim.maxTotalBytes); /* stopped before the big allocations */
    }
    freeJFix(&fs);
}

/* Every proper prefix of a file fails (the EOI is required), in an exact-size heap copy so
 * ASan sees any read past the end. */
TEST(gfxJpegTruncationEveryOffset) {
    JFixSet fs;
    ASSERT_TRUE(loadJFix(&fs));
    for (size_t i = 0; i < N_HARDEN; i++) {
        const JFix *x = findJFix(&fs, HARDEN_NAMES[i]);
        ASSERT_TRUE(x != NULL);
        for (size_t n = 0; n < x->dataLen; n++) {
            uint8_t *copy = malloc(n != 0 ? n : 1);
            memcpy(copy, x->data, n);
            DecCountAlloc st;
            GfxAllocator al;
            decAllocInit(&st, &al, -1);
            GfxImage img;
            Status s = gfxJpegDecode(copy, n, NULL, &al, &img);
            free(copy);
            if (s == STATUS_OK) {
                fprintf(stderr, "  %s truncated to %zu of %u decoded\n", x->name, n, x->dataLen);
            }
            ASSERT_TRUE(s != STATUS_OK);
            ASSERT_TRUE(img.pixels == NULL);
            ASSERT_EQ(st.live, 0);
        }
    }
    freeJFix(&fs);
}

/* Random byte corruption: never a crash or leak, and anything accepted is a well-formed opaque
 * image. Half the mutations land in the first 300 bytes (the headers). */
TEST(gfxJpegMutationFuzz) {
    JFixSet fs;
    ASSERT_TRUE(loadJFix(&fs));
    static const GfxDecodeLimits lim = {4096, 4096, (uint64_t)1 << 22, (uint64_t)64 << 20};
    uint32_t seed = 0xC0FFEE;
    int accepted = 0, runs = 0;
    for (size_t i = 0; i < N_HARDEN; i++) {
        const JFix *x = findJFix(&fs, HARDEN_NAMES[i]);
        ASSERT_TRUE(x != NULL);
        for (int it = 0; it < 2000; it++) {
            uint8_t *m = malloc(x->dataLen);
            memcpy(m, x->data, x->dataLen);
            int nm = 1 + (int)(decRng(&seed) % 3);
            for (int k = 0; k < nm; k++) {
                size_t span = (it & 1) && x->dataLen > 300 ? 300 : x->dataLen;
                size_t at = decRng(&seed) % span;
                if (decRng(&seed) % 4 == 0) {
                    m[at] ^= (uint8_t)(1u << (decRng(&seed) % 8));
                } else {
                    m[at] = (uint8_t)decRng(&seed);
                }
            }
            DecCountAlloc st;
            GfxAllocator al;
            decAllocInit(&st, &al, -1);
            GfxImage img;
            Status s = gfxImageDecode(m, x->dataLen, &lim, &al, &img);
            runs++;
            if (s == STATUS_OK) {
                accepted++;
                ASSERT_TRUE(img.width >= 1 && img.width <= 4096 && img.pixels != NULL);
                for (size_t p = 0; p < (size_t)img.width * img.height; p++) {
                    ASSERT_TRUE((img.pixels[p] >> 24) == 0xFF);
                }
                gfxImageFree(&img);
            } else {
                ASSERT_TRUE(img.pixels == NULL);
            }
            ASSERT_EQ(st.live, 0);
            free(m);
        }
    }
    fprintf(stderr, "  jpeg fuzz: %d runs, %d accepted\n", runs, accepted);
    ASSERT_TRUE(accepted > 0 && accepted < runs);
    freeJFix(&fs);
}

/* A failing allocator at each allocation point: NO_MEMORY, nothing leaked, no pixels. A decode
 * makes four allocations (state, coefficients, output, strips). */
TEST(gfxJpegAllocationFailureSweep) {
    JFixSet fs;
    ASSERT_TRUE(loadJFix(&fs));
    for (size_t i = 0; i < N_HARDEN; i++) {
        const JFix *x = findJFix(&fs, HARDEN_NAMES[i]);
        ASSERT_TRUE(x != NULL);
        for (int failAt = 0; failAt < 8; failAt++) {
            DecCountAlloc st;
            GfxAllocator al;
            decAllocInit(&st, &al, failAt);
            GfxImage img;
            Status s = gfxJpegDecode(x->data, x->dataLen, NULL, &al, &img);
            ASSERT_EQ(st.live, s == STATUS_OK ? 1 : 0); /* only the returned pixels */
            if (failAt < 4) {
                ASSERT_EQ(s, STATUS_ERR_NO_MEMORY);
                ASSERT_TRUE(img.pixels == NULL);
            } else {
                ASSERT_EQ(s, STATUS_OK);
                gfxImageFree(&img);
                ASSERT_EQ(st.live, 0);
            }
        }
    }
    freeJFix(&fs);
}

/* FF FF 00 inside entropy data is a data 0xFF (fill bytes), like FF 00. */
TEST(gfxJpegFillBytesInEntropyAreData) {
    JFixSet fs;
    ASSERT_TRUE(loadJFix(&fs));
    int done = 0;
    for (int i = 0; i < fs.n && done < 5; i++) {
        const JFix *x = &fs.fx[i];
        if (x->status != STATUS_OK || strncmp(x->name, "jg_rnd_", 7) != 0 || (x->flags & 1)) {
            continue;
        }
        GfxImage ref;
        ASSERT_EQ(decodeDefault(x->data, x->dataLen, &ref), STATUS_OK);
        /* find the SOS, then the first FF 00 after it */
        size_t p = 0;
        while (p + 1 < x->dataLen && !(x->data[p] == 0xFF && x->data[p + 1] == 0xDA)) {
            p++;
        }
        size_t q = p + 2;
        while (q + 1 < x->dataLen && !(x->data[q] == 0xFF && x->data[q + 1] == 0x00)) {
            q++;
        }
        if (q + 1 >= x->dataLen) {
            gfxImageFree(&ref);
            continue;
        }
        uint8_t *m = malloc(x->dataLen + 3);
        memcpy(m, x->data, q + 1);
        m[q + 1] = 0xFF;
        m[q + 2] = 0xFF;
        memcpy(m + q + 3, x->data + q + 1, x->dataLen - q - 1);
        GfxImage img;
        /* FF 00 -> FF FF FF 00: the extra fills are skipped */
        ASSERT_EQ(decodeDefault(m, x->dataLen + 2, &img), STATUS_OK);
        ASSERT_TRUE(memcmp(img.pixels, ref.pixels, (size_t)img.width * img.height * 4) == 0);
        gfxImageFree(&img);
        free(m);
        gfxImageFree(&ref);
        done++;
    }
    ASSERT_TRUE(done >= 3);
    freeJFix(&fs);
}

/* gfxImageDecode routes by magic: a bare SOI+FF is JPEG (a truncated one, so INVALID or
 * UNSUPPORTED, never "unknown"), and 'J','F','I','F' junk is still not sniffed. */
TEST(gfxImageDecodeSniffsJpegAndGif) {
    GfxImage img;
    static const uint8_t junk[] = {'J', 'F', 'I', 'F', 0, 0, 0, 0};
    ASSERT_EQ(gfxImageDecode(junk, sizeof(junk), NULL, NULL, &img), STATUS_ERR_UNSUPPORTED);
    ASSERT_TRUE(img.pixels == NULL);
    static const uint8_t soi[] = {0xFF, 0xD8, 0xFF, 0xD9};
    Status st = gfxImageDecode(soi, sizeof(soi), NULL, NULL, &img);
    ASSERT_TRUE(st != STATUS_OK);
    ASSERT_TRUE(img.pixels == NULL);
    static const uint8_t gif[] = {'G', 'I', 'F', '8', '9', 'a'};
    st = gfxImageDecode(gif, sizeof(gif), NULL, NULL, &img);
    ASSERT_TRUE(st != STATUS_OK);
    ASSERT_TRUE(img.pixels == NULL);
}
