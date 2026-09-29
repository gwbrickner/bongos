/* Private to libs/gfx (and its host tests): pixel math and span blending. Integer-only. */
#ifndef LIBS_GFX_INTERNAL_H
#define LIBS_GFX_INTERNAL_H

#include "gfx/gfx.h"

/* round(a*b/255), half up, exact for a,b <= 255. The one rounding rule used everywhere. */
static inline uint32_t gfxMulDiv255(uint32_t a, uint32_t b) {
    uint32_t t = a * b + 128u;
    return (t + (t >> 8)) >> 8;
}

/* Clamps every channel of a premultiplied color to its alpha. */
GfxColor gfxColorSanitize(GfxColor c);

/* One pixel: `src` (already sanitized) scaled by coverage `cov` (0..255), then combined with
 * `dst` per `op`. The result keeps the premultiplied invariant (each channel <= alpha <= 255). */
uint32_t gfxBlendPixel(uint32_t dst, uint32_t src, uint32_t cov, GfxOp op);

/* n pixels of one (sanitized) color at full coverage. */
void gfxBlendSpan(uint32_t *dst, size_t n, GfxColor col, GfxOp op);

/* n pixels of one (sanitized) color, per-pixel coverage `cov[i]`. */
void gfxBlendSpanCov(uint32_t *dst, const uint8_t *cov, size_t n, GfxColor col, GfxOp op);

#endif
