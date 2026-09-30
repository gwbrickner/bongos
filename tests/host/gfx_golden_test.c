/* Golden-image tests for libs/gfx (M12.2, D-147). Each test also asserts a few pixel values
 * directly, so the checked-in reference is never the only oracle. */
#include "framework/test.h"
#include "gfx/gfx-path.h"
#include "gfx/gfx.h"
#include "gfx_golden.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t *px;
    GfxCanvas c;
} Canvas;

static bool canvasInit(Canvas *cv, int32_t w, int32_t h, GfxColor bg) {
    cv->px = malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (cv->px == NULL) {
        return false;
    }
    GfxSurface s = {cv->px, w, h, w};
    if (gfxCanvasInit(&cv->c, s, NULL) != STATUS_OK) {
        return false;
    }
    gfxFillRect(&cv->c, (GfxRect){0, 0, w, h}, bg, GFX_OP_SRC);
    return true;
}

static void canvasFree(Canvas *cv) {
    gfxCanvasDestroy(&cv->c);
    free(cv->px);
}

/* Opaque and translucent rects, overlaps, an SRC fill, a clipped group, and an origin shift. */
TEST(gfxGoldenRects) {
    Canvas cv;
    ASSERT_TRUE(canvasInit(&cv, 96, 64, 0xFF202830u));
    GfxCanvas *c = &cv.c;
    gfxFillRect(c, (GfxRect){4, 4, 44, 30}, 0xFFC03020u, GFX_OP_SRC_OVER);
    gfxFillRect(c, (GfxRect){24, 14, 70, 50}, gfxColorPremul(0x8020A040u), GFX_OP_SRC_OVER);
    gfxFillRect(c, (GfxRect){60, 4, 92, 20}, gfxColorPremul(0x604060FFu), GFX_OP_SRC_OVER);
    gfxFillRect(c, (GfxRect){76, 40, 90, 60}, 0xFFF0E060u, GFX_OP_SRC);
    /* a clipped group: the second rect is cut by the clip on every side */
    gfxCanvasPushClip(c, (GfxRect){8, 36, 30, 60});
    gfxFillRect(c, (GfxRect){0, 30, 40, 48}, 0xFF40C0C0u, GFX_OP_SRC);
    gfxFillRect(c, (GfxRect){14, 44, 100, 100}, gfxColorPremul(0xA0FFFFFFu), GFX_OP_SRC_OVER);
    gfxCanvasPopClip(c);
    gfxCanvasSetOrigin(c, 50, 24);
    gfxFillRect(c, (GfxRect){0, 0, 6, 6}, 0xFFFFFFFFu, GFX_OP_SRC);

    ASSERT_EQ(cv.px[8 * 96 + 8], (uint32_t)0xFFC03020u);   /* opaque red */
    ASSERT_EQ(cv.px[60 * 96 + 2], (uint32_t)0xFF202830u);  /* untouched background */
    ASSERT_EQ(cv.px[41 * 96 + 8], (uint32_t)0xFF40C0C0u);  /* inside the clip */
    ASSERT_EQ(cv.px[41 * 96 + 7], (uint32_t)0xFF202830u);  /* just outside the clip: unchanged */
    ASSERT_EQ(cv.px[24 * 96 + 50], (uint32_t)0xFFFFFFFFu); /* origin-shifted */
    ASSERT_TRUE(goldenCheck("gfx_rects", &cv.c.surf));
    canvasFree(&cv);
}

static double coverageSum(const Canvas *cv, int32_t w, int32_t h) {
    double sum = 0;
    for (int32_t i = 0; i < w * h; i++) {
        sum += (double)(cv->px[i] & 0xFFu) / 255.0;
    }
    return sum;
}

