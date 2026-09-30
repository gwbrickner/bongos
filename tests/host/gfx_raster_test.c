/* Host tests for libs/gfx paths and the exact-area rasterizer (M12.2, D-142/D-143). */
#include "framework/test.h"
#include "gfx/gfx-internal.h"
#include "gfx/gfx-path.h"
#include "gfx/gfx.h"

#include <stdlib.h>
#include <string.h>

static uint32_t rngState = 1;

static void rngSeed(uint32_t s) {
    rngState = s != 0 ? s : 1;
}

static uint32_t rngNext(void) {
    uint32_t x = rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rngState = x;
    return x;
}

typedef struct {
    uint32_t *px;
    int32_t w, h;
    GfxCanvas c;
} Cv;

/* Black opaque canvas. Filling white with GFX_OP_SRC leaves the coverage in the blue channel. */
static bool cvInit(Cv *cv, int32_t w, int32_t h) {
    cv->w = w;
    cv->h = h;
    cv->px = malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (cv->px == NULL) {
        return false;
    }
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        cv->px[i] = 0xFF000000u;
    }
    GfxSurface s = {cv->px, w, h, w};
    return gfxCanvasInit(&cv->c, s, NULL) == STATUS_OK;
}

static void cvFree(Cv *cv) {
    gfxCanvasDestroy(&cv->c);
    free(cv->px);
}

static uint32_t covAt(const Cv *cv, int32_t x, int32_t y) {
    return cv->px[(size_t)y * (size_t)cv->w + (size_t)x] & 0xFFu;
}

/* A polygon path from vertices given in 1/256 pixel units, exactly representable as floats. */
static Status polyPath(GfxPath *p, const int32_t (*v)[2], int n) {
    for (int i = 0; i < n; i++) {
        float x = (float)v[i][0] / 256.0f, y = (float)v[i][1] / 256.0f;
        if (i == 0) {
            gfxPathMoveTo(p, x, y);
        } else {
            gfxPathLineTo(p, x, y);
        }
    }
    return gfxPathClose(p);
}

static Status fillWhite(GfxCanvas *c, const GfxPath *p, GfxFillRule rule) {
    return gfxFillPath(c, p, rule, 0xFFFFFFFFu, GFX_OP_SRC);
}

/* Winding number of the point (px,py) (1/256 units) w.r.t. the closed polygon, exact integers. */
static int winding(const int32_t (*v)[2], int n, int64_t px, int64_t py) {
    int wn = 0;
    for (int i = 0; i < n; i++) {
        int64_t x0 = v[i][0], y0 = v[i][1], x1 = v[(i + 1) % n][0], y1 = v[(i + 1) % n][1];
        int64_t cross = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
        if (y0 <= py) {
            if (y1 > py && cross > 0) {
                wn++;
            }
        } else if (y1 <= py && cross < 0) {
            wn--;
        }
    }
    return wn;
}

/* 16x16 supersampled reference coverage of one pixel, 0..255. */
static int oracleCov(const int32_t (*v)[2], int n, int32_t x, int32_t y, GfxFillRule rule) {
    int inside = 0;
    for (int j = 0; j < 16; j++) {
        for (int i = 0; i < 16; i++) {
            int wn = winding(v, n, (int64_t)x * 256 + i * 16 + 8, (int64_t)y * 256 + j * 16 + 8);
            inside += rule == GFX_FILL_NONZERO ? wn != 0 : (wn & 1) != 0;
        }
    }
    return (inside * 255 + 128) / 256;
}

/* Simple polygons only: star-shaped around a center, vertices at strictly increasing angles from
 * a 12-direction table. (Self-overlapping shapes are a documented limit of cover/area
 * rasterizers: opposite-winding regions overlapping *within one pixel* cancel instead of
 * unioning, so no tight per-pixel oracle exists for them; gfxWindingRules covers those rules.) */
