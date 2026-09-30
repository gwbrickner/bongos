/* Host tests for libs/gfx strokes (M12.2, D-143). */
#include "framework/test.h"
#include "gfx/gfx-internal.h"
#include "gfx/gfx-path.h"
#include "gfx/gfx.h"
#include "gfx_golden.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t *px;
    int32_t w, h;
    GfxCanvas c;
} Cv;

static bool cvInit(Cv *cv, int32_t w, int32_t h, GfxColor bg) {
    cv->w = w;
    cv->h = h;
    cv->px = malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (cv->px == NULL) {
        return false;
    }
    for (int32_t i = 0; i < w * h; i++) {
        cv->px[i] = bg;
    }
    GfxSurface s = {cv->px, w, h, w};
    return gfxCanvasInit(&cv->c, s, NULL) == STATUS_OK;
}

static void cvFree(Cv *cv) {
    gfxCanvasDestroy(&cv->c);
    free(cv->px);
}

/* Coverage of the white-on-black stroke lands in the blue channel (SRC_OVER of an opaque white
 * over an opaque black destination gives cov*255/255 = cov). */
static uint32_t cov(const Cv *cv, int32_t x, int32_t y) {
    return cv->px[(size_t)y * (size_t)cv->w + (size_t)x] & 0xFFu;
}

static double sumCov(const Cv *cv) {
    double s = 0;
    for (int32_t i = 0; i < cv->w * cv->h; i++) {
        s += (double)(cv->px[i] & 0xFFu) / 255.0;
    }
    return s;
}

static Status strokeWhite(Cv *cv, const GfxPath *p, GfxCap cap, GfxJoin join, float width,
                          float miter) {
    GfxStroke s = {width, cap, join, miter};
    return gfxStrokePath(&cv->c, p, &s, 0xFFFFFFFFu);
}

TEST(gfxStrokeButtLineExactCoverage) {
    Cv cv;
    ASSERT_TRUE(cvInit(&cv, 40, 24, 0xFF000000u));
    /* width 4 centred on y = 10.5: covers y in [8.5, 12.5); x in [10, 30) */
    ASSERT_EQ(gfxStrokeLine(&cv.c, 10.0f, 10.5f, 30.0f, 10.5f, 4.0f, 0xFFFFFFFFu), STATUS_OK);
    for (int32_t y = 0; y < 24; y++) {
        for (int32_t x = 0; x < 40; x++) {
            uint32_t want = 0;
            if (x >= 10 && x < 30) {
                want = (y >= 9 && y <= 11) ? 255u : ((y == 8 || y == 12) ? 128u : 0u);
            }
            ASSERT_EQ(cov(&cv, x, y), want);
        }
    }
    cvFree(&cv);
}

TEST(gfxStrokeCapsExtendAsSpecified) {
    GfxPath p;
    gfxPathInit(&p, NULL);
    gfxPathMoveTo(&p, 12.0f, 12.0f);
    gfxPathLineTo(&p, 28.0f, 12.0f);
    Cv butt, sq, rd;
    ASSERT_TRUE(cvInit(&butt, 40, 24, 0xFF000000u));
    ASSERT_TRUE(cvInit(&sq, 40, 24, 0xFF000000u));
    ASSERT_TRUE(cvInit(&rd, 40, 24, 0xFF000000u));
    ASSERT_EQ(strokeWhite(&butt, &p, GFX_CAP_BUTT, GFX_JOIN_BEVEL, 8.0f, 4.0f), STATUS_OK);
    ASSERT_EQ(strokeWhite(&sq, &p, GFX_CAP_SQUARE, GFX_JOIN_BEVEL, 8.0f, 4.0f), STATUS_OK);
    ASSERT_EQ(strokeWhite(&rd, &p, GFX_CAP_ROUND, GFX_JOIN_BEVEL, 8.0f, 4.0f), STATUS_OK);
    /* areas: butt = 16*8; square = 24*8; round = 16*8 + pi*4^2 */
    ASSERT_TRUE(sumCov(&butt) > 127.5 && sumCov(&butt) < 128.5);
    ASSERT_TRUE(sumCov(&sq) > 191.5 && sumCov(&sq) < 192.5);
    double want = 128.0 + 3.14159265 * 16.0;
    ASSERT_TRUE(sumCov(&rd) > want * 0.99 && sumCov(&rd) < want * 1.01);
    ASSERT_EQ(cov(&butt, 10, 12), 0u);
    ASSERT_EQ(cov(&sq, 10, 12), 255u); /* square cap reaches x = 8 */
    ASSERT_EQ(cov(&sq, 7, 12), 0u);
    ASSERT_TRUE(cov(&rd, 9, 12) > 200u); /* round cap reaches x = 8 on the axis */
    ASSERT_TRUE(cov(&rd, 8, 8) < 20u);   /* but not the square's corner */
    gfxPathFree(&p);
    cvFree(&butt);
    cvFree(&sq);
    cvFree(&rd);
}

