/* Host tests for libs/gfx blur and shadows (M12.2, D-145). */
#include "framework/test.h"
#include "gfx/gfx-blur.h"
#include "gfx/gfx-internal.h"
#include "gfx/gfx-path.h"
#include "gfx/gfx.h"
#include "gfx_golden.h"

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

/* An independent O(n*r) reference of one 1-D pass. */
static void refPass(const uint8_t *in, int32_t n, uint8_t *out, int32_t r, GfxBlurEdge edge) {
    for (int32_t x = 0; x < n; x++) {
        int32_t sum = 0;
        for (int32_t i = x - r; i <= x + r; i++) {
            int32_t j = i;
            int32_t v;
            if (j < 0 || j >= n) {
                v = edge == GFX_BLUR_EDGE_CLAMP ? in[j < 0 ? 0 : n - 1] : 0;
            } else {
                v = in[j];
            }
            sum += v;
        }
        out[x] = (uint8_t)((sum + r) / (2 * r + 1));
    }
}

/* Reference for the whole blur on a w x h mask (stride == w). */
static void refBlur(uint8_t *d, int32_t w, int32_t h, int32_t r, int passes, GfxBlurEdge edge) {
    uint8_t line[600], out[600];
    for (int p = 0; p < passes; p++) {
        for (int32_t y = 0; y < h; y++) {
            memcpy(line, d + (size_t)y * (size_t)w, (size_t)w);
            refPass(line, w, out, r, edge);
            memcpy(d + (size_t)y * (size_t)w, out, (size_t)w);
        }
    }
    for (int p = 0; p < passes; p++) {
        for (int32_t x = 0; x < w; x++) {
            for (int32_t y = 0; y < h; y++) {
                line[y] = d[(size_t)y * (size_t)w + (size_t)x];
            }
            refPass(line, h, out, r, edge);
            for (int32_t y = 0; y < h; y++) {
                d[(size_t)y * (size_t)w + (size_t)x] = out[y];
            }
        }
    }
}

TEST(gfxBlurMatchesNaiveReference) {
    rngSeed(0xB10B);
    for (int iter = 0; iter < 300; iter++) {
        int32_t w = 1 + (int32_t)(rngNext() % 40), h = 1 + (int32_t)(rngNext() % 30);
        int32_t r = (rngNext() & 3u) == 0 ? (int32_t)(rngNext() % 256) : (int32_t)(rngNext() % 9);
        int passes = 1 + (int)(rngNext() % 3);
        GfxBlurEdge edge = (rngNext() & 1u) ? GFX_BLUR_EDGE_CLAMP : GFX_BLUR_EDGE_ZERO;
        uint8_t a[40 * 30], b[40 * 30];
        for (int32_t i = 0; i < w * h; i++) {
            a[i] = (uint8_t)rngNext();
        }
        memcpy(b, a, (size_t)(w * h));
        GfxMask m = {a, w, h, w};
        ASSERT_EQ(gfxBlurBox(&m, (uint32_t)r, (uint32_t)passes, edge, NULL), STATUS_OK);
        refBlur(b, w, h, r, passes, edge);
        ASSERT_EQ(memcmp(a, b, (size_t)(w * h)), 0);
    }
}

TEST(gfxBlurStrideIsRespected) {
    /* a mask inside a wider buffer: the padding bytes must stay untouched */
    uint8_t buf[12 * 6];
    memset(buf, 0xEE, sizeof(buf));
    for (int y = 0; y < 6; y++) {
        for (int x = 0; x < 9; x++) {
            buf[y * 12 + x] = (uint8_t)(x * 20 + y);
        }
    }
    uint8_t tight[9 * 6];
    for (int y = 0; y < 6; y++) {
        memcpy(tight + y * 9, buf + y * 12, 9);
    }
    GfxMask m = {buf, 9, 6, 12}, t = {tight, 9, 6, 9};
    ASSERT_EQ(gfxBlurBox(&m, 2, 2, GFX_BLUR_EDGE_CLAMP, NULL), STATUS_OK);
    ASSERT_EQ(gfxBlurBox(&t, 2, 2, GFX_BLUR_EDGE_CLAMP, NULL), STATUS_OK);
    for (int y = 0; y < 6; y++) {
        ASSERT_EQ(memcmp(buf + y * 12, tight + y * 9, 9), 0);
        for (int x = 9; x < 12; x++) {
            ASSERT_EQ(buf[y * 12 + x], (uint8_t)0xEE);
        }
    }
}

