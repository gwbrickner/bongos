/* Host tests for libs/gfx's core: pixel math, clipping, fills, blits, mask fills (M12.2). */
#include "framework/test.h"
#include "gfx/gfx-internal.h"
#include "gfx/gfx.h"

#include <stdlib.h>
#include <string.h>

/* Seeded xorshift32: tests never use rand(), so results don't depend on libc's sequence. */
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

static uint32_t randPremul(void) {
    uint32_t a = rngNext() & 0xFFu;
    uint32_t r = a == 0 ? 0 : (rngNext() % (a + 1));
    uint32_t g = a == 0 ? 0 : (rngNext() % (a + 1));
    uint32_t b = a == 0 ? 0 : (rngNext() % (a + 1));
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static bool premulValid(uint32_t c) {
    uint32_t a = c >> 24;
    return ((c >> 16) & 0xFFu) <= a && ((c >> 8) & 0xFFu) <= a && (c & 0xFFu) <= a;
}

TEST(gfxMulDiv255Exhaustive) {
    for (uint32_t a = 0; a < 256; a++) {
        for (uint32_t b = 0; b < 256; b++) {
            uint32_t want = (2u * a * b + 255u) / 510u; /* round(a*b/255), half up */
            ASSERT_EQ(gfxMulDiv255(a, b), want);
        }
    }
}

TEST(gfxColorPremulAndSanitize) {
    ASSERT_EQ(gfxColorPremul(0xFF102030u), (GfxColor)0xFF102030u);
    ASSERT_EQ(gfxColorPremul(0x00FFFFFFu), (GfxColor)0x00000000u);
    ASSERT_EQ(gfxColorPremul(0x80FFFFFFu), (GfxColor)0x80808080u);
    ASSERT_EQ(gfxColorSanitize(0x40FF2010u), (GfxColor)0x40402010u);
    ASSERT_EQ(gfxColorSanitize(0x00FFFFFFu), (GfxColor)0x00000000u);
    ASSERT_EQ(gfxColorSanitize(0xFFFFFFFFu), (GfxColor)0xFFFFFFFFu);
}

TEST(gfxBlendKnownValues) {
    ASSERT_EQ(gfxBlendPixel(0xFF112233u, 0xFF445566u, 255, GFX_OP_SRC_OVER), (uint32_t)0xFF445566u);
    ASSERT_EQ(gfxBlendPixel(0xFF112233u, 0x00000000u, 255, GFX_OP_SRC_OVER), (uint32_t)0xFF112233u);
    ASSERT_EQ(gfxBlendPixel(0xFF112233u, 0xFF445566u, 0, GFX_OP_SRC_OVER), (uint32_t)0xFF112233u);
    /* 50% white over opaque black */
    ASSERT_EQ(gfxBlendPixel(0xFF000000u, 0x80808080u, 255, GFX_OP_SRC_OVER), (uint32_t)0xFF808080u);
    /* SRC replaces regardless of the destination; coverage 0 keeps it */
    ASSERT_EQ(gfxBlendPixel(0xFF112233u, 0x00000000u, 255, GFX_OP_SRC), (uint32_t)0x00000000u);
    ASSERT_EQ(gfxBlendPixel(0xFF112233u, 0xFF445566u, 0, GFX_OP_SRC), (uint32_t)0xFF112233u);
}

TEST(gfxBlendPremulInvariant) {
    rngSeed(0xBEEF);
    for (int i = 0; i < 200000; i++) {
        uint32_t dst = randPremul(), src = randPremul(), cov = rngNext() & 0xFFu;
        ASSERT_TRUE(premulValid(gfxBlendPixel(dst, src, cov, GFX_OP_SRC_OVER)));
        ASSERT_TRUE(premulValid(gfxBlendPixel(dst, src, cov, GFX_OP_SRC)));
    }
    /* Extremes */
    uint32_t vals[] = {0x00000000u, 0xFF000000u, 0xFFFFFFFFu, 0x01010101u, 0xFE808080u};
    for (size_t i = 0; i < 5; i++) {
        for (size_t j = 0; j < 5; j++) {
            for (uint32_t cov = 0; cov < 256; cov++) {
                ASSERT_TRUE(premulValid(gfxBlendPixel(vals[i], vals[j], cov, GFX_OP_SRC_OVER)));
                ASSERT_TRUE(premulValid(gfxBlendPixel(vals[i], vals[j], cov, GFX_OP_SRC)));
            }
        }
    }
}

/* Opaque source over anything, at any coverage, never darkens toward garbage: over an opaque
 * destination the result stays opaque. */
TEST(gfxBlendOpaqueStaysOpaque) {
    rngSeed(77);
    for (int i = 0; i < 50000; i++) {
        uint32_t dst = 0xFF000000u | (rngNext() & 0xFFFFFFu);
        uint32_t src = randPremul();
        ASSERT_EQ(gfxBlendPixel(dst, src, rngNext() & 0xFFu, GFX_OP_SRC_OVER) >> 24,
                  (uint32_t)0xFF);
    }
}

TEST(gfxCanvasInitValidation) {
    uint32_t px[16];
    GfxCanvas c;
    GfxSurface ok = {px, 4, 4, 4};
    ASSERT_EQ(gfxCanvasInit(&c, ok, NULL), STATUS_OK);
    gfxCanvasDestroy(&c);
    GfxSurface bad[] = {
        {NULL, 4, 4, 4},       {px, 0, 4, 4},     {px, 4, 0, 4},
        {px, -1, 4, 4},        {px, 4, -1, 4},    {px, 4, 4, 3},
        {px, 70000, 1, 70000}, {px, 1, 70000, 1}, {px, 65536, 65536, INT32_MAX},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ASSERT_EQ(gfxCanvasInit(&c, bad[i], NULL), STATUS_ERR_INVALID);
    }
}

typedef struct {
    uint32_t *buf; /* whole allocation, with a guard border around the surface */
    int32_t w, h, guard, stride;
    GfxCanvas c;
} Fixture;

#define GUARD_PX 0xA5A5A5A5u

static bool fixtureInit(Fixture *f, int32_t w, int32_t h, uint32_t fill) {
    f->w = w;
    f->h = h;
    f->guard = 8;
    f->stride = w + 2 * f->guard;
    size_t total = (size_t)f->stride * (size_t)(h + 2 * f->guard);
    f->buf = malloc(total * sizeof(uint32_t));
    if (f->buf == NULL) {
        return false;
    }
    for (size_t i = 0; i < total; i++) {
        f->buf[i] = GUARD_PX;
    }
    GfxSurface s = {f->buf + (size_t)f->guard * f->stride + f->guard, w, h, f->stride};
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            s.pixels[(size_t)y * s.stride + x] = fill;
        }
    }
    return gfxCanvasInit(&f->c, s, NULL) == STATUS_OK;
}

