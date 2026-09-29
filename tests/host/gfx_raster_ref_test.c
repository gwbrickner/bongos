/* Adversarial and differential tests for libs/gfx's rasterizer and fills (M12.2, D-142/D-143),
 * added by the bug-sweeper step pass over sub-steps 2-4.
 *
 * The main oracle here is independent of raster.c's algorithm: the exact area integral of the
 * winding number over each pixel, from Sutherland-Hodgman clipping of the polygon to the pixel
 * square (in double; every clip preserves the winding number inside the half-plane and zeroes it
 * outside, so the clipped polygon's signed area is that integral even for self-intersecting
 * polygons). That integral is exactly what a cover/area rasterizer computes before the fill rule
 * (D-143's "signed sum" limit), so the two agree up to raster.c's 1/256 px integer truncation. */
#pragma STDC FP_CONTRACT OFF /* bezFixed mirrors flatten.c's float evaluation */

#include "framework/test.h"
#include "gfx/gfx-internal.h"
#include "gfx/gfx-path.h"
#include "gfx/gfx.h"

#include <float.h>
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
    double x, y;
} Pt;

#define MAX_POLY 16
#define MAX_CLIP 512

/* One Sutherland-Hodgman pass: keeps the side of the line p[axis] = bound given by `keepAbove`
 * (p[axis] >= bound) or below (p[axis] <= bound). */
static int clipHalf(const Pt *in, int n, Pt *out, int axis, double bound, bool keepAbove) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        Pt s = in[i], e = in[(i + 1) % n];
        double sv = axis == 0 ? s.x : s.y, ev = axis == 0 ? e.x : e.y;
        bool sIn = keepAbove ? sv >= bound : sv <= bound;
        bool eIn = keepAbove ? ev >= bound : ev <= bound;
        if (sIn != eIn) {
            double t = (bound - sv) / (ev - sv);
            Pt c = {s.x + (e.x - s.x) * t, s.y + (e.y - s.y) * t};
            if (axis == 0) {
                c.x = bound;
            } else {
                c.y = bound;
            }
            out[m++] = c;
        }
        if (eIn) {
            out[m++] = e;
        }
    }
    return m;
}

/* Integral of the winding number over the pixel [x,x+1) x [y,y+1), in pixels. */
static double windingIntegral(const Pt *poly, int n, int32_t x, int32_t y) {
    Pt a[MAX_CLIP], b[MAX_CLIP];
    int m = clipHalf(poly, n, a, 0, (double)x, true);
    m = clipHalf(a, m, b, 0, (double)x + 1.0, false);
    m = clipHalf(b, m, a, 1, (double)y, true);
    m = clipHalf(a, m, b, 1, (double)y + 1.0, false);
    double s = 0.0;
    for (int i = 0; i < m; i++) {
        s += b[i].x * b[(i + 1) % m].y - b[(i + 1) % m].x * b[i].y;
    }
    return s / 2.0;
}

/* Whether the non-horizontal segment a-b meets the box [x0,x1] x [y0,y1] (Liang-Barsky). */
static bool segMeetsBox(Pt a, Pt b, double x0, double y0, double x1, double y1) {
    double dx = b.x - a.x, dy = b.y - a.y, t0 = 0.0, t1 = 1.0;
    double p[4] = {-dx, dx, -dy, dy}, q[4] = {a.x - x0, x1 - a.x, a.y - y0, y1 - a.y};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0) {
            if (q[i] < 0.0) {
                return false;
            }
            continue;
        }
        double r = q[i] / p[i];
        if (p[i] < 0.0) {
            if (r > t1) {
                return false;
            }
            t0 = r > t0 ? r : t0;
        } else {
            if (r < t0) {
                return false;
            }
            t1 = r < t1 ? r : t1;
        }
    }
    return true;
}

/* How many non-horizontal polygon edges come within 1/128 px of the pixel: each one's crossing
 * points are truncated to the 1/256 grid by raster.c, the only source of error. */
static int edgesNearPixel(const Pt *poly, int n, int32_t x, int32_t y) {
    const double m = 1.0 / 128.0;
    int k = 0;
    for (int i = 0; i < n; i++) {
        Pt a = poly[i], b = poly[(i + 1) % n];
        if (a.y != b.y && segMeetsBox(a, b, x - m, y - m, x + 1 + m, y + 1 + m)) {
            k++;
        }
    }
    return k;
}

/* The fill rule applied to the winding integral, as 0..255 (round half up). */
static int refCoverage(double w, GfxFillRule rule) {
    double m = w < 0 ? -w : w;
    if (rule == GFX_FILL_NONZERO) {
        m = m > 1.0 ? 1.0 : m;
    } else {
        while (m >= 2.0) {
            m -= 2.0;
        }
        m = m > 1.0 ? 2.0 - m : m;
    }
    return (int)(m * 255.0 + 0.5);
}

static void buildPath(GfxPath *p, const Pt *v, int n) {
    for (int i = 0; i < n; i++) {
        if (i == 0) {
            gfxPathMoveTo(p, (float)v[i].x, (float)v[i].y);
        } else {
            gfxPathLineTo(p, (float)v[i].x, (float)v[i].y);
        }
    }
    gfxPathClose(p);
}

/* A vertex on the 1/256 grid (exact as a float), mostly near the mask, sometimes far off it, and
 * sometimes a whole-pixel coordinate up to +-2^20 (still exact as a float). */
static double randCoord(int32_t extent) {
    uint32_t k = rngNext() % 16;
    if (k == 0) {
        static const double FAR[] = {-1048576.0, 1048576.0, -70000.0, 70000.0, -300.0, 300.0};
        return FAR[rngNext() % 6];
    }
    return (double)((int32_t)(rngNext() % (uint32_t)((extent + 16) * 256)) - 8 * 256) / 256.0;
}

/* Random polygons, self-intersecting ones included, vertices up to 2^20 px away, both fill rules:
 * a pixel no edge comes near is exact, and one that k edges come near is within (1+k)/255 of the
 * exact winding integral (each edge's crossings are truncated to 1/256 px; over 20000 polygons
 * the worst seen was 2 for k = 1 and 3 for k = 2..4). */