/* Rounded rects with radius 0, 1, 4, 12 and an oversized radius (clamped to a pill). */
TEST(gfxGoldenRoundedRects) {
    Canvas cv;
    ASSERT_TRUE(canvasInit(&cv, 128, 72, 0xFF181C24u));
    GfxPath p;
    gfxPathInit(&p, NULL);
    float radii[4] = {0.0f, 1.0f, 4.0f, 12.0f};
    for (int i = 0; i < 4; i++) {
        gfxPathReset(&p);
        ASSERT_EQ(gfxPathAddRoundedRect(&p, 6.0f + 30.0f * (float)i, 6.0f, 24.0f, 28.0f, radii[i]),
                  STATUS_OK);
        ASSERT_EQ(gfxFillPath(&cv.c, &p, GFX_FILL_NONZERO, 0xFF5090E0u, GFX_OP_SRC_OVER),
                  STATUS_OK);
    }
    gfxPathReset(&p);
    ASSERT_EQ(gfxPathAddRoundedRect(&p, 6.5f, 42.0f, 115.0f, 22.5f, 100.0f), STATUS_OK);
    ASSERT_EQ(
        gfxFillPath(&cv.c, &p, GFX_FILL_NONZERO, gfxColorPremul(0xC0E0A040u), GFX_OP_SRC_OVER),
        STATUS_OK);
    gfxPathFree(&p);
    ASSERT_EQ(cv.px[20 * 128 + 12], (uint32_t)0xFF5090E0u); /* inside the r=0 rect */
    ASSERT_EQ(cv.px[6 * 128 + 6], (uint32_t)0xFF5090E0u);   /* r=0 keeps its sharp corner */
    ASSERT_TRUE(cv.px[6 * 128 + 96] != 0xFF5090E0u);        /* r=12 rounds it off */
    ASSERT_TRUE(goldenCheck("gfx_rounded_rects", &cv.c.surf));
    canvasFree(&cv);
}

/* A circle, an ellipse, a quad-bezier leaf and a cubic S-shape, nonzero and even-odd. */
TEST(gfxGoldenCurves) {
    Canvas cv;
    ASSERT_TRUE(canvasInit(&cv, 128, 96, 0xFF101418u));
    GfxPath p;
    gfxPathInit(&p, NULL);
    ASSERT_EQ(gfxPathAddEllipse(&p, 26.0f, 26.0f, 20.0f, 20.0f), STATUS_OK);
    ASSERT_EQ(gfxFillPath(&cv.c, &p, GFX_FILL_NONZERO, 0xFFE05050u, GFX_OP_SRC_OVER), STATUS_OK);
    gfxPathReset(&p);
    ASSERT_EQ(gfxPathAddEllipse(&p, 78.0f, 26.0f, 30.0f, 14.0f), STATUS_OK);
    ASSERT_EQ(
        gfxFillPath(&cv.c, &p, GFX_FILL_NONZERO, gfxColorPremul(0xE050C050u), GFX_OP_SRC_OVER),
        STATUS_OK);
    gfxPathReset(&p); /* a leaf from two quads */
    gfxPathMoveTo(&p, 10.0f, 84.0f);
    gfxPathQuadTo(&p, 30.0f, 50.0f, 56.0f, 84.0f);
    gfxPathQuadTo(&p, 34.0f, 92.0f, 10.0f, 84.0f);
    gfxPathClose(&p);
    ASSERT_EQ(gfxFillPath(&cv.c, &p, GFX_FILL_NONZERO, 0xFFF0D060u, GFX_OP_SRC_OVER), STATUS_OK);
    gfxPathReset(&p); /* an S-shaped ribbon between two cubics */
    gfxPathMoveTo(&p, 66.0f, 60.0f);
    gfxPathCubicTo(&p, 90.0f, 40.0f, 100.0f, 90.0f, 122.0f, 62.0f);
    gfxPathLineTo(&p, 122.0f, 74.0f);
    gfxPathCubicTo(&p, 100.0f, 94.0f, 90.0f, 52.0f, 66.0f, 72.0f);
    gfxPathClose(&p);
    ASSERT_EQ(gfxFillPath(&cv.c, &p, GFX_FILL_EVENODD, 0xFF60D0F0u, GFX_OP_SRC_OVER), STATUS_OK);
    gfxPathFree(&p);
    ASSERT_EQ(cv.px[26 * 128 + 26], (uint32_t)0xFFE05050u); /* circle center */
    ASSERT_EQ(cv.px[95 * 128 + 0], (uint32_t)0xFF101418u);  /* untouched corner */
    ASSERT_TRUE(goldenCheck("gfx_curves", &cv.c.surf));
    canvasFree(&cv);
}

