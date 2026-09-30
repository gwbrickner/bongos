/* Adversarial host tests for libs/gfx's sfnt/cmap/metrics layer (M12.3, D-150..D-152), written by
 * the step-2 bug sweep (docs/sweeps/M12.3.md). Fonts are untrusted input, so these build fonts
 * from scratch in exact-size buffers (the cmap table last, so ASan sees any read past its end),
 * compare the format 4/12 lookups, the Q6 scaling and the pixel metrics against independent
 * reference implementations, pin every boundary of the size checks, and run a seeded byte-mutation
 * fuzz over the synthetic fixtures and both shipped fonts. Every accepted font must satisfy the
 * GfxFont invariants and every rejected one must leave *f all-zero. */
#include "font_testutil.h"
#include "framework/test.h"
#include "gfx/font-internal.h"
#include "gfx/gfx-font.h"

#include <stdlib.h>
#include <string.h>

/* ---- a tiny deterministic PRNG (xorshift64*) ----------------------------------------------- */

static uint64_t rngState;

static void rngSeed(uint64_t s) {
    rngState = s != 0 ? s : 0x9E3779B97F4A7C15ull;
}

static uint32_t rng(void) {
    rngState ^= rngState >> 12;
    rngState ^= rngState << 25;
    rngState ^= rngState >> 27;
    return (uint32_t)((rngState * 0x2545F4914F6CDD1Dull) >> 32);
}

static uint32_t rngBelow(uint32_t n) {
    return n == 0 ? 0 : rng() % n;
}

/* ---- a growable big-endian byte buffer ------------------------------------------------------ */

typedef struct {
    uint8_t *d;
    size_t n, cap;
} Buf;

static void bufReserve(Buf *b, size_t extra) {
    if (b->n + extra > b->cap) {
        size_t cap = b->cap != 0 ? b->cap : 256;
        while (cap < b->n + extra) {
            cap *= 2;
        }
        b->d = realloc(b->d, cap);
        b->cap = cap;
    }
}

static void put8(Buf *b, uint32_t v) {
    bufReserve(b, 1);
    b->d[b->n++] = (uint8_t)v;
}

static void put16(Buf *b, uint32_t v) {
    put8(b, v >> 8);
    put8(b, v);
}

static void put32(Buf *b, uint32_t v) {
    put16(b, v >> 16);
    put16(b, v & 0xFFFFu);
}

static void putZeros(Buf *b, size_t n) {
    if (n == 0) {
        return;
    }
    bufReserve(b, n);
    memset(b->d + b->n, 0, n);
    b->n += n;
}

static void putBytes(Buf *b, const uint8_t *p, size_t n) {
    if (n == 0) {
        return;
    }
    bufReserve(b, n);
    memcpy(b->d + b->n, p, n);
    b->n += n;
}

static void set16(uint8_t *d, size_t off, uint32_t v) {
    d[off] = (uint8_t)(v >> 8);
    d[off + 1] = (uint8_t)v;
}

static void set32(uint8_t *d, size_t off, uint32_t v) {
    set16(d, off, v >> 16);
    set16(d, off + 2, v & 0xFFFFu);
}

static uint32_t get16(const uint8_t *d, size_t off) {
    return ((uint32_t)d[off] << 8) | d[off + 1];
}

static uint32_t get32(const uint8_t *d, size_t off) {
    return (get16(d, off) << 16) | get16(d, off + 2);
}

/* ---- a from-scratch font builder ------------------------------------------------------------ */

typedef struct {
    uint32_t upem, numGlyphs, nHM, locFmt;
    int32_t hheaAsc, hheaDesc, hheaGap, headYMin, headYMax;
    bool os2;
    uint32_t os2Len, fsSelection;
    int32_t typoAsc, typoDesc, typoGap;
    const uint32_t *loca; /* numGlyphs + 1 entries (already /2 for the short format); NULL: 0s */
    uint32_t glyfLen;
    const uint8_t *cmap; /* the whole cmap table */
    size_t cmapLen;
    size_t padBefore; /* zero bytes between the directory and the first table */
    size_t totalSize; /* 0: exact; else pad the tables' front so the file is this long */
} FontSpec;

static FontSpec specDefault(const uint8_t *cmap, size_t cmapLen) {
    FontSpec s;
    memset(&s, 0, sizeof s);
    s.upem = 1000;
    s.numGlyphs = 8;
    s.nHM = 8;
    s.hheaAsc = 800;
    s.hheaDesc = -200;
    s.headYMax = 900;
    s.os2Len = 78;
    s.glyfLen = 0;
    s.cmap = cmap;
    s.cmapLen = cmapLen;
    return s;
}

enum { BT_HEAD, BT_HHEA, BT_MAXP, BT_HMTX, BT_LOCA, BT_GLYF, BT_OS2, BT_CMAP, BT_COUNT };
static const char *const btTags[BT_COUNT] = {"head", "hhea", "maxp", "hmtx",
                                             "loca", "glyf", "OS/2", "cmap"};

/* Builds the font into an exact-size malloc'd buffer; the cmap table is the last bytes of it.
 * `tableOff` (optional) receives each table's offset. */
static uint8_t *buildFont(const FontSpec *s, size_t *outN, uint32_t *tableOff) {
    Buf t[BT_COUNT];
    memset(t, 0, sizeof t);
    /* head */
    put32(&t[BT_HEAD], 0x00010000u);
    put32(&t[BT_HEAD], 0x00010000u);
    put32(&t[BT_HEAD], 0);
    put32(&t[BT_HEAD], 0x5F0F3CF5u);
    put16(&t[BT_HEAD], 0);
    put16(&t[BT_HEAD], s->upem);
    putZeros(&t[BT_HEAD], 16);
    put16(&t[BT_HEAD], 0);                     /* xMin */
    put16(&t[BT_HEAD], (uint32_t)s->headYMin); /* yMin */
    put16(&t[BT_HEAD], 1000);                  /* xMax */
    put16(&t[BT_HEAD], (uint32_t)s->headYMax); /* yMax */
    putZeros(&t[BT_HEAD], 6);                  /* macStyle, lowestRecPPEM, dirHint */
    put16(&t[BT_HEAD], s->locFmt);
    put16(&t[BT_HEAD], 0);
    /* hhea */
    put32(&t[BT_HHEA], 0x00010000u);
    put16(&t[BT_HHEA], (uint32_t)s->hheaAsc);
    put16(&t[BT_HHEA], (uint32_t)s->hheaDesc);
    put16(&t[BT_HHEA], (uint32_t)s->hheaGap);
    putZeros(&t[BT_HHEA], 24);
    put16(&t[BT_HHEA], s->nHM);
    /* maxp (version 0.5) */
    put32(&t[BT_MAXP], 0x00005000u);
    put16(&t[BT_MAXP], s->numGlyphs);
    /* hmtx: advance 100 + 10*i, lsb 0; the lsb tail is included */
    uint32_t nHM = s->nHM < s->numGlyphs ? s->nHM : s->numGlyphs;
    for (uint32_t i = 0; i < nHM; i++) {
        put16(&t[BT_HMTX], 100 + 10 * i);
        put16(&t[BT_HMTX], 0);
    }
    for (uint32_t i = nHM; i < s->numGlyphs; i++) {
        put16(&t[BT_HMTX], 0);
    }
    /* loca, glyf */
    for (uint32_t i = 0; i <= s->numGlyphs; i++) {
        uint32_t v = s->loca != NULL ? s->loca[i] : 0;
        if (s->locFmt != 0) {
            put32(&t[BT_LOCA], v);
        } else {
            put16(&t[BT_LOCA], v);
        }
    }
    putZeros(&t[BT_GLYF], s->glyfLen);
    /* OS/2 */
    if (s->os2) {
        putZeros(&t[BT_OS2], s->os2Len);
        if (s->os2Len >= 74) {
            set16(t[BT_OS2].d, 62, s->fsSelection);
            set16(t[BT_OS2].d, 68, (uint32_t)s->typoAsc);
            set16(t[BT_OS2].d, 70, (uint32_t)s->typoDesc);
            set16(t[BT_OS2].d, 72, (uint32_t)s->typoGap);
        }
    }
    putBytes(&t[BT_CMAP], s->cmap, s->cmapLen);

    uint32_t nt = s->os2 ? BT_COUNT : BT_COUNT - 1;
    size_t dirEnd = 12 + 16 * (size_t)nt;
    size_t body = 0;
    for (int i = 0; i < BT_COUNT; i++) {
        body += t[i].n;
    }
    size_t pad = s->padBefore;
    if (s->totalSize != 0) {
        pad = s->totalSize - dirEnd - body;
    }
    size_t n = dirEnd + pad + body;
    uint8_t *d = malloc(n != 0 ? n : 1);
    memset(d, 0, n);
    set32(d, 0, 0x00010000u);
    set16(d, 4, nt);
    size_t off = dirEnd + pad, rec = 12;
    for (int i = 0; i < BT_COUNT; i++) {
        if (i == BT_OS2 && !s->os2) {
            continue;
        }
        memcpy(d + rec, btTags[i], 4);
        set32(d, rec + 8, (uint32_t)off);
        set32(d, rec + 12, (uint32_t)t[i].n);
        if (tableOff != NULL) {
            tableOff[i] = (uint32_t)off;
        }
        if (t[i].n != 0) {
            memcpy(d + off, t[i].d, t[i].n);
        }
        off += t[i].n;
        rec += 16;
        free(t[i].d);
    }
    *outN = n;
    return d;
}

