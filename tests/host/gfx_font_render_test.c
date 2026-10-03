/* Host tests for libs/gfx's glyph renderer (M12.3, D-153): mask geometry against the control-box
 * formula, empty glyphs, the four subpixel bins (a stem's centroid moves by 1/4 px per bin),
 * parameter and size limits, allocation failure, and the golden `font_glyph_bins`. */
#include "font_testutil.h"
#include "framework/test.h"
#include "gfx_golden.h"
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

static double centroidX(const GfxGlyphImage *g, double *sum) {
    double s = 0, sx = 0;
    for (int32_t y = 0; y < g->mask.height; y++) {
        for (int32_t x = 0; x < g->mask.width; x++) {
            double c = g->mask.data[(size_t)y * (size_t)g->mask.stride + (size_t)x];
            s += c;
            sx += c * ((double)(g->left + x) + 0.5);
        }
    }
    *sum = s;
    return s > 0 ? sx / s : 0;
}

TEST(fontRenderMaskGeometryMatchesControlBox) {
    const GfxFont *f = fontOf(FTU_SANS);
    ASSERT_TRUE(f != NULL);
    GfxGlyphScratch sc;
    gfxGlyphScratchInit(&sc, NULL);
    static const uint32_t cps[] = {'l', 'g', 'A', '@', 0xC5};
    static const uint32_t sizes[] = {12 * 64, 20 * 64, 33 * 64 + 17};
    for (size_t ci = 0; ci < sizeof cps / sizeof cps[0]; ci++) {
        for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
            for (uint32_t bin = 0; bin < 4; bin++) {
                uint16_t gid = gfxFontGlyphIndex(f, cps[ci]);
                GfxGlyphImage img;
                ASSERT_EQ(gfxFontRenderGlyph(f, gid, sizes[si], bin, &sc, NULL, &img), STATUS_OK);
                /* the same control box from the unscaled outline (the loader is checked elsewhere)
                 */
                GfxGlyphOutline o;
                gfxGlyphOutlineInit(&o, NULL);
                ASSERT_EQ(gfxFontGlyphOutline(f, gid, NULL, &o), STATUS_OK);
                double scale = (double)sizes[si] / ((double)f->unitsPerEm * 64.0);
                double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
                for (uint32_t i = 0; i < o.nPoints; i++) {
                    double x = (double)o.xy[2 * i] * scale + bin * 0.25;
                    double y = -(double)o.xy[2 * i + 1] * scale;
                    minX = x < minX ? x : minX;
                    maxX = x > maxX ? x : maxX;
                    minY = y < minY ? y : minY;
                    maxY = y > maxY ? y : maxY;
                }
                gfxGlyphOutlineFree(&o);
                int ex0 = (int)__builtin_floor(minX + 1e-4),
                    ey0 = (int)__builtin_floor(minY + 1e-4);
                int ex1 = (int)__builtin_ceil(maxX - 1e-4), ey1 = (int)__builtin_ceil(maxY - 1e-4);
                /* float rounding may shift an exact boundary by one pixel: allow that, no more */
                ASSERT_TRUE(img.left >= ex0 - 1 && img.left <= ex0 + 1);
                ASSERT_TRUE(img.top >= ey0 - 1 && img.top <= ey0 + 1);
                ASSERT_TRUE(img.left + img.mask.width >= ex1 - 1 &&
                            img.left + img.mask.width <= ex1 + 1);
                ASSERT_TRUE(img.top + img.mask.height >= ey1 - 1 &&
                            img.top + img.mask.height <= ey1 + 1);
                ASSERT_TRUE(img.mask.stride == img.mask.width);
                double sum;
                (void)centroidX(&img, &sum);
                ASSERT_TRUE(sum > 0); /* something was drawn */
                gfxGlyphImageFree(&img);
                gfxGlyphImageFree(&img); /* idempotent */
            }
        }
    }
    gfxGlyphScratchFree(&sc);
}