TEST(gfxAaMatchesSupersampleOracle) {
    static const int32_t COS[12] = {1024, 887, 512, 0, -512, -887, -1024, -887, -512, 0, 512, 887};
    static const int32_t SIN[12] = {0, 512, 887, 1024, 887, 512, 0, -512, -887, -1024, -887, -512};
    rngSeed(0xA11CE);
    for (int rule = 0; rule < 2; rule++) {
        for (int iter = 0; iter < 80; iter++) {
            int n = 3 + (int)(rngNext() % 5);
            int32_t cx = (int32_t)(rngNext() % (44 * 256)) - 2 * 256;
            int32_t cy = (int32_t)(rngNext() % (36 * 256)) - 2 * 256;
            int32_t v[8][2];
            int dir = (int)(rngNext() % 12);
            for (int i = 0; i < n; i++) {
                int step = 12 / n; /* strictly increasing angles, whole polygon within 360 deg */
                int k = (dir + i * (step > 0 ? step : 1)) % 12;
                int32_t r = (int32_t)(2 * 256 + rngNext() % (14 * 256));
                v[i][0] = cx + (int32_t)(((int64_t)r * COS[k]) / 1024);
                v[i][1] = cy + (int32_t)(((int64_t)r * SIN[k]) / 1024);
            }
            Cv cv;
            ASSERT_TRUE(cvInit(&cv, 40, 32));
            GfxPath p;
            gfxPathInit(&p, NULL);
            ASSERT_EQ(polyPath(&p, (const int32_t(*)[2])v, n), STATUS_OK);
            GfxFillRule fr = rule ? GFX_FILL_EVENODD : GFX_FILL_NONZERO;
            ASSERT_EQ(fillWhite(&cv.c, &p, fr), STATUS_OK);
            long sumErr = 0;
            for (int32_t y = 0; y < 32; y++) {
                for (int32_t x = 0; x < 40; x++) {
                    int want = oracleCov((const int32_t(*)[2])v, n, x, y, fr);
                    int got = (int)covAt(&cv, x, y);
                    int d = got > want ? got - want : want - got;
                    ASSERT_TRUE(d <= 17);
                    sumErr += d;
                }
            }
            ASSERT_TRUE(sumErr <= 1280); /* mean abs error <= 1.0 per pixel */
            gfxPathFree(&p);
            cvFree(&cv);
        }
    }
}

/* An axis-aligned rect with fractional edges: coverage is exactly overlapX * overlapY. */
TEST(gfxAaRectExactCoverage) {
    /* 1/256-multiples: x in [1.25, 5.5), y in [2.5, 4.75) */
    int32_t x0 = 320, x1 = 1408, y0 = 640, y1 = 1216;
    Cv cv;
    ASSERT_TRUE(cvInit(&cv, 8, 8));
    GfxPath p;
    gfxPathInit(&p, NULL);
    ASSERT_EQ(gfxPathAddRect(&p, (float)x0 / 256.0f, (float)y0 / 256.0f, (float)(x1 - x0) / 256.0f,
                             (float)(y1 - y0) / 256.0f),
              STATUS_OK);
    ASSERT_EQ(fillWhite(&cv.c, &p, GFX_FILL_NONZERO), STATUS_OK);
    for (int32_t y = 0; y < 8; y++) {
        for (int32_t x = 0; x < 8; x++) {
            int32_t ox = (x * 256 + 256 < x1 ? x * 256 + 256 : x1) - (x * 256 > x0 ? x * 256 : x0);
            int32_t oy = (y * 256 + 256 < y1 ? y * 256 + 256 : y1) - (y * 256 > y0 ? y * 256 : y0);
            ox = ox < 0 ? 0 : ox;
            oy = oy < 0 ? 0 : oy;
            uint32_t want = (uint32_t)(((int64_t)ox * oy * 2 * 255 + 65536) / 131072);
            ASSERT_EQ(covAt(&cv, x, y), want);
        }
    }
    gfxPathFree(&p);
    cvFree(&cv);
}

/* Pixel-aligned shapes have coverage exactly 0 or 255, with no bleed. */
TEST(gfxAaAlignedRectHasNoFringe) {
    Cv cv;
    ASSERT_TRUE(cvInit(&cv, 10, 10));
    GfxPath p;
    gfxPathInit(&p, NULL);
    ASSERT_EQ(gfxPathAddRect(&p, 2, 3, 5, 4), STATUS_OK);
    ASSERT_EQ(fillWhite(&cv.c, &p, GFX_FILL_NONZERO), STATUS_OK);
    for (int32_t y = 0; y < 10; y++) {
        for (int32_t x = 0; x < 10; x++) {
            bool in = x >= 2 && x < 7 && y >= 3 && y < 7;
            ASSERT_EQ(covAt(&cv, x, y), in ? 255u : 0u);
        }
    }
    gfxPathFree(&p);
    cvFree(&cv);
}