/* ---- cmap builders --------------------------------------------------------------------------- */

typedef struct {
    uint32_t end, start, delta, ro;
} Seg4;

static void putF4(Buf *b, const Seg4 *segs, uint32_t nSegs, const uint32_t *gia, uint32_t nGia) {
    put16(b, 4);
    put16(b, 0xFFFF); /* length: ignored (D-151), deliberately garbage */
    put16(b, 0);
    put16(b, 2 * nSegs);
    put16(b, 0);
    put16(b, 0);
    put16(b, 0);
    for (uint32_t i = 0; i < nSegs; i++) {
        put16(b, segs[i].end);
    }
    put16(b, 0);
    for (uint32_t i = 0; i < nSegs; i++) {
        put16(b, segs[i].start);
    }
    for (uint32_t i = 0; i < nSegs; i++) {
        put16(b, segs[i].delta);
    }
    for (uint32_t i = 0; i < nSegs; i++) {
        put16(b, segs[i].ro);
    }
    for (uint32_t i = 0; i < nGia; i++) {
        put16(b, gia[i]);
    }
}

typedef struct {
    uint32_t start, last, glyph;
} Group12;

static void putF12(Buf *b, const Group12 *g, uint32_t nGroups) {
    put16(b, 12);
    put16(b, 0);
    put32(b, 16 + 12 * nGroups);
    put32(b, 0);
    put32(b, nGroups);
    for (uint32_t i = 0; i < nGroups; i++) {
        put32(b, g[i].start);
        put32(b, g[i].last);
        put32(b, g[i].glyph);
    }
}

/* A one-record cmap around a subtable body. */
static Buf cmapOne(uint32_t plat, uint32_t enc, const Buf *sub) {
    Buf b;
    memset(&b, 0, sizeof b);
    put16(&b, 0);
    put16(&b, 1);
    put16(&b, plat);
    put16(&b, enc);
    put32(&b, 12);
    putBytes(&b, sub->d, sub->n);
    return b;
}

/* ---- independent reference lookups ----------------------------------------------------------- */

/* Format 4 straight from the spec, as a linear scan over parsed arrays (the code under test is a
 * binary search over raw bytes). `tbl`/`tblLen` is the cmap table; `sub` the subtable offset. */
static uint32_t refF4(const uint8_t *tbl, size_t tblLen, size_t sub, uint32_t cp) {
    if (cp > 0xFFFF) {
        return 0;
    }
    uint32_t segs = get16(tbl, sub + 6) / 2;
    size_t endA = sub + 14, startA = endA + 2 * segs + 2, deltaA = startA + 2 * segs,
           roA = deltaA + 2 * segs;
    for (uint32_t i = 0; i < segs; i++) {
        if (get16(tbl, endA + 2 * i) < cp) {
            continue;
        }
        uint32_t start = get16(tbl, startA + 2 * i);
        if (start > cp) {
            return 0;
        }
        uint32_t delta = get16(tbl, deltaA + 2 * i), ro = get16(tbl, roA + 2 * i);
        if (ro == 0) {
            return (cp + delta) % 65536u;
        }
        size_t at = roA + 2 * i + ro + 2 * (cp - start);
        if (at + 2 > tblLen) {
            return 0;
        }
        uint32_t g = get16(tbl, at);
        return g == 0 ? 0 : (g + delta) % 65536u;
    }
    return 0;
}

static uint32_t refF12(const Group12 *g, uint32_t n, uint32_t cp) {
    for (uint32_t i = 0; i < n; i++) {
        if (cp >= g[i].start && cp <= g[i].last) {
            uint64_t gid = (uint64_t)g[i].glyph + (cp - g[i].start);
            return gid > 0xFFFF ? 0 : (uint32_t)gid;
        }
    }
    return 0;
}

/* ---- invariants ----------------------------------------------------------------------------- */

