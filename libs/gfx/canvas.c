/* See gfx.h. Canvas, clip stack, integer fills, blits, and mask fills. */
#include "gfx/gfx-internal.h"

#include <string.h>

bool gfxRectIsEmpty(GfxRect r) {
    return r.x0 >= r.x1 || r.y0 >= r.y1;
}

GfxRect gfxRectIntersect(GfxRect a, GfxRect b) {
    GfxRect r = {a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1,
                 a.y1 < b.y1 ? a.y1 : b.y1};
    if (gfxRectIsEmpty(r)) {
        GfxRect empty = {0, 0, 0, 0};
        return empty;
    }
    return r;
}

GfxRect gfxRectUnion(GfxRect a, GfxRect b) {
    if (gfxRectIsEmpty(a)) {
        return gfxRectIsEmpty(b) ? (GfxRect){0, 0, 0, 0} : b;
    }
    if (gfxRectIsEmpty(b)) {
        return a;
    }
    GfxRect r = {a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1,
                 a.y1 > b.y1 ? a.y1 : b.y1};
    return r;
}

/* a + b, saturating to the int32 range, so a huge origin or coordinate can't wrap. */
static int32_t satAdd(int32_t a, int32_t b) {
    int64_t s = (int64_t)a + (int64_t)b;
    if (s > INT32_MAX) {
        return INT32_MAX;
    }
    if (s < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)s;
}

static int32_t satSub(int32_t a, int32_t b) {
    int64_t s = (int64_t)a - (int64_t)b;
    if (s > INT32_MAX) {
        return INT32_MAX;
    }
    if (s < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)s;
}

static GfxRect toSurface(const GfxCanvas *c, GfxRect r) {
    GfxRect t = {satAdd(r.x0, c->originX), satAdd(r.y0, c->originY), satAdd(r.x1, c->originX),
                 satAdd(r.y1, c->originY)};
    return t;
}

Status gfxCanvasInit(GfxCanvas *c, GfxSurface s, const GfxAllocator *a) {
    if (s.pixels == NULL || s.width <= 0 || s.height <= 0 || s.width > GFX_SURFACE_MAX_DIM ||
        s.height > GFX_SURFACE_MAX_DIM || s.stride < s.width) {
        return STATUS_ERR_INVALID;
    }
    if ((uint64_t)s.stride * (uint64_t)s.height > (uint64_t)SIZE_MAX / sizeof(uint32_t) ||
        s.stride > INT32_MAX / 2) {
        return STATUS_ERR_INVALID;
    }
    memset(c, 0, sizeof(*c));
    c->surf = s;
    c->clip[0] = (GfxRect){0, 0, s.width, s.height};
    c->clipDepth = 1;
    c->alloc = a != NULL ? a : gfxAllocatorDefault();
    return STATUS_OK;
}

void gfxCanvasDestroy(GfxCanvas *c) {
    if (c->scratch != NULL) {
        c->alloc->free(c->alloc->ctx, c->scratch, c->scratchSize);
    }
    c->scratch = NULL;
    c->scratchSize = 0;
}

void gfxCanvasSetOrigin(GfxCanvas *c, int32_t x, int32_t y) {
    c->originX = x;
    c->originY = y;
}

void gfxCanvasTranslate(GfxCanvas *c, int32_t dx, int32_t dy) {
    c->originX = satAdd(c->originX, dx);
    c->originY = satAdd(c->originY, dy);
}

Status gfxCanvasPushClip(GfxCanvas *c, GfxRect r) {
    if (c->clipDepth >= GFX_CLIP_DEPTH) {
        return STATUS_ERR_UNSUPPORTED;
    }
    c->clip[c->clipDepth] = gfxRectIntersect(toSurface(c, r), c->clip[c->clipDepth - 1]);
    c->clipDepth++;
    return STATUS_OK;
}

Status gfxCanvasPopClip(GfxCanvas *c) {
    if (c->clipDepth <= 1) {
        return STATUS_ERR_INVALID;
    }
    c->clipDepth--;
    return STATUS_OK;
}

GfxRect gfxCanvasClipBounds(const GfxCanvas *c) {
    GfxRect r = c->clip[c->clipDepth - 1];
    GfxRect t = {satSub(r.x0, c->originX), satSub(r.y0, c->originY), satSub(r.x1, c->originX),
                 satSub(r.y1, c->originY)};
    return t;
}

static uint32_t *rowPtr(const GfxSurface *s, int32_t y) {
    return s->pixels + (size_t)y * (size_t)s->stride;
}

