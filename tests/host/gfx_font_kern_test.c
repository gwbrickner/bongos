/* Host tests for libs/gfx's pair kerning (M12.3, D-154): GPOS PairPos (formats 1 and 2, extension
 * lookups, coverage/ClassDef formats, lookup sums and first-subtable-wins) and the legacy 'kern'
 * table (Microsoft and Apple headers). Oracles: synth-gpos.ttf's expected values by construction,
 * and an independent Python GPOS reader over Liberation Sans (every ASCII pair). The rest is
 * adversarial: patched GPOS structures, truncation of the table at every length, and seeded
 * mutation fuzz of the kerning tables, all under ASan/UBSan. */
#include "font_testutil.h"
#include "framework/test.h"
#include "gfx/gfx-font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const GfxFont *fontOf(int which) {
    static GfxFont fonts[FTU_COUNT];
    static int state[FTU_COUNT];
    if (state[which] == 0) {
        size_t n = 0;
        const uint8_t *d = ftuFont(which, &n);
        state[which] = d != NULL && gfxFontInit(&fonts[which], d, n) == STATUS_OK ? 1 : 2;
    }
    return state[which] == 1 ? &fonts[which] : NULL;
}

static int idByName(const char *n) {
    if (strcmp(n, "fallback") == 0) {
        return FTU_SYNTH_FALLBACK;
    }
    if (strcmp(n, "gpos") == 0) {
        return FTU_SYNTH_GPOS;
    }
    if (strcmp(n, "sans") == 0) {
        return FTU_SANS;
    }
    if (strcmp(n, "mono") == 0) {
        return FTU_MONO;
    }
    return -1;
}

TEST(fontKernSynthGposMatchesConstruction) {
    const GfxFont *f = fontOf(FTU_SYNTH_GPOS);
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ((int)gfxFontKernSource(f), (int)GFX_FONT_KERN_GPOS);
    /* glyph order: .notdef A V T o W a Y */
    enum { A = 1, V, T, O, W, Aa, Y };
    ASSERT_EQ(gfxFontKernUnits(f, A, V),
              -110); /* L0 ext -80 + L1 class (1,2) -30; kern table +500 unused */
    ASSERT_EQ(gfxFontKernUnits(f, T, O), -180); /* L0 -120 + L1 (2,1) -60 */
    ASSERT_EQ(gfxFontKernUnits(f, T, Aa), -60); /* only the class pair */
    ASSERT_EQ(gfxFontKernUnits(f, W, Aa), -40); /* W is outside sub0's coverage: sub1 applies */
    ASSERT_EQ(gfxFontKernUnits(f, Y, O), -50);  /* value record with a device-offset field */
    ASSERT_EQ(gfxFontKernUnits(f, A, O), 0);    /* covered with value 0: ends the lookup */
    ASSERT_EQ(gfxFontKernUnits(f, O, A), 0);
    ASSERT_EQ(gfxFontKernUnits(f, 0, 0), 0);
    ASSERT_EQ(gfxFontKernUnits(f, 0xFFFF, 0xFFFF), 0);
    ASSERT_EQ(gfxFontKernUnits(NULL, A, V), 0);
    ASSERT_EQ((int)gfxFontKernSource(NULL), (int)GFX_FONT_KERN_NONE);
    /* the shadowed sub1 entry (A-V -999) must never leak through */
    ASSERT_TRUE(gfxFontKernUnits(f, A, V) != -999 && gfxFontKernUnits(f, A, V) != -80 - 999);
    ASSERT_EQ((int)f->nKernSub,
              4); /* L0's extension target, L1's two, L2's one; the mark lookup is skipped */
    const char *text = ftuOracle("synth.oracle");
    size_t pos = 0;
    char line[128];
    int seen = 0;
    while (ftuNextLine(text, &pos, line, sizeof line)) {
        char kind[16], name[16];
        int l, r, v;
        if (sscanf(line, "%15s %15s %d %d %d", kind, name, &l, &r, &v) == 5 &&
            strcmp(kind, "kernpair") == 0 && idByName(name) >= 0) {
            const GfxFont *g = fontOf(idByName(name));
            ASSERT_EQ(gfxFontKernUnits(g, (uint16_t)l, (uint16_t)r), v);
            seen++;
        }
    }
    ASSERT_EQ(seen, 7);
}