TEST(gfxBlurClampKeepsConstantExact) {
    for (uint32_t r = 0; r <= 255; r += 17) {
        for (int32_t w = 1; w <= 7; w += 3) {
            uint8_t d[7 * 5];
            memset(d, 173, sizeof(d));
            GfxMask m = {d, w, 5, 7};
            for (int y = 0; y < 5; y++) {
                memset(d + y * 7, 173, 7);
            }
            ASSERT_EQ(gfxBlurBox(&m, r, 3, GFX_BLUR_EDGE_CLAMP, NULL), STATUS_OK);
            for (int y = 0; y < 5; y++) {
                for (int x = 0; x < w; x++) {
                    ASSERT_EQ(d[y * 7 + x], (uint8_t)173);
                }
            }
        }
    }
}

TEST(gfxBlurImpulseBecomesBox) {
    uint8_t d[41 * 41];
    memset(d, 0, sizeof(d));
    d[20 * 41 + 20] = 255;
    GfxMask m = {d, 41, 41, 41};
    ASSERT_EQ(gfxBlurBox(&m, 3, 1, GFX_BLUR_EDGE_ZERO, NULL), STATUS_OK);
    /* window of 7: the horizontal pass gives (255+3)/7 = 36 over x in [17,23]; the vertical pass
     * then gives (36+3)/7 = 5 over y in [17,23]; zero everywhere else */
    for (int y = 0; y < 41; y++) {
        for (int x = 0; x < 41; x++) {
            bool in = x >= 17 && x <= 23 && y >= 17 && y <= 23;
            ASSERT_EQ(d[y * 41 + x], (uint8_t)(in ? 5 : 0));
        }
    }
    /* a single line: the same arithmetic in one dimension only (height 1 is a 1-pixel column
     * window, so the vertical pass divides by 7 too: use radius 0 vertically by a tall mask) */
    uint8_t row[41 * 7];
    memset(row, 0, sizeof(row));
    for (int y = 0; y < 7; y++) {
        row[y * 41 + 20] = 255; /* a full-height vertical bar, clamped: no vertical change */
    }
    GfxMask bar = {row, 41, 7, 41};
    ASSERT_EQ(gfxBlurBox(&bar, 3, 1, GFX_BLUR_EDGE_CLAMP, NULL), STATUS_OK);
    for (int x = 0; x < 41; x++) {
        ASSERT_EQ(row[3 * 41 + x], (uint8_t)((x >= 17 && x <= 23) ? 36 : 0));
    }
}

TEST(gfxBlurZeroEdgeConservesMassAndIsSymmetric) {
    uint8_t d[41 * 41];
    memset(d, 0, sizeof(d));
    for (int y = 16; y < 25; y++) {
        for (int x = 16; x < 25; x++) {
            d[y * 41 + x] = 200;
        }
    }
    long before = 0;
    for (size_t i = 0; i < sizeof(d); i++) {
        before += d[i];
    }
    GfxMask m = {d, 41, 41, 41};
    ASSERT_EQ(gfxBlurBox(&m, 2, 3, GFX_BLUR_EDGE_ZERO, NULL), STATUS_OK); /* spread 6 < margin 16 */
    long after = 0;
    for (size_t i = 0; i < sizeof(d); i++) {
        after += d[i];
    }
    /* each of the 6 passes rounds each output by at most 0.5 */
    long tol = (long)(0.5 * 41 * 41 * 6);
    ASSERT_TRUE(after >= before - tol && after <= before + tol);
    for (int y = 0; y < 41; y++) {
        for (int x = 0; x < 41; x++) {
            ASSERT_EQ(d[y * 41 + x], d[y * 41 + (40 - x)]);
            ASSERT_EQ(d[y * 41 + x], d[(40 - y) * 41 + x]);
        }
    }
}