TEST(gfxRasterMatchesExactWindingIntegral) {
    enum { W = 40, H = 34 };
    static uint8_t md[W * H];
    rngSeed(0x5EED1234);
    int worst = 0;
    for (int iter = 0; iter < 1500; iter++) {
        int n = 3 + (int)(rngNext() % (MAX_POLY - 2));
        Pt v[MAX_POLY];
        for (int i = 0; i < n; i++) {
            v[i].x = randCoord(W);
            v[i].y = randCoord(H);
        }
        GfxFillRule rule = (iter & 1) ? GFX_FILL_EVENODD : GFX_FILL_NONZERO;
        GfxPath p;
        gfxPathInit(&p, NULL);
        buildPath(&p, v, n);
        memset(md, 0, sizeof(md));
        GfxMask m = {md, W, H, W};
        ASSERT_EQ(gfxFillPathMask(&m, &p, rule, NULL), STATUS_OK);
        gfxPathFree(&p);
        for (int32_t y = 0; y < H; y++) {
            for (int32_t x = 0; x < W; x++) {
                int want = refCoverage(windingIntegral(v, n, x, y), rule);
                int got = md[y * W + x];
                int d = got > want ? got - want : want - got;
                int k = edgesNearPixel(v, n, x, y);
                worst = d > worst ? d : worst;
                if (d > (k == 0 ? 0 : 1 + k)) {
                    fprintf(stderr, "  iter %d pixel (%d,%d), %d edges near: got %d want %d\n",
                            iter, x, y, k, got, want);
                }
                ASSERT_TRUE(d <= (k == 0 ? 0 : 1 + k));
            }
        }
    }
    printf("  worst |raster - exact| = %d/255\n", worst);
}

/* Rows are independent (the cover/area sums restart every row) and the band origin is only a
 * translation by whole cells: the full fill, a fill clipped to each single row, and fills under
 * random sub-clips (another band origin, width and height) agree exactly on every shared pixel. */
typedef struct {
    uint8_t *cov;
    int32_t w, h;
    int32_t calls;
} Grid;

static void gridSpan(void *ctx, int32_t y, int32_t x, const uint8_t *cov, int32_t n) {
    Grid *g = ctx;
    g->calls++;
    if (y < 0 || y >= g->h || x < 0 || x + n > g->w) {
        g->calls = -1000000; /* a span outside the clip: fails the test below */
        return;
    }
    memcpy(g->cov + (size_t)y * (size_t)g->w + (size_t)x, cov, (size_t)n);
}

TEST(gfxRasterBandsAndClipsAgreeWithFullFill) {
    enum { W = 300, H = 70 };
    uint8_t *full = calloc(W * H, 1), *part = calloc(W * H, 1);
    ASSERT_TRUE(full != NULL && part != NULL);
    rngSeed(0xBA4D5);
    for (int iter = 0; iter < 60; iter++) {
        int n = 3 + (int)(rngNext() % 10);
        Pt v[MAX_POLY];
        for (int i = 0; i < n; i++) {
            v[i].x = randCoord(W);
            v[i].y = randCoord(H);
        }
        GfxPath p;
        gfxPathInit(&p, NULL);
        buildPath(&p, v, n);
        GfxRect all = {0, 0, W, H};
        GfxEdgeList l;
        gfxEdgeListInit(&l, NULL, all);
        ASSERT_EQ(gfxFlattenPath(&p, 0, 0, &l), STATUS_OK);
        GfxFillRule rule = (iter & 1) ? GFX_FILL_EVENODD : GFX_FILL_NONZERO;
        void *scratch = NULL;
        size_t scratchSize = 0;
        const GfxAllocator *a = gfxAllocatorDefault();
        memset(full, 0, W * H);
        Grid g = {full, W, H, 0};
        ASSERT_EQ(
            gfxRasterFill(a, &scratch, &scratchSize, l.edges, l.count, all, rule, gridSpan, &g),
            STATUS_OK);
        ASSERT_TRUE(g.calls >= 0);
        /* one row at a time */
        memset(part, 0, W * H);
        Grid r = {part, W, H, 0};
        for (int32_t y = 0; y < H; y++) {
            ASSERT_EQ(gfxRasterFill(a, &scratch, &scratchSize, l.edges, l.count,
                                    (GfxRect){0, y, W, y + 1}, rule, gridSpan, &r),
                      STATUS_OK);
        }
        ASSERT_TRUE(r.calls >= 0);
        ASSERT_EQ(memcmp(full, part, W * H), 0);
        /* random sub-clips: exactly the full fill inside, untouched outside */
        for (int k = 0; k < 8; k++) {
            int32_t x0 = (int32_t)(rngNext() % W), x1 = (int32_t)(rngNext() % (W + 1));
            int32_t y0 = (int32_t)(rngNext() % H), y1 = (int32_t)(rngNext() % (H + 1));
            GfxRect c = {x0, y0, x1, y1};
            memset(part, 0, W * H);
            Grid s = {part, W, H, 0};
            ASSERT_EQ(
                gfxRasterFill(a, &scratch, &scratchSize, l.edges, l.count, c, rule, gridSpan, &s),
                STATUS_OK);
            ASSERT_TRUE(s.calls >= 0);
            for (int32_t y = 0; y < H; y++) {
                for (int32_t x = 0; x < W; x++) {
                    bool in = x >= x0 && x < x1 && y >= y0 && y < y1;
                    ASSERT_EQ(part[y * W + x], in ? full[y * W + x] : (uint8_t)0);
                }
            }
        }
        a->free(a->ctx, scratch, scratchSize);
        gfxEdgeListFree(&l);
        gfxPathFree(&p);
    }
    free(full);
    free(part);
}

/* The same polygon on canvases of different widths (band heights 16, 16, 8, 2 rows) and under a
 * canvas clip at a random offset gives identical pixels in the shared region. */