TEST(fontRenderKnownRectangle) {
    /* synth-grid 'A' is the box (50,0)-(450,700) at upem 1000: at 20 px it covers x 1..9, y -14..0
     * exactly, so the mask is 8x14 and fully covered (bin 0) */
    const GfxFont *f = fontOf(FTU_SYNTH_GRID);
    ASSERT_TRUE(f != NULL);
    GfxGlyphScratch sc;
    gfxGlyphScratchInit(&sc, NULL);
    GfxGlyphImage img;
    ASSERT_EQ(gfxFontRenderGlyph(f, gfxFontGlyphIndex(f, 'A'), 20 * 64, 0, &sc, NULL, &img),
              STATUS_OK);
    ASSERT_EQ(img.left, 1);
    ASSERT_EQ(img.top, -14);
    ASSERT_EQ(img.mask.width, 8);
    ASSERT_EQ(img.mask.height, 14);
    for (int y = 0; y < 14; y++) {
        for (int x = 0; x < 8; x++) {
            ASSERT_EQ((int)img.mask.data[y * img.mask.stride + x], 255);
        }
    }
    gfxGlyphImageFree(&img);
    /* bin 2 shifts by half a pixel: columns 0 and 8 get half coverage (128 after rounding) */
    ASSERT_EQ(gfxFontRenderGlyph(f, gfxFontGlyphIndex(f, 'A'), 20 * 64, 2, &sc, NULL, &img),
              STATUS_OK);
    ASSERT_EQ(img.left, 1);
    ASSERT_EQ(img.mask.width, 9);
    ASSERT_EQ((int)img.mask.data[0], 128);
    ASSERT_EQ((int)img.mask.data[1], 255);
    ASSERT_EQ((int)img.mask.data[8], 128);
    gfxGlyphImageFree(&img);
    /* the area is preserved: coverage sums to the box area in pixels */
    ASSERT_EQ(gfxFontRenderGlyph(f, gfxFontGlyphIndex(f, 'A'), 33 * 64, 1, &sc, NULL, &img),
              STATUS_OK);
    double sum;
    (void)centroidX(&img, &sum);
    double area = 400.0 * 700.0 * (33.0 / 1000.0) * (33.0 / 1000.0);
    ASSERT_TRUE(sum / 255.0 > area - 0.6 && sum / 255.0 < area + 0.6);
    gfxGlyphImageFree(&img);
    gfxGlyphScratchFree(&sc);
}

TEST(fontRenderEmptyGlyph) {
    const GfxFont *f = fontOf(FTU_SANS);
    GfxGlyphScratch sc;
    gfxGlyphScratchInit(&sc, NULL);
    GfxGlyphImage img;
    ASSERT_EQ(gfxFontRenderGlyph(f, gfxFontGlyphIndex(f, ' '), 16 * 64, 0, &sc, NULL, &img),
              STATUS_OK);
    ASSERT_TRUE(img.mask.data == NULL);
    ASSERT_EQ(img.mask.width, 0);
    ASSERT_EQ(img.mask.height, 0);
    gfxGlyphImageFree(&img);
    gfxGlyphScratchFree(&sc);
}

TEST(fontRenderSubpixelBinsMoveByAQuarterPixel) {
    const GfxFont *f = fontOf(FTU_SANS);
    GfxGlyphScratch sc;
    gfxGlyphScratchInit(&sc, NULL);
    /* 'l' is the rectangle x 138..318 (upem 2048), y 0..1484: in a row that is fully covered
     * vertically, column coverage is the exact horizontal overlap of the stem shifted by bin/4 px
     * (the rasterizer's own error is at most 3/255, see the M12.2 raster tests) */
    static const uint32_t sizes[] = {14 * 64, 24 * 64, 47 * 64, 9 * 64 + 33};
    for (size_t z = 0; z < 4; z++) {
        double scale = (double)sizes[z] / (2048.0 * 64.0);
        for (uint32_t bin = 0; bin < 4; bin++) {
            GfxGlyphImage img;
            ASSERT_EQ(
                gfxFontRenderGlyph(f, gfxFontGlyphIndex(f, 'l'), sizes[z], bin, &sc, NULL, &img),
                STATUS_OK);
            double x0 = 138 * scale + bin * 0.25, x1 = 318 * scale + bin * 0.25;
            const uint8_t *row =
                img.mask.data + (size_t)(img.mask.height / 2) * (size_t)img.mask.stride;
            for (int32_t x = 0; x < img.mask.width; x++) {
                double px = (double)(img.left + x);
                double lo = x0 > px ? x0 : px, hi = x1 < px + 1 ? x1 : px + 1;
                double want = (hi > lo ? hi - lo : 0.0) * 255.0;
                if (row[x] < want - 3.0 || row[x] > want + 3.0) {
                    fprintf(stderr, "  column %d: %d want %.1f (size %u bin %u)\n", (int)x,
                            (int)row[x], want, sizes[z], bin);
                }
                ASSERT_TRUE(row[x] >= want - 3.0 && row[x] <= want + 3.0);
            }
            gfxGlyphImageFree(&img);
        }
    }
    gfxGlyphScratchFree(&sc);
}