TEST(fontKernTableUsedWithoutGpos) {
    const GfxFont *f = fontOf(FTU_SYNTH_FALLBACK);
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ((int)gfxFontKernSource(f), (int)GFX_FONT_KERN_TABLE);
    ASSERT_EQ(gfxFontKernUnits(f, 1, 2), -100);
    ASSERT_EQ(gfxFontKernUnits(f, 2, 1), 0);
    ASSERT_EQ(gfxFontKernUnits(f, 1, 1), 0);
    ASSERT_EQ(gfxFontKernUnits(f, 0, 2), 0);
    ASSERT_EQ((int)gfxFontKernSource(fontOf(FTU_SYNTH_GRID)), (int)GFX_FONT_KERN_NONE);
    ASSERT_EQ(gfxFontKernUnits(fontOf(FTU_SYNTH_GRID), 1, 2), 0);
}

TEST(fontKernLiberationMatchesIndependentParse) {
    const GfxFont *sans = fontOf(FTU_SANS);
    const GfxFont *mono = fontOf(FTU_MONO);
    ASSERT_TRUE(sans != NULL && mono != NULL);
    ASSERT_EQ((int)gfxFontKernSource(sans),
              (int)GFX_FONT_KERN_GPOS); /* under latn only, not DFLT */
    ASSERT_EQ((int)gfxFontKernSource(mono), (int)GFX_FONT_KERN_NONE);
    /* expected[l][r] from the oracle's nonzero pairs; everything else must be 0 */
    const char *text = ftuOracle("liberation.oracle");
    ASSERT_TRUE(text != NULL);
    /* key (l << 16 | r) -> value, for ASCII glyph ids only (< 2048) */
    static int32_t expect[2048][2048];
    memset(expect, 0, sizeof expect);
    size_t pos = 0;
    static char line[8192];
    int pairs = 0;
    while (ftuNextLine(text, &pos, line, sizeof line)) {
        char kind[16], name[16];
        int l, r, v;
        if (sscanf(line, "%15s %15s %d %d %d", kind, name, &l, &r, &v) == 5 &&
            strcmp(kind, "kernpair") == 0 && strcmp(name, "sans") == 0) {
            ASSERT_TRUE(l < 2048 && r < 2048);
            expect[l][r] = v;
            pairs++;
        }
    }
    ASSERT_TRUE(pairs > 80);
    int nonzero = 0;
    for (uint32_t a = 0x20; a < 0x7F; a++) {
        for (uint32_t b = 0x20; b < 0x7F; b++) {
            uint16_t l = gfxFontGlyphIndex(sans, a), r = gfxFontGlyphIndex(sans, b);
            int32_t got = gfxFontKernUnits(sans, l, r);
            if (got != expect[l][r]) {
                fprintf(stderr, "  kern %c%c: got %d want %d\n", (int)a, (int)b, (int)got,
                        (int)expect[l][r]);
            }
            ASSERT_EQ(got, expect[l][r]);
            nonzero += got != 0;
            ASSERT_EQ(
                gfxFontKernUnits(mono, gfxFontGlyphIndex(mono, a), gfxFontGlyphIndex(mono, b)), 0);
        }
    }
    ASSERT_EQ(nonzero, pairs);
    /* the classic pairs kern tighter (negative) */
    ASSERT_TRUE(gfxFontKernUnits(sans, gfxFontGlyphIndex(sans, 'A'), gfxFontGlyphIndex(sans, 'V')) <
                0);
    ASSERT_TRUE(gfxFontKernUnits(sans, gfxFontGlyphIndex(sans, 'T'), gfxFontGlyphIndex(sans, 'o')) <
                0);
}

/* ---- patched fonts ------------------------------------------------------------------------ */

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