TEST(gfxFillPathSameAtEveryBandHeight) {
    static const int32_t WIDTHS[] = {37, 8192, 16384, 65536};
    rngSeed(0xB00B5);
    for (int iter = 0; iter < 6; iter++) {
        int n = 3 + (int)(rngNext() % 10);
        Pt v[MAX_POLY];
        for (int i = 0; i < n; i++) {
            v[i].x = randCoord(37);
            v[i].y = randCoord(40);
        }
        GfxPath p;
        gfxPathInit(&p, NULL);
        buildPath(&p, v, n);
        uint32_t *ref = NULL;
        for (int k = 0; k < 4; k++) {
            int32_t w = WIDTHS[k], h = 40;
            uint32_t *px = malloc((size_t)w * h * sizeof(uint32_t));
            ASSERT_TRUE(px != NULL);
            for (size_t i = 0; i < (size_t)w * h; i++) {
                px[i] = 0xFF000000u;
            }
            GfxCanvas c;
            ASSERT_EQ(gfxCanvasInit(&c, (GfxSurface){px, w, h, w}, NULL), STATUS_OK);
            ASSERT_EQ(gfxFillPath(&c, &p, (iter & 1) ? GFX_FILL_EVENODD : GFX_FILL_NONZERO,
                                  0xFFFFFFFFu, GFX_OP_SRC),
                      STATUS_OK);
            /* D-143: at most 16 rows per band, at most 1 MiB of cover+area, plus one row */
            ASSERT_TRUE(c.scratchSize <= 16 * (size_t)w * 8 + (size_t)w);
            ASSERT_TRUE(c.scratchSize <= 1048576u + (size_t)w);
            gfxCanvasDestroy(&c);
            if (k == 0) {
                ref = px;
                continue;
            }
            for (int32_t y = 0; y < h; y++) {
                ASSERT_EQ(
                    memcmp(&ref[(size_t)y * 37], &px[(size_t)y * (size_t)w], 37 * sizeof(uint32_t)),
                    0);
            }
            free(px);
        }
        free(ref);
        gfxPathFree(&p);
    }
}

/* A wide fill: the band buffers stay within D-143's 1 MiB cap (plus one coverage row). */
TEST(gfxFillPathBandBufferCap) {
    int32_t w = 65536, h = 20;
    uint32_t *px = calloc((size_t)w * h, sizeof(uint32_t));
    ASSERT_TRUE(px != NULL);
    GfxCanvas c;
    ASSERT_EQ(gfxCanvasInit(&c, (GfxSurface){px, w, h, w}, NULL), STATUS_OK);
    GfxPath p;
    gfxPathInit(&p, NULL);
    ASSERT_EQ(gfxPathAddRect(&p, -5.5f, 0.25f, 70000.0f, 19.0f), STATUS_OK);
    ASSERT_EQ(gfxFillPath(&c, &p, GFX_FILL_NONZERO, 0xFFFFFFFFu, GFX_OP_SRC), STATUS_OK);
    ASSERT_TRUE(c.scratchSize <= 1048576u + (size_t)w);
    /* the whole width is covered, including the last column (x = W-1), and row 0 is 3/4 */
    ASSERT_EQ(px[5 * (size_t)w + (size_t)w - 1], 0xFFFFFFFFu);
    ASSERT_EQ(px[5 * (size_t)w], 0xFFFFFFFFu);
    ASSERT_EQ(px[(size_t)w - 1] >> 24, 191u);
    gfxPathFree(&p);
    gfxCanvasDestroy(&c);
    free(px);
}

/* A pixel-aligned rect path fills exactly the pixels gfxFillRect does, for integer coordinates up
 * to +-2^20 (GFX_COORD_MAX), any origin (INT32 extremes included), and random clips. */
TEST(gfxFillPathRectEqualsFillRect) {
    enum { W = 48, H = 40 };
    static const int32_t COORD[] = {-1048576, -1048575, -65536, -1, 0, 1, 47, 48, 1048575, 1048576};
    static const int32_t ORIG[] = {INT32_MIN, INT32_MIN + 1, -1048600, -1048576,      -4194304, 0,
                                   1048576,   1048600,       4194304,  INT32_MAX - 1, INT32_MAX};
    uint32_t a[W * H], b[W * H];
    rngSeed(0xF111);
    for (int iter = 0; iter < 3000; iter++) {
        int32_t v[4];
        for (int i = 0; i < 4; i++) {
            v[i] = (rngNext() & 3u) == 0 ? COORD[rngNext() % 10] : (int32_t)(rngNext() % 70) - 10;
        }
        int32_t ox = (rngNext() & 3u) == 0 ? ORIG[rngNext() % 11] : (int32_t)(rngNext() % 40) - 20;
        int32_t oy = (rngNext() & 3u) == 0 ? ORIG[rngNext() % 11] : (int32_t)(rngNext() % 40) - 20;
        /* an origin that brings a far coordinate back on screen */
        if ((rngNext() & 7u) == 0) {
            ox = -v[0] + (int32_t)(rngNext() % 20);
        }
        GfxRect r = {v[0], v[1], v[2], v[3]};
        if (gfxRectIsEmpty(r)) {
            continue;
        }
        GfxCanvas ca, cb;
        for (int i = 0; i < W * H; i++) {
            a[i] = b[i] = 0xFF000000u;
        }
        ASSERT_EQ(gfxCanvasInit(&ca, (GfxSurface){a, W, H, W}, NULL), STATUS_OK);
        ASSERT_EQ(gfxCanvasInit(&cb, (GfxSurface){b, W, H, W}, NULL), STATUS_OK);
        gfxCanvasSetOrigin(&ca, ox, oy);
        gfxCanvasSetOrigin(&cb, ox, oy);
        if (rngNext() & 1u) {
            int32_t cx = (int32_t)(rngNext() % 60) - 6, cy = (int32_t)(rngNext() % 50) - 5;
            GfxRect cr = {cx, cy, cx + (int32_t)(rngNext() % 40), cy + (int32_t)(rngNext() % 40)};
            ASSERT_EQ(gfxCanvasPushClip(&ca, cr), STATUS_OK);
            ASSERT_EQ(gfxCanvasPushClip(&cb, cr), STATUS_OK);
        }
        gfxFillRect(&ca, r, 0xFFFFFFFFu, GFX_OP_SRC);
        GfxPath p;
        gfxPathInit(&p, NULL);
        ASSERT_EQ(gfxPathAddRect(&p, (float)r.x0, (float)r.y0, (float)((int64_t)r.x1 - r.x0),
                                 (float)((int64_t)r.y1 - r.y0)),
                  STATUS_OK);
        ASSERT_EQ(gfxFillPath(&cb, &p, GFX_FILL_NONZERO, 0xFFFFFFFFu, GFX_OP_SRC), STATUS_OK);
        gfxPathFree(&p);
        gfxCanvasDestroy(&ca);
        gfxCanvasDestroy(&cb);
        ASSERT_EQ(memcmp(a, b, sizeof(a)), 0);
    }
}

