/* Host tests for libs/gfx's sfnt/cmap/metrics layer (M12.3, D-150..D-152). Oracles: the values in
 * the tests/data/font oracle files, produced by tests/data/font/gen.py (synthetic fonts by
 * construction and an independent Python parse of the shipped fonts). The rest are adversarial:
 * every malformed header, table and cmap shape must give the documented Status, and a truncation
 * sweep runs the parser over every prefix of every fixture under ASan/UBSan. */
#include "compress/compress.h"
#include "font_testutil.h"
#include "framework/test.h"
#include "gfx/gfx-font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fontByName(const char *n) {
    if (strcmp(n, "fallback") == 0) {
        return FTU_SYNTH_FALLBACK;
    }
    if (strcmp(n, "gpos") == 0) {
        return FTU_SYNTH_GPOS;
    }
    if (strcmp(n, "grid") == 0) {
        return FTU_SYNTH_GRID;
    }
    if (strcmp(n, "sans") == 0) {
        return FTU_SANS;
    }
    if (strcmp(n, "mono") == 0) {
        return FTU_MONO;
    }
    return -1;
}

/* Initializes (once) and returns the parsed font, or NULL. */
static const GfxFont *parsed(int which) {
    static GfxFont fonts[FTU_COUNT];
    static int state[FTU_COUNT]; /* 0 untried, 1 ok, 2 failed */
    if (state[which] == 0) {
        size_t n = 0;
        const uint8_t *d = ftuFont(which, &n);
        state[which] = d != NULL && gfxFontInit(&fonts[which], d, n) == STATUS_OK ? 1 : 2;
    }
    return state[which] == 1 ? &fonts[which] : NULL;
}

TEST(fontDataFilesMatchOracle) {
    const char *text = ftuOracle("liberation.oracle");
    ASSERT_TRUE(text != NULL);
    size_t pos = 0;
    char line[256];
    int seen = 0;
    while (ftuNextLine(text, &pos, line, sizeof line)) {
        char kind[16], name[16];
        unsigned long size, crc;
        if (sscanf(line, "%15s %15s %lu %lx", kind, name, &size, &crc) != 4 ||
            strcmp(kind, "file") != 0) {
            continue;
        }
        size_t n = 0;
        const uint8_t *d = ftuFont(fontByName(name), &n);
        ASSERT_TRUE(d != NULL);
        ASSERT_EQ(n, (size_t)size);
        ASSERT_EQ((unsigned long)compressCrc32(0, d, n), crc);
        seen++;
    }
    ASSERT_EQ(seen, 2);
}

/* metrics/cmap/adv lines of one oracle file, checked against the parsed fonts. */
static void checkOracle(const char *file, int *counts) {
    const char *text = ftuOracle(file);
    ASSERT_TRUE(text != NULL);
    size_t pos = 0;
    char line[8192];
    while (ftuNextLine(text, &pos, line, sizeof line)) {
        char kind[16], name[16];
        if (sscanf(line, "%15s %15s", kind, name) != 2 ||
            (strcmp(kind, "metrics") != 0 && strcmp(kind, "cmap") != 0 &&
             strcmp(kind, "adv") != 0)) {
            continue;
        }
        int id = fontByName(name);
        ASSERT_TRUE(id >= 0);
        const GfxFont *f = parsed(id);
        ASSERT_TRUE(f != NULL);
        if (strcmp(kind, "metrics") == 0) {
            int upem, asc, desc, gap, ng, nhm;
            ASSERT_EQ(
                sscanf(line, "%*s %*s %d %d %d %d %d %d", &upem, &asc, &desc, &gap, &ng, &nhm), 6);
            ASSERT_EQ((int)f->unitsPerEm, upem);
            ASSERT_EQ((int)f->ascender, asc);
            ASSERT_EQ((int)f->descender, desc);
            ASSERT_EQ((int)f->lineGap, gap);
            ASSERT_EQ((int)f->numGlyphs, ng);
            ASSERT_EQ((int)f->numHMetrics, nhm);
            counts[0]++;
        } else if (strcmp(kind, "cmap") == 0) {
            unsigned long cp;
            int gid;
            ASSERT_EQ(sscanf(line, "%*s %*s %lu %d", &cp, &gid), 2);
            ASSERT_EQ((int)gfxFontGlyphIndex(f, (uint32_t)cp), gid);
            counts[1]++;
        } else if (strcmp(kind, "adv") == 0) {
            int gid, adv;
            ASSERT_EQ(sscanf(line, "%*s %*s %d %d", &gid, &adv), 2);
            ASSERT_EQ((int)gfxFontAdvanceUnits(f, (uint16_t)gid), adv);
            counts[2]++;
        }
    }
}