static int recIdx(const Copy *c, const char *tag) {
    uint32_t nt = ftuGet16(c->d, 4);
    for (uint32_t i = 0; i < nt; i++) {
        if (memcmp(c->d + 12 + 16 * i, tag, 4) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Replaces (by appending) the 'kern' table of `c` with `kern`; the directory points at the copy. */
static void setKern(Copy *c, const uint8_t *kern, size_t n) {
    size_t at = (c->n + 3u) & ~(size_t)3u;
    c->d = realloc(c->d, at + n);
    memset(c->d + c->n, 0, at - c->n);
    memcpy(c->d + at, kern, n);
    int r = recIdx(c, "kern");
    ftuPut32(c->d, 12 + 16 * (uint32_t)r + 8, (uint32_t)at);
    ftuPut32(c->d, 12 + 16 * (uint32_t)r + 12, (uint32_t)n);
    c->n = at + n;
}

static Status initCopy(const Copy *c, GfxFont *f) {
    return gfxFontInit(f, c->d, c->n);
}

static size_t putPairs(uint8_t *p, const uint16_t (*pairs)[3], size_t n) {
    for (size_t i = 0; i < n; i++) {
        ftuPut16(p, (uint32_t)(6 * i), pairs[i][0]);
        ftuPut16(p, (uint32_t)(6 * i + 2), pairs[i][1]);
        ftuPut16(p, (uint32_t)(6 * i + 4), pairs[i][2]);
    }
    return 6 * n;
}

TEST(fontKernTableVariants) {
    static const uint16_t pairs[3][3] = {{1, 2, (uint16_t)-100}, {1, 3, 25}, {4, 5, (uint16_t)-7}};
    uint8_t kern[128];
    GfxFont f;
    Copy c;

    /* Apple header: u32 version 0x00010000, u32 nTables, then u32 length + u16 coverage + u16 tuple
     */
    memset(kern, 0, sizeof kern);
    ftuPut32(kern, 0, 0x00010000u);
    ftuPut32(kern, 4, 1);
    ftuPut32(kern, 8, 16 + 18);
    ftuPut16(kern, 12, 0); /* horizontal, format 0 */
    ftuPut16(kern, 16, 3);
    putPairs(kern + 24, pairs, 3);
    c = copyOf(FTU_SYNTH_FALLBACK);
    setKern(&c, kern, 24 + 18);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)gfxFontKernSource(&f), (int)GFX_FONT_KERN_TABLE);
    ASSERT_EQ(gfxFontKernUnits(&f, 1, 2), -100);
    ASSERT_EQ(gfxFontKernUnits(&f, 1, 3), 25);
    ASSERT_EQ(gfxFontKernUnits(&f, 4, 5), -7);
    ASSERT_EQ(gfxFontKernUnits(&f, 5, 4), 0);
    /* Apple coverage bits: vertical (0x8000), cross-stream (0x4000), variation (0x2000) are ignored
     */
    static const uint16_t bad[3] = {0x8000, 0x4000, 0x2000};
    for (int i = 0; i < 3; i++) {
        ftuPut16(kern, 12, bad[i]);
        c.n = ((size_t)ftuGet32(c.d, 12 + 16 * (uint32_t)recIdx(&c, "kern") + 8));
        setKern(&c, kern, 24 + 18);
        ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
        ASSERT_EQ((int)gfxFontKernSource(&f), (int)GFX_FONT_KERN_NONE);
        ASSERT_EQ(gfxFontKernUnits(&f, 1, 2), 0);
    }
    /* Apple format 2 (coverage low byte) is ignored */
    ftuPut16(kern, 12, 2);
    setKern(&c, kern, 24 + 18);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)gfxFontKernSource(&f), (int)GFX_FONT_KERN_NONE);
    free(c.d);

    /* Microsoft header, coverage variants: version 0, nTables 1, subtable {ver, len, coverage} */
    static const struct {
        uint16_t coverage;
        int wantSource;
    } ms[] = {
        {0x0001, GFX_FONT_KERN_TABLE}, /* horizontal, format 0 */
        {0x0000, GFX_FONT_KERN_NONE},  /* not horizontal */
        {0x0003, GFX_FONT_KERN_NONE},  /* minimum values */
        {0x0005, GFX_FONT_KERN_NONE},  /* cross-stream */
        {0x0201, GFX_FONT_KERN_NONE},  /* format 2 */
        {0x0101, GFX_FONT_KERN_NONE},  /* format 1 */
    };
    for (size_t i = 0; i < sizeof ms / sizeof ms[0]; i++) {
        memset(kern, 0, sizeof kern);
        ftuPut16(kern, 2, 1);
        ftuPut16(kern, 6, 14 + 18);
        ftuPut16(kern, 8, ms[i].coverage);
        ftuPut16(kern, 10, 3);
        putPairs(kern + 18, pairs, 3);
        c = copyOf(FTU_SYNTH_FALLBACK);
        setKern(&c, kern, 18 + 18);
        ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
        ASSERT_EQ((int)gfxFontKernSource(&f), ms[i].wantSource);
        ASSERT_EQ(gfxFontKernUnits(&f, 1, 3), ms[i].wantSource == GFX_FONT_KERN_TABLE ? 25 : 0);
        free(c.d);
    }

    /* an unusable first subtable is skipped and the second one used; nPairs over-claims are clamped
     */
    memset(kern, 0, sizeof kern);
    ftuPut16(kern, 2, 2);
    ftuPut16(kern, 6, 16); /* subtable 1: format 2, length 16 */
    ftuPut16(kern, 8, 0x0201);
    uint32_t s2 = 4 + 16;
    ftuPut16(kern, s2 + 2, 14 + 18); /* subtable 2: format 0 */
    ftuPut16(kern, s2 + 4, 0x0001);
    ftuPut16(kern, s2 + 6, 0xFFFF); /* claims 65535 pairs, has 3 */
    putPairs(kern + s2 + 14, pairs, 3);
    c = copyOf(FTU_SYNTH_FALLBACK);
    setKern(&c, kern, s2 + 14 + 18);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)gfxFontKernSource(&f), (int)GFX_FONT_KERN_TABLE);
    ASSERT_EQ((int)f.kernPairs, 3);
    ASSERT_EQ(gfxFontKernUnits(&f, 4, 5), -7);
    /* a subtable length of 0 (a header that cannot advance) ends the walk instead of looping */
    ftuPut16(kern, 6, 0);
    ftuPut16(kern, 8, 0x0201);
    setKern(&c, kern, s2 + 14 + 18);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)gfxFontKernSource(&f), (int)GFX_FONT_KERN_NONE);
    free(c.d);

    /* garbage headers */
    static const uint8_t junk[] = {0xFF, 0xFF, 0xFF, 0xFF, 1, 2, 3, 4, 5, 6, 7, 8};
    c = copyOf(FTU_SYNTH_FALLBACK);
    setKern(&c, junk, sizeof junk);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)gfxFontKernSource(&f), (int)GFX_FONT_KERN_NONE);
    setKern(&c, junk, 3);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)gfxFontKernSource(&f), (int)GFX_FONT_KERN_NONE);
    free(c.d);
}