static uint32_t fixturePx(const Fixture *f, int32_t x, int32_t y) {
    return f->c.surf.pixels[(size_t)y * f->c.surf.stride + x];
}

static bool guardIntact(const Fixture *f) {
    for (int32_t y = -f->guard; y < f->h + f->guard; y++) {
        for (int32_t x = -f->guard; x < f->w + f->guard; x++) {
            bool inside = x >= 0 && x < f->w && y >= 0 && y < f->h;
            if (!inside && f->c.surf.pixels[(ptrdiff_t)y * f->stride + x] != GUARD_PX) {
                return false;
            }
        }
    }
    return true;
}

static void fixtureFree(Fixture *f) {
    gfxCanvasDestroy(&f->c);
    free(f->buf);
}

TEST(gfxFillRectBasics) {
    Fixture f;
    ASSERT_TRUE(fixtureInit(&f, 10, 6, 0xFF000000u));
    gfxFillRect(&f.c, (GfxRect){2, 1, 5, 4}, 0xFFFF0000u, GFX_OP_SRC_OVER);
    for (int32_t y = 0; y < 6; y++) {
        for (int32_t x = 0; x < 10; x++) {
            bool in = x >= 2 && x < 5 && y >= 1 && y < 4;
            ASSERT_EQ(fixturePx(&f, x, y), in ? 0xFFFF0000u : 0xFF000000u);
        }
    }
    /* empty and inverted rects draw nothing */
    gfxFillRect(&f.c, (GfxRect){5, 5, 5, 9}, 0xFF00FF00u, GFX_OP_SRC);
    gfxFillRect(&f.c, (GfxRect){9, 0, 2, 6}, 0xFF00FF00u, GFX_OP_SRC);
    ASSERT_EQ(fixturePx(&f, 5, 5), (uint32_t)0xFF000000u);
    ASSERT_TRUE(guardIntact(&f));
    fixtureFree(&f);
}