TEST(fontLiberationMatchesIndependentParse) {
    int counts[3] = {0, 0, 0};
    checkOracle("liberation.oracle", counts);
    ASSERT_EQ(counts[0], 2);
    ASSERT_TRUE(counts[1] > 2000); /* two fonts x ~1500 codepoints */
    ASSERT_TRUE(counts[2] > 4000); /* an advance for every glyph of both fonts */
    /* metrics are the D-152 rule: Liberation sets no USE_TYPO_METRICS, so hhea */
    ASSERT_EQ((int)parsed(FTU_SANS)->ascender, 1854);
}

TEST(fontSynthMatchesConstruction) {
    int counts[3] = {0, 0, 0};
    checkOracle("synth.oracle", counts);
    ASSERT_EQ(counts[0], 3);
    ASSERT_TRUE(counts[1] > 100);
    ASSERT_TRUE(counts[2] > 100);
}

TEST(fontCmapChoiceAndLookups) {
    const GfxFont *fb = parsed(FTU_SYNTH_FALLBACK);
    ASSERT_TRUE(fb != NULL);
    /* (3,10) format 12 outranks (3,1) format 4; U+E000 exists only in the format 12 subtable */
    ASSERT_EQ((int)fb->cmapFormat, 12);
    ASSERT_EQ((int)gfxFontGlyphIndex(fb, 0xE000), 18);
    ASSERT_EQ((int)gfxFontGlyphIndex(fb, 0xE003), 21);
    ASSERT_EQ((int)gfxFontGlyphIndex(fb, 0xE004), 0);
    ASSERT_EQ((int)gfxFontGlyphIndex(fb, 0x4E0F), 16);
    ASSERT_EQ((int)gfxFontGlyphIndex(fb, 0x4E10), 0);
    ASSERT_EQ((int)gfxFontGlyphIndex(fb, 0x3002), 17);
    /* typo metrics (USE_TYPO_METRICS set) win over hhea 800/-200 */
    ASSERT_EQ((int)fb->ascender, 700);
    ASSERT_EQ((int)fb->descender, -300);
    ASSERT_EQ((int)fb->lineGap, 100);
    ASSERT_TRUE(!fb->locaLong);
    /* the hmtx tail rule: the last three glyphs reuse the advance of glyph numHMetrics-1 */
    ASSERT_EQ((int)fb->numHMetrics, (int)fb->numGlyphs - 3);
    ASSERT_EQ((int)gfxFontAdvanceUnits(fb, (uint16_t)(fb->numGlyphs - 1)), 1000);
    ASSERT_EQ((int)gfxFontAdvanceUnits(fb, fb->numGlyphs), 0);
    ASSERT_EQ((int)gfxFontAdvanceUnits(fb, 0xFFFF), 0);

    const GfxFont *gr = parsed(FTU_SYNTH_GRID); /* format 4 with an idRangeOffset segment */
    ASSERT_TRUE(gr != NULL);
    ASSERT_EQ((int)gr->cmapFormat, 4);
    ASSERT_TRUE(gfxFontGlyphIndex(gr, 'A') != 0);
    ASSERT_TRUE(gfxFontGlyphIndex(gr, 'A') != gfxFontGlyphIndex(gr, 'B'));
    ASSERT_EQ((int)gfxFontAdvanceUnits(gr, gfxFontGlyphIndex(gr, 'A')), 500);
    ASSERT_EQ((int)gfxFontAdvanceUnits(gr, gfxFontGlyphIndex(gr, 0x4E00)), 1000);
    ASSERT_EQ((int)gfxFontGlyphIndex(gr, 0x7F), 0);
    ASSERT_EQ((int)gfxFontGlyphIndex(gr, 0x1F), 0);
    ASSERT_EQ((int)gfxFontGlyphIndex(gr, 0x10000), 0);

    /* out-of-range codepoints */
    ASSERT_EQ((int)gfxFontGlyphIndex(fb, 0x110000), 0);
    ASSERT_EQ((int)gfxFontGlyphIndex(fb, 0xFFFFFFFFu), 0);
    ASSERT_EQ((int)gfxFontGlyphIndex(NULL, 'A'), 0);
    GfxFont zero;
    memset(&zero, 0, sizeof zero);
    ASSERT_EQ((int)gfxFontGlyphIndex(&zero, 'A'), 0);
    ASSERT_EQ((int)gfxFontAdvanceUnits(&zero, 0), 0);
}

