/* Structured random-file host test for libs/gfx's JPEG decoder (M12.7 bug-sweeper, D-160..D-162).
 * Unlike gfxJpegMutationFuzz (which flips bytes of valid fixtures), this builds whole files from
 * random parts so the entropy decoder runs deep on arbitrary data: random frame sizes, every
 * sampling factor 1..4 per component (including gray declared 2x2 and non-power-of-two ratios),
 * nearly complete random Huffman tables over legal and illegal symbols, sequential and
 * progressive scan scripts (DC first/refine, AC bands first/refine), restart intervals with
 * RSTn markers after random-length segments, stuffed and unstuffed FF bytes. Every result must be
 * OK/INVALID/UNSUPPORTED with nothing left allocated; every OK image must be opaque, identical
 * whatever garbage the allocator's fresh blocks hold (no uninitialized reads), decode again under
 * a budget equal to its measured peak, and fail with UNSUPPORTED one byte below it. */
#include "framework/test.h"
#include "gfx/gfx-image.h"
#include "gfx/gfx.h"
#include "gfx_decode_testutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *b;
    size_t n, cap;
} JzBuf;

static void jzPut(JzBuf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 256;
        b->b = realloc(b->b, b->cap);
    }
    memcpy(b->b + b->n, p, n);
    b->n += n;
}

static void jzByte(JzBuf *b, uint32_t v) {
    uint8_t c = (uint8_t)v;
    jzPut(b, &c, 1);
}

static void jzBe16(JzBuf *b, uint32_t v) {
    jzByte(b, v >> 8);
    jzByte(b, v & 0xFF);
}

static uint32_t jzR(uint32_t *s, uint32_t n) {
    return n == 0 ? 0 : decRng(s) % n;
}

/* A random Huffman table over `syms` as a DHT segment. A caterpillar (one code of each length
 * 1..16, plus the all-ones code T.81 reserves) is split at random leaves until every symbol has a
 * code, so the tree is complete except for the reserved 16-bit code and random bits almost never
 * hit an unused code. Short symbol lists are padded with duplicates (legal in a DHT) to the 16
 * the caterpillar needs. syms[0] (if `firstShort`) keeps the shortest code; the rest are
 * shuffled. */
static void jzDht(JzBuf *b, uint32_t *s, uint32_t tc, uint32_t th, uint8_t *syms, uint32_t n,
                  bool firstShort) {
    for (uint32_t i = n; i < 16; i++) {
        syms[i] = syms[jzR(s, n)];
    }
    n = n < 16 ? 16 : n;
    uint32_t from = firstShort ? 1 : 0;
    for (uint32_t i = n; i > from + 1; i--) {
        uint32_t k = from + jzR(s, i - from);
        uint8_t t = syms[i - 1];
        syms[i - 1] = syms[k];
        syms[k] = t;
    }
    uint8_t len[260];
    uint32_t leaves = 0;
    for (uint32_t l = 1; l <= 16; l++) {
        len[leaves++] = (uint8_t)l;
    }
    len[leaves++] = 16;
    while (leaves < n + 1) {
        uint32_t k = jzR(s, leaves);
        if (len[k] >= 16) {
            continue;
        }
        len[k]++;
        len[leaves++] = len[k];
    }
    uint8_t bits[16] = {0};
    for (uint32_t i = 0; i < leaves; i++) {
        bits[len[i] - 1]++;
    }
    bits[15]--; /* the all-ones code */
    jzByte(b, 0xFF);
    jzByte(b, 0xC4);
    jzBe16(b, 2 + 17 + n);
    jzByte(b, (tc << 4) | th);
    jzPut(b, bits, 16);
    jzPut(b, syms, n);
}

static void jzDcTable(JzBuf *b, uint32_t *s, uint32_t th) {
    uint8_t syms[32];
    uint32_t n = 0;
    uint32_t top = jzR(s, 20) == 0 ? 16 : 12; /* sometimes an illegal DC size 12..15 */
    for (uint32_t v = 0; v < top; v++) {
        if (jzR(s, 4) != 0 || v == 0) {
            syms[n++] = (uint8_t)v;
        }
    }
    jzDht(b, s, 0, th, syms, n, false);
}