/* A right-angle polyline: miter fills the outer corner, bevel and round don't fully. */
TEST(gfxStrokeJoinStyles) {
    GfxPath p;
    gfxPathInit(&p, NULL);
    gfxPathMoveTo(&p, 8.0f, 30.0f);
    gfxPathLineTo(&p, 8.0f, 8.0f);
    gfxPathLineTo(&p, 30.0f, 8.0f);
    Cv m, b, r;
    ASSERT_TRUE(cvInit(&m, 40, 40, 0xFF000000u));
    ASSERT_TRUE(cvInit(&b, 40, 40, 0xFF000000u));
    ASSERT_TRUE(cvInit(&r, 40, 40, 0xFF000000u));
    ASSERT_EQ(strokeWhite(&m, &p, GFX_CAP_BUTT, GFX_JOIN_MITER, 6.0f, 4.0f), STATUS_OK);
    ASSERT_EQ(strokeWhite(&b, &p, GFX_CAP_BUTT, GFX_JOIN_BEVEL, 6.0f, 4.0f), STATUS_OK);
    ASSERT_EQ(strokeWhite(&r, &p, GFX_CAP_BUTT, GFX_JOIN_ROUND, 6.0f, 4.0f), STATUS_OK);
    /* the outer corner pixel (5,5) lies inside the miter square [5,11)x[5,11) */
    ASSERT_EQ(cov(&m, 5, 5), 255u);
    ASSERT_TRUE(cov(&b, 5, 5) < 10u);
    ASSERT_TRUE(cov(&r, 5, 5) < cov(&m, 5, 5) && cov(&r, 5, 5) < 200u);
    /* areas: arms 22*6 + 22*6 minus the overlapping square counted once */
    double arms = 22.0 * 6.0 + 22.0 * 6.0 - 0.0; /* butt arms overlap in the inner 3x3 corner */
    (void)arms;
    ASSERT_TRUE(sumCov(&m) > sumCov(&r) && sumCov(&r) > sumCov(&b));
    gfxPathFree(&p);
    cvFree(&m);
    cvFree(&b);
    cvFree(&r);
}

/* Too sharp for the miter limit: falls back to a bevel (no spike beyond the joint). */
TEST(gfxStrokeMiterLimitFallsBackToBevel) {
    GfxPath p;
    gfxPathInit(&p, NULL);
    gfxPathMoveTo(&p, 4.0f, 30.0f);
    gfxPathLineTo(&p, 20.0f, 10.0f);
    gfxPathLineTo(&p, 36.0f, 30.0f); /* a ~77 degree apex: ratio ~1.6 */
    Cv lo, hi;
    ASSERT_TRUE(cvInit(&lo, 40, 40, 0xFF000000u));
    ASSERT_TRUE(cvInit(&hi, 40, 40, 0xFF000000u));
    ASSERT_EQ(strokeWhite(&lo, &p, GFX_CAP_BUTT, GFX_JOIN_MITER, 4.0f, 1.2f), STATUS_OK);
    ASSERT_EQ(strokeWhite(&hi, &p, GFX_CAP_BUTT, GFX_JOIN_MITER, 4.0f, 4.0f), STATUS_OK);
    ASSERT_TRUE(sumCov(&hi) > sumCov(&lo) + 1.0);
    ASSERT_TRUE(cov(&hi, 20, 7) > 100u); /* the spike above the apex */
    ASSERT_EQ(cov(&lo, 20, 7), 0u);
    /* a limit <= 1 always bevels */
    Cv one;
    ASSERT_TRUE(cvInit(&one, 40, 40, 0xFF000000u));
    ASSERT_EQ(strokeWhite(&one, &p, GFX_CAP_BUTT, GFX_JOIN_MITER, 4.0f, 1.0f), STATUS_OK);
    ASSERT_EQ(memcmp(one.px, lo.px, 40 * 40 * sizeof(uint32_t)), 0);
    gfxPathFree(&p);
    cvFree(&lo);
    cvFree(&hi);
    cvFree(&one);
}