TEST(gfxBlurRejectsBadParameters) {
    uint8_t d[16] = {0};
    GfxMask m = {d, 4, 4, 4};
    ASSERT_EQ(gfxBlurBox(&m, 256, 1, GFX_BLUR_EDGE_ZERO, NULL), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxBlurBox(&m, 1, 0, GFX_BLUR_EDGE_ZERO, NULL), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxBlurBox(&m, 1, 4, GFX_BLUR_EDGE_ZERO, NULL), STATUS_ERR_INVALID);
    GfxMask nul = {NULL, 4, 4, 4}, shortStride = {d, 4, 4, 3}, empty = {d, 0, 4, 4};
    ASSERT_EQ(gfxBlurBox(&nul, 1, 1, GFX_BLUR_EDGE_ZERO, NULL), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxBlurBox(&shortStride, 1, 1, GFX_BLUR_EDGE_ZERO, NULL), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxBlurBox(&empty, 1, 1, GFX_BLUR_EDGE_ZERO, NULL), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxBlurBox(NULL, 1, 1, GFX_BLUR_EDGE_ZERO, NULL), STATUS_ERR_INVALID);
    /* radius 0 is the identity */
    uint8_t e[4] = {1, 2, 3, 4};
    GfxMask id = {e, 4, 1, 4};
    ASSERT_EQ(gfxBlurBox(&id, 0, 3, GFX_BLUR_EDGE_ZERO, NULL), STATUS_OK);
    ASSERT_EQ(e[2], (uint8_t)3);
}

static void *nullAlloc(void *ctx, size_t n) {
    (void)ctx;
    (void)n;
    return NULL;
}

static void noFree(void *ctx, void *p, size_t n) {
    (void)ctx;
    (void)p;
    (void)n;
}

TEST(gfxBlurAllocationFailureLeavesMaskUnchanged) {
    GfxAllocator none = {nullAlloc, noFree, NULL};
    uint8_t d[9] = {0, 0, 0, 0, 255, 0, 0, 0, 0};
    GfxMask m = {d, 3, 3, 3};
    ASSERT_EQ(gfxBlurBox(&m, 1, 1, GFX_BLUR_EDGE_ZERO, &none), STATUS_ERR_NO_MEMORY);
    ASSERT_EQ(d[4], (uint8_t)255);
    ASSERT_EQ(d[0], (uint8_t)0);
}

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

TEST(gfxShadowIsSoftAndOffset) {
    Cv cv;
    ASSERT_TRUE(cvInit(&cv, 100, 80, 0xFFFFFFFFu));
    ASSERT_EQ(gfxDrawShadow(&cv.c, 20.0f, 10.0f, 60.0f, 40.0f, 4.0f, 3, 0, 8, 0xFF000000u, NULL),
              STATUS_OK);
    /* the shadow sits 8 px lower: darkest inside the shifted shape, fading to the background */
    uint32_t center = cv.px[38 * 100 + 50] & 0xFFu; /* inside the shifted shape [18,58) */
    uint32_t below = cv.px[62 * 100 + 50] & 0xFFu;  /* in the blur skirt below it */
    uint32_t above = cv.px[12 * 100 + 50] & 0xFFu;  /* the skirt above the original card */
    uint32_t farAway = cv.px[2 * 100 + 2] & 0xFFu;
    ASSERT_EQ(center, 0u);
    ASSERT_TRUE(below > center && below < 250u);
    ASSERT_TRUE(above > below); /* shifted down: the upper skirt is fainter than the lower one */
    ASSERT_EQ(farAway, 255u);
    /* every pixel keeps alpha 255 and stays a valid premultiplied color */
    for (int32_t i = 0; i < 100 * 80; i++) {
        ASSERT_EQ(cv.px[i] >> 24, (uint32_t)0xFF);
    }
    cvFree(&cv);
}