static bool allZero(const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool tableFits(uint32_t off, uint32_t len, size_t n) {
    return (uint64_t)off + len <= n;
}

/* Checks the documented GfxFont invariants of an accepted font and runs every step-2 entry point
 * over it (ASan/UBSan watch the reads). Returns false on a broken invariant. */
static bool checkAccepted(const GfxFont *f, const uint8_t *d, size_t n) {
    if (f->data != d || f->size != n || f->unitsPerEm < 16 || f->unitsPerEm > 16384 ||
        f->numGlyphs < 1 || f->numHMetrics < 1 || f->numHMetrics > f->numGlyphs ||
        f->ascender < 0 || f->descender > 0 || f->lineGap < 0) {
        return false;
    }
    if (!tableFits(f->glyfOff, f->glyfLen, n) || !tableFits(f->locaOff, f->locaLen, n) ||
        !tableFits(f->hmtxOff, f->hmtxLen, n) || !tableFits(f->cmapOff, f->cmapLen, n) ||
        !tableFits(f->gposOff, f->gposLen, n)) {
        return false;
    }
    if (f->hmtxLen < 4u * f->numHMetrics ||
        f->locaLen < (f->numGlyphs + 1u) * (f->locaLong ? 4u : 2u)) {
        return false;
    }
    if ((f->cmapFormat != 4 && f->cmapFormat != 12) || f->cmapSub < f->cmapOff ||
        (uint64_t)f->cmapSub + 2 > (uint64_t)f->cmapOff + f->cmapLen) {
        return false;
    }
    if (f->cmapSymbol && f->cmapFormat != 4) {
        return false;
    }
    static const uint32_t probes[] = {0,       0x20,     0x41,     0x7F,       0xA0,   0xFF,
                                      0x100,   0x2010,   0x3002,   0x4E00,     0xD7FF, 0xE000,
                                      0xF000,  0xF041,   0xFFFD,   0xFFFE,     0xFFFF, 0x10000,
                                      0x1F600, 0x10FFFF, 0x110000, 0xFFFFFFFFu};
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        if (gfxFontGlyphIndex(f, probes[i]) >= f->numGlyphs) {
            return false;
        }
    }
    for (uint32_t cp = 0; cp < 0x3100; cp += 3) {
        uint16_t g = gfxFontGlyphIndex(f, cp);
        if (g >= f->numGlyphs) {
            return false;
        }
        (void)gfxFontAdvanceUnits(f, g);
    }
    for (uint32_t k = 0; k < 64; k++) {
        uint16_t g = gfxFontGlyphIndex(f, rng() % 0x110000u);
        if (g >= f->numGlyphs) {
            return false;
        }
    }
    uint32_t step = f->numGlyphs > 4096 ? f->numGlyphs / 1024 : 1;
    for (uint32_t g = 0; g <= 0xFFFF; g += (g < f->numGlyphs + 2u ? step : 0x1000)) {
        (void)gfxFontAdvanceUnits(f, (uint16_t)g);
        uint32_t a = 0xDEAD, b = 0xBEEF;
        Status st = fontGlyphRange(f, (uint16_t)g, &a, &b);
        if (g >= f->numGlyphs) {
            if (st != STATUS_ERR_INVALID) {
                return false;
            }
        } else if (st == STATUS_OK) {
            if (a > b || b > f->glyfLen) {
                return false;
            }
        } else if (st != STATUS_ERR_INVALID) {
            return false;
        }
    }
    static const uint32_t sizes[] = {GFX_FONT_MIN_SIZE_Q6, 1024, 1000, GFX_FONT_MAX_SIZE_Q6};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        GfxFontMetricsPx m;
        if (gfxFontMetrics(f, sizes[i], &m) != STATUS_OK || m.ascent < 0 || m.descent < 0 ||
            m.lineGap < 0 || m.lineHeight < 1) {
            return false;
        }
    }
    (void)gfxFontScaleQ6(f, INT32_MIN, 0xFFFFFFFFu);
    (void)gfxFontScaleQ6(f, INT32_MAX, 0xFFFFFFFFu);
    return true;
}

/* Runs gfxFontInit on the bytes and checks the result both ways. */
static bool initAndCheck(const uint8_t *d, size_t n, Status *outSt) {
    GfxFont f;
    memset(&f, 0xA5, sizeof f);
    Status st = gfxFontInit(&f, d, n);
    if (outSt != NULL) {
        *outSt = st;
    }
    if (st == STATUS_OK) {
        return checkAccepted(&f, d, n);
    }
    return (st == STATUS_ERR_INVALID || st == STATUS_ERR_UNSUPPORTED) && allZero(&f, sizeof f);
}

/* ---- tests ------------------------------------------------------------------------------------
 */

/* A simple valid cmap: (3,1) format 4 mapping 'A'..'E' to glyphs 1..5. */
static Buf simpleCmap(uint32_t plat, uint32_t enc) {
    Seg4 segs[2] = {{'E', 'A', (1u - 'A') & 0xFFFFu, 0}, {0xFFFF, 0xFFFF, 1, 0}};
    Buf sub;
    memset(&sub, 0, sizeof sub);
    putF4(&sub, segs, 2, NULL, 0);
    Buf c = cmapOne(plat, enc, &sub);
    free(sub.d);
    return c;
}

TEST(fontAdvBuilderBaseline) {
    Buf c = simpleCmap(3, 1);
    FontSpec s = specDefault(c.d, c.n);
    size_t n;
    uint8_t *d = buildFont(&s, &n, NULL);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    ASSERT_TRUE(checkAccepted(&f, d, n));
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 'A'), 1);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 'E'), 5);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 'F'), 0);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 0xFFFF), 0); /* (0xFFFF + 1) & 0xFFFF */
    ASSERT_EQ((int)gfxFontAdvanceUnits(&f, 7), 170);
    free(d);
    free(c.d);
}

/* Every failure path, including the late ones after the header parse (cmap selection), leaves *f
 * all-zero (the gfx-font.h contract), never a half-filled font. */
TEST(fontAdvInitFailureZeroesFont) {
    struct {
        int kind;
        Status want;
    } cases[] = {
        {0, STATUS_ERR_UNSUPPORTED}, /* only a (1,0) cmap: no Unicode subtable */
        {1, STATUS_ERR_INVALID},     /* chosen format-4 subtable with an odd segCountX2 */
        {2, STATUS_ERR_INVALID},     /* chosen format-12 subtable with too many groups */
        {3, STATUS_ERR_INVALID},     /* cmap shorter than its record count */
        {4, STATUS_ERR_INVALID},     /* hmtx too short */
        {5, STATUS_ERR_INVALID},     /* loca too short */
        {6, STATUS_ERR_UNSUPPORTED}, /* version OTTO */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Buf c = simpleCmap(cases[i].kind == 0 ? 1 : 3, cases[i].kind == 0 ? 0 : 1);
        if (cases[i].kind == 1) {
            set16(c.d, 12 + 6, 3);
        } else if (cases[i].kind == 2) {
            Group12 g = {'A', 'E', 1};
            Buf sub;
            memset(&sub, 0, sizeof sub);
            putF12(&sub, &g, 1);
            set32(sub.d, 12, 2);
            free(c.d);
            c = cmapOne(3, 10, &sub);
            free(sub.d);
        } else if (cases[i].kind == 3) {
            set16(c.d, 2, 2);
            c.n = 12 + 4; /* the second record would need 8 more bytes */
        }
        FontSpec s = specDefault(c.d, c.n);
        uint32_t off[BT_COUNT];
        size_t n;
        uint8_t *d = buildFont(&s, &n, off);
        if (cases[i].kind == 4) {
            set32(d, 12 + 16 * BT_HMTX + 12, 4 * s.nHM - 1);
        } else if (cases[i].kind == 5) {
            set32(d, 12 + 16 * BT_LOCA + 12, 2 * s.numGlyphs + 1);
        } else if (cases[i].kind == 6) {
            memcpy(d, "OTTO", 4);
        }
        GfxFont f;
        memset(&f, 0xA5, sizeof f);
        ASSERT_EQ(gfxFontInit(&f, d, n), cases[i].want);
        ASSERT_TRUE(allZero(&f, sizeof f));
        free(d);
        free(c.d);
    }
}

/* Directory and table-range boundaries: exact fits pass, one byte more fails; offsets near
 * 4 GiB never wrap. */
