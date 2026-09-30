/* libs/gfx blur and drop shadows (M12.2, D-145). A8 masks only: shadows are a blurred coverage
 * mask tinted at draw time, so ARGB blur is not needed. Integer-only. */
#ifndef LIBS_GFX_BLUR_H
#define LIBS_GFX_BLUR_H

#include "gfx/gfx.h"

typedef enum {
    GFX_BLUR_EDGE_ZERO,  /* pixels outside the mask read as 0 (shadows: needs a margin) */
    GFX_BLUR_EDGE_CLAMP, /* pixels outside read as the nearest edge pixel (constant stays exact) */
} GfxBlurEdge;

#define GFX_BLUR_MAX_RADIUS 255

/* In-place separable box blur: `passes` (1..3) horizontal passes, then as many vertical ones, each
 * a window of 2*radius+1 with output (sum + radius) / (2*radius+1). O(pixels) per pass whatever
 * the radius; a radius larger than the mask is fine. radius 0 is the identity. Allocates one
 * scratch line (max(width,height) bytes) with `a` (NULL = default). Failure modes: INVALID
 * (radius > 255, passes not in 1..3, a NULL or malformed mask), NO_MEMORY (the mask is left
 * unchanged). Never sleeps. */
Status gfxBlurBox(GfxMask *m, uint32_t radius, uint32_t passes, GfxBlurEdge edge,
                  const GfxAllocator *a);

/* Draws a soft shadow of the rounded rect (x, y, w, h, cornerR) moved by (offX, offY): its
 * coverage is blurred with 3 box passes of `blurRadius` (<= 255; the visible spread is about
 * 3*blurRadius) and composited SRC_OVER in `col` (premultiplied; sanitized). x/y are in canvas
 * coordinates (origin and clip apply). w <= 0 or h <= 0 draws nothing. Allocates a temporary mask
 * of (w + 2*margin) x (h + 2*margin) bytes with `a` (NULL = the canvas allocator). Failure modes:
 * INVALID (non-finite geometry, blurRadius > 255), UNSUPPORTED (a mask over 16384 in either
 * dimension), NO_MEMORY. Nothing is drawn on failure. */
Status gfxDrawShadow(GfxCanvas *c, float x, float y, float w, float h, float cornerR,
                     uint32_t blurRadius, int32_t offX, int32_t offY, GfxColor col,
                     const GfxAllocator *a);

#endif
