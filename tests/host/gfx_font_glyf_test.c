/* Host tests for libs/gfx's glyf loader and outline -> path conversion (M12.3, D-153). Oracles:
 * expanded outlines from the tests/data/font oracle files (composites built by construction in
 * gen.py, and an independent Python glyf parser over the shipped fonts), plus deliberately
 * malformed glyphs in synth-bad.ttf whose exact Status is listed in bad.oracle. Everything runs
 * under ASan/UBSan/LSan with a counting, failure-injecting allocator. */
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
    if (strcmp(n, "sans") == 0) {
        return FTU_SANS;
    }
    if (strcmp(n, "mono") == 0) {
        return FTU_MONO;
    }
    return -1;
}

/* One oracle outline: `ends` and `pts` (x, y, on) parsed from the text. */
typedef struct {
    int font, gid;
    uint32_t nPts, nContours;
    uint32_t *ends;
    double *xy;
    int *on;
} OracleOutline;

static void oracleFree(OracleOutline *o) {
    free(o->ends);
    free(o->xy);
    free(o->on);
}

/* Reads the next `outline` record starting at *pos; 0 at the end of the file. */
static int nextOutline(const char *text, size_t *pos, OracleOutline *o) {
    static char line[1 << 19];
    memset(o, 0, sizeof *o);
    while (ftuNextLine(text, pos, line, sizeof line)) {
        char kind[16], name[16];
        unsigned gid, np, nc;
        if (sscanf(line, "%15s %15s %u %u %u", kind, name, &gid, &np, &nc) != 5 ||
            strcmp(kind, "outline") != 0) {
            continue;
        }
        o->font = idByName(name);
        o->gid = (int)gid;
        o->nPts = np;
        o->nContours = nc;
        o->ends = malloc(sizeof(uint32_t) * (nc + 1u));
        o->xy = malloc(sizeof(double) * 2u * (np + 1u));
        o->on = malloc(sizeof(int) * (np + 1u));
        if (!ftuNextLine(text, pos, line, sizeof line) || strncmp(line, "ends", 4) != 0) {
            return -1;
        }
        char *p = line + 4;
        for (uint32_t i = 0; i < nc; i++) {
            o->ends[i] = (uint32_t)strtoul(p, &p, 10);
        }
        if (!ftuNextLine(text, pos, line, sizeof line) || strncmp(line, "pts", 3) != 0) {
            return -1;
        }
        p = line + 3;
        for (uint32_t i = 0; i < np; i++) {
            o->xy[2 * i] = strtod(p, &p);
            o->xy[2 * i + 1] = strtod(p, &p);
            o->on[i] = (int)strtol(p, &p, 10);
        }
        return 1;
    }
    return 0;
}

/* Compares a loaded outline to the oracle under x' = a*x + c*y + e (a, d, e, f only used here). */
static int outlinesMatch(const GfxGlyphOutline *g, const OracleOutline *o, const GfxFontXform *xf,
                         double tol) {
    if (g->nPoints != o->nPts || g->nContours != o->nContours) {
        return 0;
    }
    for (uint32_t i = 0; i < o->nContours; i++) {
        if (g->contourEnd[i] != o->ends[i]) {
            return 0;
        }
    }
    for (uint32_t i = 0; i < o->nPts; i++) {
        double x = o->xy[2 * i], y = o->xy[2 * i + 1];
        double ex = xf->a * x + xf->c * y + xf->e, ey = xf->b * x + xf->d * y + xf->f;
        double dx = (double)g->xy[2 * i] - ex, dy = (double)g->xy[2 * i + 1] - ey;
        if (dx > tol || dx < -tol || dy > tol || dy < -tol) {
            return 0;
        }
        if ((g->onCurve[i] & 1u) != (unsigned)o->on[i] || g->onCurve[i] > 1u) {
            return 0;
        }
    }
    return 1;
}