TEST(fontSymbolCmapRetriesF000) {
    const GfxFont *sy = parsed(FTU_SYNTH_SYMBOL);
    ASSERT_TRUE(sy != NULL);
    ASSERT_TRUE(sy->cmapSymbol);
    ASSERT_EQ((int)gfxFontGlyphIndex(sy, 0x41), 1);
    ASSERT_EQ((int)gfxFontGlyphIndex(sy, 0xF041), 1);
    ASSERT_EQ((int)gfxFontGlyphIndex(sy, 0x42), 0);
    ASSERT_EQ((int)gfxFontGlyphIndex(sy, 0x100), 0); /* no retry above 0xFF */
    ASSERT_TRUE(!parsed(FTU_SYNTH_FALLBACK)->cmapSymbol);
}

TEST(fontScaleAndMetrics) {
    const GfxFont *f = parsed(FTU_SANS); /* upem 2048 */
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ(gfxFontScaleQ6(f, 1, 1024), 1); /* 0.5 rounds away from zero */
    ASSERT_EQ(gfxFontScaleQ6(f, -1, 1024), -1);
    ASSERT_EQ(gfxFontScaleQ6(f, 3, 1024), 2); /* 1.5 */
    ASSERT_EQ(gfxFontScaleQ6(f, -3, 1024), -2);
    ASSERT_EQ(gfxFontScaleQ6(f, 0, 1024), 0);
    ASSERT_EQ(gfxFontScaleQ6(f, 2048, 64 * 20), 64 * 20); /* one em is the size */
    ASSERT_EQ(gfxFontScaleQ6(f, INT32_MAX, GFX_FONT_MAX_SIZE_Q6), INT32_MAX); /* saturates */
    ASSERT_EQ(gfxFontScaleQ6(f, INT32_MIN, GFX_FONT_MAX_SIZE_Q6), INT32_MIN);

    GfxFontMetricsPx m;
    ASSERT_EQ(gfxFontMetrics(f, 16 * 64, &m), STATUS_OK);
    ASSERT_EQ(m.ascent, 15); /* 1854/2048*16 = 14.48 rounds up */
    ASSERT_EQ(m.descent, 4); /* 434/2048*16 = 3.39 rounds up */
    ASSERT_EQ(m.lineGap, 1); /* 67/2048*16 = 0.52 rounds */
    ASSERT_EQ(m.lineHeight, 20);
    ASSERT_EQ(gfxFontMetrics(f, 63, &m), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontMetrics(f, GFX_FONT_MAX_SIZE_Q6 + 1, &m), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontMetrics(f, GFX_FONT_MIN_SIZE_Q6, &m), STATUS_OK);
    ASSERT_TRUE(m.lineHeight >= 1);
    ASSERT_EQ(gfxFontMetrics(f, GFX_FONT_MAX_SIZE_Q6, &m), STATUS_OK);
    ASSERT_EQ(gfxFontMetrics(NULL, 1024, &m), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontMetrics(f, 1024, NULL), STATUS_ERR_INVALID);
    const GfxFont *fb = parsed(FTU_SYNTH_FALLBACK); /* typo 700/-300/100, upem 1000 */
    ASSERT_EQ(gfxFontMetrics(fb, 10 * 64, &m), STATUS_OK);
    ASSERT_EQ(m.ascent, 7);
    ASSERT_EQ(m.descent, 3);
    ASSERT_EQ(m.lineGap, 1);
    ASSERT_EQ(m.lineHeight, 11);
}