/* A stroked circle outline (a closed curve with round joins): the ring is solid (no seams between
 * the many pieces) and its area is 2*pi*r*w. */
TEST(gfxStrokeCircleRingHasNoSeams) {
    Cv cv;
    ASSERT_TRUE(cvInit(&cv, 80, 80, 0xFF000000u));
    GfxPath p;
    gfxPathInit(&p, NULL);
    ASSERT_EQ(gfxPathAddEllipse(&p, 40.0f, 40.0f, 25.0f, 25.0f), STATUS_OK);
    ASSERT_EQ(strokeWhite(&cv, &p, GFX_CAP_BUTT, GFX_JOIN_ROUND, 6.0f, 4.0f), STATUS_OK);
    for (int32_t y = 0; y < 80; y++) {
        for (int32_t x = 0; x < 80; x++) {
            /* squared distance from the center of the pixel */
            double dx = (double)x + 0.5 - 40.0, dy = (double)y + 0.5 - 40.0;
            double d2 = dx * dx + dy * dy;
            if (d2 > 23.0 * 23.0 && d2 < 27.0 * 27.0) {
                ASSERT_EQ(cov(&cv, x, y), 255u); /* well inside the ring: every pixel solid */
            }
            if (d2 < 21.0 * 21.0 || d2 > 29.0 * 29.0) {
                ASSERT_EQ(cov(&cv, x, y), 0u);
            }
        }
    }
    double want = 2.0 * 3.14159265 * 25.0 * 6.0;
    ASSERT_TRUE(sumCov(&cv) > want * 0.98 && sumCov(&cv) < want * 1.02);
    gfxPathFree(&p);
    cvFree(&cv);
}

TEST(gfxStrokeClosedPolygonJoinsAtStart) {
    Cv cv;
    ASSERT_TRUE(cvInit(&cv, 40, 40, 0xFF000000u));
    GfxPath p;
    gfxPathInit(&p, NULL);
    ASSERT_EQ(gfxPathAddRect(&p, 10.0f, 10.0f, 20.0f, 20.0f), STATUS_OK);
    ASSERT_EQ(strokeWhite(&cv, &p, GFX_CAP_BUTT, GFX_JOIN_MITER, 4.0f, 4.0f), STATUS_OK);
    /* miter joins at all four corners, including the closing one at (10,10) */
    ASSERT_EQ(cov(&cv, 8, 8), 255u);
    ASSERT_EQ(cov(&cv, 31, 8), 255u);
    ASSERT_EQ(cov(&cv, 31, 31), 255u);
    ASSERT_EQ(cov(&cv, 8, 31), 255u);
    ASSERT_EQ(cov(&cv, 20, 20), 0u);
    ASSERT_EQ(cov(&cv, 7, 20), 0u);
    gfxPathFree(&p);
    cvFree(&cv);
}

TEST(gfxStrokeZeroLengthSubpaths) {
    Cv butt, sq, rd;
    ASSERT_TRUE(cvInit(&butt, 20, 20, 0xFF000000u));
    ASSERT_TRUE(cvInit(&sq, 20, 20, 0xFF000000u));
    ASSERT_TRUE(cvInit(&rd, 20, 20, 0xFF000000u));
    GfxPath p;
    gfxPathInit(&p, NULL);
    gfxPathMoveTo(&p, 10.0f, 10.0f);
    gfxPathLineTo(&p, 10.0f, 10.0f); /* repeated point: still no length */
    ASSERT_EQ(strokeWhite(&butt, &p, GFX_CAP_BUTT, GFX_JOIN_BEVEL, 6.0f, 4.0f), STATUS_OK);
    ASSERT_EQ(strokeWhite(&sq, &p, GFX_CAP_SQUARE, GFX_JOIN_BEVEL, 6.0f, 4.0f), STATUS_OK);
    ASSERT_EQ(strokeWhite(&rd, &p, GFX_CAP_ROUND, GFX_JOIN_BEVEL, 6.0f, 4.0f), STATUS_OK);
    ASSERT_TRUE(sumCov(&butt) == 0.0);
    ASSERT_TRUE(sumCov(&sq) > 35.5 && sumCov(&sq) < 36.5);
    double want = 3.14159265 * 9.0;
    ASSERT_TRUE(sumCov(&rd) > want * 0.97 && sumCov(&rd) < want * 1.03);
    gfxPathFree(&p);
    cvFree(&butt);
    cvFree(&sq);
    cvFree(&rd);
}