/* toFixed rounds to the nearest 1/256 px exactly: a coordinate already on the grid is never
 * moved, whatever its magnitude, so drawing at x + k equals drawing at x shifted by an origin of
 * k. (Float `v*256 + 0.5` rounds 0.49999997 up to 1, and an odd grid value in [2^23, 2^24) up to
 * the next even one.) */
TEST(gfxFlattenRoundsExactlyToGrid) {
    /* x = 32768 + 1/256 px (fixed 2^23 + 1, odd) vs the same edge at 8 + 1/256 moved by origin */
    const float offsets[] = {32768.0f, 40000.0f, 65520.0f, 0.0f}; /* sums stay below 2^16 */
    for (int k = 0; k < 4; k++) {
        uint8_t m1[16 * 2], m2[16 * 2];
        uint32_t pa[16 * 2], pb[16 * 2];
        for (int i = 0; i < 32; i++) {
            pa[i] = pb[i] = 0xFF000000u;
        }
        GfxCanvas ca, cb;
        ASSERT_EQ(gfxCanvasInit(&ca, (GfxSurface){pa, 16, 2, 16}, NULL), STATUS_OK);
        ASSERT_EQ(gfxCanvasInit(&cb, (GfxSurface){pb, 16, 2, 16}, NULL), STATUS_OK);
        gfxCanvasSetOrigin(&cb, -(int32_t)offsets[k], 0);
        GfxPath p, q;
        gfxPathInit(&p, NULL);
        gfxPathInit(&q, NULL);
        ASSERT_EQ(gfxPathAddRect(&p, 8.0f + 1.0f / 256.0f, 0, 4, 2), STATUS_OK);
        ASSERT_EQ(gfxPathAddRect(&q, offsets[k] + 8.0f + 1.0f / 256.0f, 0, 4, 2), STATUS_OK);
        ASSERT_EQ(gfxFillPath(&ca, &p, GFX_FILL_NONZERO, 0xFFFFFFFFu, GFX_OP_SRC), STATUS_OK);
        ASSERT_EQ(gfxFillPath(&cb, &q, GFX_FILL_NONZERO, 0xFFFFFFFFu, GFX_OP_SRC), STATUS_OK);
        for (int i = 0; i < 32; i++) {
            m1[i] = (uint8_t)pa[i];
            m2[i] = (uint8_t)pb[i];
        }
        ASSERT_EQ(m1[8], (uint8_t)254); /* 255/256 of pixel 8 */
        ASSERT_EQ(memcmp(m1, m2, sizeof(m1)), 0);
        gfxPathFree(&p);
        gfxPathFree(&q);
        gfxCanvasDestroy(&ca);
        gfxCanvasDestroy(&cb);
    }
    /* just under half a grid step rounds down (to 0), not up */
    uint8_t md[4] = {0, 0, 0, 0};
    GfxMask m = {md, 4, 1, 4};
    GfxPath p;
    gfxPathInit(&p, NULL);
    float under = (0.5f - 1.0f / 33554432.0f) / 256.0f; /* (0.5 - 2^-25) / 256, exact */
    ASSERT_EQ(gfxPathAddRect(&p, under, 0, 2, 1), STATUS_OK);
    ASSERT_EQ(gfxFillPathMask(&m, &p, GFX_FILL_NONZERO, NULL), STATUS_OK);
    ASSERT_EQ(md[0], (uint8_t)255);
    gfxPathFree(&p);
}

/* Through the public API: a fill under a random canvas clip equals the unclipped fill inside the
 * clip, bit for bit (a repaint of a damaged sub-rect must reproduce the original pixels), and
 * leaves everything outside it untouched. */
TEST(gfxFillPathUnderClipEqualsFullFill) {
    enum { W = 64, H = 48 };
    static uint32_t full[W * H], part[W * H];
    rngSeed(0xC11F);
    for (int iter = 0; iter < 300; iter++) {
        int n = 3 + (int)(rngNext() % 10);
        Pt v[MAX_POLY];
        for (int i = 0; i < n; i++) {
            v[i].x = randCoord(W);
            v[i].y = randCoord(H);
        }
        GfxPath p;
        gfxPathInit(&p, NULL);
        buildPath(&p, v, n);
        GfxFillRule rule = (iter & 1) ? GFX_FILL_EVENODD : GFX_FILL_NONZERO;
        for (int i = 0; i < W * H; i++) {
            full[i] = part[i] = 0xFF000000u;
        }
        GfxCanvas cf, cp;
        ASSERT_EQ(gfxCanvasInit(&cf, (GfxSurface){full, W, H, W}, NULL), STATUS_OK);
        ASSERT_EQ(gfxCanvasInit(&cp, (GfxSurface){part, W, H, W}, NULL), STATUS_OK);
        int32_t x0 = (int32_t)(rngNext() % W), y0 = (int32_t)(rngNext() % H);
        GfxRect clip = {x0, y0, x0 + 1 + (int32_t)(rngNext() % W),
                        y0 + 1 + (int32_t)(rngNext() % H)};
        ASSERT_EQ(gfxCanvasPushClip(&cp, clip), STATUS_OK);
        ASSERT_EQ(gfxFillPath(&cf, &p, rule, 0xFFFFFFFFu, GFX_OP_SRC), STATUS_OK);
        ASSERT_EQ(gfxFillPath(&cp, &p, rule, 0xFFFFFFFFu, GFX_OP_SRC), STATUS_OK);
        gfxCanvasDestroy(&cf);
        gfxCanvasDestroy(&cp);
        gfxPathFree(&p);
        for (int32_t y = 0; y < H; y++) {
            for (int32_t x = 0; x < W; x++) {
                bool in = x >= clip.x0 && x < clip.x1 && y >= clip.y0 && y < clip.y1;
                if (part[y * W + x] != (in ? full[y * W + x] : 0xFF000000u)) {
                    fprintf(stderr, "  iter %d pixel (%d,%d) clip {%d,%d,%d,%d}: %08x vs %08x\n",
                            iter, x, y, clip.x0, clip.y0, clip.x1, clip.y1, part[y * W + x],
                            full[y * W + x]);
                }
                ASSERT_EQ(part[y * W + x], in ? full[y * W + x] : 0xFF000000u);
            }
        }
    }
}