TEST(gfxShadowParameterHandling) {
    Cv cv;
    ASSERT_TRUE(cvInit(&cv, 40, 40, 0xFFFFFFFFu));
    ASSERT_EQ(gfxDrawShadow(&cv.c, 5.0f, 5.0f, 0.0f, 10.0f, 2.0f, 3, 0, 0, 0xFF000000u, NULL),
              STATUS_OK); /* empty */
    ASSERT_EQ(gfxDrawShadow(&cv.c, __builtin_nanf(""), 5.0f, 10.0f, 10.0f, 2.0f, 3, 0, 0,
                            0xFF000000u, NULL),
              STATUS_ERR_INVALID);
    ASSERT_EQ(gfxDrawShadow(&cv.c, 5.0f, 5.0f, 10.0f, 10.0f, 2.0f, 256, 0, 0, 0xFF000000u, NULL),
              STATUS_ERR_INVALID);
    ASSERT_EQ(gfxDrawShadow(&cv.c, 0.0f, 0.0f, 40000.0f, 10.0f, 2.0f, 3, 0, 0, 0xFF000000u, NULL),
              STATUS_ERR_UNSUPPORTED);
    for (int32_t i = 0; i < 40 * 40; i++) {
        ASSERT_EQ(cv.px[i], (uint32_t)0xFFFFFFFFu); /* none of those drew anything */
    }
    /* huge offsets: wholly off-canvas or clamped, never a crash */
    ASSERT_EQ(gfxDrawShadow(&cv.c, 5.0f, 5.0f, 10.0f, 10.0f, 2.0f, 3, INT32_MAX, INT32_MIN,
                            0xFF000000u, NULL),
              STATUS_OK);
    GfxAllocator none = {nullAlloc, noFree, NULL};
    ASSERT_EQ(gfxDrawShadow(&cv.c, 5.0f, 5.0f, 10.0f, 10.0f, 2.0f, 3, 0, 0, 0xFF000000u, &none),
              STATUS_ERR_NO_MEMORY);
    cvFree(&cv);
}

/* With blurRadius 0 a shadow is exactly the shape's coverage, moved by the offset: it must match
 * filling the same rounded rect there (to float rounding: the shadow builds it at a different
 * absolute position), at fractional and negative positions and under a canvas origin. */
TEST(gfxShadowRadiusZeroIsTheShape) {
    struct {
        float x, y, w, h, r;
        int32_t offX, offY, orgX, orgY;
    } cases[] = {
        {10.25f, 7.5f, 20.5f, 12.75f, 3.0f, 3, -2, 0, 0},
        {-3.75f, 4.125f, 30.0f, 9.5f, 4.5f, 7, 5, 0, 0},
        {110.5f, 60.25f, 17.25f, 21.0f, 0.0f, -95, -50, -2, 3},
        {0.0f, 0.0f, 12.0f, 12.0f, 6.0f, 0, 0, 20, 10},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Cv a, b;
        ASSERT_TRUE(cvInit(&a, 64, 48, 0xFFE0C0A0u));
        ASSERT_TRUE(cvInit(&b, 64, 48, 0xFFE0C0A0u));
        gfxCanvasSetOrigin(&a.c, cases[i].orgX, cases[i].orgY);
        gfxCanvasSetOrigin(&b.c, cases[i].orgX, cases[i].orgY);
        ASSERT_EQ(gfxDrawShadow(&a.c, cases[i].x, cases[i].y, cases[i].w, cases[i].h, cases[i].r, 0,
                                cases[i].offX, cases[i].offY, 0xC0204060u, NULL),
                  STATUS_OK);
        GfxPath p;
        gfxPathInit(&p, NULL);
        ASSERT_EQ(gfxPathAddRoundedRect(&p, cases[i].x + (float)cases[i].offX,
                                        cases[i].y + (float)cases[i].offY, cases[i].w, cases[i].h,
                                        cases[i].r),
                  STATUS_OK);
        ASSERT_EQ(gfxFillPath(&b.c, &p, GFX_FILL_NONZERO, 0xC0204060u, GFX_OP_SRC_OVER), STATUS_OK);
        gfxPathFree(&p);
        int differing = 0;
        for (int32_t k = 0; k < 64 * 48; k++) {
            for (int sh = 0; sh < 32; sh += 8) {
                int32_t d = (int32_t)((a.px[k] >> sh) & 0xFFu) - (int32_t)((b.px[k] >> sh) & 0xFFu);
                ASSERT_TRUE(d >= -1 && d <= 1);
            }
            differing += a.px[k] != 0xFFE0C0A0u;
        }
        ASSERT_TRUE(differing > 40); /* and it drew something */
        cvFree(&a);
        cvFree(&b);
    }
}