TEST(gfxFillRectSanitizesColor) {
    Fixture f;
    ASSERT_TRUE(fixtureInit(&f, 4, 4, 0xFF000000u));
    /* alpha 0x40 with channels above it: a straight-alpha color passed by mistake */
    gfxFillRect(&f.c, (GfxRect){0, 0, 4, 4}, 0x40FFFFFFu, GFX_OP_SRC_OVER);
    ASSERT_TRUE(premulValid(fixturePx(&f, 1, 1)));
    ASSERT_EQ(fixturePx(&f, 1, 1) >> 24, (uint32_t)0xFF);
    fixtureFree(&f);
}

TEST(gfxClipStackAndOrigin) {
    Fixture f;
    ASSERT_TRUE(fixtureInit(&f, 20, 20, 0xFF000000u));
    ASSERT_EQ(gfxCanvasPushClip(&f.c, (GfxRect){5, 5, 15, 15}), STATUS_OK);
    ASSERT_EQ(gfxCanvasPushClip(&f.c, (GfxRect){10, 0, 30, 30}), STATUS_OK); /* -> [10,15)x[5,15) */
    gfxFillRect(&f.c, (GfxRect){-100, -100, 100, 100}, 0xFFFFFFFFu, GFX_OP_SRC);
    for (int32_t y = 0; y < 20; y++) {
        for (int32_t x = 0; x < 20; x++) {
            bool in = x >= 10 && x < 15 && y >= 5 && y < 15;
            ASSERT_EQ(fixturePx(&f, x, y), in ? 0xFFFFFFFFu : 0xFF000000u);
        }
    }
    GfxRect b = gfxCanvasClipBounds(&f.c);
    ASSERT_EQ(b.x0, 10);
    ASSERT_EQ(b.y1, 15);
    ASSERT_EQ(gfxCanvasPopClip(&f.c), STATUS_OK);
    ASSERT_EQ(gfxCanvasPopClip(&f.c), STATUS_OK);
    ASSERT_EQ(gfxCanvasPopClip(&f.c), STATUS_ERR_INVALID); /* only the base clip is left */

    /* origin translates fills and clip pushes alike */
    gfxCanvasSetOrigin(&f.c, 3, 4);
    gfxFillRect(&f.c, (GfxRect){0, 0, 2, 2}, 0xFF00FF00u, GFX_OP_SRC);
    ASSERT_EQ(fixturePx(&f, 3, 4), (uint32_t)0xFF00FF00u);
    ASSERT_EQ(fixturePx(&f, 4, 5), (uint32_t)0xFF00FF00u);
    ASSERT_EQ(fixturePx(&f, 5, 5), (uint32_t)0xFF000000u);
    ASSERT_TRUE(guardIntact(&f));
    fixtureFree(&f);
}

TEST(gfxClipStackDepthLimit) {
    Fixture f;
    ASSERT_TRUE(fixtureInit(&f, 4, 4, 0));
    for (uint32_t i = 1; i < GFX_CLIP_DEPTH; i++) {
        ASSERT_EQ(gfxCanvasPushClip(&f.c, (GfxRect){0, 0, 4, 4}), STATUS_OK);
    }
    ASSERT_EQ(gfxCanvasPushClip(&f.c, (GfxRect){0, 0, 4, 4}), STATUS_ERR_UNSUPPORTED);
    fixtureFree(&f);
}

/* Random rects at huge, negative and fractional-ish coordinates through random nested clips and
 * origins must never touch a pixel outside the surface (guard border) or outside the active
 * clip. */