/* ---- adversarial inputs ------------------------------------------------------------------ */

typedef struct {
    uint8_t *d;
    size_t n;
} Copy;

static Copy copyOf(int which) {
    Copy c;
    const uint8_t *src = ftuFont(which, &c.n);
    c.d = malloc(c.n);
    memcpy(c.d, src, c.n);
    return c;
}

/* Directory record index of a tag, or -1. */
static int recOf(const Copy *c, const char *tag) {
    uint32_t nt = ftuGet16(c->d, 4);
    for (uint32_t i = 0; i < nt; i++) {
        if (memcmp(c->d + 12 + 16 * i, tag, 4) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t tOff(const Copy *c, const char *tag) {
    return ftuTable(c->d, c->n, tag, NULL);
}

static void setTag(Copy *c, const char *tag, const char *to) {
    int r = recOf(c, tag);
    memcpy(c->d + 12 + 16 * r, to, 4);
}

static void setLen(Copy *c, const char *tag, uint32_t len) {
    ftuPut32(c->d, 12 + 16 * (uint32_t)recOf(c, tag) + 12, len);
}

static uint32_t getLen(const Copy *c, const char *tag) {
    uint32_t len = 0;
    ftuTable(c->d, c->n, tag, &len);
    return len;
}

static Status initCopy(const Copy *c, GfxFont *f) {
    return gfxFontInit(f, c->d, c->n);
}

TEST(fontInitRejectsBadContainers) {
    GfxFont f;
    size_t n;
    const uint8_t *d = ftuFont(FTU_SYNTH_FALLBACK, &n);
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    ASSERT_EQ(gfxFontInit(&f, NULL, n), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontInit(&f, d, 0), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontInit(&f, d, 11), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontInit(NULL, d, n), STATUS_ERR_INVALID);
    ASSERT_TRUE(f.data == NULL); /* zeroed on failure */
    ASSERT_EQ(gfxFontInit(&f, d, (size_t)GFX_FONT_MAX_FILE_BYTES + 1), STATUS_ERR_UNSUPPORTED);
    ASSERT_TRUE(f.data == NULL);

    static const struct {
        const char *tag;
        Status want;
    } magics[] = {
        {"OTTO", STATUS_ERR_UNSUPPORTED},     {"ttcf", STATUS_ERR_UNSUPPORTED},
        {"wOFF", STATUS_ERR_UNSUPPORTED},     {"wOF2", STATUS_ERR_UNSUPPORTED},
        {"\0\2\0\0", STATUS_ERR_UNSUPPORTED}, {"true", STATUS_OK},
    };
    for (size_t i = 0; i < sizeof magics / sizeof magics[0]; i++) {
        Copy c = copyOf(FTU_SYNTH_FALLBACK);
        memcpy(c.d, magics[i].tag, 4);
        ASSERT_EQ(initCopy(&c, &f), magics[i].want);
        free(c.d);
    }
    Copy c = copyOf(FTU_SYNTH_FALLBACK);
    ftuPut16(c.d, 4, 0xFFFF); /* directory larger than the file */
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID);
    ftuPut16(c.d, 4, 0);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID); /* no tables at all */
    free(c.d);
}

TEST(fontInitRejectsBadTables) {
    GfxFont f;
    Copy c;

    c = copyOf(FTU_SYNTH_FALLBACK); /* a record whose off+len wraps u32 */
    setLen(&c, "head", 0xFFFFFFFFu);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID);
    free(c.d);
    c = copyOf(FTU_SYNTH_FALLBACK); /* a table starting past the end */
    ftuPut32(c.d, 12 + 16 * (uint32_t)recOf(&c, "head") + 8, 0xFFFFFFF0u);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID);
    free(c.d);

    /* head */
    c = copyOf(FTU_SYNTH_FALLBACK);
    uint32_t head = tOff(&c, "head");
    ftuPut32(c.d, head + 12, 0x5F0F3CF4u);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID); /* bad magic */
    ftuPut32(c.d, head + 12, 0x5F0F3CF5u);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    static const uint16_t badUpem[] = {0, 1, 15, 16385, 0xFFFF};
    for (size_t i = 0; i < sizeof badUpem / sizeof badUpem[0]; i++) {
        ftuPut16(c.d, head + 18, badUpem[i]);
        ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID);
    }
    ftuPut16(c.d, head + 18, 16);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ftuPut16(c.d, head + 18, 16384);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ftuPut16(c.d, head + 18, 1000);
    ftuPut16(c.d, head + 50, 2); /* indexToLocFormat */
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID);
    ftuPut16(c.d, head + 50, 0);
    ftuPut16(c.d, head + 52, 1); /* glyphDataFormat */
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_UNSUPPORTED);
    ftuPut16(c.d, head + 52, 0);
    setLen(&c, "head", 53);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID);
    free(c.d);

    /* maxp, hhea, hmtx, loca sizes and counts */
    c = copyOf(FTU_SYNTH_FALLBACK);
    uint32_t maxp = tOff(&c, "maxp"), hhea = tOff(&c, "hhea");
    uint32_t ng = ftuGet16(c.d, maxp + 4);
    ftuPut16(c.d, maxp + 4, 0);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID); /* numGlyphs 0 */
    ftuPut16(c.d, maxp + 4, (uint32_t)ng);
    setLen(&c, "maxp", 5);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID);
    setLen(&c, "maxp", 32);
    ftuPut16(c.d, hhea + 34, 0);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID); /* numberOfHMetrics 0 */
    ftuPut16(c.d, hhea + 34, 0xFFFF);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID); /* clamped to numGlyphs: hmtx is too short */
    ftuPut16(c.d, hhea + 34, ng - 3);
    setLen(&c, "hhea", 35);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID);
    setLen(&c, "hhea", 36);
    uint32_t hm = getLen(&c, "hmtx");
    setLen(&c, "hmtx", 4 * (ng - 3) - 1);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID);
    setLen(&c, "hmtx", 4 * (ng - 3)); /* the lsb tail is not required */
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    setLen(&c, "hmtx", hm);
    uint32_t loca = getLen(&c, "loca");
    setLen(&c, "loca", loca - 1);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID);
    setLen(&c, "loca", loca);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    free(c.d);

    /* missing tables */
    static const struct {
        const char *tag, *to;
        Status want;
    } missing[] = {
        {"glyf", "glyX", STATUS_ERR_INVALID},
        {"loca", "locX", STATUS_ERR_INVALID},
        {"glyf", "CFF ", STATUS_ERR_UNSUPPORTED},
        {"glyf", "CFF2", STATUS_ERR_UNSUPPORTED},
        {"head", "heaX", STATUS_ERR_INVALID},
        {"hhea", "hheX", STATUS_ERR_INVALID},
        {"maxp", "maxX", STATUS_ERR_INVALID},
        {"hmtx", "hmtX", STATUS_ERR_INVALID},
        {"cmap", "cmaX", STATUS_ERR_INVALID},
        {"kern", "kerX", STATUS_OK}, /* optional */
        {"OS/2", "OS/X", STATUS_OK}, /* optional */
    };
    for (size_t i = 0; i < sizeof missing / sizeof missing[0]; i++) {
        c = copyOf(FTU_SYNTH_FALLBACK);
        setTag(&c, missing[i].tag, missing[i].to);
        ASSERT_EQ(initCopy(&c, &f), missing[i].want);
        free(c.d);
    }

    /* without OS/2 the hhea metrics apply; the fallback font's hhea is 800/-200/0 */
    c = copyOf(FTU_SYNTH_FALLBACK);
    setTag(&c, "OS/2", "OS/X");
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)f.ascender, 800);
    ASSERT_EQ((int)f.descender, -200);
    ASSERT_EQ((int)f.lineGap, 0);
    /* hhea all zero and no OS/2: the head box (yMax 900, yMin 0) */
    uint32_t h2 = tOff(&c, "hhea");
    ftuPut16(c.d, h2 + 4, 0);
    ftuPut16(c.d, h2 + 6, 0);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)f.ascender, 900);
    ASSERT_EQ((int)f.descender, 0);
    /* a positive descender and a negative gap are normalized */
    ftuPut16(c.d, h2 + 4, 700);
    ftuPut16(c.d, h2 + 6, 250);
    ftuPut16(c.d, h2 + 8, (uint32_t)-30 & 0xFFFFu);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)f.descender, -250);
    ASSERT_EQ((int)f.lineGap, 0);
    ftuPut16(c.d, h2 + 4, (uint32_t)-5 & 0xFFFFu); /* a negative ascender becomes 0 */
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)f.ascender, 0);
    free(c.d);

    /* a duplicate record: the first one wins (a bogus second head is ignored) */
    c = copyOf(FTU_SYNTH_FALLBACK);
    ASSERT_TRUE(recOf(&c, "kern") > recOf(&c, "head"));
    ftuPut32(c.d, 12 + 16 * (uint32_t)recOf(&c, "kern") + 8, 0xFFFFFF00u);
    setTag(&c, "kern", "head");
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    free(c.d);

    /* numberOfHMetrics above numGlyphs clamps to numGlyphs (the grid font has a full hmtx) */
    c = copyOf(FTU_SYNTH_GRID);
    uint32_t gh = tOff(&c, "hhea"), gm = tOff(&c, "maxp");
    uint32_t gng = ftuGet16(c.d, gm + 4);
    ASSERT_EQ((int)ftuGet16(c.d, gh + 34), (int)gng);
    ftuPut16(c.d, gh + 34, gng + 5);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)f.numHMetrics, (int)gng);
    free(c.d);
}