TEST(gfxStrokeInvalidWidthsAndPaths) {
    Cv cv;
    ASSERT_TRUE(cvInit(&cv, 20, 20, 0xFF000000u));
    GfxPath p;
    gfxPathInit(&p, NULL);
    gfxPathMoveTo(&p, 2.0f, 2.0f);
    gfxPathLineTo(&p, 18.0f, 18.0f);
    ASSERT_EQ(strokeWhite(&cv, &p, GFX_CAP_BUTT, GFX_JOIN_BEVEL, 0.0f, 4.0f), STATUS_OK);
    ASSERT_EQ(strokeWhite(&cv, &p, GFX_CAP_BUTT, GFX_JOIN_BEVEL, -3.0f, 4.0f), STATUS_OK);
    ASSERT_EQ(strokeWhite(&cv, &p, GFX_CAP_BUTT, GFX_JOIN_BEVEL, __builtin_nanf(""), 4.0f),
              STATUS_ERR_INVALID);
    ASSERT_EQ(strokeWhite(&cv, &p, GFX_CAP_BUTT, GFX_JOIN_MITER, 2.0f, __builtin_inff()),
              STATUS_ERR_INVALID);
    ASSERT_TRUE(sumCov(&cv) == 0.0); /* nothing drawn by any of them */
    /* a path with a sticky error propagates it */
    GfxPath bad;
    gfxPathInit(&bad, NULL);
    gfxPathLineTo(&bad, 1.0f, 1.0f);
    ASSERT_EQ(strokeWhite(&cv, &bad, GFX_CAP_BUTT, GFX_JOIN_BEVEL, 2.0f, 4.0f), STATUS_ERR_INVALID);
    /* a giant width and huge coordinates stay safe (clamped) */
    GfxPath huge;
    gfxPathInit(&huge, NULL);
    gfxPathMoveTo(&huge, -3.0e38f, 5.0f);
    gfxPathLineTo(&huge, 3.0e38f, 9.0f);
    (void)strokeWhite(&cv, &huge, GFX_CAP_ROUND, GFX_JOIN_ROUND, 3.0e38f, 4.0f);
    (void)strokeWhite(&cv, &huge, GFX_CAP_SQUARE, GFX_JOIN_MITER, 5.0f, 4.0f);
    gfxPathFree(&p);
    gfxPathFree(&bad);
    gfxPathFree(&huge);
    cvFree(&cv);
}

/* A path that turns back on itself exactly (cross product 0) still gets its round join: a half
 * disc beyond the turning point, on the side the path was heading. It used to be swept on the
 * other side (already covered by the segments), so the join vanished while a turn 0.5 px off
 * exact showed it. Both directions, horizontal and vertical, open and closed. */
TEST(gfxStrokeExactReversalKeepsRoundJoin) {
    static const float pts[4][6] = {
        {10.0f, 20.0f, 30.0f, 20.0f, 10.0f, 20.0f}, /* turns at (30,20), heading +x */
        {30.0f, 20.0f, 10.0f, 20.0f, 30.0f, 20.0f}, /* turns at (10,20), heading -x */
        {20.0f, 10.0f, 20.0f, 30.0f, 20.0f, 10.0f}, /* turns at (20,30), heading +y */
        {20.0f, 30.0f, 20.0f, 10.0f, 20.0f, 30.0f}, /* turns at (20,10), heading -y */
    };
    /* a pixel inside the half disc of radius 4 beyond each turning point, and one outside it */
    static const int32_t in[4][2] = {{32, 20}, {7, 19}, {20, 32}, {19, 7}};
    static const int32_t out[4][2] = {{35, 20}, {4, 19}, {20, 35}, {19, 4}};
    for (int k = 0; k < 4; k++) {
        for (int closed = 0; closed < 2; closed++) {
            GfxPath p;
            gfxPathInit(&p, NULL);
            gfxPathMoveTo(&p, pts[k][0], pts[k][1]);
            gfxPathLineTo(&p, pts[k][2], pts[k][3]);
            gfxPathLineTo(&p, pts[k][4], pts[k][5]);
            if (closed) {
                gfxPathClose(&p); /* the closing point repeats the start: both ends turn back */
            }
            Cv cv;
            ASSERT_TRUE(cvInit(&cv, 40, 40, 0xFF000000u));
            ASSERT_EQ(strokeWhite(&cv, &p, GFX_CAP_BUTT, GFX_JOIN_ROUND, 8.0f, 4.0f), STATUS_OK);
            if (cov(&cv, in[k][0], in[k][1]) != 255u) {
                fprintf(stderr, "  case %d closed %d: pixel (%d,%d) = %u\n", k, closed, in[k][0],
                        in[k][1], cov(&cv, in[k][0], in[k][1]));
            }
            ASSERT_EQ(cov(&cv, in[k][0], in[k][1]), 255u);
            ASSERT_EQ(cov(&cv, out[k][0], out[k][1]), 0u);
            /* 20 x 8 plus one half disc (open), or two (closed) */
            double want = 160.0 + (closed ? 2.0 : 1.0) * 3.14159265 * 8.0;
            ASSERT_TRUE(sumCov(&cv) > want * 0.97 && sumCov(&cv) < want * 1.03);
            gfxPathFree(&p);
            cvFree(&cv);
        }
    }
}

