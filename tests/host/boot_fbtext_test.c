/* Host tests for boot/common/fbtext.c (ARCHITECTURE §5.2/§19, M1.4). Uses a heap-allocated
 * buffer standing in for the real MMIO/HHDM framebuffer mapping (fbtext.c itself never reads the
 * buffer back, so a plain heap buffer exercises exactly the same write paths a real one would).
 * Every test uses a standard 32-bpp RGBX layout: redShift=0/greenShift=8/blueShift=16, size 8. */
#include "fbtext.h"
#include "framework/test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RS  0
#define RSZ 8
#define GS  8
#define GSZ 8
#define BS  16
#define BSZ 8

static uint8_t *allocBuf(uint32_t width, uint32_t height, uint32_t pitch) {
    uint8_t *buf = calloc(1, (size_t)pitch * height);
    if (buf == NULL) {
        fprintf(stderr, "allocBuf: out of memory\n");
        abort();
    }
    (void)width;
    return buf;
}

static uint32_t readPixel(uint8_t *buf, uint32_t pitch, uint32_t x, uint32_t y) {
    uint8_t *p = buf + (size_t)y * pitch + (size_t)x * 4;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

TEST(fbTextInitRejectsNullBuffer) {
    BootFbText fx;
    ASSERT_EQ(fbTextInit(&fx, NULL, 1024, 768, 1024 * 4, RS, RSZ, GS, GSZ, BS, BSZ),
              BOOT_ERR_FB_UNSUPPORTED);
}

TEST(fbTextInitRejectsShortPitch) {
    uint8_t *buf = allocBuf(1024, 768, 1024 * 4);
    BootFbText fx;
    ASSERT_EQ(fbTextInit(&fx, buf, 1024, 768, 1024 * 4 - 4, RS, RSZ, GS, GSZ, BS, BSZ),
              BOOT_ERR_FB_UNSUPPORTED);
    free(buf);
}

TEST(fbTextInitRejectsUnalignedPitch) {
    uint8_t *buf = allocBuf(1024, 768, 1024 * 4 + 3);
    BootFbText fx;
    ASSERT_EQ(fbTextInit(&fx, buf, 1024, 768, 1024 * 4 + 3, RS, RSZ, GS, GSZ, BS, BSZ),
              BOOT_ERR_FB_UNSUPPORTED);
    free(buf);
}

TEST(fbTextInitRejectsTooSmall) {
    uint8_t *buf = allocBuf(4, 4, 4 * 4);
    BootFbText fx;
    ASSERT_EQ(fbTextInit(&fx, buf, 4, 4, 4 * 4, RS, RSZ, GS, GSZ, BS, BSZ),
              BOOT_ERR_FB_UNSUPPORTED);
    free(buf);
}

TEST(fbTextInitRejectsBadChannelSize) {
    uint8_t *buf = allocBuf(1024, 768, 1024 * 4);
    BootFbText fx;
    ASSERT_EQ(fbTextInit(&fx, buf, 1024, 768, 1024 * 4, RS, 0, GS, GSZ, BS, BSZ),
              BOOT_ERR_FB_UNSUPPORTED);
    ASSERT_EQ(fbTextInit(&fx, buf, 1024, 768, 1024 * 4, RS, 9, GS, GSZ, BS, BSZ),
              BOOT_ERR_FB_UNSUPPORTED);
    free(buf);
}

TEST(fbTextInitRejectsShiftOverflow) {
    uint8_t *buf = allocBuf(1024, 768, 1024 * 4);
    BootFbText fx;
    ASSERT_EQ(fbTextInit(&fx, buf, 1024, 768, 1024 * 4, 28, 8, GS, GSZ, BS, BSZ),
              BOOT_ERR_FB_UNSUPPORTED);
    free(buf);
}

TEST(fbTextInitComputesScaleAndGrid1024x768) {
    uint8_t *buf = allocBuf(1024, 768, 1024 * 4);
    BootFbText fx;
    ASSERT_EQ(fbTextInit(&fx, buf, 1024, 768, 1024 * 4, RS, RSZ, GS, GSZ, BS, BSZ), BOOT_OK);
    ASSERT_EQ(fx.scale, (uint32_t)1);
    ASSERT_EQ(fx.cols, (uint32_t)(1024 / 8));
    ASSERT_EQ(fx.rows, (uint32_t)(768 / 16));
    free(buf);
}

TEST(fbTextInitComputesScaleAndGrid2560x1440) {
    uint8_t *buf = allocBuf(2560, 1440, 2560 * 4);
    BootFbText fx;
    ASSERT_EQ(fbTextInit(&fx, buf, 2560, 1440, 2560 * 4, RS, RSZ, GS, GSZ, BS, BSZ), BOOT_OK);
    ASSERT_EQ(fx.scale, (uint32_t)2);
    ASSERT_EQ(fx.cols, (uint32_t)(2560 / 16));
    ASSERT_EQ(fx.rows, (uint32_t)(1440 / 32));
    free(buf);
}

TEST(fbTextInitPalettePacksChannelsCorrectly) {
    uint8_t *buf = allocBuf(1024, 768, 1024 * 4);
    BootFbText fx;
    ASSERT_EQ(fbTextInit(&fx, buf, 1024, 768, 1024 * 4, RS, RSZ, GS, GSZ, BS, BSZ), BOOT_OK);
    /* palette[1] is ANSI red, 0xAA0000. RGBX with R at shift 0: low byte 0xAA, mid 0x00, high
     * 0x00. */
    ASSERT_EQ(fx.palette[1] & 0xFFu, (uint32_t)0xAA);
    ASSERT_EQ((fx.palette[1] >> 8) & 0xFFu, (uint32_t)0x00);
    ASSERT_EQ((fx.palette[1] >> 16) & 0xFFu, (uint32_t)0x00);
    /* palette[15] is white, 0xFFFFFF -- every channel maxed. */
    ASSERT_EQ(fx.palette[15] & 0xFFFFFFu, (uint32_t)0xFFFFFF);
    free(buf);
}

TEST(fbTextClearFillsEveryPixel) {
    uint32_t w = 1024, h = 768, pitch = w * 4;
    uint8_t *buf = allocBuf(w, h, pitch);
    BootFbText fx;
    ASSERT_EQ(fbTextInit(&fx, buf, w, h, pitch, RS, RSZ, GS, GSZ, BS, BSZ), BOOT_OK);
    fbTextClear(&fx, 4); /* ANSI blue {0,0,0xAA}; blueShift=16 -> packed 0xAA0000 */
    ASSERT_EQ(readPixel(buf, pitch, 0, 0) & 0xFFFFFFu, (uint32_t)0xAA0000);
    ASSERT_EQ(readPixel(buf, pitch, w - 1, h - 1) & 0xFFFFFFu, (uint32_t)0xAA0000);
    ASSERT_EQ(readPixel(buf, pitch, w / 2, h / 2) & 0xFFFFFFu, (uint32_t)0xAA0000);
    free(buf);
}

TEST(fbTextPutCharSpaceIsAllBackground) {
    uint32_t w = 1024, h = 768, pitch = w * 4;
    uint8_t *buf = allocBuf(w, h, pitch);
    BootFbText fx;
    ASSERT_EQ(fbTextInit(&fx, buf, w, h, pitch, RS, RSZ, GS, GSZ, BS, BSZ), BOOT_OK);
    fbTextClear(&fx, 0);
    fbTextPutChar(&fx, 0, 0, ' ', 7 /* light gray fg */, 1 /* red bg */);
    for (uint32_t y = 0; y < 16 * fx.scale; y++) {
        for (uint32_t x = 0; x < 8 * fx.scale; x++) {
            ASSERT_EQ(readPixel(buf, pitch, x, y) & 0xFFFFFFu,
                      (uint32_t)0x0000AA); /* red {0xAA,0,0}, redShift=0 */
        }
    }
    free(buf);
}

TEST(fbTextPutCharOutOfRangeUsesReplacementGlyph) {
    uint32_t w = 1024, h = 768, pitch = w * 4;
    uint8_t *bufA = allocBuf(w, h, pitch);
    uint8_t *bufB = allocBuf(w, h, pitch);
    BootFbText fxA, fxB;
    ASSERT_EQ(fbTextInit(&fxA, bufA, w, h, pitch, RS, RSZ, GS, GSZ, BS, BSZ), BOOT_OK);
    ASSERT_EQ(fbTextInit(&fxB, bufB, w, h, pitch, RS, RSZ, GS, GSZ, BS, BSZ), BOOT_OK);
    fbTextPutChar(&fxA, 0, 0, '\0', 7, 0);       /* '\0' -> glyph index 0 directly */
    fbTextPutChar(&fxB, 0, 0, (char)0x80, 7, 0); /* out of the 0-127 PSF2 range -> also 0 */
    ASSERT_EQ(memcmp(bufA, bufB, (size_t)pitch * h), 0);
    free(bufA);
    free(bufB);
}

TEST(fbTextPutCharOutOfGridIsNoOp) {
    uint32_t w = 1024, h = 768, pitch = w * 4;
    uint8_t *buf = allocBuf(w, h, pitch);
    uint8_t *before = allocBuf(w, h, pitch);
    BootFbText fx;
    ASSERT_EQ(fbTextInit(&fx, buf, w, h, pitch, RS, RSZ, GS, GSZ, BS, BSZ), BOOT_OK);
    memcpy(before, buf, (size_t)pitch * h);
    fbTextPutChar(&fx, fx.rows, 0, 'A', 7, 1);
    fbTextPutChar(&fx, 0, fx.cols, 'A', 7, 1);
    ASSERT_EQ(memcmp(before, buf, (size_t)pitch * h), 0);
    free(buf);
    free(before);
}

TEST(fbTextPutStringClipsAtGridEdge) {
    uint32_t w = 1024, h = 768, pitch = w * 4;
    uint8_t *buf = allocBuf(w, h, pitch);
    BootFbText fx;
    ASSERT_EQ(fbTextInit(&fx, buf, w, h, pitch, RS, RSZ, GS, GSZ, BS, BSZ), BOOT_OK);
    fbTextClear(&fx, 0);
    /* A string that runs off the right edge shouldn't crash or corrupt memory (ASan would catch
     * an out-of-bounds write); just confirm it returns normally and leaves the buffer intact
     * elsewhere. */
    char longLine[600];
    memset(longLine, 'X', sizeof(longLine) - 1);
    longLine[sizeof(longLine) - 1] = '\0';
    fbTextPutString(&fx, 0, 0, longLine, 7, 0);
    ASSERT_EQ(readPixel(buf, pitch, w - 1, h - 1) & 0xFFFFFFu, (uint32_t)0x000000);
    free(buf);
}

TEST(fbTextFillRowFillsOnlyThatRow) {
    uint32_t w = 1024, h = 768, pitch = w * 4;
    uint8_t *buf = allocBuf(w, h, pitch);
    BootFbText fx;
    ASSERT_EQ(fbTextInit(&fx, buf, w, h, pitch, RS, RSZ, GS, GSZ, BS, BSZ), BOOT_OK);
    fbTextClear(&fx, 0);
    fbTextFillRow(&fx, 2, 0, fx.cols, 4); /* ANSI blue */
    ASSERT_EQ(readPixel(buf, pitch, 0, 2 * 16 * fx.scale) & 0xFFFFFFu, (uint32_t)0xAA0000);
    ASSERT_EQ(readPixel(buf, pitch, 0, 1 * 16 * fx.scale) & 0xFFFFFFu, (uint32_t)0x000000);
    ASSERT_EQ(readPixel(buf, pitch, 0, 3 * 16 * fx.scale) & 0xFFFFFFu, (uint32_t)0x000000);
    free(buf);
}

TEST(fbTextFillRowClampsColumnRange) {
    uint32_t w = 1024, h = 768, pitch = w * 4;
    uint8_t *buf = allocBuf(w, h, pitch);
    BootFbText fx;
    ASSERT_EQ(fbTextInit(&fx, buf, w, h, pitch, RS, RSZ, GS, GSZ, BS, BSZ), BOOT_OK);
    fbTextClear(&fx, 0);
    /* col1 far beyond fx.cols must clamp, not write out of bounds (ASan catches that). */
    fbTextFillRow(&fx, 0, 0, fx.cols + 1000, 4);
    ASSERT_EQ(readPixel(buf, pitch, w - 1, 0) & 0xFFFFFFu, (uint32_t)0xAA0000);
    free(buf);
}