/* Absolute offset of the PairPos subtable `sub` of lookup `lookup` of the font's GPOS. */
static uint32_t gposSubtable(const Copy *c, uint32_t lookup, uint32_t sub, uint32_t *lookupOff) {
    uint32_t glen;
    uint32_t g = ftuTable(c->d, c->n, "GPOS", &glen);
    uint32_t ll = g + ftuGet16(c->d, g + 8);
    uint32_t lo = ll + ftuGet16(c->d, ll + 2 + 2 * lookup);
    if (lookupOff != NULL) {
        *lookupOff = lo;
    }
    uint32_t so = lo + ftuGet16(c->d, lo + 6 + 2 * sub);
    if (ftuGet16(c->d, lo) == 9) { /* extension: follow it */
        so += ftuGet32(c->d, so + 4);
    }
    return so;
}

TEST(fontKernGposAdversarialStructures) {
    enum { A = 1, V, T, O, W, Aa, Y };
    GfxFont f;
    Copy c;

    /* unpatched baseline through the same helper */
    c = copyOf(FTU_SYNTH_GPOS);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ(gfxFontKernUnits(&f, W, Aa), -40);
    uint32_t glen;
    uint32_t g = ftuTable(c.d, c.n, "GPOS", &glen);
    free(c.d);

    /* header: bad version, tiny table, zero/out-of-range list offsets => GPOS ignored, and the
     * (never applied before) 'kern' table with A-V = +500 takes over */
    static const struct {
        uint32_t off, val; /* 16-bit fields at g + off */
    } hdrPatch[] = {
        {0, 2},      /* major version 2 */
        {4, 0},      /* scriptList = 0 */
        {6, 0},      /* featureList = 0 */
        {8, 0},      /* lookupList = 0 */
        {4, 0xFFFF}, /* scriptList past the table */
        {6, 0xFFFF}, /* featureList past the table */
        {8, 0xFFFF}, /* lookupList past the table */
    };
    for (size_t i = 0; i < sizeof hdrPatch / sizeof hdrPatch[0]; i++) {
        c = copyOf(FTU_SYNTH_GPOS);
        ftuPut16(c.d, g + hdrPatch[i].off, hdrPatch[i].val);
        ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
        ASSERT_EQ((int)gfxFontKernSource(&f), (int)GFX_FONT_KERN_TABLE);
        ASSERT_EQ(gfxFontKernUnits(&f, A, V), 500);
        free(c.d);
    }
    c = copyOf(FTU_SYNTH_GPOS);
    ftuPut16(c.d, g + 2, 1); /* minor version 1 is accepted */
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)gfxFontKernSource(&f), (int)GFX_FONT_KERN_GPOS);
    ftuPut16(c.d, g + 2, 2); /* minor version 2 is not */
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)gfxFontKernSource(&f), (int)GFX_FONT_KERN_TABLE);
    ftuPut16(c.d, g + 2, 0);
    /* a GPOS shorter than its header */
    int r = recIdx(&c, "GPOS");
    ftuPut32(c.d, 12 + 16 * (uint32_t)r + 12, 9);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)gfxFontKernSource(&f), (int)GFX_FONT_KERN_TABLE);
    free(c.d);

    /* the kern feature renamed: no kern lookups, so the kern table is used */
    c = copyOf(FTU_SYNTH_GPOS);
    for (uint32_t i = 0; i + 4 <= glen; i++) {
        if (memcmp(c.d + g + i, "kern", 4) == 0) {
            c.d[g + i + 1] = 'x';
        }
    }
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ((int)gfxFontKernSource(&f), (int)GFX_FONT_KERN_TABLE);
    ASSERT_EQ(gfxFontKernUnits(&f, A, V), 500);
    free(c.d);

    /* subtable-level patches (offsets found by walking the lookup list) */
    Copy base = copyOf(FTU_SYNTH_GPOS);
    uint32_t lo1, lo0;
    uint32_t l0 = gposSubtable(&base, 0, 0, &lo0);   /* L0 -> extension -> PairPos format 1 */
    uint32_t l1s0 = gposSubtable(&base, 1, 0, &lo1); /* L1 sub0: format 2 */
    uint32_t l1s1 = gposSubtable(&base, 1, 1, NULL); /* L1 sub1: format 1 */
    uint32_t l2 = gposSubtable(&base, 2, 0, NULL);   /* L2: format 1, device-offset value format */
    ASSERT_EQ((int)ftuGet16(base.d, l0), 1);
    ASSERT_EQ((int)ftuGet16(base.d, l1s0), 2);
    ASSERT_EQ((int)ftuGet16(base.d, l1s1), 1);
    ASSERT_EQ((int)ftuGet16(base.d, l2), 1);
    static const struct {
        int which; /* 0 = l0, 1 = l1s0, 2 = l1s1, 3 = l2 */
        uint32_t off, val;
        uint16_t l, r;
        int want;
        const char *what;
    } cases[] = {
        {2, 8, 0, W, Aa, 0, "pairSetCount 0: covered glyph has no pair set"},
        {0, 8, 1, T, O, -60, "L0: pairSetCount 1 with 2 coverage entries: T has no set any more"},
        {0, 2, 0, A, V, -30, "L0 coverage offset 0: subtable invalid"},
        {2, 4, 0x0104, W, Aa, 0, "reserved value-format bits: subtable skipped"},
        {1, 4, 0x0100, T, O, -120, "reserved bits in sub0: skipped (only L0 applies)"},
        {1, 12, 0, T, O, -120, "class1Count 0: no class match"},
        {1, 14, 0, T, O, -120, "class2Count 0: no class match"},
        {1, 12, 0xFFFF, T, O, -180, "class1Count huge: still in range, record in bounds?"},
        {3, 8, 0xFFFF, Y, O, 0, "pairSetCount huge: offsets array does not fit"},
        {2, 12, 0xFFFF, W, Aa, 0, "pairSet offset past the table"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        c = copyOf(FTU_SYNTH_GPOS);
        uint32_t at = (cases[i].which == 0   ? l0
                       : cases[i].which == 1 ? l1s0
                       : cases[i].which == 2 ? l1s1
                                             : l2) +
                      cases[i].off;
        ftuPut16(c.d, at, cases[i].val);
        ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
        int got = (int)gfxFontKernUnits(&f, cases[i].l, cases[i].r);
        if (got != cases[i].want) {
            fprintf(stderr, "  case %zu (%s): got %d want %d\n", i, cases[i].what, got,
                    cases[i].want);
        }
        /* the aim is "safe and deterministic"; the exact values above are what the rules give */
        ASSERT_EQ(got, cases[i].want);
        free(c.d);
    }

    /* coverage / classdef formats: an unknown coverage format never matches; ClassDef format 9 is
     * class 0 */
    c = copyOf(FTU_SYNTH_GPOS);
    uint32_t cov1 = l1s0 + ftuGet16(c.d, l1s0 + 2);
    ftuPut16(c.d, cov1, 3);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ(gfxFontKernUnits(&f, T, O), -120);
    ASSERT_EQ(gfxFontKernUnits(&f, A, V), -80 - 999); /* sub0 no longer shadows sub1's entry */
    ftuPut16(c.d, cov1, 2);
    uint32_t cd1 = l1s0 + ftuGet16(c.d, l1s0 + 8);
    ftuPut16(c.d, cd1, 9);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ(gfxFontKernUnits(&f, T, O), -120); /* class 0 x class 1 = 0 */
    free(c.d);

    /* extension lookups: an offset past the table, a wrong format, a wrong inner type */
    static const struct {
        uint32_t off; /* from the extension subtable */
        uint32_t val;
        int size;
    } ext[] = {{0, 2, 2}, {2, 7, 2}, {4, 0xFFFFFFF0u, 4}, {4, 0, 4}};
    uint32_t e0 = lo0 + ftuGet16(base.d, lo0 + 6);
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        c = copyOf(FTU_SYNTH_GPOS);
        if (ext[i].size == 2) {
            ftuPut16(c.d, e0 + ext[i].off, ext[i].val);
        } else {
            ftuPut32(c.d, e0 + ext[i].off, ext[i].val);
        }
        ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
        (void)gfxFontKernUnits(&f, A, V); /* L0 is gone or garbage: only safety matters */
        (void)gfxFontKernUnits(&f, T, O);
        free(c.d);
    }
    /* L0 skipped entirely (a broken extension): the rest still works */
    c = copyOf(FTU_SYNTH_GPOS);
    ftuPut16(c.d, e0 + 2, 7);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ(gfxFontKernUnits(&f, A, V), -30);
    ASSERT_EQ(gfxFontKernUnits(&f, T, O), -60);
    free(c.d);

    /* lookup indices out of range, lookup type not kern-like */
    uint32_t fl = g + ftuGet16(base.d, g + 6);
    uint32_t feat0 = fl + ftuGet16(base.d, fl + 2 + 4); /* feature 0: kern -> lookups [2, 0] */
    c = copyOf(FTU_SYNTH_GPOS);
    ftuPut16(c.d, feat0 + 4,
             99); /* first index 99 >= lookupCount: dropped; [0] and feature 1 remain */
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ(gfxFontKernUnits(&f, Y, O), 0);
    ASSERT_EQ(gfxFontKernUnits(&f, T, O), -180);
    free(c.d);
    /* type 4 (mark) referenced as kern is skipped */
    c = copyOf(FTU_SYNTH_GPOS);
    ftuPut16(c.d, feat0 + 4, 3);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_EQ(gfxFontKernUnits(&f, Y, O), 0);
    free(c.d);

    /* more than 256 subtables: capped, deterministic, safe */
    c = copyOf(FTU_SYNTH_GPOS);
    ftuPut16(c.d, lo1 + 4, 0xFFFF);
    ASSERT_EQ(initCopy(&c, &f), STATUS_OK);
    ASSERT_TRUE(f.nKernSub <= GFX_FONT_MAX_KERN_SUBTABLES);
    for (uint16_t a = 0; a < 8; a++) {
        for (uint16_t b = 0; b < 8; b++) {
            (void)gfxFontKernUnits(&f, a, b);
        }
    }
    free(c.d);
    free(base.d);
}

