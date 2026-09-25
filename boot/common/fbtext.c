/* See fbtext.h. No libc, no division beyond plain uint32_t (D-065 forbids only `long`/64-bit
 * division, and every value here already fits in uint32_t after fbTextInit's own validation). */
#include "include/fbtext.h"

#include <stddef.h>

#include "include/console-font.h"

/* ANSI 16-color order (D-069), {R, G, B} each 0-255. */
static const uint8_t ansiPalette[FB_TEXT_PALETTE_SIZE][3] = {
    {0x00, 0x00, 0x00}, {0xAA, 0x00, 0x00}, {0x00, 0xAA, 0x00}, {0xAA, 0x55, 0x00},
    {0x00, 0x00, 0xAA}, {0xAA, 0x00, 0xAA}, {0x00, 0xAA, 0xAA}, {0xAA, 0xAA, 0xAA},
    {0x55, 0x55, 0x55}, {0xFF, 0x55, 0x55}, {0x55, 0xFF, 0x55}, {0xFF, 0xFF, 0x55},
    {0x55, 0x55, 0xFF}, {0xFF, 0x55, 0xFF}, {0x55, 0xFF, 0xFF}, {0xFF, 0xFF, 0xFF},
};

static uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static uint32_t minU32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

/* `(component >> (8 - size)) << shift`, D-069's exact formula for turning an 8-bit channel value
 * into its bits within a 32-bit pixel. */
static uint32_t packChannel(uint8_t component, uint8_t shift, uint8_t size) {
    return ((uint32_t)(component >> (8 - size))) << shift;
}

BootStatus fbTextInit(BootFbText *fx, uint8_t *pixels, uint32_t width, uint32_t height,
                      uint32_t pitchBytes, uint8_t redShift, uint8_t redSize, uint8_t greenShift,
                      uint8_t greenSize, uint8_t blueShift, uint8_t blueSize) {
    if (fx == NULL) {
        return BOOT_ERR_FB_UNSUPPORTED;
    }
    fx->pixels = 0;
    fx->width = fx->height = fx->pitchBytes = fx->scale = fx->cols = fx->rows = 0;
    for (int i = 0; i < FB_TEXT_PALETTE_SIZE; i++) {
        fx->palette[i] = 0;
    }

    if (pixels == NULL || width < CONSOLE_FONT_WIDTH || height < CONSOLE_FONT_HEIGHT) {
        return BOOT_ERR_FB_UNSUPPORTED;
    }
    if (pitchBytes < width * 4u || pitchBytes % 4u != 0u) {
        return BOOT_ERR_FB_UNSUPPORTED;
    }
    if (redSize < 1 || redSize > 8 || greenSize < 1 || greenSize > 8 || blueSize < 1 ||
        blueSize > 8) {
        return BOOT_ERR_FB_UNSUPPORTED;
    }
    if ((uint32_t)redShift + redSize > 32 || (uint32_t)greenShift + greenSize > 32 ||
        (uint32_t)blueShift + blueSize > 32) {
        return BOOT_ERR_FB_UNSUPPORTED;
    }

    fx->pixels = pixels;
    fx->width = width;
    fx->height = height;
    fx->pitchBytes = pitchBytes;

    uint32_t scale = clampU32(minU32(width / 1280u, height / 720u), 1u, 4u);
    fx->scale = scale;
    fx->cols = minU32(width / (CONSOLE_FONT_WIDTH * scale), 480u);
    fx->rows = minU32(height / (CONSOLE_FONT_HEIGHT * scale), 270u);

    for (int i = 0; i < FB_TEXT_PALETTE_SIZE; i++) {
        fx->palette[i] = packChannel(ansiPalette[i][0], redShift, redSize) |
                         packChannel(ansiPalette[i][1], greenShift, greenSize) |
                         packChannel(ansiPalette[i][2], blueShift, blueSize);
    }
    return BOOT_OK;
}