/* Total coverage tracks the shoelace area. */
TEST(gfxAaCoverageSumMatchesArea) {
    rngSeed(0xC0DE);
    for (int iter = 0; iter < 80; iter++) {
        /* a random convex-ish quad well inside the canvas */
        int32_t v[4][2];
        int32_t cx = 20 * 256, cy = 16 * 256;
        for (int i = 0; i < 4; i++) {
            int32_t r = (int32_t)(3 * 256 + rngNext() % (9 * 256));
            int32_t dx[4] = {r, 0, -r, 0}, dy[4] = {0, r, 0, -r};
            v[i][0] = cx + dx[i] + (int32_t)(rngNext() % 256);
            v[i][1] = cy + dy[i] + (int32_t)(rngNext() % 256);
        }
        int64_t twiceArea = 0;
        for (int i = 0; i < 4; i++) {
            twiceArea +=
                (int64_t)v[i][0] * v[(i + 1) % 4][1] - (int64_t)v[(i + 1) % 4][0] * v[i][1];
        }
        double area = (double)(twiceArea < 0 ? -twiceArea : twiceArea) / 2.0 / 65536.0;
        Cv cv;
        ASSERT_TRUE(cvInit(&cv, 40, 32));
        GfxPath p;
        gfxPathInit(&p, NULL);
        ASSERT_EQ(polyPath(&p, (const int32_t(*)[2])v, 4), STATUS_OK);
        ASSERT_EQ(fillWhite(&cv.c, &p, GFX_FILL_NONZERO), STATUS_OK);
        double sum = 0;
        for (int32_t y = 0; y < 32; y++) {
            for (int32_t x = 0; x < 40; x++) {
                sum += covAt(&cv, x, y) / 255.0;
            }
        }
        double err = sum > area ? sum - area : area - sum;
        ASSERT_TRUE(err <= 0.5 + area * 0.005);
        gfxPathFree(&p);
        cvFree(&cv);
    }
}

/* Drawing at origin (dx,dy) is the same image as drawing at 0 shifted by (dx,dy). */
TEST(gfxTranslationInvariance) {
    int32_t v[5][2] = {{300, 200}, {5000, 900}, {3500, 6000}, {700, 4400}, {2500, 2600}};
    Cv a, b;
    ASSERT_TRUE(cvInit(&a, 40, 30));
    ASSERT_TRUE(cvInit(&b, 40, 30));
    GfxPath p;
    gfxPathInit(&p, NULL);
    ASSERT_EQ(polyPath(&p, (const int32_t(*)[2])v, 5), STATUS_OK);
    ASSERT_EQ(fillWhite(&a.c, &p, GFX_FILL_EVENODD), STATUS_OK);
    gfxCanvasSetOrigin(&b.c, 9, 7);
    ASSERT_EQ(fillWhite(&b.c, &p, GFX_FILL_EVENODD), STATUS_OK);
    for (int32_t y = 0; y < 30; y++) {
        for (int32_t x = 0; x < 40; x++) {
            uint32_t want = (x >= 9 && y >= 7) ? covAt(&a, x - 9, y - 7) : 0;
            if (x >= 9 && y >= 7 && x - 9 < 40 && y - 7 < 30) {
                ASSERT_EQ(covAt(&b, x, y), want);
            }
        }
    }
    gfxPathFree(&p);
    cvFree(&a);
    cvFree(&b);
}