/* Reversing the stroked path changes nothing about the union. */
TEST(gfxStrokeDirectionIndependent) {
    GfxPath fwd, rev;
    gfxPathInit(&fwd, NULL);
    gfxPathInit(&rev, NULL);
    gfxPathMoveTo(&fwd, 6.0f, 30.0f);
    gfxPathLineTo(&fwd, 18.0f, 8.0f);
    gfxPathLineTo(&fwd, 30.0f, 28.0f);
    gfxPathMoveTo(&rev, 30.0f, 28.0f);
    gfxPathLineTo(&rev, 18.0f, 8.0f);
    gfxPathLineTo(&rev, 6.0f, 30.0f);
    Cv a, b;
    ASSERT_TRUE(cvInit(&a, 40, 40, 0xFF000000u));
    ASSERT_TRUE(cvInit(&b, 40, 40, 0xFF000000u));
    ASSERT_EQ(strokeWhite(&a, &fwd, GFX_CAP_ROUND, GFX_JOIN_ROUND, 5.0f, 4.0f), STATUS_OK);
    ASSERT_EQ(strokeWhite(&b, &rev, GFX_CAP_ROUND, GFX_JOIN_ROUND, 5.0f, 4.0f), STATUS_OK);
    for (int32_t i = 0; i < 40 * 40; i++) {
        uint32_t x = a.px[i] & 0xFFu, y = b.px[i] & 0xFFu;
        /* only the arc fans differ (a chord approximation swept the other way, <= 0.1 px) */
        ASSERT_TRUE((x > y ? x - y : y - x) <= 12u);
    }
    gfxPathFree(&fwd);
    gfxPathFree(&rev);
    cvFree(&a);
    cvFree(&b);
}

/* A fan of lines at many angles and widths, each with a different cap, plus three join styles. */
TEST(gfxGoldenStrokes) {
    Cv cv;
    ASSERT_TRUE(cvInit(&cv, 128, 96, 0xFF14181Cu));
    static const float ax[8] = {1.0f, 0.9239f,  0.7071f,  0.3827f,
                                0.0f, -0.3827f, -0.7071f, -0.9239f};
    static const float ay[8] = {0.0f, 0.3827f, 0.7071f, 0.9239f, 1.0f, 0.9239f, 0.7071f, 0.3827f};
    float widths[3] = {1.0f, 1.5f, 3.0f};
    GfxCap caps[3] = {GFX_CAP_BUTT, GFX_CAP_SQUARE, GFX_CAP_ROUND};
    for (int i = 0; i < 8; i++) {
        GfxPath p;
        gfxPathInit(&p, NULL);
        gfxPathMoveTo(&p, 36.0f + 4.0f * ax[i], 36.0f + 4.0f * ay[i]);
        gfxPathLineTo(&p, 36.0f + 26.0f * ax[i], 36.0f + 26.0f * ay[i]);
        GfxStroke s = {widths[i % 3], caps[(i / 3) % 3], GFX_JOIN_BEVEL, 4.0f};
        ASSERT_EQ(gfxStrokePath(&cv.c, &p, &s, 0xFFE8E8E8u), STATUS_OK);
        gfxPathFree(&p);
    }
    GfxJoin joins[3] = {GFX_JOIN_MITER, GFX_JOIN_BEVEL, GFX_JOIN_ROUND};
    GfxCap ends[3] = {GFX_CAP_BUTT, GFX_CAP_SQUARE, GFX_CAP_ROUND};
    for (int i = 0; i < 3; i++) {
        GfxPath p;
        gfxPathInit(&p, NULL);
        float ox = 74.0f, oy = 8.0f + 30.0f * (float)i;
        gfxPathMoveTo(&p, ox, oy + 20.0f);
        gfxPathLineTo(&p, ox + 14.0f, oy + 4.0f);
        gfxPathLineTo(&p, ox + 28.0f, oy + 20.0f);
        gfxPathLineTo(&p, ox + 44.0f, oy + 6.0f);
        GfxStroke s = {5.0f, ends[i], joins[i], 4.0f};
        ASSERT_EQ(gfxStrokePath(&cv.c, &p, &s, 0xFF60C0F0u), STATUS_OK);
        gfxPathFree(&p);
    }
    /* a thin closed curve on top */
    GfxPath e;
    gfxPathInit(&e, NULL);
    ASSERT_EQ(gfxPathAddRoundedRect(&e, 8.0f, 70.0f, 56.0f, 20.0f, 8.0f), STATUS_OK);
    GfxStroke t = {2.0f, GFX_CAP_BUTT, GFX_JOIN_ROUND, 4.0f};
    ASSERT_EQ(gfxStrokePath(&cv.c, &e, &t, gfxColorPremul(0xC0F0C060u)), STATUS_OK);
    gfxPathFree(&e);
    ASSERT_TRUE(goldenCheck("gfx_strokes", &cv.c.surf));
    cvFree(&cv);
}