TEST(fontAdvDirectoryBoundaries) {
    Buf c = simpleCmap(3, 1);
    FontSpec s = specDefault(c.d, c.n);
    uint32_t off[BT_COUNT];
    size_t n;
    uint8_t *d = buildFont(&s, &n, off);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK); /* cmap ends exactly at n */
    ASSERT_EQ(gfxFontInit(&f, d, n - 1), STATUS_ERR_INVALID);

    uint8_t *e = malloc(n);
    const size_t cmapRec = 12 + 16 * (BT_COUNT - 2); /* no OS/2 by default: cmap is record 6 */
    ASSERT_TRUE(memcmp(d + cmapRec, "cmap", 4) == 0);
    static const struct {
        uint32_t off, len;
        Status want;
    } rng4g[] = {
        {0xFFFFFFFFu, 1, STATUS_ERR_INVALID},           {0xFFFFFFF0u, 0x20, STATUS_ERR_INVALID},
        {0x80000000u, 0x80000000u, STATUS_ERR_INVALID}, {0, 0xFFFFFFFFu, STATUS_ERR_INVALID},
        {0x00000001u, 0xFFFFFFFFu, STATUS_ERR_INVALID},
    };
    for (size_t i = 0; i < sizeof rng4g / sizeof rng4g[0]; i++) {
        memcpy(e, d, n);
        set32(e, cmapRec + 8, rng4g[i].off);
        set32(e, cmapRec + 12, rng4g[i].len);
        ASSERT_TRUE(initAndCheck(e, n, NULL));
        ASSERT_EQ(gfxFontInit(&f, e, n), rng4g[i].want);
    }
    /* glyf of length 0 placed exactly at the end of the file is a legal empty table */
    memcpy(e, d, n);
    set32(e, 12 + 16 * BT_GLYF + 8, (uint32_t)n);
    set32(e, 12 + 16 * BT_GLYF + 12, 0);
    ASSERT_EQ(gfxFontInit(&f, e, n), STATUS_OK);
    set32(e, 12 + 16 * BT_GLYF + 8, (uint32_t)n + 1);
    ASSERT_EQ(gfxFontInit(&f, e, n), STATUS_ERR_INVALID);

    /* numTables: the directory may end exactly at the file end (tables beyond are then missing) */
    memcpy(e, d, n);
    uint32_t maxTables = (uint32_t)((n - 12) / 16);
    set16(e, 4, maxTables);
    Status st;
    ASSERT_TRUE(initAndCheck(e, n, &st));
    ASSERT_TRUE(initAndCheck(e, 12 + 16 * (size_t)maxTables, &st));
    ASSERT_TRUE(initAndCheck(e, 12 + 16 * (size_t)maxTables - 1, &st));
    ASSERT_EQ(st, STATUS_ERR_INVALID);
    /* one record more than fits: the last record runs 1..16 bytes past the end. Every record
     * after the real seven lies in table bytes (unknown tags), so only the directory check can
     * reject it. */
    memcpy(e, d, n);
    set16(e, 4, maxTables);
    ASSERT_EQ(gfxFontInit(&f, e, n), STATUS_OK);
    set16(e, 4, maxTables + 1);
    ASSERT_EQ(gfxFontInit(&f, e, n), STATUS_ERR_INVALID);
    free(e);
    free(d);
    free(c.d);
}

/* The largest legal file (exactly GFX_FONT_MAX_FILE_BYTES) with every table in its last bytes:
 * offsets near 64 MiB are handled without truncation; one byte more is UNSUPPORTED. */
TEST(fontAdvLargestFile) {
    Buf c = simpleCmap(3, 1);
    FontSpec s = specDefault(c.d, c.n);
    s.os2 = true;
    s.fsSelection = 0x80;
    s.typoAsc = 750;
    s.typoDesc = -250;
    s.typoGap = 90;
    s.totalSize = GFX_FONT_MAX_FILE_BYTES;
    uint32_t off[BT_COUNT];
    size_t n;
    uint8_t *d = buildFont(&s, &n, off);
    ASSERT_EQ(n, (size_t)GFX_FONT_MAX_FILE_BYTES);
    ASSERT_TRUE(off[BT_HEAD] > GFX_FONT_MAX_FILE_BYTES - 4096);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    ASSERT_TRUE(checkAccepted(&f, d, n));
    ASSERT_EQ((int)f.ascender, 750);
    ASSERT_EQ((int)f.descender, -250);
    ASSERT_EQ((int)f.lineGap, 90);
    ASSERT_EQ(f.cmapSub, off[BT_CMAP] + 12);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 'C'), 3);
    ASSERT_EQ((int)gfxFontAdvanceUnits(&f, 3), 130);
    free(d);
    free(c.d);
}

/* Format 4 against the linear spec reference over all 65536 codepoints, for random sorted
 * segment sets: delta wraparound, idRangeOffset into the glyph array (even and odd, and pointing
 * past the table end), start > end, duplicate endCodes and a final 0xFFFF segment. The subtable
 * is the last thing in the buffer, so a read past the cmap end is an ASan report. */
TEST(fontAdvFormat4MatchesReference) {
    rngSeed(0xF0A7F0A7u);
    for (int iter = 0; iter < 60; iter++) {
        uint32_t nSegs = 1 + rngBelow(iter < 10 ? 4 : 200);
        Seg4 *segs = calloc(nSegs, sizeof *segs);
        uint32_t nGia = rngBelow(400);
        uint32_t *gia = calloc(nGia + 1, sizeof *gia);
        for (uint32_t i = 0; i < nGia; i++) {
            gia[i] = rngBelow(4) == 0 ? rng() & 0xFFFFu : rngBelow(300);
        }
        uint32_t prev = 0;
        for (uint32_t i = 0; i < nSegs; i++) {
            uint32_t room = 0xFFFFu - prev;
            uint32_t end = prev + (rngBelow(8) == 0 ? 0 : rngBelow(room / (nSegs - i) * 2 + 1));
            if (end > 0xFFFFu) {
                end = 0xFFFFu;
            }
            if (i == nSegs - 1 && rngBelow(2) == 0) {
                end = 0xFFFFu;
            }
            uint32_t start = rngBelow(10) == 0 ? (end + 1 + rngBelow(50)) & 0xFFFFu
                                               : end - rngBelow((end - prev) + 1);
            segs[i].end = end;
            segs[i].start = start;
            segs[i].delta = rngBelow(3) == 0 ? (rng() & 0xFFFFu) : ((0x10000u - start) & 0xFFFFu);
            switch (rngBelow(5)) {
                case 0:
                case 1:
                    segs[i].ro = 0;
                    break;
                case 2: /* into the glyph array, as a real font does */
                    segs[i].ro = 2 * (nSegs - i) + 2 * rngBelow(nGia + 1);
                    break;
                case 3: /* odd: straddles two entries */
                    segs[i].ro = (2 * (nSegs - i) + 2 * rngBelow(nGia + 1) + 1) & 0xFFFFu;
                    break;
                default:
                    segs[i].ro = rng() & 0xFFFFu;
                    break;
            }
            prev = end;
        }
        Buf sub;
        memset(&sub, 0, sizeof sub);
        putF4(&sub, segs, nSegs, gia, nGia);
        static const uint32_t pe4[3][2] = {{3, 1}, {0, 3}, {0, 0}};
        const uint32_t *pe = pe4[rngBelow(3)];
        Buf cm = cmapOne(pe[0], pe[1], &sub);
        FontSpec s = specDefault(cm.d, cm.n);
        s.numGlyphs = 300;
        s.nHM = 300;
        uint32_t off[BT_COUNT];
        size_t n;
        uint8_t *d = buildFont(&s, &n, off);
        GfxFont f;
        ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
        ASSERT_EQ((int)f.cmapFormat, 4);
        const uint8_t *tbl = d + off[BT_CMAP];
        for (uint32_t cp = 0; cp <= 0x10000u; cp++) {
            uint32_t want = refF4(tbl, cm.n, 12, cp);
            if (want >= s.numGlyphs) {
                want = 0;
            }
            uint32_t got = gfxFontGlyphIndex(&f, cp);
            if (got != want) {
                fprintf(stderr, "  iter %d cp %#x: got %u want %u\n", iter, cp, got, want);
            }
            ASSERT_EQ(got, want);
        }
        free(d);
        free(cm.d);
        free(sub.d);
        free(segs);
        free(gia);
    }
}

/* Format 12 against the linear reference for random sorted, disjoint groups, including groups
 * whose glyph ids run past 0xFFFF (they must give 0, never wrap to a small valid glyph) and
 * codepoints above U+10FFFF in the table. */