static void checkOracleFile(const char *file, const GfxFontXform *xf, double tol, int limit,
                            int *seen) {
    const char *text = ftuOracle(file);
    ASSERT_TRUE(text != NULL);
    size_t pos = 0;
    OracleOutline o;
    int r = 1;
    while ((limit == 0 || *seen < limit) && (r = nextOutline(text, &pos, &o)) > 0) {
        const GfxFont *f = fontOf(o.font);
        GfxGlyphOutline g;
        gfxGlyphOutlineInit(&g, NULL);
        Status st =
            f != NULL ? gfxFontGlyphOutline(f, (uint16_t)o.gid, xf, &g) : STATUS_ERR_INVALID;
        int ok = st == STATUS_OK && outlinesMatch(&g, &o, xf, tol);
        if (!ok) {
            fprintf(stderr, "  outline mismatch: font %d glyph %d (status %d)\n", o.font, o.gid,
                    (int)st);
        }
        gfxGlyphOutlineFree(&g);
        oracleFree(&o);
        ASSERT_TRUE(ok);
        (*seen)++;
    }
    ASSERT_TRUE(r >= 0);
}

static const GfxFontXform identityXf = {1, 0, 0, 1, 0, 0};

TEST(fontGlyfMatchesConstructionAndIndependentParse) {
    int synth = 0, lib = 0;
    checkOracleFile("synth.oracle", &identityXf, 1e-4, 0, &synth);
    ASSERT_TRUE(synth >= 22); /* every glyph of synth-fallback, composites and all-off-curve */
    checkOracleFile("liberation.oracle", &identityXf, 1e-3, 0, &lib);
    ASSERT_TRUE(lib > 130); /* AQ@&%g and every Latin-1 letter of both fonts */
}

TEST(fontGlyfAppliesTheTransform) {
    /* the y flip and scale of the renderer: x' = 0.5x + 3, y' = -0.5y + 4 (exact in float) */
    const GfxFontXform xf = {0.5f, 0, 0, -0.5f, 3.0f, 4.0f};
    int n = 0;
    checkOracleFile("liberation.oracle", &xf, 1e-3, 60, &n);
    ASSERT_EQ(n, 60);
    int m = 0;
    checkOracleFile("synth.oracle", &xf, 1e-3, 0, &m);
    /* a shear/rotation exercises the c and b terms */
    const GfxFontXform rot = {0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 0.0f};
    int k = 0;
    checkOracleFile("synth.oracle", &rot, 1e-3, 0, &k);
    ASSERT_TRUE(k >= 22);
}

TEST(fontGlyfLoadsEveryShippedGlyph) {
    static const int which[] = {FTU_SANS, FTU_MONO};
    for (size_t k = 0; k < 2; k++) {
        const GfxFont *f = fontOf(which[k]);
        ASSERT_TRUE(f != NULL);
        GfxGlyphOutline g;
        gfxGlyphOutlineInit(&g, NULL);
        uint32_t nonEmpty = 0;
        for (uint32_t gid = 0; gid < f->numGlyphs; gid++) {
            Status st = gfxFontGlyphOutline(f, (uint16_t)gid, NULL, &g);
            if (st != STATUS_OK) {
                fprintf(stderr, "  glyph %u of font %d -> %d\n", gid, which[k], (int)st);
            }
            ASSERT_EQ(st, STATUS_OK);
            if (g.nPoints != 0) {
                nonEmpty++;
                GfxPath p;
                gfxPathInit(&p, NULL);
                ASSERT_EQ(gfxGlyphOutlineToPath(&g, 0.0f, 0.0f, &p), STATUS_OK);
                ASSERT_TRUE(p.nVerbs >= 3);
                gfxPathFree(&p);
            }
        }
        ASSERT_TRUE(nonEmpty > 1500);
        gfxGlyphOutlineFree(&g);
    }
}

/* ---- malformed glyphs -------------------------------------------------------------------- */

static Status wantOf(const char *s) {
    if (strcmp(s, "OK") == 0) {
        return STATUS_OK;
    }
    if (strcmp(s, "INVALID") == 0) {
        return STATUS_ERR_INVALID;
    }
    if (strcmp(s, "UNSUPPORTED") == 0) {
        return STATUS_ERR_UNSUPPORTED;
    }
    return 12345;
}