/* AC symbols for sequential/first scans (refine = false) or refinement scans (refine = true). */
static void jzAcTable(JzBuf *b, uint32_t *s, uint32_t th, bool refine, bool progressive) {
    uint8_t syms[256];
    uint32_t n = 0;
    bool illegal = jzR(s, 20) == 0;
    uint32_t pick = 4 + jzR(s, 60);
    for (uint32_t i = 0; i < 256 && n < pick * 3; i++) {
        uint32_t r = i >> 4, sz = i & 15;
        bool ok;
        if (sz == 0) {
            ok = r == 0 || r == 15 || progressive;
        } else {
            ok = refine ? sz == 1 : sz <= 10;
        }
        /* long zero runs overflow the band (INVALID), so they are rare */
        uint32_t w = (r <= 1 || sz == 0) ? pick * 4 : pick / 16;
        if ((ok || illegal) && jzR(s, 256) < w + (i == 0 ? 256 : 0)) {
            syms[n++] = (uint8_t)i;
        }
    }
    if (n == 0) {
        syms[n++] = 0;
    }
    jzDht(b, s, 1, th, syms, n, syms[0] == 0 && jzR(s, 2) == 0);
}

/* Random entropy bytes, FF-stuffed (mostly), for `units` restart segments of up to `per` bytes. */
static void jzEntropy(JzBuf *b, uint32_t *s, uint64_t total, uint32_t ri, uint32_t perUnit) {
    uint64_t segs = ri == 0 ? 1 : (total + ri - 1) / ri;
    uint32_t rst = jzR(s, 50) == 0 ? jzR(s, 8) : 0; /* sometimes a wrong first RST number */
    for (uint64_t g = 0; g < segs; g++) {
        uint64_t units = ri == 0 ? total : ri;
        uint64_t max = units * perUnit + 1;
        if (max > 200000) {
            max = 200000;
        }
        uint32_t len = jzR(s, 4) == 0 ? jzR(s, (uint32_t)max)
                                      : (uint32_t)(max / 2 + jzR(s, (uint32_t)max / 2 + 1));
        for (uint32_t i = 0; i < len; i++) {
            uint32_t v = decRng(s) & 0xFF;
            jzByte(b, v);
            if (v == 0xFF) {
                uint32_t r = jzR(s, 40);
                if (r == 0) {
                    jzByte(b, 0xFF); /* a fill byte: FF FF 00 */
                    jzByte(b, 0x00);
                } else if (r != 1) {
                    jzByte(b, 0x00);
                } /* r == 1: an unstuffed FF (a marker or garbage) */
            }
        }
        if (g + 1 < segs) {
            jzByte(b, 0xFF);
            jzByte(b, 0xD0 + (rst & 7));
            rst++;
        }
    }
}

typedef struct {
    uint32_t nf, h[3], v[3], w, ht, hmax, vmax, mcusX, mcusY, id[3];
    bool progressive;
} JzFrame;

static uint64_t jzCompBlocks(const JzFrame *f, uint32_t c) {
    uint64_t cw = ((uint64_t)f->w * f->h[c] + f->hmax - 1) / f->hmax;
    uint64_t ch = ((uint64_t)f->ht * f->v[c] + f->vmax - 1) / f->vmax;
    return ((cw + 7) / 8) * ((ch + 7) / 8);
}

static void jzSos(JzBuf *b, uint32_t *s, const JzFrame *f, const uint32_t *comps, uint32_t ns,
                  uint32_t ss, uint32_t se, uint32_t ah, uint32_t al, uint32_t ri) {
    jzByte(b, 0xFF);
    jzByte(b, 0xDA);
    jzBe16(b, 6 + 2 * ns);
    jzByte(b, ns);
    for (uint32_t i = 0; i < ns; i++) {
        jzByte(b, f->id[comps[i]]);
        jzByte(b, (jzR(s, 2) << 4) | jzR(s, 2));
    }
    jzByte(b, ss);
    jzByte(b, se);
    jzByte(b, (ah << 4) | al);
    uint64_t total;
    uint32_t bpm = 0;
    if (ns == 1) {
        total = jzCompBlocks(f, comps[0]);
        bpm = 1;
    } else {
        total = (uint64_t)f->mcusX * f->mcusY;
        for (uint32_t i = 0; i < ns; i++) {
            bpm += f->h[comps[i]] * f->v[comps[i]];
        }
    }
    uint32_t per = (ss == 0 && f->progressive) ? bpm * 3 : bpm * (se - ss + 1) * 2 + 2;
    jzEntropy(b, s, total, ri, per);
}