/* Curves with extreme finite control points (quads elevate to cubics whose control points can
 * overflow to +-inf in float) never reach an undefined float-to-int cast (UBSan aborts on one) and
 * only ever return OK or the path's own error. */
TEST(gfxFillPathExtremeCurvesAreSafe) {
    static const float BIG[] = {FLT_MAX,  -FLT_MAX, 3.0e38f,  -3.0e38f,   1.0e38f,
                                -1.0e38f, 1.0e30f,  -1.0e30f, 1048577.0f, -1048577.0f,
                                1.0e7f,   -1.0e7f,  0.0f,     17.5f};
    uint32_t px[24 * 24];
    uint8_t md[24 * 24];
    GfxCanvas c;
    ASSERT_EQ(gfxCanvasInit(&c, (GfxSurface){px, 24, 24, 24}, NULL), STATUS_OK);
    GfxMask m = {md, 24, 24, 24};
    rngSeed(0xFA11);
    for (int iter = 0; iter < 3000; iter++) {
        float f[8];
        for (int i = 0; i < 8; i++) {
            f[i] = (rngNext() & 1u) ? BIG[rngNext() % 14] : (float)(rngNext() % 40) - 8.0f;
        }
        GfxPath p;
        gfxPathInit(&p, NULL);
        switch (rngNext() % 4) {
            case 0:
                gfxPathMoveTo(&p, f[0], f[1]);
                gfxPathQuadTo(&p, f[2], f[3], f[4], f[5]);
                gfxPathQuadTo(&p, f[6], f[7], f[0], f[3]);
                break;
            case 1:
                gfxPathMoveTo(&p, f[0], f[1]);
                gfxPathCubicTo(&p, f[2], f[3], f[4], f[5], f[6], f[7]);
                gfxPathLineTo(&p, f[1], f[0]);
                break;
            case 2:
                gfxPathAddEllipse(&p, f[0], f[1], f[2], f[3]);
                break;
            default:
                gfxPathAddRoundedRect(&p, f[0], f[1], f[2], f[3], f[4]);
                break;
        }
        for (int i = 0; i < 24 * 24; i++) {
            px[i] = 0xFF000000u;
        }
        memset(md, 0, sizeof(md));
        gfxCanvasSetOrigin(&c, (rngNext() & 1u) ? (int32_t)rngNext() : 0, 0);
        Status st = gfxFillPath(&c, &p, (rngNext() & 1u) ? GFX_FILL_EVENODD : GFX_FILL_NONZERO,
                                0xFFFFFFFFu, GFX_OP_SRC_OVER);
        ASSERT_TRUE(st == STATUS_OK || st == p.error);
        st = gfxFillPathMask(&m, &p, GFX_FILL_NONZERO, NULL);
        ASSERT_TRUE(st == STATUS_OK || st == p.error);
        if (p.error != STATUS_OK) {
            ASSERT_EQ(p.error, STATUS_ERR_INVALID); /* only a non-finite computed coordinate */
            for (int i = 0; i < 24 * 24; i++) {
                ASSERT_EQ(px[i], 0xFF000000u);
                ASSERT_EQ(md[i], (uint8_t)0);
            }
        }
        gfxPathFree(&p);
    }
    gfxCanvasDestroy(&c);
}

/* A zig-zag subpath of exactly `edges` non-horizontal edges inside a 64x64 clip (the closing edge
 * included), plus `above` edges wholly above the clip, which never count toward the cap. */
static void zigzag(GfxPath *p, int edges, int above) {
    gfxPathMoveTo(p, 0, 0);
    for (int i = 0; i < edges - 1; i++) {
        gfxPathLineTo(p, (float)(i & 1) * 60.0f + 2.0f, (float)(i % 60) + 1.0f);
    }
    gfxPathClose(p);
    if (above > 0) {
        gfxPathMoveTo(p, 0, -100);
        for (int i = 0; i < above - 1; i++) {
            gfxPathLineTo(p, (float)(i & 1) * 60.0f, -(float)(i % 50) - 2.0f);
        }
        gfxPathClose(p);
    }
}

TEST(gfxEdgeCapBoundary) {
    uint32_t *px = malloc(64 * 64 * sizeof(uint32_t));
    ASSERT_TRUE(px != NULL);
    GfxCanvas c;
    ASSERT_EQ(gfxCanvasInit(&c, (GfxSurface){px, 64, 64, 64}, NULL), STATUS_OK);
    static const struct {
        int edges, above;
        Status want;
    } CASES[] = {
        {8192, 0, STATUS_OK},
        {8193, 0, STATUS_ERR_UNSUPPORTED},
        {8192, 5000, STATUS_OK},
        {4000, 9000, STATUS_OK},
    };
    for (size_t k = 0; k < sizeof(CASES) / sizeof(CASES[0]); k++) {
        for (int i = 0; i < 64 * 64; i++) {
            px[i] = 0xFF000000u;
        }
        GfxPath p;
        gfxPathInit(&p, NULL);
        zigzag(&p, CASES[k].edges, CASES[k].above);
        ASSERT_EQ(p.error, STATUS_OK);
        ASSERT_EQ(gfxFillPath(&c, &p, GFX_FILL_NONZERO, 0xFFFFFFFFu, GFX_OP_SRC), CASES[k].want);
        bool touched = false;
        for (int i = 0; i < 64 * 64; i++) {
            touched |= px[i] != 0xFF000000u;
        }
        ASSERT_EQ(touched, CASES[k].want == STATUS_OK);
        /* the edge list the fill would build has exactly the in-clip edges */
        GfxEdgeList l;
        gfxEdgeListInit(&l, NULL, (GfxRect){0, 0, 64, 64});
        ASSERT_EQ(gfxFlattenPath(&p, 0, 0, &l), CASES[k].want);
        if (CASES[k].want == STATUS_OK) {
            ASSERT_EQ(l.count, (uint32_t)CASES[k].edges);
        }
        gfxEdgeListFree(&l);
        gfxPathFree(&p);
    }
    /* the raster core enforces the same cap on its own */
    GfxEdge *e = calloc(GFX_RASTER_MAX_EDGES + 1, sizeof(GfxEdge));
    ASSERT_TRUE(e != NULL);
    for (uint32_t i = 0; i <= GFX_RASTER_MAX_EDGES; i++) {
        e[i] = (GfxEdge){256, 0, 256, 256, (i & 1) ? 1 : -1};
    }
    void *scratch = NULL;
    size_t scratchSize = 0;
    ASSERT_EQ(gfxRasterFill(gfxAllocatorDefault(), &scratch, &scratchSize, e,
                            GFX_RASTER_MAX_EDGES + 1, (GfxRect){0, 0, 4, 4}, GFX_FILL_NONZERO, NULL,
                            NULL),
              STATUS_ERR_UNSUPPORTED);
    ASSERT_TRUE(scratch == NULL);
    free(e);
    gfxCanvasDestroy(&c);
    free(px);
}