void gfxFillRect(GfxCanvas *c, GfxRect r, GfxColor col, GfxOp op) {
    GfxRect d = gfxRectIntersect(toSurface(c, r), c->clip[c->clipDepth - 1]);
    if (gfxRectIsEmpty(d)) {
        return;
    }
    col = gfxColorSanitize(col);
    for (int32_t y = d.y0; y < d.y1; y++) {
        gfxBlendSpan(rowPtr(&c->surf, y) + d.x0, (size_t)(d.x1 - d.x0), col, op);
    }
}

void gfxBlit(GfxCanvas *c, int32_t dx, int32_t dy, const GfxSurface *src, GfxRect srcRect, GfxOp op,
             uint8_t alpha) {
    if (src == NULL || src->pixels == NULL || src->width <= 0 || src->height <= 0 ||
        (alpha == 0 && op == GFX_OP_SRC_OVER)) {
        return;
    }
    /* Clamp the source rect to the source surface, in 64-bit so nothing wraps. */
    GfxRect sr = gfxRectIntersect(srcRect, (GfxRect){0, 0, src->width, src->height});
    if (gfxRectIsEmpty(sr)) {
        return;
    }
    /* Destination rect of the (clamped) source rect. The clamping moved sr's origin, so shift the
     * destination by the same amount. */
    int64_t ddx = (int64_t)dx + ((int64_t)sr.x0 - srcRect.x0) + c->originX;
    int64_t ddy = (int64_t)dy + ((int64_t)sr.y0 - srcRect.y0) + c->originY;
    int64_t w = sr.x1 - sr.x0, h = sr.y1 - sr.y0;
    GfxRect clip = c->clip[c->clipDepth - 1];
    int64_t x0 = ddx > clip.x0 ? ddx : clip.x0;
    int64_t y0 = ddy > clip.y0 ? ddy : clip.y0;
    int64_t x1 = ddx + w < clip.x1 ? ddx + w : clip.x1;
    int64_t y1 = ddy + h < clip.y1 ? ddy + h : clip.y1;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    int32_t sx0 = sr.x0 + (int32_t)(x0 - ddx);
    int32_t sy0 = sr.y0 + (int32_t)(y0 - ddy);
    int32_t cols = (int32_t)(x1 - x0), rows = (int32_t)(y1 - y0);

    /* An overlapping move within one surface: pick the direction that reads before it writes. */
    bool sameBuffer = src->pixels == c->surf.pixels;
    bool backwardsY = sameBuffer && (int32_t)y0 > sy0;
    bool backwardsX = sameBuffer && (int32_t)y0 == sy0 && (int32_t)x0 > sx0;
    for (int32_t i = 0; i < rows; i++) {
        int32_t ry = backwardsY ? rows - 1 - i : i;
        const uint32_t *sp = rowPtr(src, sy0 + ry) + sx0;
        uint32_t *dp = rowPtr(&c->surf, (int32_t)y0 + ry) + (int32_t)x0;
        if (op == GFX_OP_SRC && alpha == 255) {
            memmove(dp, sp, (size_t)cols * sizeof(uint32_t));
            continue;
        }
        for (int32_t j = 0; j < cols; j++) {
            int32_t rx = backwardsX ? cols - 1 - j : j;
            dp[rx] = gfxBlendPixel(dp[rx], gfxColorSanitize(sp[rx]), alpha, op);
        }
    }
}

void gfxFillMask(GfxCanvas *c, int32_t dx, int32_t dy, const GfxMask *m, GfxColor col) {
    if (m == NULL || m->data == NULL || m->width <= 0 || m->height <= 0 || m->stride < m->width) {
        return;
    }
    int64_t ddx = (int64_t)dx + c->originX, ddy = (int64_t)dy + c->originY;
    GfxRect clip = c->clip[c->clipDepth - 1];
    int64_t x0 = ddx > clip.x0 ? ddx : clip.x0;
    int64_t y0 = ddy > clip.y0 ? ddy : clip.y0;
    int64_t x1 = ddx + m->width < clip.x1 ? ddx + m->width : clip.x1;
    int64_t y1 = ddy + m->height < clip.y1 ? ddy + m->height : clip.y1;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    col = gfxColorSanitize(col);
    for (int64_t y = y0; y < y1; y++) {
        const uint8_t *cov = m->data + (size_t)(y - ddy) * (size_t)m->stride + (size_t)(x0 - ddx);
        gfxBlendSpanCov(rowPtr(&c->surf, (int32_t)y) + (int32_t)x0, cov, (size_t)(x1 - x0), col,
                        GFX_OP_SRC_OVER);
    }
}
