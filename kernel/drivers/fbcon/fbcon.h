/* The framebuffer console (ARCHITECTURE §19, ROADMAP M1.4): a scrolling text console built on
 * boot/common/fbtext.c's glyph-blit primitive, mirrored into by klog (kernel/core/klog.c) so
 * every serial log line also appears on screen. Single-threaded, boot-time-and-beyond driver;
 * contract below. */
#ifndef KERNEL_DRIVERS_FBCON_FBCON_H
#define KERNEL_DRIVERS_FBCON_FBCON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bootinfo.h"
#include "uapi/status.h"

/* Initializes the console from `fb` (BootInfo.fb) mapped through the HHDM at
 * `hhdmBase + fb->phys` (D-068's framebuffer mapping). Returns STATUS_ERR_UNSUPPORTED (fbcon
 * stays inactive) if `fb->phys == 0` or the geometry fails fbTextInit's validation -- the caller
 * logs a warning and continues serial-only, never panics over a missing/bad framebuffer. On
 * success, clears the screen and resets the cursor and color to their defaults (fg 7, bg 0). No
 * locks, single CPU, not IRQ-safe: called once from kernelMain before any concurrency exists.
 * Once klog's own spinlock exists (a later milestone), fbcon is only ever called with it held. */
Status fbconInit(const BootFramebuffer *fb, uint64_t hhdmBase);

/* Writes `n` bytes of `s` to the console at the current cursor position and color: '\n' moves to
 * the next row, scrolling the whole screen up one row if already at the bottom; '\r' returns to
 * column 0; '\t' advances to the next multiple-of-8 column; every other byte is drawn as a glyph
 * and advances the cursor, wrapping to the next row (scrolling if needed) past the last column.
 * A silent no-op if fbconInit() hasn't succeeded. Never reads the framebuffer back -- scrolling
 * redraws every cell from an in-memory shadow buffer, not from the (possibly UC/UC-/pathologically
 * slow to read) framebuffer itself. No locks (see the contract note above). */
void fbconWrite(const char *s, size_t n);

/* Sets the ANSI palette index (0-15) used by subsequent fbconWrite() calls; out-of-range values
 * are clamped to 0 by the underlying fbtext glyph draw. */
void fbconSetColor(uint8_t fg, uint8_t bg);

/* Clears the screen to the current background color and resets the cursor to (0, 0). A no-op if
 * fbconInit() hasn't succeeded. */
void fbconClear(void);

/* True once fbconInit() has succeeded; false before that or if it failed. */
bool fbconActive(void);

/* The text grid geometry fbconInit() computed (0 if inactive) -- exposed for ktests to check
 * against the D-069 scale/cols/rows formula independently of fbcon's internal state. */
uint32_t fbconCols(void);
uint32_t fbconRows(void);
uint32_t fbconScale(void);

#endif
