/* Golden-image tests for libs/gfx (M12.2, D-147). Each test also asserts a few pixel values
 * directly, so the checked-in reference is never the only oracle. */
#include "framework/test.h"
#include "gfx/gfx.h"
#include "gfx_golden.h"

#include <stdlib.h>

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