TEST(fontAdvFormat12MatchesReference) {
    rngSeed(0x12121212u);
    for (int iter = 0; iter < 200; iter++) {
        uint32_t nGroups = rngBelow(iter < 20 ? 3 : 120);
        Group12 *g = calloc(nGroups + 1, sizeof *g);
        uint32_t next = rngBelow(0x100);
        for (uint32_t i = 0; i < nGroups; i++) {
            uint32_t span = rngBelow(8) == 0 ? rngBelow(0x20000) : rngBelow(64);
            g[i].start = next;
            g[i].last = next + span;
            switch (next < 8 ? 3 : rngBelow(6)) {
                case 0: /* straddles 0x10000: the tail would wrap to 0, 1, 2... */
                    g[i].glyph = 0x10000u - 1 - rngBelow(span + 1);
                    break;
                case 1:
                    g[i].glyph = rng();
                    break;
                case 2: /* start > last: an empty (malformed) group */
                    g[i].last = g[i].start - 1 - rngBelow(4);
                    g[i].glyph = 1;
                    break;
                default:
                    g[i].glyph = rngBelow(400);
                    break;
            }
            uint32_t hi = g[i].last > g[i].start ? g[i].last : g[i].start;
            next = hi + 1 + (rngBelow(3) == 0 ? 0 : rngBelow(0x800));
        }
        Buf sub;
        memset(&sub, 0, sizeof sub);
        putF12(&sub, g, nGroups);
        Buf cm = cmapOne(rngBelow(2) == 0 ? 3 : 0, 10, &sub);
        if (get16(cm.d, 4) == 0) {
            set16(cm.d, 6, 4); /* (0,4) */
        }
        FontSpec s = specDefault(cm.d, cm.n);
        s.numGlyphs = 300;
        s.nHM = 300;
        size_t n;
        uint8_t *d = buildFont(&s, &n, NULL);
        GfxFont f;
        ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
        ASSERT_EQ((int)f.cmapFormat, 12);
        for (uint32_t i = 0; i < nGroups; i++) {
            uint32_t pts[] = {g[i].start - 1,
                              g[i].start,
                              g[i].start + 1,
                              g[i].last - 1,
                              g[i].last,
                              g[i].last + 1,
                              g[i].start + rngBelow(g[i].last - g[i].start + 1)};
            for (size_t k = 0; k < sizeof pts / sizeof pts[0]; k++) {
                uint32_t cp = pts[k];
                uint32_t want = cp > 0x10FFFFu ? 0 : refF12(g, nGroups, cp);
                if (want >= s.numGlyphs) {
                    want = 0;
                }
                ASSERT_EQ((uint32_t)gfxFontGlyphIndex(&f, cp), want);
            }
        }
        for (int k = 0; k < 256; k++) {
            uint32_t cp = rngBelow(0x110000);
            uint32_t want = refF12(g, nGroups, cp);
            ASSERT_EQ((uint32_t)gfxFontGlyphIndex(&f, cp), want >= s.numGlyphs ? 0 : want);
        }
        free(d);
        free(cm.d);
        free(sub.d);
        free(g);
    }
    /* the exact wrap case: startGlyph 0xFFFF, so cp+1 would be glyph 0x10000 -> 0 and cp+2
     * 0x10001, which a 16-bit truncation would turn into the valid glyph 1 */
    Group12 w = {0x100, 0x110, 0xFFFFu};
    Buf sub;
    memset(&sub, 0, sizeof sub);
    putF12(&sub, &w, 1);
    Buf cm = cmapOne(3, 10, &sub);
    FontSpec s = specDefault(cm.d, cm.n);
    size_t n;
    uint8_t *d = buildFont(&s, &n, NULL);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    for (uint32_t cp = 0x100; cp <= 0x110; cp++) {
        ASSERT_EQ((int)gfxFontGlyphIndex(&f, cp), 0);
    }
    free(d);
    free(cm.d);
    free(sub.d);
}

/* Subtable size checks at their exact boundaries. */
TEST(fontAdvCmapSubtableBoundaries) {
    GfxFont f;
    /* format 4: the fixed part (16 + 4*segX2 bytes) ending exactly at the cmap end is OK */
    Seg4 segs[3] = {{'Z', 'A', (1u - 'A') & 0xFFFFu, 0}, {'z', 'a', 1u, 0}, {0xFFFF, 0xFFFF, 1, 0}};
    Buf sub;
    memset(&sub, 0, sizeof sub);
    putF4(&sub, segs, 3, NULL, 0);
    ASSERT_EQ(sub.n, (size_t)(16 + 4 * 6));
    Buf cm = cmapOne(3, 1, &sub);
    FontSpec s = specDefault(cm.d, cm.n);
    s.numGlyphs = 200;
    s.nHM = 200;
    uint32_t off[BT_COUNT];
    size_t n;
    uint8_t *d = buildFont(&s, &n, off);
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 'Z'), 26);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 'a'), 'a' + 1);
    set16(d, off[BT_CMAP] + 12 + 6, 8); /* one more segment: 2 bytes past the end */
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_ERR_INVALID);
    set16(d, off[BT_CMAP] + 12 + 6, 2); /* fewer segments are fine (the arrays shift) */
    ASSERT_TRUE(initAndCheck(d, n, NULL));
    /* the cmap table 2 bytes shorter than the 3-segment fixed part: INVALID */
    set16(d, off[BT_CMAP] + 12 + 6, 6);
    set32(d, 12 + 16 * BT_CMAP - 16 + 12, (uint32_t)cm.n - 2); /* no OS/2: cmap is record 6 */
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_ERR_INVALID);
    set32(d, 12 + 16 * BT_CMAP - 16 + 12, (uint32_t)cm.n);
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    free(d);
    free(cm.d);
    free(sub.d);

    /* format 12: numGroups exactly filling the table is OK, one more is INVALID */
    Group12 g[2] = {{0x41, 0x5A, 1}, {0x1F600, 0x1F60F, 40}};
    memset(&sub, 0, sizeof sub);
    putF12(&sub, g, 2);
    cm = cmapOne(3, 10, &sub);
    s = specDefault(cm.d, cm.n);
    s.numGlyphs = 60;
    s.nHM = 1;
    d = buildFont(&s, &n, off);
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 0x1F60F), 55);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 0x1F610), 0);
    set32(d, off[BT_CMAP] + 12 + 12, 3);
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_ERR_INVALID);
    set32(d, off[BT_CMAP] + 12 + 12, 0); /* zero groups: valid, nothing maps */
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 0x41), 0);
    /* the (3,10) record's subtable header must fit: 15 bytes of format-12 header is INVALID */
    set32(d, off[BT_CMAP] + 8, (uint32_t)(cm.n - 15));
    set16(d, n - 15, 12);
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_ERR_INVALID);
    /* a record pointing at the last byte of the table (format needs 2 bytes): not a candidate */
    set32(d, off[BT_CMAP] + 8, (uint32_t)(cm.n - 1));
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_ERR_UNSUPPORTED);
    free(d);
    free(cm.d);
    free(sub.d);
}

