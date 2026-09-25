/* See fbcon.h. Built on boot/common/fbtext.c's glyph-blit primitive (the same one the UEFI
 * loader's boot menu uses, boot/uefi/menu.c) -- this file adds the scrolling/cursor/shadow-cell
 * layer fbtext.c deliberately doesn't have. */
#include "fbcon.h"

#include "fbtext.h"

/* The caps fbTextInit() itself already clamps cols/rows to (D-069); sized here too so the shadow
 * buffer is a fixed, statically-allocated array (no allocator exists yet, M2.2) rather than a
 * runtime-sized one. */
#define FBCON_MAX_COLS 480u
#define FBCON_MAX_ROWS 270u

static BootFbText fx;
static bool active = false;
static uint8_t curFg = 7, curBg = 0;
static uint32_t cursorRow = 0, cursorCol = 0;
/* One cell per character cell: the low 8 bits are the character, bits 8-11 are fg, bits 12-15 are
 * bg. Never read back from the framebuffer itself (D-069) -- this is the only source of truth
 * for "what's on screen", used to redraw every visible cell after a scroll. */
static uint16_t cells[FBCON_MAX_ROWS][FBCON_MAX_COLS];

static void setCell(uint32_t row, uint32_t col, char ch, uint8_t fg, uint8_t bg) {
    cells[row][col] =
        (uint16_t)((uint8_t)ch | (((uint32_t)fg & 0xFu) << 8) | (((uint32_t)bg & 0xFu) << 12));
}

static void drawCell(uint32_t row, uint32_t col) {
    uint16_t cell = cells[row][col];
    char ch = (char)(cell & 0xFFu);
    uint8_t fg = (uint8_t)((cell >> 8) & 0xFu);
    uint8_t bg = (uint8_t)((cell >> 12) & 0xFu);
    fbTextPutChar(&fx, row, col, ch, fg, bg);
}

/* Redraws only the cells whose shadow value actually changed, instead of every cell on screen:
 * on real hardware, with the framebuffer mapped UC-/UC until M2.3 remaps it WC (D-068), a full
 * redrawAll() on every newline makes scrolling very slow. Most rows shift unchanged from the row
 * below, so this is typically just the bottom row's worth of draws, not the whole screen's. */
static void scroll(void) {
    for (uint32_t r = 1; r < fx.rows; r++) {
        for (uint32_t c = 0; c < fx.cols; c++) {
            uint16_t moved = cells[r][c];
            if (cells[r - 1][c] != moved) {
                cells[r - 1][c] = moved;
                drawCell(r - 1, c);
            }
        }
    }
    for (uint32_t c = 0; c < fx.cols; c++) {
        uint16_t before = cells[fx.rows - 1][c];
        setCell(fx.rows - 1, c, ' ', curFg, curBg);
        if (cells[fx.rows - 1][c] != before) {
            drawCell(fx.rows - 1, c);
        }
    }
}

static void newline(void) {
    cursorCol = 0;
    if (cursorRow + 1 >= fx.rows) {
        scroll();
    } else {
        cursorRow++;
    }
}

static void putGlyph(char ch) {
    setCell(cursorRow, cursorCol, ch, curFg, curBg);
    drawCell(cursorRow, cursorCol);
    cursorCol++;
    if (cursorCol >= fx.cols) {
        newline();
    }
}

Status fbconInit(const BootFramebuffer *fb, uint64_t hhdmBase) {
    active = false;
    if (fb == NULL || fb->phys == 0) {
        return STATUS_ERR_UNSUPPORTED;
    }
    uint8_t *pixels = (uint8_t *)(uintptr_t)(hhdmBase + fb->phys);
    BootStatus bst =
        fbTextInit(&fx, pixels, fb->width, fb->height, fb->pitch, fb->redShift, fb->redSize,
                   fb->greenShift, fb->greenSize, fb->blueShift, fb->blueSize);
    if (bst != BOOT_OK) {
        return STATUS_ERR_UNSUPPORTED;
    }
    /* Defensive: fbTextInit() already clamps to these same caps, so this can't actually trip --
     * but the shadow buffer below would silently corrupt adjacent memory if it ever did. */
    if (fx.cols == 0 || fx.rows == 0 || fx.cols > FBCON_MAX_COLS || fx.rows > FBCON_MAX_ROWS) {
        return STATUS_ERR_UNSUPPORTED;
    }

    curFg = 7;
    curBg = 0;
    cursorRow = 0;
    cursorCol = 0;
    for (uint32_t r = 0; r < fx.rows; r++) {
        for (uint32_t c = 0; c < fx.cols; c++) {
            setCell(r, c, ' ', curFg, curBg);
        }
    }
    fbTextClear(&fx, curBg);
    active = true;
    return STATUS_OK;
}

void fbconSetColor(uint8_t fg, uint8_t bg) {
    curFg = fg;
    curBg = bg;
}

void fbconClear(void) {
    if (!active) {
        return;
    }
    for (uint32_t r = 0; r < fx.rows; r++) {
        for (uint32_t c = 0; c < fx.cols; c++) {
            setCell(r, c, ' ', curFg, curBg);
        }
    }
    cursorRow = 0;
    cursorCol = 0;
    fbTextClear(&fx, curBg);
}

bool fbconActive(void) {
    return active;
}

uint32_t fbconCols(void) {
    return active ? fx.cols : 0;
}

uint32_t fbconRows(void) {
    return active ? fx.rows : 0;
}

uint32_t fbconScale(void) {
    return active ? fx.scale : 0;
}

void fbconWrite(const char *s, size_t n) {
    if (!active) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\n') {
            newline();
        } else if (c == '\r') {
            cursorCol = 0;
        } else if (c == '\t') {
            uint32_t next = (cursorCol / 8u + 1u) * 8u;
            if (next > fx.cols) {
                next = fx.cols;
            }
            /* Compute the space count up front rather than looping on `cursorCol < next`: once
             * putGlyph() wraps (cursorCol reaches fx.cols and resets to 0), that condition can
             * never distinguish "just wrapped" from "never started" when cursorCol was already 0
             * (e.g. fx.cols == 1, wrapping every call) -- an infinite loop. `next <= fx.cols`
             * guarantees at most the final iteration here can wrap, so a fixed count is safe. */
            uint32_t count = next - cursorCol;
            for (uint32_t j = 0; j < count; j++) {
                putGlyph(' ');
            }
        } else {
            putGlyph(c);
        }
    }
}