/* Far from the origin the shoelace products are ~x^2 and their float rounding can swamp the area
 * of a thin piece and flip its winding, which under the nonzero fill would cancel it: strokes must
 * come out the same wherever they are (drawn here via the canvas origin). */
TEST(gfxStrokeFarFromOriginIsSolid) {
    float bases[] = {0.0f, 1000.0f, 65536.0f, 300000.0f, 1000000.0f};
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        Cv cv;
        ASSERT_TRUE(cvInit(&cv, 40, 24, 0xFF000000u));
        gfxCanvasSetOrigin(&cv.c, -(int32_t)bases[i], -(int32_t)bases[i]);
        GfxPath p;
        gfxPathInit(&p, NULL);
        gfxPathMoveTo(&p, bases[i] + 5.0f, bases[i] + 5.5f);
        gfxPathLineTo(&p, bases[i] + 30.0f, bases[i] + 9.5f);
        gfxPathLineTo(&p, bases[i] + 12.0f, bases[i] + 19.5f);
        ASSERT_EQ(strokeWhite(&cv, &p, GFX_CAP_ROUND, GFX_JOIN_ROUND, 1.5f, 4.0f), STATUS_OK);
        /* the same shape at the origin, for reference */
        Cv ref;
        ASSERT_TRUE(cvInit(&ref, 40, 24, 0xFF000000u));
        GfxPath q;
        gfxPathInit(&q, NULL);
        gfxPathMoveTo(&q, 5.0f, 5.5f);
        gfxPathLineTo(&q, 30.0f, 9.5f);
        gfxPathLineTo(&q, 12.0f, 19.5f);
        ASSERT_EQ(strokeWhite(&ref, &q, GFX_CAP_ROUND, GFX_JOIN_ROUND, 1.5f, 4.0f), STATUS_OK);
        double a = sumCov(&cv), r = sumCov(&ref);
        if (a < r * 0.97 || a > r * 1.03) {
            fprintf(stderr, "  base %.0f: coverage %.2f vs %.2f at the origin\n", (double)bases[i],
                    a, r);
        }
        ASSERT_TRUE(a > r * 0.97 && a < r * 1.03);
        gfxPathFree(&p);
        gfxPathFree(&q);
        cvFree(&cv);
        cvFree(&ref);
    }
}

/* The SVG miter rule at its boundary: a right angle has miter ratio sqrt(2) = 1.41421; a limit
 * just above keeps the miter, just below falls back to a bevel. */
TEST(gfxStrokeMiterLimitBoundary) {
    GfxPath p;
    gfxPathInit(&p, NULL);
    gfxPathMoveTo(&p, 8.0f, 30.0f);
    gfxPathLineTo(&p, 8.0f, 8.0f);
    gfxPathLineTo(&p, 30.0f, 8.0f);
    Cv above, below;
    ASSERT_TRUE(cvInit(&above, 40, 40, 0xFF000000u));
    ASSERT_TRUE(cvInit(&below, 40, 40, 0xFF000000u));
    ASSERT_EQ(strokeWhite(&above, &p, GFX_CAP_BUTT, GFX_JOIN_MITER, 6.0f, 1.4143f), STATUS_OK);
    ASSERT_EQ(strokeWhite(&below, &p, GFX_CAP_BUTT, GFX_JOIN_MITER, 6.0f, 1.4142f), STATUS_OK);
    ASSERT_EQ(cov(&above, 5, 5), 255u); /* the miter square's outer corner */
    ASSERT_TRUE(cov(&below, 5, 5) < 10u);
    gfxPathFree(&p);
    cvFree(&above);
    cvFree(&below);
}