/* Rank order is independent of record order, and ties go to the first record. */
TEST(fontAdvCmapRankOrderAndTies) {
    /* five subtables, one per rank, each mapping 'A' to a distinct glyph (rank r -> glyph r) */
    static const uint32_t pe[5][3] = {{3, 0, 4}, {0, 3, 4}, {3, 1, 4}, {0, 4, 12}, {3, 10, 12}};
    rngSeed(0xABCDEFu);
    for (int perm = 0; perm < 120; perm++) {
        uint32_t order[5] = {0, 1, 2, 3, 4};
        for (uint32_t i = 4; i > 0; i--) {
            uint32_t j = rngBelow(i + 1), t = order[i];
            order[i] = order[j];
            order[j] = t;
        }
        uint32_t nRec = 1 + rngBelow(5);
        Buf b;
        memset(&b, 0, sizeof b);
        put16(&b, 0);
        put16(&b, nRec);
        size_t subStart = 4 + 8 * nRec;
        Buf subs;
        memset(&subs, 0, sizeof subs);
        uint32_t best = 0;
        for (uint32_t k = 0; k < nRec; k++) {
            uint32_t r = order[k];
            best = r + 1 > best ? r + 1 : best;
            put16(&b, pe[r][0]);
            put16(&b, pe[r][1]);
            put32(&b, (uint32_t)(subStart + subs.n));
            uint32_t cp = pe[r][0] == 3 && pe[r][1] == 0 ? 0xF041u : 'A';
            if (pe[r][2] == 4) {
                Seg4 sg[2] = {{cp, cp, ((r + 1) - cp) & 0xFFFFu, 0}, {0xFFFF, 0xFFFF, 1, 0}};
                putF4(&subs, sg, 2, NULL, 0);
            } else {
                Group12 gg = {cp, cp, r + 1};
                putF12(&subs, &gg, 1);
            }
        }
        putBytes(&b, subs.d, subs.n);
        FontSpec s = specDefault(b.d, b.n);
        size_t n;
        uint8_t *d = buildFont(&s, &n, NULL);
        GfxFont f;
        ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
        ASSERT_EQ((uint32_t)gfxFontGlyphIndex(&f, 'A'), best);
        ASSERT_EQ(f.cmapSymbol, best == 1);
        free(d);
        free(b.d);
        free(subs.d);
    }
    /* two (3,1) format-4 records: the first wins */
    Buf b;
    memset(&b, 0, sizeof b);
    put16(&b, 0);
    put16(&b, 2);
    Buf subs;
    memset(&subs, 0, sizeof subs);
    for (uint32_t k = 0; k < 2; k++) {
        put16(&b, 3);
        put16(&b, 1);
        put32(&b, (uint32_t)(20 + subs.n));
        Seg4 sg[2] = {{'A', 'A', ((k + 1) - 'A') & 0xFFFFu, 0}, {0xFFFF, 0xFFFF, 1, 0}};
        putF4(&subs, sg, 2, NULL, 0);
    }
    putBytes(&b, subs.d, subs.n);
    FontSpec s = specDefault(b.d, b.n);
    size_t n;
    uint8_t *d = buildFont(&s, &n, NULL);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 'A'), 1);
    free(d);
    free(b.d);
    free(subs.d);
}

/* hmtx: numberOfHMetrics 1 (every glyph reuses advance 0), the table ending exactly at the
 * required 4*nHM bytes in the last bytes of the file, loca and glyph ranges. */
TEST(fontAdvHmtxLocaEdges) {
    Buf c = simpleCmap(3, 1);
    FontSpec s = specDefault(c.d, c.n);
    s.numGlyphs = 5;
    s.nHM = 1;
    uint32_t loca[6] = {0, 5, 5, 9, 20, 40}; /* short format: x2 -> 0,10,10,18,40,80 */
    s.loca = loca;
    s.glyfLen = 80;
    uint32_t off[BT_COUNT];
    size_t n;
    uint8_t *d = buildFont(&s, &n, off);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    for (uint16_t g = 0; g < 5; g++) {
        ASSERT_EQ((int)gfxFontAdvanceUnits(&f, g), 100);
    }
    ASSERT_EQ((int)gfxFontAdvanceUnits(&f, 5), 0);
    uint32_t a, b;
    static const uint32_t wantA[5] = {0, 10, 10, 18, 40}, wantB[5] = {10, 10, 18, 40, 80};
    for (uint16_t g = 0; g < 5; g++) {
        ASSERT_EQ(fontGlyphRange(&f, g, &a, &b), STATUS_OK);
        ASSERT_EQ(a, wantA[g]);
        ASSERT_EQ(b, wantB[g]);
    }
    ASSERT_EQ(fontGlyphRange(&f, 5, &a, &b), STATUS_ERR_INVALID);
    ASSERT_EQ(fontGlyphRange(&f, 0xFFFF, &a, &b), STATUS_ERR_INVALID);
    free(d);
    /* the hmtx tail reuses glyph numberOfHMetrics-1 (not glyph 0): nHM 3 of 5 */
    s.nHM = 3;
    d = buildFont(&s, &n, off);
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    static const int wantAdv[5] = {100, 110, 120, 120, 120};
    for (uint16_t g = 0; g < 5; g++) {
        ASSERT_EQ((int)gfxFontAdvanceUnits(&f, g), wantAdv[g]);
    }
    s.nHM = 1;
    free(d);
    d = buildFont(&s, &n, off);
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    /* end == glyfLen is OK, one past is not; a decreasing pair is INVALID */
    set32(d, 12 + 16 * BT_GLYF + 12, 79);
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    ASSERT_EQ(fontGlyphRange(&f, 3, &a, &b), STATUS_OK);
    ASSERT_EQ(fontGlyphRange(&f, 4, &a, &b), STATUS_ERR_INVALID);
    set16(d, off[BT_LOCA] + 2 * 2, 4); /* glyph 1 = [10, 8), glyph 2 = [8, 18) */
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    ASSERT_EQ(fontGlyphRange(&f, 1, &a, &b), STATUS_ERR_INVALID);
    ASSERT_EQ(fontGlyphRange(&f, 2, &a, &b), STATUS_OK);
    ASSERT_EQ(a, 8u);
    free(d);

    /* long loca with values past 4 GiB/2 and a full-length hmtx at the file's end */
    s.locFmt = 1;
    s.nHM = 5;
    uint32_t lloca[6] = {0, 3, 3, 0x7FFFFFFFu, 0xFFFFFFFFu, 3};
    s.loca = lloca;
    s.glyfLen = 3;
    d = buildFont(&s, &n, off);
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    ASSERT_TRUE(f.locaLong);
    ASSERT_EQ(fontGlyphRange(&f, 0, &a, &b), STATUS_OK);
    ASSERT_EQ(b, 3u); /* odd offsets are fine in the long format */
    ASSERT_EQ(fontGlyphRange(&f, 1, &a, &b), STATUS_OK);
    ASSERT_EQ(fontGlyphRange(&f, 2, &a, &b), STATUS_ERR_INVALID);
    ASSERT_EQ(fontGlyphRange(&f, 3, &a, &b), STATUS_ERR_INVALID);
    ASSERT_EQ(fontGlyphRange(&f, 4, &a, &b), STATUS_ERR_INVALID);
    ASSERT_EQ((int)gfxFontAdvanceUnits(&f, 4), 140);
    set32(d, 12 + 16 * BT_LOCA + 12, 4 * 6 - 1);
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_ERR_INVALID);
    free(d);
    free(c.d);
}

/* Q6 scaling against an exact 128-bit reference: every upem bound, both signs, exact halves,
 * saturation, and random operands. */
static int32_t refScale(int32_t units, uint32_t size, uint32_t upem) {
    __int128 n = (__int128)units * size;
    __int128 a = n < 0 ? -n : n;
    __int128 q = a / upem, r = a % upem;
    if (2 * r >= upem) {
        q++;
    }
    __int128 v = n < 0 ? -q : q;
    if (v > INT32_MAX) {
        return INT32_MAX;
    }
    if (v < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)v;
}