/* ---- truncation and mutation fuzz --------------------------------------------------------- */

static void kernSafety(const GfxFont *f) {
    for (uint32_t a = 0; a < 40; a++) {
        for (uint32_t b = 0; b < 40; b++) {
            int32_t v = gfxFontKernUnits(f, (uint16_t)a, (uint16_t)b);
            /* every value is a sum of at most GFX_FONT_MAX_KERN_LOOKUPS int16s */
            if (v > 32 * 32767 || v < -32 * 32768) {
                hostTestFailures++;
                fprintf(stderr, "  kern out of range: %d\n", (int)v);
            }
        }
    }
    (void)gfxFontKernUnits(f, 0xFFFF, 0xFFFF);
    (void)gfxFontKernSource(f);
}

TEST(fontKernTruncationOfTheKerningTables) {
    static const int fonts[] = {FTU_SYNTH_GPOS, FTU_SYNTH_FALLBACK, FTU_SANS};
    for (size_t k = 0; k < 3; k++) {
        Copy c = copyOf(fonts[k]);
        static const char *const tags[] = {"GPOS", "kern"};
        for (int t = 0; t < 2; t++) {
            int r = recIdx(&c, tags[t]);
            if (r < 0) {
                continue;
            }
            uint32_t len = ftuGet32(c.d, 12 + 16 * (uint32_t)r + 12);
            /* every length for the small tables, a stride plus the edges for Liberation's GPOS */
            uint32_t step = len > 600 ? 37 : 1;
            for (uint32_t l = 0; l <= len + 2; l += (l + step > len ? 1 : step)) {
                ftuPut32(c.d, 12 + 16 * (uint32_t)r + 12, l);
                GfxFont f;
                Status st = initCopy(&c, &f);
                ASSERT_TRUE(st == STATUS_OK || st == STATUS_ERR_INVALID);
                if (st == STATUS_OK) {
                    kernSafety(&f);
                }
            }
            ftuPut32(c.d, 12 + 16 * (uint32_t)r + 12, len);
        }
        free(c.d);
    }
}