TEST(fontGlyfMalformedGlyphsGiveTheDocumentedStatus) {
    const GfxFont *f = fontOf(FTU_SYNTH_BAD);
    ASSERT_TRUE(f != NULL);
    const char *text = ftuOracle("bad.oracle");
    ASSERT_TRUE(text != NULL);
    size_t pos = 0;
    char line[128];
    int n = 0;
    GfxGlyphOutline g;
    gfxGlyphOutlineInit(&g, NULL);
    while (ftuNextLine(text, &pos, line, sizeof line)) {
        char kind[8], want[16];
        int gid;
        if (sscanf(line, "%7s %d %15s", kind, &gid, want) != 3 || strcmp(kind, "bad") != 0) {
            continue;
        }
        Status got = gfxFontGlyphOutline(f, (uint16_t)gid, NULL, &g);
        if (gid == 17 || gid == 18) { /* patched below */
            ASSERT_EQ(got, STATUS_OK);
        } else {
            if (got != wantOf(want)) {
                fprintf(stderr, "  bad glyph %d: got %d, want %s\n", gid, (int)got, want);
            }
            ASSERT_EQ(got, wantOf(want));
        }
        n++;
    }
    ASSERT_EQ(n, 34);
    /* an empty glyph and a composite of one load with no points */
    ASSERT_EQ(gfxFontGlyphOutline(f, 19, NULL, &g), STATUS_OK);
    ASSERT_EQ(g.nPoints, 0u);
    ASSERT_EQ(gfxFontGlyphOutline(f, 20, NULL, &g), STATUS_OK);
    ASSERT_EQ(g.nPoints, 0u);
    /* the deepest allowed chain still produces the triangle */
    ASSERT_EQ(gfxFontGlyphOutline(f, 21, NULL, &g), STATUS_OK);
    ASSERT_EQ(g.nPoints, 3u);
    /* extreme coordinates stay exact in float */
    ASSERT_EQ(gfxFontGlyphOutline(f, 31, NULL, &g), STATUS_OK);
    ASSERT_EQ(g.nPoints, 4u);
    ASSERT_TRUE(g.xy[2] == 32767.0f && g.xy[5] == 32767.0f && g.xy[6] == 0.0f);
    /* glyph ids past numGlyphs, and bad arguments */
    ASSERT_EQ(gfxFontGlyphOutline(f, f->numGlyphs, NULL, &g), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontGlyphOutline(f, 0xFFFF, NULL, &g), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontGlyphOutline(NULL, 0, NULL, &g), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontGlyphOutline(f, 0, NULL, NULL), STATUS_ERR_INVALID);
    GfxFont zero;
    memset(&zero, 0, sizeof zero);
    ASSERT_EQ(gfxFontGlyphOutline(&zero, 0, NULL, &g), STATUS_ERR_INVALID);
    gfxGlyphOutlineFree(&g);
}

TEST(fontGlyfBadLocaAffectsOnlyThatGlyph) {
    size_t n;
    const uint8_t *src = ftuFont(FTU_SYNTH_BAD, &n);
    uint8_t *d = malloc(n);
    memcpy(d, src, n);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    ASSERT_TRUE(f.locaLong);
    GfxGlyphOutline g;
    gfxGlyphOutlineInit(&g, NULL);
    /* glyph 17: start > end (non-monotonic) */
    uint32_t l17 = ftuGet32(d, f.locaOff + 4 * 17), l18 = ftuGet32(d, f.locaOff + 4 * 18);
    ftuPut32(d, f.locaOff + 4 * 17, l18 + 4);
    ASSERT_EQ(gfxFontGlyphOutline(&f, 17, NULL, &g), STATUS_ERR_INVALID);
    ftuPut32(d, f.locaOff + 4 * 17, l17);
    ASSERT_EQ(gfxFontGlyphOutline(&f, 17, NULL, &g), STATUS_OK);
    /* glyph 18: end beyond the glyf table */
    uint32_t l19 = ftuGet32(d, f.locaOff + 4 * 19);
    ftuPut32(d, f.locaOff + 4 * 19, f.glyfLen + 4);
    ASSERT_EQ(gfxFontGlyphOutline(&f, 18, NULL, &g), STATUS_ERR_INVALID);
    ftuPut32(d, f.locaOff + 4 * 19, 0xFFFFFFFFu);
    ASSERT_EQ(gfxFontGlyphOutline(&f, 18, NULL, &g), STATUS_ERR_INVALID);
    ftuPut32(d, f.locaOff + 4 * 19, l19);
    ASSERT_EQ(gfxFontGlyphOutline(&f, 18, NULL, &g), STATUS_OK);
    /* neighbours were never affected */
    ASSERT_EQ(gfxFontGlyphOutline(&f, 16, NULL, &g), STATUS_OK);
    ASSERT_EQ(g.nPoints, 3u);
    gfxGlyphOutlineFree(&g);
    free(d);
}