TEST(gfxClipNeverWritesOutside) {
    rngSeed(0x1234ABCD);
    int32_t extremes[] = {INT32_MIN, INT32_MIN + 1, -1000000,      -1,       0, 1, 7,
                          33,        1000000,       INT32_MAX - 1, INT32_MAX};
    for (int iter = 0; iter < 400; iter++) {
        Fixture f;
        ASSERT_TRUE(fixtureInit(&f, 33, 21, 0xFF000000u));
        int depth = 0;
        for (int k = 0; k < 6; k++) {
            int32_t v[4];
            for (int i = 0; i < 4; i++) {
                v[i] = (rngNext() & 3u) == 0 ? extremes[rngNext() % 11]
                                             : (int32_t)(rngNext() % 80) - 20;
            }
            if (rngNext() & 1u) {
                gfxCanvasTranslate(&f.c, (int32_t)(rngNext() % 200) - 100,
                                   extremes[rngNext() % 11]);
            }
            if ((rngNext() & 3u) == 0 &&
                gfxCanvasPushClip(&f.c, (GfxRect){v[0], v[1], v[2], v[3]}) == STATUS_OK) {
                depth++;
            }
            GfxRect clip = gfxCanvasClipBounds(&f.c);
            (void)clip;
            gfxFillRect(&f.c, (GfxRect){v[1], v[0], v[3], v[2]}, 0xFFFFFFFFu,
                        (rngNext() & 1u) ? GFX_OP_SRC : GFX_OP_SRC_OVER);
            ASSERT_TRUE(guardIntact(&f));
        }
        (void)depth;
        fixtureFree(&f);
    }
}

TEST(gfxFillOutsideActiveClipUntouched) {
    Fixture f;
    ASSERT_TRUE(fixtureInit(&f, 16, 16, 0xFF000000u));
    ASSERT_EQ(gfxCanvasPushClip(&f.c, (GfxRect){4, 4, 8, 8}), STATUS_OK);
    rngSeed(9);
    for (int i = 0; i < 100; i++) {
        int32_t x = (int32_t)(rngNext() % 40) - 10, y = (int32_t)(rngNext() % 40) - 10;
        gfxFillRect(&f.c,
                    (GfxRect){x, y, x + (int32_t)(rngNext() % 30), y + (int32_t)(rngNext() % 30)},
                    0xFF0000FFu, GFX_OP_SRC);
    }
    for (int32_t y = 0; y < 16; y++) {
        for (int32_t x = 0; x < 16; x++) {
            if (x < 4 || x >= 8 || y < 4 || y >= 8) {
                ASSERT_EQ(fixturePx(&f, x, y), (uint32_t)0xFF000000u);
            }
        }
    }
    ASSERT_TRUE(guardIntact(&f));
    fixtureFree(&f);
}

static void fillPattern(uint32_t *px, int32_t w, int32_t h) {
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            px[y * w + x] =
                0xFF000000u | ((uint32_t)(x * 16) << 16) | ((uint32_t)(y * 16) << 8) | 0x40;
        }
    }
}

TEST(gfxBlitCopyClipsSourceAndDestination) {
    uint32_t sp[8 * 8];
    fillPattern(sp, 8, 8);
    GfxSurface src = {sp, 8, 8, 8};
    Fixture f;
    ASSERT_TRUE(fixtureInit(&f, 10, 10, 0xFF000000u));
    /* srcRect hangs off the source's left/top edge; destination hangs off the canvas's right */
    gfxBlit(&f.c, 6, 2, &src, (GfxRect){-2, -1, 6, 4}, GFX_OP_SRC, 255);
    /* srcRect clamps to [0,6)x[0,4); the clamp moved its origin by (2,1), so it lands at (8,3) and
     * is clipped to x<10 */
    ASSERT_EQ(fixturePx(&f, 8, 3), sp[0]);
    ASSERT_EQ(fixturePx(&f, 9, 3), sp[1]);
    ASSERT_EQ(fixturePx(&f, 9, 5), sp[2 * 8 + 1]);
    ASSERT_EQ(fixturePx(&f, 7, 3), (uint32_t)0xFF000000u);
    ASSERT_EQ(fixturePx(&f, 8, 6), sp[3 * 8]);
    ASSERT_EQ(fixturePx(&f, 8, 7), (uint32_t)0xFF000000u);
    ASSERT_TRUE(guardIntact(&f));
    /* completely outside: nothing happens */
    gfxBlit(&f.c, 100, 100, &src, (GfxRect){0, 0, 8, 8}, GFX_OP_SRC, 255);
    gfxBlit(&f.c, 0, 0, &src, (GfxRect){20, 20, 30, 30}, GFX_OP_SRC, 255);
    gfxBlit(&f.c, 0, 0, &src, (GfxRect){INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX}, GFX_OP_SRC,
            255);
    /* srcRect's origin maps to (dx, dy), so a huge negative srcRect origin lands far off-canvas */
    ASSERT_EQ(fixturePx(&f, 0, 0), (uint32_t)0xFF000000u);
    ASSERT_TRUE(guardIntact(&f));
    fixtureFree(&f);
}