static uint64_t rngState = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 7;
    rngState ^= rngState << 17;
    return (uint32_t)(rngState >> 16);
}

TEST(fontKernMutationFuzz) {
    static const struct {
        int font;
        int runs;
    } plan[] = {{FTU_SYNTH_GPOS, 4000}, {FTU_SYNTH_FALLBACK, 1000}, {FTU_SANS, 300}};
    int accepted = 0, withKern = 0;
    for (size_t p = 0; p < 3; p++) {
        Copy base = copyOf(plan[p].font);
        static const char *const tags[] = {"GPOS", "kern"};
        Copy c = copyOf(plan[p].font);
        for (int run = 0; run < plan[p].runs; run++) {
            memcpy(c.d, base.d, base.n);
            const char *tag = tags[rnd() % 2];
            int r = recIdx(&c, tag);
            if (r < 0) {
                tag = tags[0];
                r = recIdx(&c, tag);
            }
            uint32_t off = ftuGet32(c.d, 12 + 16 * (uint32_t)r + 8);
            uint32_t len = ftuGet32(c.d, 12 + 16 * (uint32_t)r + 12);
            if (len == 0) {
                continue;
            }
            int edits = 1 + (int)(rnd() % 4);
            for (int e = 0; e < edits; e++) {
                uint32_t at = off + rnd() % len;
                static const uint8_t special[] = {0x00, 0xFF, 0x7F, 0x80, 0x01};
                c.d[at] = (rnd() & 3u) == 0 ? special[rnd() % sizeof special] : (uint8_t)rnd();
            }
            GfxFont f;
            Status st = initCopy(&c, &f);
            ASSERT_TRUE(st == STATUS_OK || st == STATUS_ERR_INVALID ||
                        st == STATUS_ERR_UNSUPPORTED);
            if (st == STATUS_OK) {
                accepted++;
                withKern += gfxFontKernSource(&f) != GFX_FONT_KERN_NONE;
                kernSafety(&f);
                ASSERT_TRUE(f.nKernSub <= GFX_FONT_MAX_KERN_SUBTABLES);
            }
        }
        free(c.d);
        free(base.d);
    }
    ASSERT_TRUE(accepted > 4000);
    ASSERT_TRUE(withKern > 2000); /* most mutants still have some kerning: the paths were reached */
}