TEST(fontGlyfFanOutBombIsBoundedAndSelfReferenceTerminates) {
    const GfxFont *f = fontOf(FTU_SYNTH_BAD);
    ASSERT_TRUE(f != NULL);
    FtuAlloc st;
    GfxAllocator a;
    ftuAllocInit(&st, &a, -1);
    GfxGlyphOutline g;
    gfxGlyphOutlineInit(&g, &a);
    ASSERT_EQ(gfxFontGlyphOutline(f, 14, NULL, &g), STATUS_ERR_UNSUPPORTED);
    ASSERT_TRUE(st.peakBytes < (1u << 20));
    /* the failed outline is still valid, reusable and freeable */
    ASSERT_EQ(gfxFontGlyphOutline(f, 0, NULL, &g), STATUS_OK);
    ASSERT_EQ(g.nPoints, 3u);
    ASSERT_EQ(gfxFontGlyphOutline(f, 7, NULL, &g), STATUS_ERR_UNSUPPORTED);
    gfxGlyphOutlineFree(&g);
    ASSERT_EQ(st.live, 0);
    ASSERT_EQ(st.liveBytes, (size_t)0);
}

TEST(fontGlyfAllocationFailureSweep) {
    /* a Liberation composite (accented letter: grows past the initial 64 points) and a synthetic
     * depth-2 composite */
    const GfxFont *sans = fontOf(FTU_SANS);
    const GfxFont *fb = fontOf(FTU_SYNTH_FALLBACK);
    ASSERT_TRUE(sans != NULL && fb != NULL);
    struct {
        const GfxFont *f;
        uint16_t gid;
    } cases[] = {{sans, gfxFontGlyphIndex(sans, 0xC5)},
                 {sans, gfxFontGlyphIndex(sans, 'g')},
                 {fb, 20},
                 {fb, 0}};
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        int sawOk = 0;
        for (int failAt = 0; failAt < 40 && !sawOk; failAt++) {
            FtuAlloc st;
            GfxAllocator a;
            ftuAllocInit(&st, &a, failAt);
            GfxGlyphOutline g;
            gfxGlyphOutlineInit(&g, &a);
            Status s = gfxFontGlyphOutline(cases[c].f, cases[c].gid, NULL, &g);
            ASSERT_TRUE(s == STATUS_OK || s == STATUS_ERR_NO_MEMORY);
            sawOk = s == STATUS_OK;
            if (sawOk) {
                ASSERT_TRUE(g.nPoints > 0);
            }
            gfxGlyphOutlineFree(&g);
            ASSERT_EQ(st.live, 0);
        }
        ASSERT_TRUE(sawOk);
    }
}

/* ---- outline -> path ---------------------------------------------------------------------- */

static void buildOutline(GfxGlyphOutline *o, const float *xy, const uint8_t *on, uint32_t np,
                         const uint32_t *ends, uint32_t nc) {
    static float fxy[64];
    static uint8_t fon[32];
    static uint32_t fends[8];
    memcpy(fxy, xy, sizeof(float) * 2 * np);
    memcpy(fon, on, np);
    memcpy(fends, ends, sizeof(uint32_t) * nc);
    gfxGlyphOutlineInit(o, NULL);
    o->xy = fxy;
    o->onCurve = fon;
    o->contourEnd = fends;
    o->nPoints = np;
    o->nContours = nc;
    /* capPoints/capContours stay 0: the arrays are static and never freed */
}

