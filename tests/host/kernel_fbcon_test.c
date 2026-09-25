/* Host tests for kernel/drivers/fbcon/fbcon.c (ARCHITECTURE §19, M1.4). Uses a heap-allocated
 * buffer standing in for the real HHDM-mapped framebuffer, with hhdmBase=0 so fbconInit's
 * `hhdmBase + fb->phys` addition is a no-op and fb->phys can just be the buffer's own pointer
 * value -- fbcon never reads the framebuffer back, so a plain heap buffer exercises exactly the
 * same write paths a real mapping would. */
#include "fbcon.h"
#include "framework/test.h"

#include <stdio.h>
#include <stdlib.h>

static uint8_t *makeFb(BootFramebuffer *fb, uint32_t width, uint32_t height) {
    uint32_t pitch = width * 4u;
    uint8_t *buf = calloc(1, (size_t)pitch * height);
    if (buf == NULL) {
        fprintf(stderr, "makeFb: out of memory\n");
        abort();
    }
    fb->phys = (uint64_t)(uintptr_t)buf;
    fb->width = width;
    fb->height = height;
    fb->pitch = pitch;
    fb->bpp = 32;
    fb->redShift = 0;
    fb->redSize = 8;
    fb->greenShift = 8;
    fb->greenSize = 8;
    fb->blueShift = 16;
    fb->blueSize = 8;
    fb->reserved[0] = fb->reserved[1] = 0;
    return buf;
}

/* Regression test for the reviewer-found hang: with fx.cols == 1, a '\t' used to loop on
 * `while (cursorCol < next)` forever, since putGlyph() wrapping cursorCol from 0 back to 0 (via
 * newline()) never satisfies `cursorCol < before`. If this test doesn't return, the bug is
 * back. */
TEST(fbconTabDoesNotHangAtOneColumn) {
    BootFramebuffer fb;
    /* width in [8,15) with CONSOLE_FONT_WIDTH=8 and scale 1 (width < 1280) -> fx.cols == 1. */
    uint8_t *buf = makeFb(&fb, 8, 32);
    ASSERT_EQ(fbconInit(&fb, 0), STATUS_OK);
    ASSERT_EQ(fbconCols(), 1u);

    fbconWrite("\t", 1);
    fbconWrite("a\tb\tc", 5);
    fbconWrite("\t\t\t", 3);
    free(buf);
}

TEST(fbconTabAdvancesToNextMultipleOfEight) {
    BootFramebuffer fb;
    uint8_t *buf = makeFb(&fb, 640, 160); /* scale 1, cols = 80, rows = 10 */
    ASSERT_EQ(fbconInit(&fb, 0), STATUS_OK);
    ASSERT_EQ(fbconCols(), 80u);

    fbconWrite("a", 1);   /* cursorCol: 0 -> 1 */
    fbconWrite("\t", 1);  /* tab from col 1 -> col 8 */
    fbconWrite("b", 1);   /* col 8 -> 9 */
    fbconWrite("\t", 1);  /* col 9 -> 16 */
    /* No assertion on internal cursor state (private to fbcon.c) -- this just exercises the
     * normal, non-edge-case path alongside the cols==1 regression above. */
    free(buf);
}

TEST(fbconTabClampsToLastColumn) {
    BootFramebuffer fb;
    /* CONSOLE_FONT_WIDTH=8, width=40 -> cols = 5: a tab from col 3 wants col 8, which is past
     * cols (5) and must clamp instead of wrapping mid-tab. */
    uint8_t *buf = makeFb(&fb, 40, 16);
    ASSERT_EQ(fbconInit(&fb, 0), STATUS_OK);
    ASSERT_EQ(fbconCols(), 5u);

    fbconWrite("abc", 3); /* cursorCol -> 3 */
    fbconWrite("\t", 1);  /* wants col 8, clamps to col 5 == cols -> wraps to next row once */
    fbconWrite("z", 1);   /* must land on the new row's column 0, not hang or overrun */
    free(buf);
}