/* Area of a filled ellipse is pi*rx*ry to within flattening and rounding error. */
TEST(gfxEllipseAreaMatches) {
    float radii[][2] = {{20.0f, 20.0f}, {30.0f, 12.0f}, {7.5f, 25.25f}, {2.0f, 2.0f}};
    for (size_t i = 0; i < 4; i++) {
        Canvas cv;
        ASSERT_TRUE(canvasInit(&cv, 80, 64, 0xFF000000u));
        GfxPath p;
        gfxPathInit(&p, NULL);
        ASSERT_EQ(gfxPathAddEllipse(&p, 40.3f, 31.7f, radii[i][0], radii[i][1]), STATUS_OK);
        ASSERT_EQ(gfxFillPath(&cv.c, &p, GFX_FILL_NONZERO, 0xFFFFFFFFu, GFX_OP_SRC), STATUS_OK);
        double want = 3.14159265358979 * radii[i][0] * radii[i][1];
        double got = coverageSum(&cv, 80, 64);
        double err = got > want ? got - want : want - got;
        ASSERT_TRUE(err <= want * 0.01 + 0.5);
        gfxPathFree(&p);
        canvasFree(&cv);
    }
}

/* A rounded rect with r = 0 is exactly the plain rect, and a huge r is a pill (r = min/2). */
TEST(gfxRoundedRectDegenerateRadii) {
    Canvas a, b;
    ASSERT_TRUE(canvasInit(&a, 40, 30, 0xFF000000u));
    ASSERT_TRUE(canvasInit(&b, 40, 30, 0xFF000000u));
    GfxPath p, q;
    gfxPathInit(&p, NULL);
    gfxPathInit(&q, NULL);
    ASSERT_EQ(gfxPathAddRoundedRect(&p, 3.25f, 4.5f, 30.0f, 18.0f, 0.0f), STATUS_OK);
    ASSERT_EQ(gfxPathAddRect(&q, 3.25f, 4.5f, 30.0f, 18.0f), STATUS_OK);
    ASSERT_EQ(gfxFillPath(&a.c, &p, GFX_FILL_NONZERO, 0xFFFFFFFFu, GFX_OP_SRC), STATUS_OK);
    ASSERT_EQ(gfxFillPath(&b.c, &q, GFX_FILL_NONZERO, 0xFFFFFFFFu, GFX_OP_SRC), STATUS_OK);
    ASSERT_EQ(memcmp(a.px, b.px, 40 * 30 * sizeof(uint32_t)), 0);
    /* negative radius behaves like zero; radius above min(w,h)/2 equals exactly min(w,h)/2 */
    gfxPathReset(&p);
    gfxPathReset(&q);
    ASSERT_EQ(gfxPathAddRoundedRect(&p, 3.0f, 4.0f, 30.0f, 18.0f, 500.0f), STATUS_OK);
    ASSERT_EQ(gfxPathAddRoundedRect(&q, 3.0f, 4.0f, 30.0f, 18.0f, 9.0f), STATUS_OK);
    ASSERT_EQ(memcmp(p.pts, q.pts, p.nPts * sizeof(float)), 0);
    ASSERT_EQ(p.nPts, q.nPts);
    gfxPathReset(&p);
    ASSERT_EQ(gfxPathAddRoundedRect(&p, 3.0f, 4.0f, 30.0f, 18.0f, -5.0f), STATUS_OK);
    ASSERT_EQ(p.nVerbs, (uint32_t)5); /* move + 3 lines + close, i.e. a plain rect */
    gfxPathFree(&p);
    gfxPathFree(&q);
    canvasFree(&a);
    canvasFree(&b);
}