TEST(fontRenderIsDeterministic) {
    const GfxFont *f = fontOf(FTU_SANS);
    GfxGlyphScratch sc;
    gfxGlyphScratchInit(&sc, NULL);
    for (uint32_t bin = 0; bin < 4; bin++) {
        GfxGlyphImage a, b;
        uint16_t g = gfxFontGlyphIndex(f, 'g');
        ASSERT_EQ(gfxFontRenderGlyph(f, g, 19 * 64 + 5, bin, &sc, NULL, &a), STATUS_OK);
        ASSERT_EQ(gfxFontRenderGlyph(f, g, 19 * 64 + 5, bin, &sc, NULL, &b), STATUS_OK);
        ASSERT_EQ(a.mask.width, b.mask.width);
        ASSERT_EQ(a.mask.height, b.mask.height);
        ASSERT_TRUE(
            memcmp(a.mask.data, b.mask.data, (size_t)a.mask.width * (size_t)a.mask.height) == 0);
        gfxGlyphImageFree(&a);
        gfxGlyphImageFree(&b);
    }
    gfxGlyphScratchFree(&sc);
}

TEST(fontRenderRejectsBadParametersAndLimits) {
    const GfxFont *f = fontOf(FTU_SANS);
    GfxGlyphScratch sc;
    gfxGlyphScratchInit(&sc, NULL);
    GfxGlyphImage img;
    uint16_t g = gfxFontGlyphIndex(f, 'A');
    ASSERT_EQ(gfxFontRenderGlyph(f, g, 63, 0, &sc, NULL, &img), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontRenderGlyph(f, g, GFX_FONT_MAX_SIZE_Q6 + 1, 0, &sc, NULL, &img),
              STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontRenderGlyph(f, g, 16 * 64, 4, &sc, NULL, &img), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontRenderGlyph(f, g, 16 * 64, 0xFFFFFFFFu, &sc, NULL, &img), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontRenderGlyph(NULL, g, 16 * 64, 0, &sc, NULL, &img), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontRenderGlyph(f, g, 16 * 64, 0, NULL, NULL, &img), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontRenderGlyph(f, g, 16 * 64, 0, &sc, NULL, NULL), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontRenderGlyph(f, f->numGlyphs, 16 * 64, 0, &sc, NULL, &img), STATUS_ERR_INVALID);
    ASSERT_TRUE(img.mask.data == NULL); /* *out is cleared on failure */
    /* the smallest and largest sizes work */
    ASSERT_EQ(gfxFontRenderGlyph(f, g, GFX_FONT_MIN_SIZE_Q6, 0, &sc, NULL, &img), STATUS_OK);
    ASSERT_TRUE(img.mask.width >= 1 && img.mask.height >= 1);
    gfxGlyphImageFree(&img);
    ASSERT_EQ(gfxFontRenderGlyph(f, g, GFX_FONT_MAX_SIZE_Q6, 3, &sc, NULL, &img), STATUS_OK);
    ASSERT_TRUE(img.mask.width <= GFX_FONT_MAX_GLYPH_DIM &&
                img.mask.height <= GFX_FONT_MAX_GLYPH_DIM);
    gfxGlyphImageFree(&img);
    gfxGlyphScratchFree(&sc);
}