TEST(gfxWindingRules) {
    /* two nested squares, same direction: nonzero fills the hole, even-odd doesn't */
    Cv nz, eo, opp;
    ASSERT_TRUE(cvInit(&nz, 20, 20));
    ASSERT_TRUE(cvInit(&eo, 20, 20));
    ASSERT_TRUE(cvInit(&opp, 20, 20));
    GfxPath same, rev;
    gfxPathInit(&same, NULL);
    gfxPathInit(&rev, NULL);
    ASSERT_EQ(gfxPathAddRect(&same, 2, 2, 16, 16), STATUS_OK);
    ASSERT_EQ(gfxPathAddRect(&same, 6, 6, 8, 8), STATUS_OK);
    ASSERT_EQ(gfxPathAddRect(&rev, 2, 2, 16, 16), STATUS_OK);
    /* the inner square wound the other way */
    gfxPathMoveTo(&rev, 6, 6);
    gfxPathLineTo(&rev, 6, 14);
    gfxPathLineTo(&rev, 14, 14);
    gfxPathLineTo(&rev, 14, 6);
    ASSERT_EQ(gfxPathClose(&rev), STATUS_OK);
    ASSERT_EQ(fillWhite(&nz.c, &same, GFX_FILL_NONZERO), STATUS_OK);
    ASSERT_EQ(fillWhite(&eo.c, &same, GFX_FILL_EVENODD), STATUS_OK);
    ASSERT_EQ(fillWhite(&opp.c, &rev, GFX_FILL_NONZERO), STATUS_OK);
    ASSERT_EQ(covAt(&nz, 10, 10), 255u);
    ASSERT_EQ(covAt(&eo, 10, 10), 0u);
    ASSERT_EQ(covAt(&opp, 10, 10), 0u);
    ASSERT_EQ(covAt(&eo, 3, 3), 255u);
    ASSERT_EQ(covAt(&opp, 3, 3), 255u);
    ASSERT_EQ(covAt(&nz, 0, 0), 0u);
    /* reversing the whole outline changes nothing for nonzero */
    Cv cw, ccw;
    ASSERT_TRUE(cvInit(&cw, 20, 20));
    ASSERT_TRUE(cvInit(&ccw, 20, 20));
    GfxPath a, b;
    gfxPathInit(&a, NULL);
    gfxPathInit(&b, NULL);
    gfxPathMoveTo(&a, 2.25f, 2);
    gfxPathLineTo(&a, 17, 5.5f);
    gfxPathLineTo(&a, 8, 16.75f);
    gfxPathClose(&a);
    gfxPathMoveTo(&b, 2.25f, 2);
    gfxPathLineTo(&b, 8, 16.75f);
    gfxPathLineTo(&b, 17, 5.5f);
    gfxPathClose(&b);
    ASSERT_EQ(fillWhite(&cw.c, &a, GFX_FILL_NONZERO), STATUS_OK);
    ASSERT_EQ(fillWhite(&ccw.c, &b, GFX_FILL_NONZERO), STATUS_OK);
    ASSERT_EQ(memcmp(cw.px, ccw.px, 20 * 20 * sizeof(uint32_t)), 0);
    gfxPathFree(&same);
    gfxPathFree(&rev);
    gfxPathFree(&a);
    gfxPathFree(&b);
    cvFree(&nz);
    cvFree(&eo);
    cvFree(&opp);
    cvFree(&cw);
    cvFree(&ccw);
}

/* Filling under a canvas clip equals the full fill restricted to the clip. */
TEST(gfxFillPathRespectsClip) {
    int32_t v[4][2] = {{-1000, 500}, {9000, -300}, {8000, 7000}, {200, 6500}};
    Cv full, clipped;
    ASSERT_TRUE(cvInit(&full, 40, 30));
    ASSERT_TRUE(cvInit(&clipped, 40, 30));
    GfxPath p;
    gfxPathInit(&p, NULL);
    ASSERT_EQ(polyPath(&p, (const int32_t(*)[2])v, 4), STATUS_OK);
    ASSERT_EQ(fillWhite(&full.c, &p, GFX_FILL_NONZERO), STATUS_OK);
    ASSERT_EQ(gfxCanvasPushClip(&clipped.c, (GfxRect){7, 5, 29, 21}), STATUS_OK);
    ASSERT_EQ(fillWhite(&clipped.c, &p, GFX_FILL_NONZERO), STATUS_OK);
    for (int32_t y = 0; y < 30; y++) {
        for (int32_t x = 0; x < 40; x++) {
            bool in = x >= 7 && x < 29 && y >= 5 && y < 21;
            ASSERT_EQ(covAt(&clipped, x, y), in ? covAt(&full, x, y) : 0u);
        }
    }
    gfxPathFree(&p);
    cvFree(&full);
    cvFree(&clipped);
}