/* Shadow under a card: the classic use, and the golden for it. */
TEST(gfxGoldenShadowCard) {
    Cv cv;
    ASSERT_TRUE(cvInit(&cv, 128, 96, 0xFFE8ECF0u));
    ASSERT_EQ(gfxDrawShadow(&cv.c, 24.0f, 20.0f, 80.0f, 48.0f, 10.0f, 6, 0, 8,
                            gfxColorPremul(0x70000000u), NULL),
              STATUS_OK);
    ASSERT_EQ(gfxDrawShadow(&cv.c, 24.0f, 20.0f, 80.0f, 48.0f, 10.0f, 2, 0, 2,
                            gfxColorPremul(0x50000000u), NULL),
              STATUS_OK);
    GfxPath p;
    gfxPathInit(&p, NULL);
    ASSERT_EQ(gfxPathAddRoundedRect(&p, 24.0f, 20.0f, 80.0f, 48.0f, 10.0f), STATUS_OK);
    ASSERT_EQ(gfxFillPath(&cv.c, &p, GFX_FILL_NONZERO, 0xFFFFFFFFu, GFX_OP_SRC_OVER), STATUS_OK);
    gfxPathFree(&p);
    ASSERT_EQ(cv.px[40 * 128 + 60], (uint32_t)0xFFFFFFFFu); /* the card's face */
    ASSERT_TRUE(cv.px[76 * 128 + 60] < 0xFFE8ECF0u);        /* shadow below the card */
    ASSERT_EQ(cv.px[2 * 128 + 2], (uint32_t)0xFFE8ECF0u);   /* untouched corner */
    ASSERT_TRUE(goldenCheck("gfx_shadow_card", &cv.c.surf));
    cvFree(&cv);
}

/* A shadow that lies wholly outside the clip returns OK without allocating (an allocator that
 * always fails must not matter), while one that touches the clip still needs its mask. */
TEST(gfxShadowOutsideClipDoesNoWork) {
    Cv cv;
    ASSERT_TRUE(cvInit(&cv, 40, 40, 0xFFFFFFFFu));
    GfxAllocator none = {nullAlloc, noFree, NULL};
    ASSERT_EQ(gfxDrawShadow(&cv.c, 200.0f, 200.0f, 30.0f, 30.0f, 4.0f, 3, 0, 0, 0xFF000000u, &none),
              STATUS_OK); /* far right/below the canvas */
    ASSERT_EQ(gfxDrawShadow(&cv.c, -100.0f, 5.0f, 30.0f, 30.0f, 4.0f, 3, 0, 0, 0xFF000000u, &none),
              STATUS_OK); /* far left */
    ASSERT_EQ(gfxCanvasPushClip(&cv.c, (GfxRect){0, 0, 10, 10}), STATUS_OK);
    ASSERT_EQ(gfxDrawShadow(&cv.c, 25.0f, 25.0f, 10.0f, 10.0f, 2.0f, 3, 0, 0, 0xFF000000u, &none),
              STATUS_OK); /* inside the canvas but outside the pushed clip */
    ASSERT_EQ(gfxDrawShadow(&cv.c, 2.0f, 2.0f, 10.0f, 10.0f, 2.0f, 3, 0, 0, 0xFF000000u, &none),
              STATUS_ERR_NO_MEMORY); /* touches the clip: needs the mask */
    for (int32_t i = 0; i < 40 * 40; i++) {
        ASSERT_EQ(cv.px[i], (uint32_t)0xFFFFFFFFu);
    }
    cvFree(&cv);
}