/* The int32 cell arithmetic at its worst case: 4096 contours whose 4096 downward edges all sit in
 * one cell (x = 255/256) and whose upward edges are right of the band, i.e. the maximum same-sign
 * area one cell can collect under the 8192-edge cap. UBSan aborts on a signed overflow; the
 * coverage is also exact (winding 4096 over 1/256 of the pixel is an integral of exactly 16). */
TEST(gfxRasterWorstCaseCellAccumulation) {
    for (int rule = 0; rule < 2; rule++) {
        uint8_t md[2] = {0, 0};
        GfxMask m = {md, 1, 2, 1};
        GfxPath p;
        gfxPathInit(&p, NULL);
        for (int i = 0; i < 4096; i++) {
            gfxPathMoveTo(&p, 255.0f / 256.0f, 0);
            gfxPathLineTo(&p, 255.0f / 256.0f, 1);
            gfxPathLineTo(&p, 100, 1);
            gfxPathLineTo(&p, 100, 0);
            gfxPathClose(&p);
        }
        ASSERT_EQ(gfxFillPathMask(&m, &p, rule ? GFX_FILL_EVENODD : GFX_FILL_NONZERO, NULL),
                  STATUS_OK);
        ASSERT_EQ(md[0], rule ? (uint8_t)0 : (uint8_t)255);
        ASSERT_EQ(md[1], (uint8_t)0);
        gfxPathFree(&p);
    }
}

/* After a close, the next segment starts a new subpath at the old start point; an unclosed subpath
 * is closed by the next moveTo and at the end; a lone moveTo adds nothing. */
TEST(gfxPathCloseAndMoveSemantics) {
    GfxPath implicit, spelled;
    gfxPathInit(&implicit, NULL);
    gfxPathInit(&spelled, NULL);
    gfxPathMoveTo(&implicit, 2, 2);
    gfxPathLineTo(&implicit, 20, 3);
    gfxPathLineTo(&implicit, 15, 17.5f);
    gfxPathClose(&implicit);
    ASSERT_EQ(gfxPathClose(&implicit), STATUS_OK); /* closing twice is a no-op */
    gfxPathLineTo(&implicit, 3, 25);               /* new subpath from (2,2) */
    gfxPathLineTo(&implicit, 9.25f, 30);
    gfxPathMoveTo(&implicit, 40, 40); /* closes the previous one */
    gfxPathMoveTo(&implicit, 30, 5);  /* a lone moveTo */
    gfxPathLineTo(&implicit, 38, 9);
    gfxPathLineTo(&implicit, 33.5f, 20); /* left open: closed at the end */

    gfxPathMoveTo(&spelled, 2, 2);
    gfxPathLineTo(&spelled, 20, 3);
    gfxPathLineTo(&spelled, 15, 17.5f);
    gfxPathClose(&spelled);
    gfxPathMoveTo(&spelled, 2, 2);
    gfxPathLineTo(&spelled, 3, 25);
    gfxPathLineTo(&spelled, 9.25f, 30);
    gfxPathClose(&spelled);
    gfxPathMoveTo(&spelled, 30, 5);
    gfxPathLineTo(&spelled, 38, 9);
    gfxPathLineTo(&spelled, 33.5f, 20);
    gfxPathClose(&spelled);
    ASSERT_EQ(implicit.error, STATUS_OK);
    ASSERT_EQ(spelled.error, STATUS_OK);
    for (int rule = 0; rule < 2; rule++) {
        uint8_t a[48 * 48], b[48 * 48];
        memset(a, 0, sizeof(a));
        memset(b, 0, sizeof(b));
        GfxMask ma = {a, 48, 48, 48}, mb = {b, 48, 48, 48};
        GfxFillRule fr = rule ? GFX_FILL_EVENODD : GFX_FILL_NONZERO;
        ASSERT_EQ(gfxFillPathMask(&ma, &implicit, fr, NULL), STATUS_OK);
        ASSERT_EQ(gfxFillPathMask(&mb, &spelled, fr, NULL), STATUS_OK);
        ASSERT_EQ(memcmp(a, b, sizeof(a)), 0);
        ASSERT_EQ(a[10 * 48 + 12], (uint8_t)255); /* inside the first triangle */
        ASSERT_EQ(a[24 * 48 + 5], (uint8_t)255);  /* inside the implicit second subpath */
        ASSERT_EQ(a[12 * 48 + 34], (uint8_t)255); /* inside the one closed at the end */
    }
    gfxPathFree(&implicit);
    gfxPathFree(&spelled);
}

/* Cubic flattening: the curve is split into n uniform-t pieces, n is the smallest with
 * 4 n^2 tol >= 3 M (tol = 0.1 px, M = the larger second difference, computed here in double), and
 * the curve stays within tol of every chord (checked in double at 7 points per piece). The emitted
 * edges must be exactly P(i/n) on the 1/256 grid, in order; the points are evaluated here in float
 * the way D-142 fixes it (Bernstein form, no contraction), so the comparison can be exact. */