static void putPixel(BootFbText *fx, uint32_t px, uint32_t py, uint32_t color) {
    if (px >= fx->width || py >= fx->height) {
        return;
    }
    uint8_t *dst = fx->pixels + (uint64_t)py * fx->pitchBytes + (uint64_t)px * 4u;
    dst[0] = (uint8_t)(color & 0xFF);
    dst[1] = (uint8_t)((color >> 8) & 0xFF);
    dst[2] = (uint8_t)((color >> 16) & 0xFF);
    dst[3] = (uint8_t)((color >> 24) & 0xFF);
}

static uint32_t paletteColor(const BootFbText *fx, uint8_t idx) {
    if (idx >= FB_TEXT_PALETTE_SIZE) {
        idx = 0;
    }
    return fx->palette[idx];
}

void fbTextClear(BootFbText *fx, uint8_t bg) {
    if (fx == NULL || fx->pixels == NULL) {
        return;
    }
    uint32_t color = paletteColor(fx, bg);
    for (uint32_t y = 0; y < fx->height; y++) {
        for (uint32_t x = 0; x < fx->width; x++) {
            putPixel(fx, x, y, color);
        }
    }
}

void fbTextPutChar(BootFbText *fx, uint32_t row, uint32_t col, char ch, uint8_t fg, uint8_t bg) {
    if (fx == NULL || fx->pixels == NULL || row >= fx->rows || col >= fx->cols) {
        return;
    }
    uint32_t idx = (uint32_t)(unsigned char)ch;
    if (idx >= CONSOLE_FONT_LENGTH) {
        idx = 0; /* the replacement glyph */
    }
    const uint8_t *glyphRows = fontConsolePsf + PSF2_HEADERSIZE + idx * CONSOLE_FONT_HEIGHT;
    uint32_t fgColor = paletteColor(fx, fg);
    uint32_t bgColor = paletteColor(fx, bg);
    uint32_t scale = fx->scale;
    uint32_t originX = col * CONSOLE_FONT_WIDTH * scale;
    uint32_t originY = row * CONSOLE_FONT_HEIGHT * scale;

    for (uint32_t fr = 0; fr < CONSOLE_FONT_HEIGHT; fr++) {
        uint8_t rowBits = glyphRows[fr];
        for (uint32_t fc = 0; fc < CONSOLE_FONT_WIDTH; fc++) {
            uint32_t bit = (rowBits >> (CONSOLE_FONT_WIDTH - 1 - fc)) & 1u;
            uint32_t color = bit ? fgColor : bgColor;
            uint32_t baseX = originX + fc * scale;
            uint32_t baseY = originY + fr * scale;
            for (uint32_t dy = 0; dy < scale; dy++) {
                for (uint32_t dx = 0; dx < scale; dx++) {
                    putPixel(fx, baseX + dx, baseY + dy, color);
                }
            }
        }
    }
}

void fbTextPutString(BootFbText *fx, uint32_t row, uint32_t col, const char *s, uint8_t fg,
                     uint8_t bg) {
    if (fx == NULL || s == NULL) {
        return;
    }
    uint32_t c = col;
    for (uint32_t i = 0; s[i] != '\0'; i++) {
        fbTextPutChar(fx, row, c, s[i], fg, bg);
        c++;
    }
}

void fbTextFillRow(BootFbText *fx, uint32_t row, uint32_t col0, uint32_t col1, uint8_t bg) {
    if (fx == NULL || fx->pixels == NULL || row >= fx->rows || col1 <= col0) {
        return;
    }
    if (col1 > fx->cols) {
        col1 = fx->cols;
    }
    uint32_t color = paletteColor(fx, bg);
    uint32_t scale = fx->scale;
    uint32_t y0 = row * CONSOLE_FONT_HEIGHT * scale;
    uint32_t y1 = y0 + CONSOLE_FONT_HEIGHT * scale;
    uint32_t x0 = col0 * CONSOLE_FONT_WIDTH * scale;
    uint32_t x1 = col1 * CONSOLE_FONT_WIDTH * scale;
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = x0; x < x1; x++) {
            putPixel(fx, x, y, color);
        }
    }
}