static void fontForUpem(uint32_t upem, GfxFont *f, uint8_t **keep) {
    Buf c = simpleCmap(3, 1);
    FontSpec s = specDefault(c.d, c.n);
    s.upem = upem;
    size_t n;
    *keep = buildFont(&s, &n, NULL);
    Status st = gfxFontInit(f, *keep, n);
    free(c.d);
    if (st != STATUS_OK) {
        memset(f, 0, sizeof *f);
    }
}

TEST(fontAdvScaleQ6MatchesReference) {
    static const uint32_t upems[] = {16, 17, 1000, 1024, 2048, 2049, 16383, 16384};
    static const int32_t units[] = {0,     1,      -1,         2,         -2,           7,
                                    -7,    8,      -8,         32767,     -32768,       65535,
                                    65536, -65536, 0x7FFFFFFF, INT32_MIN, INT32_MIN + 1};
    static const uint32_t sizes[] = {0, 1, 63, 64, 1024, 32768, 65536, 0x7FFFFFFFu, 0xFFFFFFFFu};
    rngSeed(0x5CA1E);
    for (size_t u = 0; u < sizeof upems / sizeof upems[0]; u++) {
        GfxFont f;
        uint8_t *keep;
        fontForUpem(upems[u], &f, &keep);
        ASSERT_EQ((uint32_t)f.unitsPerEm, upems[u]);
        for (size_t i = 0; i < sizeof units / sizeof units[0]; i++) {
            for (size_t j = 0; j < sizeof sizes / sizeof sizes[0]; j++) {
                ASSERT_EQ(gfxFontScaleQ6(&f, units[i], sizes[j]),
                          refScale(units[i], sizes[j], upems[u]));
            }
        }
        /* exact halves both ways: units*size = k*upem + upem/2 (even upem) */
        if (upems[u] % 2 == 0) {
            for (int32_t k = -40; k <= 40; k++) {
                int32_t un = (int32_t)(k * (int32_t)upems[u] + (int32_t)upems[u] / 2);
                int32_t want = un >= 0 ? k + 1 : k;
                ASSERT_EQ(gfxFontScaleQ6(&f, un, 1), want);
            }
        }
        for (int k = 0; k < 20000; k++) {
            int32_t un = (int32_t)rng();
            if (k % 2 == 0) {
                un %= 70000;
            }
            uint32_t sz = k % 3 == 0 ? rng() : rngBelow(GFX_FONT_MAX_SIZE_Q6 + 1);
            ASSERT_EQ(gfxFontScaleQ6(&f, un, sz), refScale(un, sz, upems[u]));
        }
        free(keep);
    }
    ASSERT_EQ(gfxFontScaleQ6(NULL, 5, 64), 0);
}

/* Pixel metrics against an independent reference at the int16 extremes, every size bound, the
 * lineHeight floor of 1, and the normalization of hhea signs (including -32768). */
static int32_t refCeil(int64_t a, int64_t den) {
    int64_t q = a / den;
    return (int32_t)(q * den < a ? q + 1 : q);
}

TEST(fontAdvMetricsExtremes) {
    static const struct {
        int32_t asc, desc, gap;
        int32_t wantAsc, wantDesc, wantGap;
    } hh[] = {
        {32767, -32768, 32767, 32767, -32768, 32767},
        {1, -1, 0, 1, -1, 0},
        {-32768, 32767, -32768, 0, -32767, 0},
        {0, 5, 0, 0, -5, 0}, /* hhea counts as present (desc != 0) */
        {3, -32768, -1, 3, -32768, 0},
    };
    static const uint32_t upems[] = {16, 1000, 16384};
    static const uint32_t sizes[] = {GFX_FONT_MIN_SIZE_Q6,     GFX_FONT_MIN_SIZE_Q6 + 1, 1000, 1024,
                                     GFX_FONT_MAX_SIZE_Q6 - 1, GFX_FONT_MAX_SIZE_Q6};
    Buf c = simpleCmap(3, 1);
    for (size_t i = 0; i < sizeof hh / sizeof hh[0]; i++) {
        for (size_t u = 0; u < sizeof upems / sizeof upems[0]; u++) {
            FontSpec s = specDefault(c.d, c.n);
            s.upem = upems[u];
            s.hheaAsc = hh[i].asc;
            s.hheaDesc = hh[i].desc;
            s.hheaGap = hh[i].gap;
            size_t n;
            uint8_t *d = buildFont(&s, &n, NULL);
            GfxFont f;
            ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
            ASSERT_EQ((int)f.ascender, hh[i].wantAsc);
            ASSERT_EQ((int)f.descender, hh[i].wantDesc);
            ASSERT_EQ((int)f.lineGap, hh[i].wantGap);
            for (size_t z = 0; z < sizeof sizes / sizeof sizes[0]; z++) {
                GfxFontMetricsPx m;
                ASSERT_EQ(gfxFontMetrics(&f, sizes[z], &m), STATUS_OK);
                int64_t den = (int64_t)upems[u] * 64;
                int32_t a = refCeil((int64_t)hh[i].wantAsc * sizes[z], den);
                int32_t de = refCeil(-(int64_t)hh[i].wantDesc * sizes[z], den);
                int32_t g = (int32_t)((2 * (int64_t)hh[i].wantGap * sizes[z] + den) / (2 * den));
                ASSERT_EQ(m.ascent, a);
                ASSERT_EQ(m.descent, de);
                ASSERT_EQ(m.lineGap, g);
                ASSERT_EQ(m.lineHeight, a + de + g < 1 ? 1 : a + de + g);
            }
            free(d);
        }
    }
    /* all-zero metrics (hhea 0/0, no OS/2, head box 0..0): lineHeight is floored to 1 */
    FontSpec s = specDefault(c.d, c.n);
    s.hheaAsc = 0;
    s.hheaDesc = 0;
    s.headYMax = 0;
    s.headYMin = 0;
    size_t n;
    uint8_t *d = buildFont(&s, &n, NULL);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    GfxFontMetricsPx m;
    ASSERT_EQ(gfxFontMetrics(&f, GFX_FONT_MAX_SIZE_Q6, &m), STATUS_OK);
    ASSERT_EQ(m.ascent, 0);
    ASSERT_EQ(m.descent, 0);
    ASSERT_EQ(m.lineHeight, 1);
    free(d);
    /* an OS/2 shorter than 78 bytes is ignored even with USE_TYPO_METRICS; hhea applies */
    s = specDefault(c.d, c.n);
    s.os2 = true;
    s.os2Len = 77;
    s.fsSelection = 0x80;
    d = buildFont(&s, &n, NULL);
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    ASSERT_EQ((int)f.ascender, 800);
    free(d);
    /* OS/2 without USE_TYPO_METRICS loses to nonzero hhea, wins over zero hhea */
    s = specDefault(c.d, c.n);
    s.os2 = true;
    s.fsSelection = 0x40; /* REGULAR, not USE_TYPO_METRICS */
    s.typoAsc = 600;
    s.typoDesc = -100;
    s.typoGap = 7;
    d = buildFont(&s, &n, NULL);
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    ASSERT_EQ((int)f.ascender, 800);
    free(d);
    s.hheaAsc = 0;
    s.hheaDesc = 0;
    d = buildFont(&s, &n, NULL);
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    ASSERT_EQ((int)f.ascender, 600);
    ASSERT_EQ((int)f.descender, -100);
    ASSERT_EQ((int)f.lineGap, 7);
    free(d);
    /* head box fallback: yMax/yMin, and a yMin of -32768 */
    s = specDefault(c.d, c.n);
    s.hheaAsc = 0;
    s.hheaDesc = 0;
    s.hheaGap = 55; /* ignored: the head box has no gap */
    s.headYMax = 1234;
    s.headYMin = -32768;
    d = buildFont(&s, &n, NULL);
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    ASSERT_EQ((int)f.ascender, 1234);
    ASSERT_EQ((int)f.descender, -32768);
    ASSERT_EQ((int)f.lineGap, 0);
    ASSERT_EQ(gfxFontMetrics(&f, 64, &m), STATUS_OK);
    ASSERT_EQ(m.descent, 33); /* ceil(32768 / 1000) */
    free(d);
    free(c.d);
}