static double absd(double v) {
    return v < 0 ? -v : v;
}

static double bez(double a, double b, double c, double d, double t) {
    double u = 1.0 - t;
    return u * u * u * a + 3.0 * u * u * t * b + 3.0 * u * t * t * c + t * t * t * d;
}

static int32_t bezFixed(const float *v, int i, int n) {
    float t = (float)i / (float)n;
    float u = 1.0f - t;
    float b0 = u * u * u, b1 = 3.0f * u * u * t, b2 = 3.0f * u * t * t, b3 = t * t * t;
    float r = (b0 * v[0] + b1 * v[1]) + b2 * v[2] + b3 * v[3];
    if (i == 0) {
        r = v[0];
    } else if (i == n) {
        r = v[3];
    }
    float sc = r * 256.0f; /* >= 0; rounded half up exactly (not via `sc + 0.5f`) */
    int32_t k = (int32_t)sc;
    return sc - (float)k >= 0.5f ? k + 1 : k;
}

TEST(gfxFlattenCubicWithinTolerance) {
    rngSeed(0xCBC);
    int sawMany = 0;
    for (int iter = 0; iter < 400; iter++) {
        double x[4], y[4];
        float fx[4], fy[4];
        for (int i = 0; i < 4; i++) {
            x[i] = (double)(rngNext() % (200 * 64)) / 64.0; /* exact in float */
            y[i] = (double)(rngNext() % (200 * 64)) / 64.0;
            fx[i] = (float)x[i];
            fy[i] = (float)y[i];
        }
        double m1 = absd(x[0] - 2 * x[1] + x[2]) + absd(y[0] - 2 * y[1] + y[2]);
        double m2 = absd(x[1] - 2 * x[2] + x[3]) + absd(y[1] - 2 * y[2] + y[3]);
        double mm = m1 > m2 ? m1 : m2;
        int n = 1;
        while (n < 256 && (double)n * n * 0.4 < 3.0 * mm) {
            n++;
        }
        sawMany += n >= 20;
        GfxEdge want[260];
        int nw = 0;
        for (int i = 0; i <= n; i++) {
            int i1 = i < n ? i + 1 : 0; /* the last one is the closing edge P(1) -> P(0) */
            int32_t X0 = bezFixed(fx, i, n), Y0 = bezFixed(fy, i, n);
            int32_t X1 = bezFixed(fx, i1, n), Y1 = bezFixed(fy, i1, n);
            if (Y0 != Y1) {
                want[nw++] = Y0 < Y1 ? (GfxEdge){X0, Y0, X1, Y1, 1} : (GfxEdge){X1, Y1, X0, Y0, -1};
            }
            if (i == n || n == 256) {
                continue;
            }
            double t0 = (double)i / n, t1 = (double)(i + 1) / n;
            double ax = bez(x[0], x[1], x[2], x[3], t0), ay = bez(y[0], y[1], y[2], y[3], t0);
            double bx = bez(x[0], x[1], x[2], x[3], t1), by = bez(y[0], y[1], y[2], y[3], t1);
            double len2 = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
            for (int s = 1; s < 8; s++) {
                double t = t0 + (t1 - t0) * s / 8.0;
                double px = bez(x[0], x[1], x[2], x[3], t), py = bez(y[0], y[1], y[2], y[3], t);
                double cross = (bx - ax) * (ay - py) - (ax - px) * (by - ay);
                double d2 = (px - ax) * (px - ax) + (py - ay) * (py - ay);
                ASSERT_TRUE(len2 > 0 ? cross * cross <= 0.01 * len2 + 1e-9 : d2 <= 0.01 + 1e-9);
            }
        }
        GfxPath p;
        gfxPathInit(&p, NULL);
        gfxPathMoveTo(&p, fx[0], fy[0]);
        gfxPathCubicTo(&p, fx[1], fy[1], fx[2], fy[2], fx[3], fy[3]);
        GfxEdgeList l;
        gfxEdgeListInit(&l, NULL, (GfxRect){-1000, -1000, 1000, 1000});
        ASSERT_EQ(gfxFlattenPath(&p, 0, 0, &l), STATUS_OK);
        ASSERT_EQ(l.count, (uint32_t)nw);
        for (int i = 0; i < nw; i++) {
            ASSERT_EQ(memcmp(&want[i], &l.edges[i], sizeof(GfxEdge)), 0);
        }
        gfxEdgeListFree(&l);
        gfxPathFree(&p);
    }
    ASSERT_TRUE(sawMany >= 100);
}

/* An allocator that fails the Nth allocation (and every one after it if `sticky`) and tracks
 * live blocks and their sizes. */
typedef struct {
    int failAt, count, live;
    bool sticky;
    size_t liveBytes;
} FailAlloc;

static void *failAlloc(void *ctx, size_t n) {
    FailAlloc *f = ctx;
    int i = f->count++;
    if (i == f->failAt || (f->sticky && i > f->failAt && f->failAt >= 0)) {
        return NULL;
    }
    f->live++;
    f->liveBytes += n;
    return malloc(n != 0 ? n : 1);
}

static void failFree(void *ctx, void *p, size_t n) {
    FailAlloc *f = ctx;
    f->live--;
    f->liveBytes -= n; /* a wrong size here shows up as a nonzero total at the end */
    free(p);
}

/* Every allocation failure in gfxFillPathMask returns NO_MEMORY with the mask untouched, and
 * nothing leaks or is freed with the wrong size. */