/* The off-clip early-out at each of the four clip edges, under several canvas origins and with and
 * without a pushed clip: a mask that exactly touches the clip from outside (its exclusive end on
 * the clip's start, or its start on the clip's exclusive end) must not allocate; one pixel further
 * in must (a failing allocator tells the two apart). The mask is the shape's integer pixel bounds
 * plus 2 and a margin of 3r+1 on each side (D-145). Clips are chosen inside the surface, so the
 * canvas-coordinate clip is exactly the rect pushed (or the surface shifted by the origin). */
TEST(gfxShadowClipEarlyOutIsExactAtEveryEdge) {
    static const struct {
        int32_t orgX, orgY;
        bool push;
    } setups[] = {{0, 0, false}, {0, 0, true}, {-7, 5, true}, {13, -5, true}, {-20, 9, false}};
    GfxAllocator none = {nullAlloc, noFree, NULL};
    const int32_t w = 6, h = 5, r = 2, offX = 3, offY = -4;
    const int32_t margin = 3 * r + 1, mw = w + 2 + 2 * margin, mh = h + 2 + 2 * margin;
    for (size_t si = 0; si < sizeof(setups) / sizeof(setups[0]); si++) {
        Cv cv;
        ASSERT_TRUE(cvInit(&cv, 64, 48, 0xFFFFFFFFu));
        gfxCanvasSetOrigin(&cv.c, setups[si].orgX, setups[si].orgY);
        GfxRect clip = {-setups[si].orgX, -setups[si].orgY, 64 - setups[si].orgX,
                        48 - setups[si].orgY};
        if (setups[si].push) {
            clip = (GfxRect){10, 8, 30, 25};
            ASSERT_EQ(gfxCanvasPushClip(&cv.c, clip), STATUS_OK);
        }
        /* mask origin -> shape position: x = mx0 + margin - offX */
        for (int side = 0; side < 4; side++) {
            for (int inside = 0; inside < 2; inside++) {
                int32_t mx0 = 0, my0 = 0;
                switch (side) {
                    case 0: /* left: mask end on clip.x0 */
                        mx0 = clip.x0 - mw + inside;
                        my0 = clip.y0 + 2;
                        break;
                    case 1: /* right: mask start on clip.x1 */
                        mx0 = clip.x1 - inside;
                        my0 = clip.y0 + 2;
                        break;
                    case 2: /* top */
                        mx0 = clip.x0 + 2;
                        my0 = clip.y0 - mh + inside;
                        break;
                    default: /* bottom */
                        mx0 = clip.x0 + 2;
                        my0 = clip.y1 - inside;
                        break;
                }
                float x = (float)(mx0 + margin - offX), y = (float)(my0 + margin - offY);
                Status st = gfxDrawShadow(&cv.c, x, y, (float)w, (float)h, 1.0f, (uint32_t)r, offX,
                                          offY, 0xFF000000u, &none);
                ASSERT_EQ(st, inside ? STATUS_ERR_NO_MEMORY : STATUS_OK);
            }
        }
        for (int32_t i = 0; i < 64 * 48; i++) {
            ASSERT_EQ(cv.px[i], (uint32_t)0xFFFFFFFFu);
        }
        cvFree(&cv);
    }
}

typedef struct {
    size_t allocs, live;
} CountingAlloc;

static void *countAlloc(void *ctx, size_t n) {
    CountingAlloc *ca = ctx;
    void *p = malloc(n != 0 ? n : 1);
    if (p != NULL) {
        ca->allocs++;
        ca->live++;
    }
    return p;
}

static void countFree(void *ctx, void *p, size_t n) {
    CountingAlloc *ca = ctx;
    (void)n;
    if (p != NULL) {
        ca->live--;
    }
    free(p);
}