TEST(fontCmapAdversarial) {
    GfxFont f;
    Copy c = copyOf(FTU_SYNTH_FALLBACK);
    uint32_t cm = tOff(&c, "cmap");
    /* records: 0 = (3,1) format 4, 1 = (3,10) format 12 */
    ASSERT_EQ((int)ftuGet16(c.d, cm + 4), 3);
    ASSERT_EQ((int)ftuGet16(c.d, cm + 6), 1);
    ASSERT_EQ((int)ftuGet16(c.d, cm + 12), 3);
    ASSERT_EQ((int)ftuGet16(c.d, cm + 14), 10);
    uint32_t sub4 = cm + ftuGet32(c.d, cm + 8), sub12 = cm + ftuGet32(c.d, cm + 16);

    /* rank fallbacks: kill the format 12 record and format 4 is used (U+E000 unmapped there) */
    ftuPut16(c.d, cm + 14, 2);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)f.cmapFormat, 4);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 0xE000), 0);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 0x4E00), 1);
    /* (0,3) format 4 is usable, (0,4) format 12 outranks (3,1) format 4 */
    ftuPut16(c.d, cm + 4, 0);
    ftuPut16(c.d, cm + 6, 3);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)f.cmapFormat, 4);
    ftuPut16(c.d, cm + 12, 0);
    ftuPut16(c.d, cm + 14, 4);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)f.cmapFormat, 12);
    /* no Unicode subtable: (1,0) only */
    ftuPut16(c.d, cm + 4, 1);
    ftuPut16(c.d, cm + 6, 0);
    ftuPut16(c.d, cm + 12, 1);
    ftuPut16(c.d, cm + 14, 0);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_UNSUPPORTED);
    /* an unsupported format (6) in a Unicode record: nothing usable */
    ftuPut16(c.d, cm + 4, 3);
    ftuPut16(c.d, cm + 6, 1);
    ftuPut16(c.d, sub4, 6);
    ftuPut16(c.d, cm + 12, 1);
    ftuPut16(c.d, cm + 14, 0);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_UNSUPPORTED);
    free(c.d);

    /* format 4 shapes (make format 4 the winner first) */
    c = copyOf(FTU_SYNTH_FALLBACK);
    cm = tOff(&c, "cmap");
    sub4 = cm + ftuGet32(c.d, cm + 8);
    sub12 = cm + ftuGet32(c.d, cm + 16);
    ftuPut16(c.d, cm + 14, 2);
    ftuPut16(c.d, sub4 + 6, 3); /* odd segCountX2 */
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID);
    ftuPut16(c.d, sub4 + 6, 0);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID);
    ftuPut16(c.d, sub4 + 6, 0xFFFE); /* arrays past the end of the table */
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID);
    free(c.d);

    /* format 12: a huge group count, and start > end */
    c = copyOf(FTU_SYNTH_FALLBACK);
    cm = tOff(&c, "cmap");
    sub12 = cm + ftuGet32(c.d, cm + 16);
    uint32_t groups = ftuGet32(c.d, sub12 + 12);
    ASSERT_EQ((int)groups, 3);
    ftuPut32(c.d, sub12 + 12, 0x0FFFFFFFu);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID);
    ftuPut32(c.d, sub12 + 12, 0xFFFFFFFFu);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID);
    ftuPut32(c.d, sub12 + 12, groups);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ftuPut32(c.d, sub12 + 16, 0x3005); /* group 0: start 0x3005 > end 0x3002 */
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 0x3006), 0);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 0x3005), 0);
    /* a group whose start glyph + offset passes 0xFFFF, and one past numGlyphs */
    ftuPut32(c.d, sub12 + 16, 0x3002);
    ftuPut32(c.d, sub12 + 16 + 12 + 8, 0xFFFFFFFFu); /* group 1: startGlyph 0xFFFFFFFF */
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 0x4E00), 0);
    ftuPut32(c.d, sub12 + 16 + 12 + 8, 0x0000FFFFu);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 0x4E00), 0); /* glyph 0xFFFF >= numGlyphs */
    /* a record offset past the table is not a candidate */
    ftuPut32(c.d, cm + 16, 0xFFFFFFF0u);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)f.cmapFormat, 4);
    free(c.d);

    /* a table too short for its own record count */
    c = copyOf(FTU_SYNTH_FALLBACK);
    setLen(&c, "cmap", 3);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID);
    setLen(&c, "cmap", 10);
    ASSERT_EQ(initCopy(&c, &f), STATUS_ERR_INVALID); /* 2 records need 20 bytes */
    free(c.d);

    /* idRangeOffset pointing past the cmap end, and a delta that lands on a bad glyph */
    c = copyOf(FTU_SYNTH_GRID);
    cm = tOff(&c, "cmap");
    sub4 = cm + ftuGet32(c.d, cm + 8); /* the grid font has a single subtable */
    uint32_t segX2 = ftuGet16(c.d, sub4 + 6);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_TRUE(gfxFontGlyphIndex(&f, 'A') != 0);
    uint32_t rangeArr = sub4 + 16 + 3 * segX2;
    ftuPut16(c.d, rangeArr, 0xFFFE); /* segment 0 (ASCII) */
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 'A'), 0);
    ftuPut16(c.d, rangeArr, 1); /* odd offset: reads across, but stays in bounds */
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    (void)gfxFontGlyphIndex(&f, 'A');
    (void)gfxFontGlyphIndex(&f, 0x7E);
    /* the segment for U+2010: idDelta so that the glyph is >= numGlyphs */
    uint32_t deltaArr = sub4 + 16 + 2 * segX2, seg = 0;
    while (ftuGet16(c.d, sub4 + 14 + 2 * seg) != 0x2010) {
        seg++;
    }
    uint32_t good = ftuGet16(c.d, deltaArr + 2 * seg);
    ftuPut16(c.d, deltaArr + 2 * seg, (0xFFF0u - 0x2010u) & 0xFFFFu);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)gfxFontGlyphIndex(&f, 0x2010), 0);
    ftuPut16(c.d, deltaArr + 2 * seg, good);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_TRUE(gfxFontGlyphIndex(&f, 0x2010) != 0);
    /* unsorted endCodes must not crash the binary search (results are just wrong) */
    uint32_t endArr = sub4 + 14;
    for (uint32_t i = 0; i < segX2 / 2; i++) {
        ftuPut16(c.d, endArr + 2 * i, (i * 40503u) & 0xFFFFu);
    }
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    for (uint32_t cp = 0; cp < 0x10000; cp += 97) {
        (void)gfxFontGlyphIndex(&f, cp);
    }
    free(c.d);
}