TEST(gfxBlitAlphaAndOps) {
    uint32_t sp[4] = {0xFFFF0000u, 0x80800000u, 0x00000000u, 0xFF00FF00u};
    GfxSurface src = {sp, 2, 2, 2};
    Fixture f;
    ASSERT_TRUE(fixtureInit(&f, 2, 2, 0xFF000000u));
    gfxBlit(&f.c, 0, 0, &src, (GfxRect){0, 0, 2, 2}, GFX_OP_SRC_OVER, 255);
    ASSERT_EQ(fixturePx(&f, 0, 0), (uint32_t)0xFFFF0000u);
    ASSERT_EQ(fixturePx(&f, 1, 0), (uint32_t)0xFF800000u);
    ASSERT_EQ(fixturePx(&f, 0, 1), (uint32_t)0xFF000000u); /* transparent source */
    /* global alpha 0 draws nothing for OVER */
    gfxBlit(&f.c, 0, 0, &src, (GfxRect){0, 0, 2, 2}, GFX_OP_SRC_OVER, 0);
    ASSERT_EQ(fixturePx(&f, 0, 0), (uint32_t)0xFFFF0000u);
    /* SRC with a non-255 alpha still mixes with the destination */
    gfxBlit(&f.c, 0, 0, &src, (GfxRect){0, 0, 2, 2}, GFX_OP_SRC, 128);
    ASSERT_TRUE(premulValid(fixturePx(&f, 0, 0)));
    ASSERT_TRUE((fixturePx(&f, 0, 1) >> 24) < 255u);
    /* an invalid (straight-alpha-looking) source pixel is sanitized when blended */
    uint32_t bad[1] = {0x10FFFFFFu};
    GfxSurface badSrc = {bad, 1, 1, 1};
    gfxBlit(&f.c, 0, 0, &badSrc, (GfxRect){0, 0, 1, 1}, GFX_OP_SRC_OVER, 255);
    ASSERT_TRUE(premulValid(fixturePx(&f, 0, 0)));
    fixtureFree(&f);
}

/* Moving a region within one surface must behave as if the source were copied first, whichever
 * direction the move goes. */
TEST(gfxBlitOverlappingSelfCopy) {
    for (int dir = 0; dir < 4; dir++) {
        Fixture f;
        ASSERT_TRUE(fixtureInit(&f, 8, 8, 0));
        uint32_t orig[64];
        for (int i = 0; i < 64; i++) {
            orig[i] = 0xFF000000u | (uint32_t)(i + 1);
            f.c.surf.pixels[(i / 8) * f.c.surf.stride + (i % 8)] = orig[i];
        }
        int32_t dx = (dir & 1) ? 2 : -2, dy = (dir & 2) ? 1 : 0;
        if (dy == 0 && dir == 0) {
            dy = -1;
        }
        gfxBlit(&f.c, dx, dy, &f.c.surf, (GfxRect){0, 0, 8, 8}, GFX_OP_SRC, 255);
        for (int32_t y = 0; y < 8; y++) {
            for (int32_t x = 0; x < 8; x++) {
                int32_t sx = x - dx, sy = y - dy;
                if (sx >= 0 && sx < 8 && sy >= 0 && sy < 8) {
                    ASSERT_EQ(fixturePx(&f, x, y), orig[sy * 8 + sx]);
                }
            }
        }
        ASSERT_TRUE(guardIntact(&f));
        fixtureFree(&f);
    }
}