/* The band buffer shrinks on a very wide clip (fewer rows per band); rows must not depend on
 * the band height. */
TEST(gfxRasterBandHeightDoesNotChangeOutput) {
    int32_t v[4][2] = {{300, 200}, {24000, 3000}, {20000, 9800}, {100, 7000}};
    GfxPath p;
    gfxPathInit(&p, NULL);
    ASSERT_EQ(polyPath(&p, (const int32_t(*)[2])v, 4), STATUS_OK);
    Cv narrow, wide;
    ASSERT_TRUE(cvInit(&narrow, 100, 40));
    ASSERT_TRUE(cvInit(&wide, 30000, 40)); /* band height = 131072/30000 = 4 rows, not 16 */
    ASSERT_EQ(fillWhite(&narrow.c, &p, GFX_FILL_NONZERO), STATUS_OK);
    ASSERT_EQ(fillWhite(&wide.c, &p, GFX_FILL_NONZERO), STATUS_OK);
    for (int32_t y = 0; y < 40; y++) {
        ASSERT_EQ(memcmp(&narrow.px[(size_t)y * 100], &wide.px[(size_t)y * 30000],
                         100 * sizeof(uint32_t)),
                  0);
    }
    gfxPathFree(&p);
    cvFree(&narrow);
    cvFree(&wide);
}

TEST(gfxPathRejectsBadInput) {
    GfxPath p;
    gfxPathInit(&p, NULL);
    ASSERT_EQ(gfxPathLineTo(&p, 1, 1), STATUS_ERR_INVALID); /* no current point */
    gfxPathReset(&p);
    ASSERT_EQ(gfxPathMoveTo(&p, 0, 0), STATUS_OK);
    ASSERT_EQ(gfxPathLineTo(&p, __builtin_nanf(""), 1), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxPathLineTo(&p, 1, 1), STATUS_ERR_INVALID); /* sticky */
    Cv cv;
    ASSERT_TRUE(cvInit(&cv, 8, 8));
    ASSERT_EQ(fillWhite(&cv.c, &p, GFX_FILL_NONZERO), STATUS_ERR_INVALID);
    ASSERT_EQ(covAt(&cv, 4, 4), 0u);
    gfxPathReset(&p);
    ASSERT_EQ(gfxPathMoveTo(&p, __builtin_inff(), 0), STATUS_ERR_INVALID);
    gfxPathReset(&p);
    ASSERT_EQ(gfxPathClose(&p), STATUS_ERR_INVALID);
    gfxPathFree(&p);
    cvFree(&cv);
}

TEST(gfxFillPathHugeCoordinatesAreSafe) {
    float big[] = {1e30f, -1e30f, 3.0e38f, 1048576.0f, -1048576.0f, 0.0f, 65536.0f};
    Cv cv;
    ASSERT_TRUE(cvInit(&cv, 32, 32));
    rngSeed(31337);
    for (int iter = 0; iter < 300; iter++) {
        GfxPath p;
        gfxPathInit(&p, NULL);
        for (int i = 0; i < 4; i++) {
            float x = (rngNext() & 1u) ? big[rngNext() % 7] : (float)(rngNext() % 64) - 16.0f;
            float y = (rngNext() & 1u) ? big[rngNext() % 7] : (float)(rngNext() % 64) - 16.0f;
            if (i == 0) {
                gfxPathMoveTo(&p, x, y);
            } else {
                gfxPathLineTo(&p, x, y);
            }
        }
        gfxPathClose(&p);
        gfxCanvasSetOrigin(&cv.c, (int32_t)(rngNext() % 200) - 100,
                           (rngNext() & 1u) ? INT32_MAX : INT32_MIN);
        (void)gfxFillPath(&cv.c, &p, (rngNext() & 1u) ? GFX_FILL_EVENODD : GFX_FILL_NONZERO,
                          0xFFFFFFFFu, GFX_OP_SRC_OVER);
        gfxPathFree(&p);
    }
    cvFree(&cv);
}