/* Every prefix of every fixture: init must fail cleanly or produce a font whose lookups are safe.
 */
static void exercise(const GfxFont *f) {
    for (uint32_t cp = 0; cp < 0x3200; cp += 7) {
        uint16_t g = gfxFontGlyphIndex(f, cp);
        (void)gfxFontAdvanceUnits(f, g);
    }
    (void)gfxFontGlyphIndex(f, 0x4E00);
    (void)gfxFontGlyphIndex(f, 0xF041);
    (void)gfxFontGlyphIndex(f, 0x10FFFF);
    (void)gfxFontAdvanceUnits(f, 0xFFFF);
    GfxFontMetricsPx m;
    (void)gfxFontMetrics(f, 1024, &m);
}

TEST(fontTruncationSweep) {
    static const int synth[] = {FTU_SYNTH_FALLBACK, FTU_SYNTH_GPOS, FTU_SYNTH_GRID,
                                FTU_SYNTH_SYMBOL};
    int ok = 0, bad = 0;
    for (size_t k = 0; k < sizeof synth / sizeof synth[0]; k++) {
        size_t n;
        const uint8_t *d = ftuFont(synth[k], &n);
        uint8_t *tmp = malloc(n);
        for (size_t len = 0; len <= n; len++) {
            memcpy(tmp, d, len); /* exactly `len` bytes: ASan flags any read past them */
            uint8_t *exact = malloc(len != 0 ? len : 1);
            memcpy(exact, tmp, len);
            GfxFont f;
            Status st = gfxFontInit(&f, exact, len);
            if (st == STATUS_OK) {
                exercise(&f);
                ok++;
            } else {
                ASSERT_TRUE(st == STATUS_ERR_INVALID || st == STATUS_ERR_UNSUPPORTED);
                bad++;
            }
            free(exact);
        }
        free(tmp);
    }
    ASSERT_TRUE(bad > 100);
    ASSERT_TRUE(ok >= 4); /* at least the whole files */

    /* Liberation Sans: every table boundary +-{0,1,2} and 256 pseudo-random lengths */
    size_t n;
    const uint8_t *d = ftuFont(FTU_SANS, &n);
    uint32_t nt = ftuGet16(d, 4);
    for (uint32_t i = 0; i < nt + 256; i++) {
        size_t base = i < nt ? ftuGet32(d, 12 + 16 * i + 8) : (size_t)(i * 2654435761u) % n;
        for (int delta = -2; delta <= (i < nt ? 2 : 0); delta++) {
            size_t len = (size_t)((long)base + delta);
            if (len > n) {
                continue;
            }
            uint8_t *exact = malloc(len != 0 ? len : 1);
            memcpy(exact, d, len);
            GfxFont f;
            Status st = gfxFontInit(&f, exact, len);
            if (st == STATUS_OK) {
                exercise(&f);
            } else {
                ASSERT_TRUE(st == STATUS_ERR_INVALID || st == STATUS_ERR_UNSUPPORTED);
            }
            free(exact);
        }
    }
}

