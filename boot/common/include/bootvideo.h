/* Shared video-mode selection rule (ARCHITECTURE §5.5, D-068/D-109), used by both the UEFI
 * loader's GOP mode pick (boot/uefi/gop.c) and the BIOS loader's VBE mode pick (M2.5) -- only how
 * each firmware *enumerates* modes differs; the accept/pick/framebuffer-fill logic here is one
 * pure, host-tested implementation shared by both. */
#ifndef BOOT_COMMON_BOOTVIDEO_H
#define BOOT_COMMON_BOOTVIDEO_H

#include <stdbool.h>
#include <stdint.h>

#include "bootinfo.h"

#define BOOT_VIDEO_MAX_WIDTH  3840u
#define BOOT_VIDEO_MAX_HEIGHT 2160u

/* One candidate mode, already reduced to a firmware-agnostic shape: a raw bitmask per color
 * channel (0 = channel absent), regardless of whether the firmware reported it as a named format
 * (GOP's RGBX/BGRX) or a bitmask directly (VBE, or GOP's own PixelBitMask). `id` is whatever the
 * firmware uses to select this mode again (a GOP mode number, a VBE mode number). */
typedef struct {
    uint32_t id;
    uint32_t width, height;
    uint32_t pitch; /* bytes per scanline */
    uint32_t redMask, greenMask, blueMask, reservedMask;
    uint64_t fbPhys;
} BootVideoMode;

/* True iff `m` is an acceptable 32-bit-pixel mode (D-068/D-109): `pitch >= width*4` and a
 * multiple of 4; red/green/blue masks each non-zero, contiguous, and at most 8 bits wide; and the
 * highest set bit across red|green|blue|reserved falls in 24..31 (a real 32-bit pixel, not e.g. a
 * 16-bit 565 mode zero-extended into a 32-bit field). No locks, boot-time or host-test only;
 * pure. */
bool bootVideoAccept(const BootVideoMode *m);

/* A single streaming pass over a firmware's mode list (D-109): tracks the best "auto" candidate
 * (largest area within BOOT_VIDEO_MAX_WIDTH x BOOT_VIDEO_MAX_HEIGHT, ties broken by width then by
 * the lower `id` -- compared explicitly, so callers whose mode list isn't enumerated in `id`
 * order still get a deterministic answer) and the best exact WIDTHxHEIGHT match (lowest `id`)
 * side by side, so the caller doesn't need two passes or to buffer the whole mode list. */
typedef struct {
    uint32_t resWidth, resHeight; /* both 0: no explicit resolution requested */
    bool haveAuto;
    BootVideoMode bestAuto;
    bool haveExact;
    BootVideoMode bestExact;
} BootVideoPicker;

/* Starts a picking pass. `resWidth`/`resHeight` both 0 means "auto" only; otherwise an exact
 * match is tracked too. No locks, boot-time only; pure. */
void bootVideoPickerInit(BootVideoPicker *p, uint32_t resWidth, uint32_t resHeight);

/* Offers one candidate mode (already filtered through bootVideoAccept by the caller -- this
 * function does not call it again). No locks, boot-time only; pure. */
void bootVideoPickerOffer(BootVideoPicker *p, const BootVideoMode *m);

/* Resolves the pass: if an explicit resolution was requested and found, fills `*out` with it and
 * sets `*exactFellBack = false`; if requested but not found, falls back to the auto pick (fills
 * `*out` from it, `*exactFellBack = true`) if one exists; if no explicit resolution was
 * requested, fills `*out` from the auto pick (`*exactFellBack = false`). Returns false (leaving
 * `*out`/`*exactFellBack` unchanged) only if nothing acceptable was ever offered. No locks,
 * boot-time only; pure. */
bool bootVideoPickerResult(const BootVideoPicker *p, BootVideoMode *out, bool *exactFellBack);

/* Converts an accepted mode into BootInfo's BootFramebuffer shape (ARCHITECTURE §5.3): bpp is
 * always 32; each channel's shift/size come from its mask (ctz/popcount). No locks, boot-time
 * only; pure. Caller must have already confirmed bootVideoAccept(m). */
void bootVideoToFramebuffer(const BootVideoMode *m, BootFramebuffer *fb);

#endif