TEST(gfxFillMaskCoverageAndClip) {
    uint8_t md[4 * 2] = {0, 64, 128, 255, 255, 128, 64, 0};
    GfxMask m = {md, 4, 2, 4};
    Fixture f;
    ASSERT_TRUE(fixtureInit(&f, 6, 4, 0xFF000000u));
    gfxFillMask(&f.c, 1, 1, &m, 0xFFFFFFFFu);
    ASSERT_EQ(fixturePx(&f, 1, 1), (uint32_t)0xFF000000u); /* coverage 0 */
    ASSERT_EQ(fixturePx(&f, 4, 1), (uint32_t)0xFFFFFFFFu); /* coverage 255 */
    uint32_t mid = fixturePx(&f, 3, 1);                    /* coverage 128 */
    ASSERT_EQ(mid, (uint32_t)0xFF808080u);
    ASSERT_TRUE(premulValid(fixturePx(&f, 2, 1)));
    /* hanging off every edge, and far away, is safe */
    gfxFillMask(&f.c, -2, -1, &m, 0xFF00FF00u);
    gfxFillMask(&f.c, 5, 3, &m, 0xFF00FF00u);
    gfxFillMask(&f.c, INT32_MAX, INT32_MIN, &m, 0xFF00FF00u);
    ASSERT_TRUE(guardIntact(&f));
    fixtureFree(&f);
}

TEST(gfxRectHelpers) {
    GfxRect a = {0, 0, 10, 10}, b = {5, 5, 20, 8};
    GfxRect i = gfxRectIntersect(a, b);
    ASSERT_EQ(i.x0, 5);
    ASSERT_EQ(i.y1, 8);
    GfxRect none = gfxRectIntersect(a, (GfxRect){10, 0, 12, 10}); /* touching only */
    ASSERT_TRUE(gfxRectIsEmpty(none));
    GfxRect u = gfxRectUnion(a, b);
    ASSERT_EQ(u.x1, 20);
    ASSERT_EQ(u.y0, 0);
    GfxRect empty = {3, 3, 3, 9};
    GfxRect u2 = gfxRectUnion(empty, b);
    ASSERT_EQ(u2.x0, 5);
    ASSERT_TRUE(gfxRectIsEmpty(gfxRectUnion(empty, empty)));
}

/* SRC with a global alpha below 255 blends pixel by pixel (no memmove); an overlapping move within
 * one surface must still read every source pixel before it is overwritten, in every direction,
 * horizontal-only moves included. Reference: the same blit from a separate copy. */
TEST(gfxBlitOverlappingSelfBlendMatchesCopy) {
    static const int32_t D[][2] = {{2, 0}, {-2, 0}, {0, 1}, {0, -1}, {3, 2}, {-3, -2}, {1, -1}};
    for (size_t k = 0; k < sizeof(D) / sizeof(D[0]); k++) {
        for (int op = 0; op < 2; op++) {
            Fixture f, g;
            ASSERT_TRUE(fixtureInit(&f, 9, 7, 0));
            ASSERT_TRUE(fixtureInit(&g, 9, 7, 0));
            rngSeed(0x5E1F + (uint32_t)k);
            for (int32_t y = 0; y < 7; y++) {
                for (int32_t x = 0; x < 9; x++) {
                    uint32_t v = randPremul();
                    f.c.surf.pixels[y * f.c.surf.stride + x] = v;
                    g.c.surf.pixels[y * g.c.surf.stride + x] = v;
                }
            }
            uint32_t copy[9 * 7];
            for (int32_t y = 0; y < 7; y++) {
                for (int32_t x = 0; x < 9; x++) {
                    copy[y * 9 + x] = fixturePx(&f, x, y);
                }
            }
            GfxSurface src = {copy, 9, 7, 9};
            GfxOp o = op ? GFX_OP_SRC_OVER : GFX_OP_SRC;
            uint8_t alpha = op ? 255 : 128; /* SRC_OVER at 255 is the same per-pixel path */
            gfxBlit(&f.c, 1 + D[k][0], 1 + D[k][1], &f.c.surf, (GfxRect){1, 1, 8, 6}, o, alpha);
            gfxBlit(&g.c, 1 + D[k][0], 1 + D[k][1], &src, (GfxRect){1, 1, 8, 6}, o, alpha);
            for (int32_t y = 0; y < 7; y++) {
                for (int32_t x = 0; x < 9; x++) {
                    ASSERT_EQ(fixturePx(&f, x, y), fixturePx(&g, x, y));
                }
            }
            ASSERT_TRUE(guardIntact(&f));
            fixtureFree(&f);
            fixtureFree(&g);
        }
    }
}

/* The exact values of both ops against an independent formulation of D-141's math: each product
 * is round(a*b/255) half up, i.e. (2ab + 255) / 510. */
