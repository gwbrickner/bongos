/* See gfx-blur.h. */
#include "gfx/gfx-blur.h"

#include "gfx/gfx-internal.h"

#include <string.h>

/* Pixel `i` of a line of n with the edge rule applied. */
static inline int32_t sample(const uint8_t *line, int32_t n, int32_t i, GfxBlurEdge edge) {
    if (i < 0) {
        return edge == GFX_BLUR_EDGE_CLAMP ? line[0] : 0;
    }
    if (i >= n) {
        return edge == GFX_BLUR_EDGE_CLAMP ? line[n - 1] : 0;
    }
    return line[i];
}

/* Blurs `line` (n bytes) into out[0], out[step], ... A sliding window: sum starts over
 * [-r, r] and moves by adding the entering pixel and removing the leaving one. `line` is a copy
 * of the source, so writing `out` in place never reads a value that was already overwritten. */
static void blurLine(const uint8_t *line, int32_t n, uint8_t *out, size_t step, int32_t r,
                     GfxBlurEdge edge) {
    int32_t win = 2 * r + 1;
    int32_t sum = 0;
    for (int32_t i = -r; i <= r; i++) {
        sum += sample(line, n, i, edge);
    }
    for (int32_t x = 0; x < n; x++) {
        out[(size_t)x * step] = (uint8_t)((sum + r) / win);
        sum += sample(line, n, x + r + 1, edge) - sample(line, n, x - r, edge);
    }
}

Status gfxBlurBox(GfxMask *m, uint32_t radius, uint32_t passes, GfxBlurEdge edge,
                  const GfxAllocator *a) {
    if (m == NULL || m->data == NULL || m->width <= 0 || m->height <= 0 || m->stride < m->width ||
        radius > GFX_BLUR_MAX_RADIUS || passes < 1 || passes > 3) {
        return STATUS_ERR_INVALID;
    }
    if (radius == 0) {
        return STATUS_OK;
    }
    if (a == NULL) {
        a = gfxAllocatorDefault();
    }
    size_t lineCap = (size_t)(m->width > m->height ? m->width : m->height);
    uint8_t *line = a->alloc(a->ctx, lineCap);
    if (line == NULL) {
        return STATUS_ERR_NO_MEMORY;
    }
    int32_t r = (int32_t)radius;
    for (uint32_t p = 0; p < passes; p++) {
        for (int32_t y = 0; y < m->height; y++) {
            uint8_t *row = m->data + (size_t)y * (size_t)m->stride;
            memcpy(line, row, (size_t)m->width);
            blurLine(line, m->width, row, 1, r, edge);
        }
    }
    for (uint32_t p = 0; p < passes; p++) {
        for (int32_t x = 0; x < m->width; x++) {
            uint8_t *col = m->data + x;
            for (int32_t y = 0; y < m->height; y++) {
                line[y] = col[(size_t)y * (size_t)m->stride];
            }
            blurLine(line, m->height, col, (size_t)m->stride, r, edge);
        }
    }
    a->free(a->ctx, line, lineCap);
    return STATUS_OK;
}

static int32_t floorToInt(float v) {
    if (v > GFX_COORD_MAX) {
        v = GFX_COORD_MAX;
    } else if (v < -GFX_COORD_MAX) {
        v = -GFX_COORD_MAX;
    }
    int32_t i = (int32_t)v;
    return (float)i > v ? i - 1 : i;
}

#define SHADOW_MAX_DIM 16384
#define SHADOW_PASSES  3

Status gfxDrawShadow(GfxCanvas *c, float x, float y, float w, float h, float cornerR,
                     uint32_t blurRadius, int32_t offX, int32_t offY, GfxColor col,
                     const GfxAllocator *a) {
    if (!__builtin_isfinite(x) || !__builtin_isfinite(y) || !__builtin_isfinite(w) ||
        !__builtin_isfinite(h) || !__builtin_isfinite(cornerR) ||
        blurRadius > GFX_BLUR_MAX_RADIUS) {
        return STATUS_ERR_INVALID;
    }
    if (!(w > 0.0f) || !(h > 0.0f)) {
        return STATUS_OK;
    }
    if (a == NULL) {
        a = c->alloc;
    }
    int64_t margin = (int64_t)blurRadius * SHADOW_PASSES + 1;
    /* Mask origin (canvas coordinates) and size: the shifted shape's pixel bounds plus margin. */
    int64_t sx = (int64_t)floorToInt(x) + offX, sy = (int64_t)floorToInt(y) + offY;
    int64_t mw = (int64_t)floorToInt(w) + 2 + 2 * margin;
    int64_t mh = (int64_t)floorToInt(h) + 2 + 2 * margin;
    if (mw > SHADOW_MAX_DIM || mh > SHADOW_MAX_DIM) {
        return STATUS_ERR_UNSUPPORTED;
    }
    int64_t mx0 = sx - margin, my0 = sy - margin;
    if (mx0 < INT32_MIN || my0 < INT32_MIN || mx0 + mw > INT32_MAX || my0 + mh > INT32_MAX) {
        return STATUS_OK; /* wholly outside any possible surface */
    }
    size_t bytes = (size_t)mw * (size_t)mh;
    uint8_t *data = a->alloc(a->ctx, bytes);
    if (data == NULL) {
        return STATUS_ERR_NO_MEMORY;
    }
    memset(data, 0, bytes);
    GfxMask mask = {data, (int32_t)mw, (int32_t)mh, (int32_t)mw};

    GfxPath p;
    gfxPathInit(&p, a);
    /* the shape, positioned inside the mask at the same sub-pixel offset it has on the canvas */
    float fx = (x - (float)floorToInt(x)) + (float)margin;
    float fy = (y - (float)floorToInt(y)) + (float)margin;
    Status st = gfxPathAddRoundedRect(&p, fx, fy, w, h, cornerR);
    if (st == STATUS_OK) {
        st = gfxFillPathMask(&mask, &p, GFX_FILL_NONZERO, a);
    }
    gfxPathFree(&p);
    if (st == STATUS_OK) {
        st = gfxBlurBox(&mask, blurRadius, SHADOW_PASSES, GFX_BLUR_EDGE_ZERO, a);
    }
    if (st == STATUS_OK) {
        gfxFillMask(c, (int32_t)mx0, (int32_t)my0, &mask, col);
    }
    a->free(a->ctx, data, bytes);
    return st;
}