/* ---- the seeded mutation fuzz --------------------------------------------------------------- */

typedef struct {
    uint32_t off, len;
} Region;

/* Regions worth aiming at: the directory, each required/metric table's head, the cmap header,
 * records and the chosen subtable's header and arrays, loca and hmtx. */
static uint32_t fuzzRegions(const uint8_t *d, size_t n, Region *out, uint32_t cap) {
    uint32_t k = 0;
    uint32_t nt = get16(d, 4);
    out[k++] = (Region){0, (uint32_t)(12 + 16 * (nt < 64 ? nt : 64))};
    static const char *const tags[] = {"head", "hhea", "maxp", "OS/2", "cmap",
                                       "loca", "hmtx", "glyf", "kern", "GPOS"};
    for (size_t t = 0; t < sizeof tags / sizeof tags[0] && k + 3 < cap; t++) {
        uint32_t len = 0;
        uint32_t off = ftuTable(d, n, tags[t], &len);
        if (off == 0 || (uint64_t)off + len > n) {
            continue;
        }
        out[k++] = (Region){off, len < 96 ? len : 96};
        if (strcmp(tags[t], "cmap") == 0 && len >= 12) {
            uint32_t nsub = get16(d, off + 2);
            for (uint32_t i = 0; i < nsub && i < 8 && k + 2 < cap; i++) {
                uint32_t sub = off + get32(d, off + 4 + 8 * i + 4);
                if (sub < n) {
                    uint32_t l = (uint32_t)(n - sub < 4096 ? n - sub : 4096);
                    out[k++] = (Region){sub, l < 64 ? l : 64};
                    out[k++] = (Region){sub, l};
                }
            }
        } else if (len > 96) {
            out[k++] = (Region){off, len}; /* anywhere in loca/hmtx/glyf */
        }
    }
    return k;
}

static void fuzzOne(uint8_t *buf, const uint8_t *src, size_t n, const Region *rg, uint32_t nRg) {
    memcpy(buf, src, n);
    uint32_t writes = 1 + rngBelow(rngBelow(4) == 0 ? 16 : 4);
    for (uint32_t w = 0; w < writes; w++) {
        const Region *r = &rg[rngBelow(nRg)];
        if (r->len == 0) {
            continue;
        }
        uint32_t at = r->off + rngBelow(r->len);
        uint32_t kind = rngBelow(8);
        if (kind == 0 && at + 4 <= n) { /* an interesting 32-bit value (offsets, counts) */
            static const uint32_t vals[] = {0,           1,           0x7FFFFFFFu, 0x80000000u,
                                            0xFFFFFFFFu, 0xFFFFFFF0u, 0x10000u,    0xFFFFu};
            uint32_t v = rngBelow(3) == 0 ? (uint32_t)n - rngBelow(8) : vals[rngBelow(8)];
            set32(buf, at, v);
        } else if (kind == 1 && at + 2 <= n) { /* an interesting 16-bit value */
            static const uint32_t vals[] = {0, 1, 2, 3, 0x7FFF, 0x8000, 0xFFFE, 0xFFFF};
            set16(buf, at, vals[rngBelow(8)]);
        } else {
            buf[at] = (uint8_t)(kind == 2 ? buf[at] ^ (1u << rngBelow(8)) : rng());
        }
    }
}

/* A one-off deep run: build with -DFONT_FUZZ_SCALE=50 (the sweep did, see docs/sweeps/M12.3.md). */
#ifndef FONT_FUZZ_SCALE
#define FONT_FUZZ_SCALE 1u
#endif

TEST(fontAdvMutationFuzz) {
    static const struct {
        int which;
        uint32_t iters;
    } plan[] = {
        {FTU_SYNTH_FALLBACK, 4000}, {FTU_SYNTH_GPOS, 2000}, {FTU_SYNTH_GRID, 3000},
        {FTU_SYNTH_SYMBOL, 2000},   {FTU_SANS, 600},        {FTU_MONO, 400},
    };
    rngSeed(0xB0B0F0A7u);
    uint32_t ok = 0, bad = 0;
    for (size_t p = 0; p < sizeof plan / sizeof plan[0]; p++) {
        size_t n;
        const uint8_t *src = ftuFont(plan[p].which, &n);
        ASSERT_TRUE(src != NULL);
        Region rg[48];
        uint32_t nRg = fuzzRegions(src, n, rg, 48);
        ASSERT_TRUE(nRg >= 6);
        uint8_t *buf = malloc(n); /* exact size: ASan catches any read past the end */
        for (uint32_t it = 0; it < plan[p].iters * FONT_FUZZ_SCALE; it++) {
            fuzzOne(buf, src, n, rg, nRg);
            Status st;
            bool good = initAndCheck(buf, n, &st);
            if (!good) {
                fprintf(stderr, "  font %d iter %u: status %d broke an invariant\n", plan[p].which,
                        it, (int)st);
            }
            ASSERT_TRUE(good);
            if (st == STATUS_OK) {
                ok++;
            } else {
                bad++;
            }
            /* and the same bytes truncated at a random point inside the mutated region's table */
            if (it % 8 == 0) {
                size_t cut = rngBelow((uint32_t)n) + 1;
                uint8_t *t = malloc(cut);
                memcpy(t, buf, cut);
                good = initAndCheck(t, cut, &st);
                free(t);
                ASSERT_TRUE(good);
            }
        }
        free(buf);
#ifdef FONT_FUZZ_VERBOSE
        fprintf(stderr, "  font %d: ok %u bad %u (cumulative)\n", plan[p].which, ok, bad);
#endif
    }
    /* the fuzz must actually reach both outcomes, or it is testing nothing */
    ASSERT_TRUE(ok > 1000);
    ASSERT_TRUE(bad > 1000);
}

/* The (3,0) symbol retry applies to U+0000..U+00FF only: U+1041 must not reach U+F041 through
 * 0xF000 | cp. */
TEST(fontAdvSymbolRetryRange) {
    Seg4 segs[2] = {{0xF041, 0xF041, (3u - 0xF041u) & 0xFFFFu, 0}, {0xFFFF, 0xFFFF, 1, 0}};
    Buf sub;
    memset(&sub, 0, sizeof sub);
    putF4(&sub, segs, 2, NULL, 0);
    Buf cm = cmapOne(3, 0, &sub);
    FontSpec s = specDefault(cm.d, cm.n);
    size_t n;
    uint8_t *d = buildFont(&s, &n, NULL);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    ASSERT_TRUE(f.cmapSymbol);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 0x41), 3);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 0xF041), 3);
    static const uint32_t no[] = {0x141, 0x1041, 0xF141, 0x10041, 0x1F041, 0x10F041};
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++) {
        ASSERT_EQ((int)gfxFontGlyphIndex(&f, no[i]), 0);
    }
    free(d);
    free(cm.d);
    free(sub.d);
}