TEST(fontOutlineToPathStartPointCases) {
    GfxGlyphOutline o;
    GfxPath p;
    gfxPathInit(&p, NULL);

    /* first point on-curve: moveTo it, then line, line, close */
    const float a[] = {0, 0, 10, 0, 10, 10};
    const uint8_t aOn[] = {1, 1, 1};
    const uint32_t aEnd[] = {2};
    buildOutline(&o, a, aOn, 3, aEnd, 1);
    ASSERT_EQ(gfxGlyphOutlineToPath(&o, 1.0f, 2.0f, &p), STATUS_OK);
    ASSERT_EQ(p.nVerbs, 4u);
    ASSERT_EQ(p.verbs[0], (uint8_t)GFX_VERB_MOVE);
    ASSERT_EQ(p.verbs[1], (uint8_t)GFX_VERB_LINE);
    ASSERT_EQ(p.verbs[2], (uint8_t)GFX_VERB_LINE);
    ASSERT_EQ(p.verbs[3], (uint8_t)GFX_VERB_CLOSE);
    ASSERT_TRUE(p.pts[0] == 1.0f && p.pts[1] == 2.0f); /* translated by (dx, dy) */
    ASSERT_TRUE(p.pts[2] == 11.0f && p.pts[3] == 2.0f);

    /* first off-curve, last on-curve: start at the last point, iterate first..last-1 */
    gfxPathReset(&p);
    const float b[] = {5, 0, 10, 10, 0, 10};
    const uint8_t bOn[] = {0, 1, 1};
    buildOutline(&o, b, bOn, 3, aEnd, 1);
    ASSERT_EQ(gfxGlyphOutlineToPath(&o, 0.0f, 0.0f, &p), STATUS_OK);
    ASSERT_EQ(p.verbs[0], (uint8_t)GFX_VERB_MOVE);
    ASSERT_TRUE(p.pts[0] == 0.0f && p.pts[1] == 10.0f); /* the last point */
    ASSERT_EQ(p.verbs[1], (uint8_t)GFX_VERB_QUAD);      /* off (5,0) then on (10,10) */
    ASSERT_TRUE(p.pts[2] == 5.0f && p.pts[3] == 0.0f && p.pts[4] == 10.0f && p.pts[5] == 10.0f);
    ASSERT_EQ(p.nVerbs, 3u); /* move, quad, close (the close draws back to (0,10)) */

    /* all off-curve: start at the midpoint of last and first, then 4 quads through midpoints */
    gfxPathReset(&p);
    const float c[] = {10, 10, 0, 10, 0, 0, 10, 0};
    const uint8_t cOn[] = {0, 0, 0, 0};
    const uint32_t cEnd[] = {3};
    buildOutline(&o, c, cOn, 4, cEnd, 1);
    ASSERT_EQ(gfxGlyphOutlineToPath(&o, 0.0f, 0.0f, &p), STATUS_OK);
    ASSERT_EQ(p.nVerbs, 6u);
    ASSERT_TRUE(p.pts[0] == 10.0f && p.pts[1] == 5.0f); /* mid((10,0),(10,10)) */
    ASSERT_EQ(p.verbs[1], (uint8_t)GFX_VERB_QUAD);
    ASSERT_TRUE(p.pts[2] == 10.0f && p.pts[3] == 10.0f);  /* control: first point */
    ASSERT_TRUE(p.pts[4] == 5.0f && p.pts[5] == 10.0f);   /* to mid(first, second) */
    ASSERT_TRUE(p.pts[10] == 0.0f && p.pts[11] == 0.0f);  /* third quad: control third point */
    ASSERT_TRUE(p.pts[12] == 5.0f && p.pts[13] == 0.0f);  /* to mid(third, fourth) */
    ASSERT_EQ(p.verbs[4], (uint8_t)GFX_VERB_QUAD);        /* the closing quad */
    ASSERT_TRUE(p.pts[14] == 10.0f && p.pts[15] == 0.0f); /* control: the last point */
    ASSERT_TRUE(p.pts[16] == 10.0f && p.pts[17] == 5.0f); /* back to the start */

    /* contours with fewer than 2 points are skipped; several contours append in order */
    gfxPathReset(&p);
    const float d[] = {0, 0, 10, 0, 10, 10, 20, 20, 30, 30};
    const uint8_t dOn[] = {1, 1, 1, 1, 1};
    const uint32_t dEnd[] = {0, 3, 4}; /* 1 point, 3 points, 1 point */
    buildOutline(&o, d, dOn, 5, dEnd, 3);
    ASSERT_EQ(gfxGlyphOutlineToPath(&o, 0.0f, 0.0f, &p), STATUS_OK);
    ASSERT_EQ(p.nVerbs, 4u); /* only the middle contour: move, line, line, close */
    ASSERT_TRUE(p.pts[0] == 10.0f);

    /* malformed contour ends are rejected without reading out of bounds */
    gfxPathReset(&p);
    const uint32_t badEnd[] = {9};
    buildOutline(&o, a, aOn, 3, badEnd, 1);
    ASSERT_EQ(gfxGlyphOutlineToPath(&o, 0.0f, 0.0f, &p), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxGlyphOutlineToPath(NULL, 0.0f, 0.0f, &p), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxGlyphOutlineToPath(&o, 0.0f, 0.0f, NULL), STATUS_ERR_INVALID);
    gfxPathFree(&p);
}