/* Sum of the shoelace areas of the outline's closed pieces (all wound positively). */
static double outlineArea(const GfxPath *o) {
    double total = 0.0, sx = 0.0, sy = 0.0, px = 0.0, py = 0.0, a = 0.0;
    const float *pt = o->pts;
    for (uint32_t i = 0; i < o->nVerbs; i++) {
        switch (o->verbs[i]) {
            case GFX_VERB_MOVE:
                sx = px = pt[0];
                sy = py = pt[1];
                a = 0.0;
                pt += 2;
                break;
            case GFX_VERB_LINE:
                a += px * pt[1] - pt[0] * py;
                px = pt[0];
                py = pt[1];
                pt += 2;
                break;
            case GFX_VERB_CLOSE:
                a += px * sy - sx * py;
                total += a * 0.5;
                break;
            default:
                return -1.0; /* the stroker only emits polygons */
        }
    }
    return total;
}

/* Round caps stay complete half discs at any radius: the arc step has a floor, so a big radius
 * cannot run out of ARC_MAX_STEPS before it reaches the end of the arc. */
TEST(gfxStrokeRoundCapsCompleteAtHugeRadius) {
    float radii[] = {2.0f, 50.0f, 900.0f, 5000.0f, 20000.0f, 200000.0f};
    for (size_t i = 0; i < sizeof(radii) / sizeof(radii[0]); i++) {
        double h = radii[i];
        GfxPath in, out;
        gfxPathInit(&in, NULL);
        gfxPathInit(&out, NULL);
        gfxPathMoveTo(&in, 0.0f, 0.0f);
        gfxPathLineTo(&in, 10.0f, 0.0f);
        GfxStroke s = {2.0f * radii[i], GFX_CAP_ROUND, GFX_JOIN_ROUND, 4.0f};
        ASSERT_EQ(gfxStrokeToPath(&in, &s, &out), STATUS_OK);
        double want = 10.0 * 2.0 * h + 3.14159265358979 * h * h;
        double got = outlineArea(&out);
        /* inscribed chords of sagitta s lose about (2pi/3)*h*s per half disc; s is 0.1 px, or
         * 1.2e-4*h once the step floor (0.03 rad) applies, above h = 889 */
        double tol = 4.5 * h * (h > 889.0 ? h * 1.2e-4 : 0.1) + 1e-5 * want;
        if (!(got <= want * (1.0 + 1e-5) && got >= want - tol)) {
            fprintf(stderr, "  radius %g: outline area %.1f, want %.1f (tol %.1f)\n", h, got, want,
                    tol);
        }
        ASSERT_TRUE(got <= want * (1.0 + 1e-5) && got >= want - tol);
        gfxPathFree(&in);
        gfxPathFree(&out);
    }
}

/* Coordinates and widths far past GFX_COORD_MAX are clamped to it, not turned into inf/NaN:
 * the strokes still succeed and draw. */
TEST(gfxStrokeClampsHugeValues) {
    Cv cv;
    ASSERT_TRUE(cvInit(&cv, 20, 20, 0xFF000000u));
    GfxPath huge;
    gfxPathInit(&huge, NULL);
    gfxPathMoveTo(&huge, -3.0e38f, 10.0f);
    gfxPathLineTo(&huge, 3.0e38f, 10.0f);
    ASSERT_EQ(strokeWhite(&cv, &huge, GFX_CAP_SQUARE, GFX_JOIN_MITER, 4.0f, 4.0f), STATUS_OK);
    ASSERT_EQ(cov(&cv, 0, 9), 255u); /* a horizontal band y in [8, 12) across the canvas */
    ASSERT_EQ(cov(&cv, 19, 10), 255u);
    ASSERT_EQ(cov(&cv, 10, 5), 0u);
    GfxPath p;
    gfxPathInit(&p, NULL);
    gfxPathMoveTo(&p, 5.0f, 5.0f);
    gfxPathLineTo(&p, 15.0f, 15.0f);
    gfxPathLineTo(&p, 5.0f, 15.0f);
    GfxCap caps[3] = {GFX_CAP_SQUARE, GFX_CAP_SQUARE, GFX_CAP_ROUND};
    GfxJoin joins[3] = {GFX_JOIN_BEVEL, GFX_JOIN_MITER, GFX_JOIN_ROUND};
    for (int k = 0; k < 3; k++) {
        Cv all;
        ASSERT_TRUE(cvInit(&all, 20, 20, 0xFF000000u));
        ASSERT_EQ(strokeWhite(&all, &p, caps[k], joins[k], 3.0e38f, 4.0f), STATUS_OK);
        for (int32_t i = 0; i < 20 * 20; i++) {
            ASSERT_EQ(all.px[i] & 0xFFu, 0xFFu); /* the whole canvas is inside the stroke */
        }
        cvFree(&all);
    }
    gfxPathFree(&huge);
    gfxPathFree(&p);
    cvFree(&cv);
}