TEST(fontRenderHugeGlyphsAndEdgeBombsAreRefused) {
    const GfxFont *bad = fontOf(FTU_SYNTH_BAD);
    ASSERT_TRUE(bad != NULL);
    FtuAlloc st;
    GfxAllocator a;
    ftuAllocInit(&st, &a, -1);
    GfxGlyphScratch sc;
    gfxGlyphScratchInit(&sc, &a);
    GfxGlyphImage img;
    /* glyph 31 spans 32767 units: at upem 1000 and 512 px that is 16800 px, over the mask limit,
     * and no mask (or any big allocation) is made */
    ASSERT_EQ(gfxFontRenderGlyph(bad, 31, GFX_FONT_MAX_SIZE_Q6, 0, &sc, &a, &img),
              STATUS_ERR_UNSUPPORTED);
    ASSERT_TRUE(st.peakBytes < (1u << 20));
    ASSERT_TRUE(img.mask.data == NULL);
    /* the same glyph where it just fits (2031 px) and just does not (2064 px) */
    ASSERT_EQ(gfxFontRenderGlyph(bad, 31, 62 * 64, 0, &sc, &a, &img), STATUS_OK);
    ASSERT_TRUE(img.mask.width <= GFX_FONT_MAX_GLYPH_DIM);
    gfxGlyphImageFree(&img);
    ASSERT_EQ(gfxFontRenderGlyph(bad, 31, 63 * 64, 0, &sc, &a, &img), STATUS_ERR_UNSUPPORTED);
    /* 3000 triangles = 9000 edges, over the rasterizer's 8192-edge limit */
    ASSERT_EQ(gfxFontRenderGlyph(bad, 33, 40 * 64, 0, &sc, &a, &img), STATUS_ERR_UNSUPPORTED);
    ASSERT_TRUE(img.mask.data == NULL);
    /* malformed glyphs propagate the loader's status */
    ASSERT_EQ(gfxFontRenderGlyph(bad, 1, 20 * 64, 0, &sc, &a, &img), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontRenderGlyph(bad, 7, 20 * 64, 0, &sc, &a, &img), STATUS_ERR_UNSUPPORTED);
    gfxGlyphScratchFree(&sc);
    ASSERT_EQ(st.live, 0);
    ASSERT_EQ(st.liveBytes, (size_t)0);
}

TEST(fontRenderAllocationFailureSweep) {
    const GfxFont *f = fontOf(FTU_SANS);
    uint16_t g = gfxFontGlyphIndex(f, 0xC5);
    int sawOk = 0;
    for (int failAt = 0; failAt < 60 && !sawOk; failAt++) {
        FtuAlloc st;
        GfxAllocator a;
        ftuAllocInit(&st, &a, failAt);
        GfxGlyphScratch sc;
        gfxGlyphScratchInit(&sc, &a);
        GfxGlyphImage img;
        Status s = gfxFontRenderGlyph(f, g, 24 * 64, 1, &sc, &a, &img);
        ASSERT_TRUE(s == STATUS_OK || s == STATUS_ERR_NO_MEMORY);
        if (s == STATUS_OK) {
            sawOk = 1;
            ASSERT_TRUE(img.mask.data != NULL);
        } else {
            ASSERT_TRUE(img.mask.data == NULL);
        }
        gfxGlyphImageFree(&img);
        gfxGlyphScratchFree(&sc);
        ASSERT_EQ(st.live, 0);
        ASSERT_EQ(st.liveBytes, (size_t)0);
    }
    ASSERT_TRUE(sawOk);
}

/* ---- the golden: l o W at 24 px, each in the four bins ------------------------------------- */

TEST(fontGoldenGlyphBins) {
    const GfxFont *f = fontOf(FTU_SANS);
    ASSERT_TRUE(f != NULL);
    enum { CELL_W = 40, CELL_H = 40, BASE = 30, COLS = 3, ROWS = 4 };
    static uint8_t px[COLS * CELL_W * ROWS * CELL_H];
    memset(px, 0, sizeof px);
    GfxMask big = {px, COLS * CELL_W, ROWS * CELL_H, COLS * CELL_W};
    GfxGlyphScratch sc;
    gfxGlyphScratchInit(&sc, NULL);
    static const uint32_t cps[COLS] = {'l', 'o', 'W'};
    for (int c = 0; c < COLS; c++) {
        for (uint32_t bin = 0; bin < ROWS; bin++) {
            GfxGlyphImage img;
            ASSERT_EQ(
                gfxFontRenderGlyph(f, gfxFontGlyphIndex(f, cps[c]), 24 * 64, bin, &sc, NULL, &img),
                STATUS_OK);
            for (int y = 0; y < img.mask.height; y++) {
                for (int x = 0; x < img.mask.width; x++) {
                    int dx = c * CELL_W + 8 + img.left + x;
                    int dy = (int)bin * CELL_H + BASE + img.top + y;
                    ASSERT_TRUE(dx >= 0 && dx < big.width && dy >= 0 && dy < big.height);
                    px[dy * big.stride + dx] = img.mask.data[y * img.mask.stride + x];
                }
            }
            gfxGlyphImageFree(&img);
        }
    }
    gfxGlyphScratchFree(&sc);
    ASSERT_TRUE(goldenCheckMask("font_glyph_bins", &big));
}