static uint32_t refMul(uint32_t a, uint32_t b) {
    return (2u * a * b + 255u) / 510u;
}

TEST(gfxBlendMatchesReferenceFormula) {
    rngSeed(0xD141);
    for (int i = 0; i < 300000; i++) {
        uint32_t dst = randPremul(), src = randPremul();
        uint32_t cov = (i & 7) == 0 ? 255u : (i & 7) == 1 ? 0u : (rngNext() & 0xFFu);
        uint32_t s[4], d[4], over = 0, srcOp = 0;
        for (int c = 0; c < 4; c++) {
            s[c] = refMul((src >> (8 * c)) & 0xFFu, cov);
            d[c] = (dst >> (8 * c)) & 0xFFu;
        }
        for (int c = 0; c < 4; c++) {
            over |= (s[c] + refMul(d[c], 255u - s[3])) << (8 * c);
            srcOp |= (s[c] + refMul(d[c], 255u - cov)) << (8 * c);
        }
        ASSERT_EQ(gfxBlendPixel(dst, src, cov, GFX_OP_SRC_OVER), over);
        ASSERT_EQ(gfxBlendPixel(dst, src, cov, GFX_OP_SRC), srcOp);
    }
    /* the span helpers agree with the pixel function */
    uint32_t a[64], b[64];
    uint8_t cv[64];
    for (int k = 0; k < 200; k++) {
        uint32_t col = randPremul();
        GfxOp op = (k & 1) ? GFX_OP_SRC : GFX_OP_SRC_OVER;
        for (int i = 0; i < 64; i++) {
            a[i] = b[i] = randPremul();
            cv[i] = (uint8_t)((i & 3) == 0 ? 0 : (i & 3) == 1 ? 255 : rngNext());
        }
        gfxBlendSpanCov(a, cv, 64, col, op);
        for (int i = 0; i < 64; i++) {
            ASSERT_EQ(a[i], cv[i] == 0 ? b[i] : gfxBlendPixel(b[i], col, cv[i], op));
        }
        gfxBlendSpan(a, 64, col, op);
        for (int i = 0; i < 64; i++) {
            uint32_t prev = cv[i] == 0 ? b[i] : gfxBlendPixel(b[i], col, cv[i], op);
            ASSERT_EQ(a[i], gfxBlendPixel(prev, col, 255u, op));
        }
    }
}

/* Saturation, not wrap-around: a rect reaching INT32_MAX/MIN under a nonzero origin still covers
 * the whole surface, an origin pushed past INT32_MAX stays there (nothing becomes visible), and
 * the clip bounds of a saturated origin are empty rather than a wrapped, bogus rect. */
TEST(gfxOriginAndRectSaturate) {
    static const int32_t O[][2] = {{5, 5}, {-5, -5}, {INT32_MAX, 0}, {INT32_MIN, 0}};
    for (size_t k = 0; k < 2; k++) {
        Fixture f;
        ASSERT_TRUE(fixtureInit(&f, 6, 5, 0xFF000000u));
        gfxCanvasSetOrigin(&f.c, O[k][0], O[k][1]);
        gfxFillRect(&f.c, (GfxRect){INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX}, 0xFFFFFFFFu,
                    GFX_OP_SRC);
        for (int32_t y = 0; y < 5; y++) {
            for (int32_t x = 0; x < 6; x++) {
                ASSERT_EQ(fixturePx(&f, x, y), 0xFFFFFFFFu);
            }
        }
        ASSERT_TRUE(guardIntact(&f));
        fixtureFree(&f);
    }
    Fixture f;
    ASSERT_TRUE(fixtureInit(&f, 6, 5, 0xFF000000u));
    gfxCanvasTranslate(&f.c, INT32_MAX, INT32_MIN);
    gfxCanvasTranslate(&f.c, INT32_MAX, INT32_MIN); /* wrapping would give (-2, 0) */
    ASSERT_EQ(f.c.originX, INT32_MAX);
    ASSERT_EQ(f.c.originY, INT32_MIN);
    gfxFillRect(&f.c, (GfxRect){-4, 0, 4, 4}, 0xFFFFFFFFu, GFX_OP_SRC);
    ASSERT_EQ(fixturePx(&f, 0, 0), 0xFF000000u);
    for (size_t k = 2; k < 4; k++) {
        gfxCanvasSetOrigin(&f.c, O[k][0], O[k][1]);
        GfxRect b = gfxCanvasClipBounds(&f.c);
        ASSERT_TRUE(b.x0 <= b.x1);
        ASSERT_TRUE(k == 2 ? (b.x0 == -INT32_MAX && b.x1 == 6 - INT32_MAX)
                           : (b.x0 == INT32_MAX && b.x1 == INT32_MAX));
    }
    fixtureFree(&f);
}