/* D-152 fallbacks: an all-zero OS/2 typo set and a positive head yMin (sweep S4 leads). */
TEST(fontMetricsHeadBoxFallback) {
    GfxFont f;
    Copy c = copyOf(FTU_SYNTH_FALLBACK);
    uint32_t hhea = tOff(&c, "hhea"), os2 = tOff(&c, "OS/2"), head = tOff(&c, "head");
    ftuPut16(c.d, hhea + 4, 0);
    ftuPut16(c.d, hhea + 6, 0);
    ftuPut16(c.d, os2 + 62, 0); /* no USE_TYPO_METRICS */
    ftuPut16(c.d, os2 + 68, 0);
    ftuPut16(c.d, os2 + 70, 0);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)f.ascender, 900); /* the head box, not a 0/0 typo pair */
    ASSERT_EQ((int)f.descender, 0);
    ftuPut16(c.d, head + 38, 50); /* yMin above the baseline */
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)f.descender, 0);
    ftuPut16(c.d, head + 38, (uint32_t)-120 & 0xFFFFu);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)f.descender, -120);
    ftuPut16(c.d, os2 + 70, (uint32_t)-250 & 0xFFFFu); /* a nonzero typo descender counts */
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)f.descender, -250);
    free(c.d);
    GfxFont zero;
    memset(&zero, 0, sizeof zero);
    zero.data = (const uint8_t *)"x";
    GfxFontMetricsPx m;
    ASSERT_EQ(gfxFontMetrics(&zero, 1024, &m), STATUS_ERR_INVALID); /* unitsPerEm == 0 */
}