/* Builds one random file into `b`. */
static void jzFile(JzBuf *b, uint32_t *s) {
    JzFrame f;
    memset(&f, 0, sizeof(f));
    f.nf = jzR(s, 3) == 0 ? 1 : 3;
    f.progressive = jzR(s, 2) == 0;
    for (;;) {
        uint32_t blocks = 0;
        for (uint32_t c = 0; c < f.nf; c++) {
            bool sub = jzR(s, 3) == 0;
            f.h[c] = sub ? 1 + jzR(s, 4) : (c == 0 && jzR(s, 2) ? 2 : 1);
            f.v[c] = sub ? 1 + jzR(s, 4) : (c == 0 && jzR(s, 2) ? 2 : 1);
            blocks += f.h[c] * f.v[c];
        }
        if (blocks <= 10 || f.nf == 1 || jzR(s, 20) == 0) {
            break;
        }
    }
    f.w = 1 + jzR(s, jzR(s, 4) == 0 ? 200 : 40);
    f.ht = 1 + jzR(s, jzR(s, 4) == 0 ? 200 : 40);
    for (uint32_t c = 0; c < f.nf; c++) {
        f.hmax = f.h[c] > f.hmax ? f.h[c] : f.hmax;
        f.vmax = f.v[c] > f.vmax ? f.v[c] : f.vmax;
        f.id[c] = jzR(s, 3) == 0 ? (uint32_t) "RGB"[c] : c + 1;
    }
    f.mcusX = (f.w + 8 * f.hmax - 1) / (8 * f.hmax);
    f.mcusY = (f.ht + 8 * f.vmax - 1) / (8 * f.vmax);
    jzByte(b, 0xFF);
    jzByte(b, 0xD8);
    uint32_t app = jzR(s, 3);
    if (app == 0) {
        jzPut(b, "\xFF\xE0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00", 18);
    } else if (app == 1) {
        jzPut(b,
              "\xFF\xEE\x00\x0E"
              "Adobe\x00\x64\x00\x00\x00\x00",
              15);
        jzByte(b, jzR(s, 3));
    }
    for (uint32_t t = 0; t < 4; t++) {
        bool p16 = jzR(s, 3) == 0;
        jzByte(b, 0xFF);
        jzByte(b, 0xDB);
        jzBe16(b, 2 + 1 + (p16 ? 128 : 64));
        jzByte(b, (p16 ? 0x10u : 0u) | t);
        for (uint32_t k = 0; k < 64; k++) {
            uint32_t q = jzR(s, 8) == 0 ? decRng(s) & (p16 ? 0xFFFF : 0xFF) : 1 + jzR(s, 40);
            if (p16) {
                jzBe16(b, q);
            } else {
                jzByte(b, q);
            }
        }
    }
    jzByte(b, 0xFF);
    jzByte(b, f.progressive ? 0xC2 : (jzR(s, 2) ? 0xC0 : 0xC1));
    jzBe16(b, 8 + 3 * f.nf);
    jzByte(b, 8);
    jzBe16(b, f.ht);
    jzBe16(b, f.w);
    jzByte(b, f.nf);
    for (uint32_t c = 0; c < f.nf; c++) {
        jzByte(b, f.id[c]);
        jzByte(b, (f.h[c] << 4) | f.v[c]);
        jzByte(b, jzR(s, 4));
    }
    for (uint32_t t = 0; t < 2; t++) {
        jzDcTable(b, s, t);
    }
    uint32_t ri = jzR(s, 3) == 0 ? 1 + jzR(s, 6) : 0;
    if (ri != 0) {
        jzPut(b, "\xFF\xDD\x00\x04", 4);
        jzBe16(b, ri);
    }
    uint32_t all[3] = {0, 1, 2};
    if (!f.progressive) {
        for (uint32_t t = 0; t < 2; t++) {
            jzAcTable(b, s, t, false, false);
        }
        if (f.nf == 3 && jzR(s, 2) == 0) {
            jzSos(b, s, &f, all, 3, 0, 63, 0, 0, ri);
        } else {
            for (uint32_t c = 0; c < f.nf; c++) {
                uint32_t one = jzR(s, 10) == 0 ? jzR(s, f.nf) : c;
                jzSos(b, s, &f, &one, 1, 0, 63, 0, 0, ri);
            }
        }
    } else {
        uint32_t dcAl = jzR(s, 3);
        if (f.nf == 3 && jzR(s, 2) == 0) {
            jzSos(b, s, &f, all, 3, 0, 0, 0, dcAl, ri);
        } else {
            for (uint32_t c = 0; c < f.nf; c++) {
                jzSos(b, s, &f, &c, 1, 0, 0, 0, dcAl, ri);
            }
        }
        for (uint32_t c = 0; c < f.nf; c++) {
            uint32_t ss = 1;
            while (ss <= 63 && jzR(s, 5) != 0) {
                uint32_t se = ss + jzR(s, 64 - ss);
                uint32_t al = jzR(s, 4);
                for (uint32_t t = 0; t < 2; t++) {
                    jzAcTable(b, s, t, false, true);
                }
                jzSos(b, s, &f, &c, 1, ss, se, 0, al, ri);
                for (uint32_t a = al; a > 0 && jzR(s, 4) != 0; a--) {
                    for (uint32_t t = 0; t < 2; t++) {
                        jzAcTable(b, s, t, true, true);
                    }
                    jzSos(b, s, &f, &c, 1, ss, se, a, a - 1, ri);
                }
                ss = se + 1;
            }
        }
        for (uint32_t a = dcAl; a > 0 && jzR(s, 2) != 0; a--) {
            jzSos(b, s, &f, all, f.nf, 0, 0, a, a - 1, ri);
        }
    }
    jzByte(b, 0xFF);
    jzByte(b, 0xD9);
}