TEST(gfxFillPathMaskAllocationFailure) {
    for (int failAt = 0; failAt < 12; failAt++) {
        FailAlloc fa = {failAt, 0, 0, (failAt & 1) != 0, 0};
        GfxAllocator ga = {failAlloc, failFree, &fa};
        GfxPath p;
        gfxPathInit(&p, NULL); /* the path itself uses malloc */
        ASSERT_EQ(gfxPathAddEllipse(&p, 20, 15, 18, 12), STATUS_OK);
        for (int i = 0; i < 200; i++) { /* > 256 edges in total: the edge list grows once */
            gfxPathMoveTo(&p, 1, (float)i * 0.1f);
            gfxPathLineTo(&p, 2, (float)i * 0.1f + 5.0f);
        }
        uint8_t md[40 * 30];
        memset(md, 7, sizeof(md));
        GfxMask m = {md, 40, 30, 40};
        Status st = gfxFillPathMask(&m, &p, GFX_FILL_NONZERO, &ga);
        ASSERT_TRUE(st == STATUS_OK || st == STATUS_ERR_NO_MEMORY);
        if (st != STATUS_OK) {
            for (size_t i = 0; i < sizeof(md); i++) {
                ASSERT_EQ(md[i], (uint8_t)7);
            }
        }
        ASSERT_EQ(fa.live, 0);
        ASSERT_EQ(fa.liveBytes, (size_t)0);
        gfxPathFree(&p);
    }
}

/* A failed scratch grow keeps the old scratch (still usable, freed once at destroy) and draws
 * nothing. */
TEST(gfxFillPathScratchGrowFailure) {
    FailAlloc fa = {-1, 0, 0, false, 0};
    GfxAllocator ga = {failAlloc, failFree, &fa};
    uint32_t *px = malloc(200 * 64 * sizeof(uint32_t));
    ASSERT_TRUE(px != NULL);
    for (int i = 0; i < 200 * 64; i++) {
        px[i] = 0xFF000000u;
    }
    GfxCanvas c;
    ASSERT_EQ(gfxCanvasInit(&c, (GfxSurface){px, 200, 64, 200}, &ga), STATUS_OK);
    GfxPath small, big;
    gfxPathInit(&small, NULL);
    gfxPathInit(&big, NULL);
    ASSERT_EQ(gfxPathAddRect(&small, 1, 1, 3, 3), STATUS_OK);
    ASSERT_EQ(gfxPathAddRect(&big, 0.5f, 0.5f, 199, 60), STATUS_OK);
    ASSERT_EQ(gfxFillPath(&c, &small, GFX_FILL_NONZERO, 0xFFFFFFFFu, GFX_OP_SRC), STATUS_OK);
    void *oldScratch = c.scratch;
    size_t oldSize = c.scratchSize;
    ASSERT_TRUE(oldScratch != NULL);
    uint32_t *before = malloc(200 * 64 * sizeof(uint32_t));
    ASSERT_TRUE(before != NULL);
    memcpy(before, px, 200 * 64 * sizeof(uint32_t));
    /* the edge list allocation succeeds, the scratch grow fails */
    fa.failAt = fa.count + 1;
    ASSERT_EQ(gfxFillPath(&c, &big, GFX_FILL_NONZERO, 0xFF00FF00u, GFX_OP_SRC),
              STATUS_ERR_NO_MEMORY);
    ASSERT_EQ(memcmp(before, px, 200 * 64 * sizeof(uint32_t)), 0);
    ASSERT_TRUE(c.scratch == oldScratch);
    ASSERT_EQ(c.scratchSize, oldSize);
    ASSERT_EQ(fa.live, 1);
    /* the kept scratch still works, and a later grow succeeds */
    ASSERT_EQ(gfxFillPath(&c, &small, GFX_FILL_NONZERO, 0xFF0000FFu, GFX_OP_SRC), STATUS_OK);
    ASSERT_EQ(px[2 * 200 + 2], 0xFF0000FFu);
    ASSERT_EQ(gfxFillPath(&c, &big, GFX_FILL_NONZERO, 0xFF00FF00u, GFX_OP_SRC), STATUS_OK);
    ASSERT_EQ(px[30 * 200 + 100], 0xFF00FF00u);
    gfxCanvasDestroy(&c);
    ASSERT_EQ(fa.live, 0);
    ASSERT_EQ(fa.liveBytes, (size_t)0);
    gfxCanvasDestroy(&c); /* a second destroy is harmless */
    gfxPathFree(&small);
    gfxPathFree(&big);
    free(before);
    free(px);
}

/* Path growth failures are sticky NO_MEMORY, leak nothing, free with the right sizes, and a reset
 * makes the path usable again; the verb cap is exact. */
TEST(gfxPathAllocationFailureAndVerbCap) {
    for (int failAt = 0; failAt < 16; failAt++) {
        FailAlloc fa = {failAt, 0, 0, false, 0};
        GfxAllocator ga = {failAlloc, failFree, &fa};
        GfxPath p;
        gfxPathInit(&p, &ga);
        Status last = STATUS_OK;
        gfxPathMoveTo(&p, 0, 0);
        for (int i = 0; i < 3000; i++) {
            last = (i % 3 == 0) ? gfxPathCubicTo(&p, 1, 2, 3, 4, 5, (float)i)
                                : gfxPathLineTo(&p, (float)i, 1);
        }
        ASSERT_EQ(last, p.error);
        if (fa.count > failAt) {
            ASSERT_EQ(p.error, STATUS_ERR_NO_MEMORY);
            ASSERT_EQ(gfxPathClose(&p), STATUS_ERR_NO_MEMORY);
            gfxPathReset(&p);
            ASSERT_EQ(gfxPathAddRect(&p, 0, 0, 1, 1), STATUS_OK);
        }
        gfxPathFree(&p);
        ASSERT_EQ(fa.live, 0);
        ASSERT_EQ(fa.liveBytes, (size_t)0);
    }
    GfxPath p;
    gfxPathInit(&p, NULL);
    ASSERT_EQ(gfxPathMoveTo(&p, 0, 0), STATUS_OK);
    for (uint32_t i = 1; i < GFX_PATH_MAX_VERBS; i++) {
        ASSERT_EQ(gfxPathLineTo(&p, (float)(i & 7), (float)(i & 3)), STATUS_OK);
    }
    ASSERT_EQ(p.nVerbs, GFX_PATH_MAX_VERBS);
    ASSERT_EQ(gfxPathLineTo(&p, 1, 1), STATUS_ERR_NO_MEMORY);
    ASSERT_EQ(gfxPathMoveTo(&p, 1, 1), STATUS_ERR_NO_MEMORY);
    ASSERT_EQ(p.nVerbs, GFX_PATH_MAX_VERBS);
    gfxPathFree(&p);
}