TEST(gfxFillPathTooManyEdgesLeavesCanvasUntouched) {
    Cv cv;
    ASSERT_TRUE(cvInit(&cv, 64, 64));
    GfxPath p;
    gfxPathInit(&p, NULL);
    gfxPathMoveTo(&p, 0, 0);
    for (int i = 0; i < 9000; i++) { /* a zig-zag: every segment is a non-horizontal edge */
        gfxPathLineTo(&p, (float)(i & 1) * 60.0f + 2.0f, (float)(i % 60) + 1.0f);
    }
    gfxPathClose(&p);
    ASSERT_EQ(fillWhite(&cv.c, &p, GFX_FILL_NONZERO), STATUS_ERR_UNSUPPORTED);
    for (size_t i = 0; i < 64 * 64; i++) {
        ASSERT_EQ(cv.px[i], (uint32_t)0xFF000000u);
    }
    gfxPathFree(&p);
    cvFree(&cv);
}

/* An allocator that fails the Nth allocation. */
typedef struct {
    int failAt, count;
    int live;
} FailAlloc;

static void *failAlloc(void *ctx, size_t n) {
    FailAlloc *f = ctx;
    if (f->count++ == f->failAt) {
        return NULL;
    }
    f->live++;
    return malloc(n != 0 ? n : 1);
}

static void failFree(void *ctx, void *p, size_t n) {
    (void)n;
    FailAlloc *f = ctx;
    f->live--;
    free(p);
}

TEST(gfxFillPathAllocationFailureIsCleanNoMemory) {
    for (int failAt = 0; failAt < 8; failAt++) {
        FailAlloc fa = {failAt, 0, 0};
        GfxAllocator ga = {failAlloc, failFree, &fa};
        uint32_t px[32 * 32];
        for (int i = 0; i < 32 * 32; i++) {
            px[i] = 0xFF000000u;
        }
        GfxSurface s = {px, 32, 32, 32};
        GfxCanvas c;
        ASSERT_EQ(gfxCanvasInit(&c, s, &ga), STATUS_OK);
        GfxPath p;
        gfxPathInit(&p, &ga);
        Status st = gfxPathAddRoundedRect(&p, 3, 3, 20, 20, 5);
        if (st == STATUS_OK) {
            st = gfxFillPath(&c, &p, GFX_FILL_NONZERO, 0xFFFFFFFFu, GFX_OP_SRC);
        }
        ASSERT_TRUE(st == STATUS_OK || st == STATUS_ERR_NO_MEMORY);
        gfxPathFree(&p);
        gfxCanvasDestroy(&c);
        ASSERT_EQ(fa.live, 0); /* nothing leaked on any path */
    }
}

TEST(gfxFillPathMaskUnion) {
    uint8_t md[16 * 16];
    memset(md, 0, sizeof(md));
    GfxMask m = {md, 16, 16, 16};
    GfxPath p;
    gfxPathInit(&p, NULL);
    ASSERT_EQ(gfxPathAddRect(&p, 2, 2, 8, 8), STATUS_OK);
    ASSERT_EQ(gfxFillPathMask(&m, &p, GFX_FILL_NONZERO, NULL), STATUS_OK);
    ASSERT_EQ(md[5 * 16 + 5], (uint8_t)255);
    ASSERT_EQ(md[0], (uint8_t)0);
    /* a second half-covered pass unions: 255 stays 255, an empty pixel gets the new coverage */
    gfxPathReset(&p);
    ASSERT_EQ(gfxPathAddRect(&p, 9.5f, 2, 4, 4), STATUS_OK);
    ASSERT_EQ(gfxFillPathMask(&m, &p, GFX_FILL_NONZERO, NULL), STATUS_OK);
    ASSERT_EQ(md[3 * 16 + 9], (uint8_t)255);
    ASSERT_EQ(md[3 * 16 + 12], (uint8_t)255);
    gfxPathFree(&p);
    GfxMask bad = {NULL, 4, 4, 4};
    gfxPathInit(&p, NULL);
    ASSERT_EQ(gfxFillPathMask(&bad, &p, GFX_FILL_NONZERO, NULL), STATUS_ERR_INVALID);
    gfxPathFree(&p);
}