/* The early-out must never change what is drawn: random shadows (fractional and negative
 * positions, offsets, a random origin, a random pushed clip that may stick out of the surface or be
 * empty) drawn on canvas A must equal, inside A's clip, the same shadow drawn on an unclipped
 * canvas B with no origin (the origin folded into the position; all values are exact in float),
 * and leave everything outside A's clip untouched. Both outcomes (skipped and drawn) must occur. */
TEST(gfxShadowClipEarlyOutNeverChangesTheImage) {
    rngSeed(0x5AD0u);
    int skipped = 0, drawn = 0;
    CountingAlloc ca = {0};
    GfxAllocator counting = {countAlloc, countFree, &ca};
    for (int iter = 0; iter < 3000; iter++) {
        Cv a, b;
        ASSERT_TRUE(cvInit(&a, 48, 40, 0xFFE0C0A0u));
        ASSERT_TRUE(cvInit(&b, 48, 40, 0xFFE0C0A0u));
        int32_t orgX = (int32_t)(rngNext() % 61) - 30, orgY = (int32_t)(rngNext() % 61) - 30;
        gfxCanvasSetOrigin(&a.c, orgX, orgY);
        /* positions are drawn around the surface, then moved into canvas coordinates */
        GfxRect pc = {(int32_t)(rngNext() % 60) - 10 - orgX, (int32_t)(rngNext() % 50) - 10 - orgY,
                      0, 0};
        pc.x1 = pc.x0 + (int32_t)(rngNext() % 40) - 3;
        pc.y1 = pc.y0 + (int32_t)(rngNext() % 40) - 3;
        ASSERT_EQ(gfxCanvasPushClip(&a.c, pc), STATUS_OK);
        /* the effective clip in surface coordinates, computed independently */
        GfxRect sc = {pc.x0 + orgX, pc.y0 + orgY, pc.x1 + orgX, pc.y1 + orgY};
        sc.x0 = sc.x0 < 0 ? 0 : sc.x0;
        sc.y0 = sc.y0 < 0 ? 0 : sc.y0;
        sc.x1 = sc.x1 > 48 ? 48 : sc.x1;
        sc.y1 = sc.y1 > 40 ? 40 : sc.y1;
        float x = (float)((int32_t)(rngNext() % 320) - 120) / 4.0f - (float)orgX;
        float y = (float)((int32_t)(rngNext() % 280) - 100) / 4.0f - (float)orgY;
        float w = (float)(1 + rngNext() % 80) / 4.0f, h = (float)(1 + rngNext() % 80) / 4.0f;
        uint32_t r = rngNext() % 5;
        int32_t offX = (int32_t)(rngNext() % 21) - 10, offY = (int32_t)(rngNext() % 21) - 10;
        size_t before = ca.allocs;
        ASSERT_EQ(gfxDrawShadow(&a.c, x, y, w, h, 2.0f, r, offX, offY, 0xC0102030u, &counting),
                  STATUS_OK);
        bool skip = ca.allocs == before;
        ASSERT_EQ(gfxDrawShadow(&b.c, x + (float)orgX, y + (float)orgY, w, h, 2.0f, r, offX, offY,
                                0xC0102030u, NULL),
                  STATUS_OK);
        bool anyInClip = false;
        for (int32_t py = 0; py < 40; py++) {
            for (int32_t px = 0; px < 48; px++) {
                bool in = px >= sc.x0 && px < sc.x1 && py >= sc.y0 && py < sc.y1;
                uint32_t av = a.px[py * 48 + px], bv = b.px[py * 48 + px];
                ASSERT_EQ(av, in ? bv : (uint32_t)0xFFE0C0A0u);
                anyInClip |= in && bv != 0xFFE0C0A0u;
            }
        }
        if (skip) {
            ASSERT_TRUE(!anyInClip); /* only ever skips a shadow that would draw nothing */
            skipped++;
        } else {
            drawn += anyInClip;
        }
        cvFree(&a);
        cvFree(&b);
    }
    ASSERT_EQ(ca.live, (size_t)0);
    ASSERT_TRUE(skipped > 300 && drawn > 300);
}

