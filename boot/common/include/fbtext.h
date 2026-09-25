/* A small framebuffer text renderer shared by the UEFI loader's boot menu (boot/uefi/menu.c) and
 * the kernel's fbcon driver (kernel/drivers/fbcon/fbcon.c): both draw the same 8x16 console font
 * (console-font.h) at the same integer scale and 16-color ANSI palette into a 32-bpp pixel
 * buffer, so this is the one place that logic lives. It never reads the buffer back (D-069: the
 * framebuffer can be UC/UC-/WC, where reads are pathologically slow) and keeps no state of its
 * own beyond `BootFbText` -- the caller (fbcon's shadow cells, or the loader's one-shot redraw)
 * owns whatever "what's currently on screen" bookkeeping it needs. Host-tested against a heap
 * buffer (tests/host/boot_fbtext_test.c) as well as used against a real MMIO/HHDM mapping. */
#ifndef BOOT_COMMON_FBTEXT_H
#define BOOT_COMMON_FBTEXT_H

#include <stdint.h>

#include "boot-status.h"

#define FB_TEXT_PALETTE_SIZE 16 /* ANSI 16-color order, D-069 */

typedef struct {
    uint8_t *pixels;                    /* caller-owned; never read, only written */
    uint32_t width, height, pitchBytes; /* pitchBytes: bytes per scanline; bpp is always 32 */
    uint32_t scale;      /* integer glyph scale, clamp(min(width/1280, height/720), 1, 4) */
    uint32_t cols, rows; /* text grid size at this scale: width/(8*scale), height/(16*scale) */
    uint32_t palette[FB_TEXT_PALETTE_SIZE]; /* precomputed 32-bit pixel values */
} BootFbText;

/* Validates the geometry (bpp is implicitly 32; pitchBytes >= width*4 and a multiple of 4; width
 * >= 8, height >= 16; each of redSize/greenSize/blueSize is 1-8 and shift+size <= 32) and fills
 * `fx` from it, precomputing the palette and the cols/rows/scale grid. Returns
 * BOOT_ERR_FB_UNSUPPORTED (fx is left zeroed) if the geometry fails validation. No locks,
 * boot-time or host-test only; pure except for reading `redShift` etc. by value. */
BootStatus fbTextInit(BootFbText *fx, uint8_t *pixels, uint32_t width, uint32_t height,
                      uint32_t pitchBytes, uint8_t redShift, uint8_t redSize, uint8_t greenShift,
                      uint8_t greenSize, uint8_t blueShift, uint8_t blueSize);

/* Fills the whole buffer with palette[bg]. `bg` >= FB_TEXT_PALETTE_SIZE is treated as 0. */
void fbTextClear(BootFbText *fx, uint8_t bg);

/* Draws one character cell at grid (row, col) (out-of-range is a silent no-op, not a fault: the
 * menu/fbcon callers compute row/col from user-controllable state like a cmdline-set resolution,
 * and a stray off-grid draw should never be worse than "nothing happened"). `ch` outside
 * 0x00-0x7F draws the replacement glyph (index 0). */
void fbTextPutChar(BootFbText *fx, uint32_t row, uint32_t col, char ch, uint8_t fg, uint8_t bg);

/* Draws a NUL-terminated string starting at (row, col), left to right, clipspringing at the grid
 * edge (a character past the last column is simply not drawn -- the caller decides whether that
 * should have been prevented). */
void fbTextPutString(BootFbText *fx, uint32_t row, uint32_t col, const char *s, uint8_t fg,
                     uint8_t bg);

/* Fills character columns [col0, col1) of one row with palette[bg] (no glyph) -- used for a
 * selection highlight bar that extends past any text on the row. col1 <= col0 is a no-op. */
void fbTextFillRow(BootFbText *fx, uint32_t row, uint32_t col0, uint32_t col1, uint8_t bg);

#endif