/* Every entry point sanitizes (D-141): a straight-alpha color or source pixel drawn over a
 * transparent destination gives exactly the clamped premultiplied value, never an invalid one. */
TEST(gfxEntryPointsSanitizeExactly) {
    for (int op = 0; op < 2; op++) {
        GfxOp o = op ? GFX_OP_SRC : GFX_OP_SRC_OVER;
        Fixture f;
        ASSERT_TRUE(fixtureInit(&f, 4, 4, 0));
        gfxFillRect(&f.c, (GfxRect){0, 0, 2, 2}, 0x40FFFF20u, o);
        ASSERT_EQ(fixturePx(&f, 1, 1), 0x40404020u);
        uint32_t bad[1] = {0x40FF20FFu};
        GfxSurface bs = {bad, 1, 1, 1};
        gfxBlit(&f.c, 3, 3, &bs, (GfxRect){0, 0, 1, 1}, o, op ? 200 : 255);
        ASSERT_EQ(fixturePx(&f, 3, 3), op ? 0x32321932u : 0x40402040u);
        fixtureFree(&f);
    }
    uint8_t md[1] = {255};
    GfxMask m = {md, 1, 1, 1};
    Fixture f;
    ASSERT_TRUE(fixtureInit(&f, 2, 2, 0));
    gfxFillMask(&f.c, 1, 0, &m, 0x40FFFFFFu);
    ASSERT_EQ(fixturePx(&f, 1, 0), 0x40404040u);
    ASSERT_EQ(fixturePx(&f, 0, 0), 0u);
    fixtureFree(&f);
}

/* A mask whose stride is below its width is invalid (as gfxFillPathMask already says) and draws
 * nothing, instead of reading past the end of its last row. */
TEST(gfxFillMaskRejectsShortStride) {
    uint8_t *md = malloc(4 * 3); /* stride 3 x height 4, but width 5 */
    ASSERT_TRUE(md != NULL);
    memset(md, 255, 4 * 3);
    GfxMask m = {md, 5, 4, 3};
    Fixture f;
    ASSERT_TRUE(fixtureInit(&f, 8, 8, 0xFF000000u));
    gfxFillMask(&f.c, 0, 0, &m, 0xFFFFFFFFu);
    for (int32_t y = 0; y < 8; y++) {
        for (int32_t x = 0; x < 8; x++) {
            ASSERT_EQ(fixturePx(&f, x, y), 0xFF000000u);
        }
    }
    fixtureFree(&f);
    free(md);
}

/* Likewise a blit source with stride < width (gfxCanvasInit rejects such a surface) is ignored
 * instead of read past its end. */
TEST(gfxBlitRejectsShortStrideSource) {
    uint32_t *sp = malloc(3 * 4 * sizeof(uint32_t)); /* stride 3 x height 4, but width 5 */
    ASSERT_TRUE(sp != NULL);
    for (int i = 0; i < 12; i++) {
        sp[i] = 0xFFFFFFFFu;
    }
    GfxSurface src = {sp, 5, 4, 3};
    Fixture f;
    ASSERT_TRUE(fixtureInit(&f, 8, 8, 0xFF000000u));
    gfxBlit(&f.c, 0, 0, &src, (GfxRect){0, 0, 5, 4}, GFX_OP_SRC, 255);
    gfxBlit(&f.c, 0, 0, &src, (GfxRect){0, 0, 5, 4}, GFX_OP_SRC_OVER, 200);
    for (int32_t y = 0; y < 8; y++) {
        for (int32_t x = 0; x < 8; x++) {
            ASSERT_EQ(fixturePx(&f, x, y), 0xFF000000u);
        }
    }
    fixtureFree(&f);
    free(sp);
}