typedef struct {
    int failAt, count, live;
    size_t liveBytes, peakBytes;
} StrokeAlloc;

static void *saAlloc(void *ctx, size_t n) {
    StrokeAlloc *a = ctx;
    if (a->count++ == a->failAt) {
        return NULL;
    }
    void *p = malloc(n != 0 ? n : 1);
    if (p != NULL) {
        a->live++;
        a->liveBytes += n;
        if (a->liveBytes > a->peakBytes) {
            a->peakBytes = a->liveBytes;
        }
    }
    return p;
}

static void saFree(void *ctx, void *p, size_t n) {
    StrokeAlloc *a = ctx;
    if (p != NULL) {
        a->live--;
        a->liveBytes -= n;
    }
    free(p);
}

/* Every allocation of a stroke failing in turn: NO_MEMORY, nothing leaked. */
TEST(gfxStrokeAllocationFailureSweep) {
    GfxPath in;
    gfxPathInit(&in, NULL);
    gfxPathMoveTo(&in, 2.0f, 2.0f);
    for (int i = 0; i < 40; i++) { /* more than one poly-array growth (32 points) */
        gfxPathLineTo(&in, 4.0f + (float)i, (i & 1) ? 30.0f : 4.0f);
    }
    gfxPathCubicTo(&in, 60.0f, 0.0f, 0.0f, 60.0f, 50.0f, 50.0f);
    bool done = false;
    for (int failAt = 0; failAt < 200 && !done; failAt++) {
        StrokeAlloc sa = {failAt, 0, 0, 0, 0};
        GfxAllocator a = {saAlloc, saFree, &sa};
        GfxPath out;
        gfxPathInit(&out, &a);
        GfxStroke s = {3.0f, GFX_CAP_ROUND, GFX_JOIN_ROUND, 4.0f};
        Status st = gfxStrokeToPath(&in, &s, &out);
        if (st == STATUS_OK) {
            done = sa.count <= failAt; /* needed fewer allocations: every one was tried */
        } else {
            ASSERT_EQ(st, STATUS_ERR_NO_MEMORY);
        }
        gfxPathFree(&out);
        ASSERT_EQ(sa.live, 0);
    }
    ASSERT_TRUE(done);
    gfxPathFree(&in);
}

/* A subpath that flattens to more than GFX_PATH_MAX_VERBS points stops growing its point arrays at
 * that cap (NO_MEMORY) instead of first buffering every point. */
TEST(gfxStrokeFlattenedPointsAreBounded) {
    GfxPath in;
    gfxPathInit(&in, NULL);
    gfxPathMoveTo(&in, 0.0f, 0.0f);
    for (int i = 0; i < 300; i++) { /* each big loop flattens to the 256-segment cap */
        gfxPathCubicTo(&in, 100000.0f, 100000.0f, -100000.0f, 100000.0f, 0.0f, 0.0f);
    }
    ASSERT_EQ(in.error, STATUS_OK);
    StrokeAlloc sa = {-1, 0, 0, 0, 0};
    GfxAllocator a = {saAlloc, saFree, &sa};
    GfxPath out;
    gfxPathInit(&out, &a);
    GfxStroke s = {2.0f, GFX_CAP_BUTT, GFX_JOIN_BEVEL, 4.0f};
    ASSERT_EQ(gfxStrokeToPath(&in, &s, &out), STATUS_ERR_NO_MEMORY);
    /* x and y arrays of 65536 floats plus the 32768-float pair they grew from (768 KiB); one
     * more doubling would need 1.5 MiB */
    ASSERT_TRUE(sa.peakBytes <= (size_t)1 << 20);
    gfxPathFree(&out);
    ASSERT_EQ(sa.live, 0);
    gfxPathFree(&in);
}