/* Origins at the ends of the int32 range, where the canvas-coordinate clip saturates: a shadow is
 * still drawn exactly where the same shadow lands with origin 0, and one that cannot reach the
 * surface is skipped without allocating. */
TEST(gfxShadowClipEarlyOutAtExtremeOrigins) {
    GfxAllocator none = {nullAlloc, noFree, NULL};
    Cv ref;
    ASSERT_TRUE(cvInit(&ref, 40, 40, 0xFFFFFFFFu));
    ASSERT_EQ(gfxDrawShadow(&ref.c, 12.0f, 12.0f, 10.0f, 10.0f, 2.0f, 2, 0, 0, 0xFF000000u, NULL),
              STATUS_OK);

    /* origin INT32_MAX: the surface is at canvas [-INT32_MAX, -INT32_MAX + 40) */
    Cv a;
    ASSERT_TRUE(cvInit(&a, 40, 40, 0xFFFFFFFFu));
    gfxCanvasSetOrigin(&a.c, INT32_MAX, INT32_MAX);
    ASSERT_EQ(gfxDrawShadow(&a.c, 2.0f, 2.0f, 10.0f, 10.0f, 2.0f, 2, -INT32_MAX + 10,
                            -INT32_MAX + 10, 0xFF000000u, &none),
              STATUS_ERR_NO_MEMORY); /* reaches the surface: needs its mask */
    ASSERT_EQ(gfxDrawShadow(&a.c, 2.0f, 2.0f, 10.0f, 10.0f, 2.0f, 2, -INT32_MAX + 10,
                            -INT32_MAX + 10, 0xFF000000u, NULL),
              STATUS_OK);
    ASSERT_EQ(memcmp(a.px, ref.px, 40 * 40 * sizeof(uint32_t)), 0);

    /* origin INT32_MIN + 36: the surface is at canvas [INT32_MAX - 35, INT32_MAX + 5), so the
     * clip's right and bottom edges saturate to INT32_MAX; the mask [INT32_MAX - 30,
     * INT32_MAX - 4) still fits in int32 and must be drawn */
    Cv b;
    ASSERT_TRUE(cvInit(&b, 40, 40, 0xFFFFFFFFu));
    gfxCanvasSetOrigin(&b.c, INT32_MIN + 36, INT32_MIN + 36);
    ASSERT_EQ(gfxDrawShadow(&b.c, 2.0f, 2.0f, 10.0f, 10.0f, 2.0f, 2, INT32_MAX - 25, INT32_MAX - 25,
                            0xFF000000u, &none),
              STATUS_ERR_NO_MEMORY);
    ASSERT_EQ(gfxDrawShadow(&b.c, 2.0f, 2.0f, 10.0f, 10.0f, 2.0f, 2, INT32_MAX - 25, INT32_MAX - 25,
                            0xFF000000u, NULL),
              STATUS_OK);
    ASSERT_EQ(memcmp(b.px, ref.px, 40 * 40 * sizeof(uint32_t)), 0);

    /* origin INT32_MIN: the whole surface lies past canvas INT32_MAX; nothing can reach it */
    Cv c;
    ASSERT_TRUE(cvInit(&c, 40, 40, 0xFFFFFFFFu));
    gfxCanvasSetOrigin(&c.c, INT32_MIN, INT32_MIN);
    ASSERT_EQ(gfxDrawShadow(&c.c, 2.0f, 2.0f, 10.0f, 10.0f, 2.0f, 2, INT32_MAX - 40, INT32_MAX - 40,
                            0xFF000000u, &none),
              STATUS_OK);
    for (int32_t i = 0; i < 40 * 40; i++) {
        ASSERT_EQ(c.px[i], (uint32_t)0xFFFFFFFFu);
    }
    cvFree(&ref);
    cvFree(&a);
    cvFree(&b);
    cvFree(&c);
}
