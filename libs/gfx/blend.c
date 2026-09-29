/* See gfx-internal.h. Premultiplied ARGB32 pixel math; every result keeps each channel <= alpha. */
#include "gfx/gfx-internal.h"

#define CH(c, shift) (((c) >> (shift)) & 0xFFu)

GfxColor gfxColorSanitize(GfxColor c) {
    uint32_t a = c >> 24;
    uint32_t r = CH(c, 16), g = CH(c, 8), b = CH(c, 0);
    r = r > a ? a : r;
    g = g > a ? a : g;
    b = b > a ? a : b;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

GfxColor gfxColorPremul(uint32_t straightArgb) {
    uint32_t a = straightArgb >> 24;
    uint32_t r = gfxMulDiv255(CH(straightArgb, 16), a);
    uint32_t g = gfxMulDiv255(CH(straightArgb, 8), a);
    uint32_t b = gfxMulDiv255(CH(straightArgb, 0), a);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static inline uint32_t scale(uint32_t c, uint32_t cov) {
    if (cov == 255u) {
        return c;
    }
    return (gfxMulDiv255(c >> 24, cov) << 24) | (gfxMulDiv255(CH(c, 16), cov) << 16) |
           (gfxMulDiv255(CH(c, 8), cov) << 8) | gfxMulDiv255(CH(c, 0), cov);
}

uint32_t gfxBlendPixel(uint32_t dst, uint32_t src, uint32_t cov, GfxOp op) {
    if (op == GFX_OP_SRC) {
        if (cov == 255u) {
            return src;
        }
        uint32_t s = scale(src, cov);
        uint32_t ic = 255u - cov;
        return (s & 0xFF000000u) + (gfxMulDiv255(dst >> 24, ic) << 24) +
               ((((s >> 16) & 0xFFu) + gfxMulDiv255(CH(dst, 16), ic)) << 16) +
               ((((s >> 8) & 0xFFu) + gfxMulDiv255(CH(dst, 8), ic)) << 8) +
               ((s & 0xFFu) + gfxMulDiv255(CH(dst, 0), ic));
    }
    uint32_t s = scale(src, cov);
    uint32_t ia = 255u - (s >> 24);
    if (ia == 255u) {
        return dst; /* fully transparent source */
    }
    if (ia == 0u) {
        return s; /* fully opaque source */
    }
    return (((s >> 24) + gfxMulDiv255(dst >> 24, ia)) << 24) |
           ((CH(s, 16) + gfxMulDiv255(CH(dst, 16), ia)) << 16) |
           ((CH(s, 8) + gfxMulDiv255(CH(dst, 8), ia)) << 8) |
           (CH(s, 0) + gfxMulDiv255(CH(dst, 0), ia));
}

void gfxBlendSpan(uint32_t *dst, size_t n, GfxColor col, GfxOp op) {
    if (op == GFX_OP_SRC_OVER) {
        if ((col >> 24) == 0u) {
            return;
        }
        if ((col >> 24) != 255u) {
            for (size_t i = 0; i < n; i++) {
                dst[i] = gfxBlendPixel(dst[i], col, 255u, op);
            }
            return;
        }
    }
    for (size_t i = 0; i < n; i++) {
        dst[i] = col;
    }
}

void gfxBlendSpanCov(uint32_t *dst, const uint8_t *cov, size_t n, GfxColor col, GfxOp op) {
    for (size_t i = 0; i < n; i++) {
        if (cov[i] != 0u) {
            dst[i] = gfxBlendPixel(dst[i], col, cov[i], op);
        }
    }
}