/* An allocator that fills every fresh block with a pattern and tracks the live bytes and peak. */
typedef struct {
    uint8_t fill;
    uint64_t live, peak;
    int count;
} JzAlloc;

static void *jzAllocFn(void *ctx, size_t n) {
    JzAlloc *a = ctx;
    uint8_t *p = malloc(n == 0 ? 1 : n);
    if (p != NULL) {
        memset(p, a->fill, n);
        a->live += n;
        a->peak = a->live > a->peak ? a->live : a->peak;
        a->count++;
    }
    return p;
}

static void jzFreeFn(void *ctx, void *p, size_t n) {
    JzAlloc *a = ctx;
    if (p != NULL) {
        a->live -= n;
        a->count--;
        free(p);
    }
}

TEST(gfxJpegStructuredRandomFiles) {
    int ok = 0, fails = 0;
    int byStatus[4] = {0};
    for (uint32_t seed = 1; seed <= 4000 && fails < 5; seed++) {
        uint32_t s = seed * 2246822519u + 1;
        JzBuf b = {0};
        jzFile(&b, &s);
        JzAlloc st1 = {0xA5, 0, 0, 0}, st2 = {0x5A, 0, 0, 0};
        GfxAllocator a1 = {jzAllocFn, jzFreeFn, &st1}, a2 = {jzAllocFn, jzFreeFn, &st2};
        GfxImage i1, i2;
        Status r1 = gfxJpegDecode(b.b, b.n, NULL, &a1, &i1);
        Status r2 = gfxJpegDecode(b.b, b.n, NULL, &a2, &i2);
        bool good = r1 == r2 &&
                    (r1 == STATUS_OK || r1 == STATUS_ERR_INVALID || r1 == STATUS_ERR_UNSUPPORTED);
        byStatus[r1 == STATUS_OK ? 0 : r1 == STATUS_ERR_INVALID ? 1 : 2]++;
        if (good && r1 == STATUS_OK) {
            ok++;
            size_t np = (size_t)i1.width * i1.height;
            good = i1.width == i2.width && i1.height == i2.height &&
                   memcmp(i1.pixels, i2.pixels, np * 4) == 0;
            for (size_t i = 0; good && i < np; i++) {
                good = (i1.pixels[i] >> 24) == 0xFF;
            }
            uint64_t peak = st1.peak;
            GfxDecodeLimits lim = {GFX_DECODE_DEFAULT_MAX_DIM, GFX_DECODE_DEFAULT_MAX_DIM,
                                   GFX_DECODE_DEFAULT_MAX_PIXELS, peak};
            JzAlloc st3 = {0, 0, 0, 0};
            GfxAllocator a3 = {jzAllocFn, jzFreeFn, &st3};
            GfxImage i3;
            if (good) {
                good = gfxJpegDecode(b.b, b.n, &lim, &a3, &i3) == STATUS_OK &&
                       memcmp(i1.pixels, i3.pixels, np * 4) == 0;
                gfxImageFree(&i3);
            }
            lim.maxTotalBytes = peak - 1;
            if (good) {
                good = gfxJpegDecode(b.b, b.n, &lim, &a3, &i3) == STATUS_ERR_UNSUPPORTED &&
                       i3.pixels == NULL;
            }
            if (st3.count != 0) {
                good = false;
            }
        } else if (good) {
            good = i1.pixels == NULL && i2.pixels == NULL && i1.width == 0 && i1.allocSize == 0;
        }
        gfxImageFree(&i1);
        gfxImageFree(&i2);
        if (st1.count != 0 || st2.count != 0) {
            good = false;
        }
        if (!good) {
            fprintf(stderr, "  seed %u: status %d/%d, live %d/%d\n", seed, (int)r1, (int)r2,
                    st1.count, st2.count);
            fails++;
        }
        free(b.b);
    }
    fprintf(stderr, "  structured JPEG: %d OK, %d INVALID, %d other\n", byStatus[0], byStatus[1],
            byStatus[2]);
    ASSERT_EQ(fails, 0);
    ASSERT_TRUE(ok >= 200); /* the generator must reach the pixel pipeline, not just fail early */
}
