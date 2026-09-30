/* Glyph rendering: outline -> A8 coverage mask through libs/gfx's exact-area fill (M12.3, D-153).
 * One of the two font files that use float (D-142/D-153): no libm, `FP_CONTRACT` off, every
 * float-to-int cast clamped first. No hinting and no gamma: linear coverage, unhinted outlines,
 * quarter-pixel horizontal positioning baked into the transform (integer baselines). */
#pragma STDC FP_CONTRACT OFF

#include "gfx/font-internal.h"

#include <string.h>

/* Contract: no allocation. The scratch keeps `a` (NULL: default), which must outlive it. */
void gfxGlyphScratchInit(GfxGlyphScratch *s, const GfxAllocator *a) {
    gfxGlyphOutlineInit(&s->outline, a);
    gfxPathInit(&s->path, a);
}

void gfxGlyphScratchFree(GfxGlyphScratch *s) {
    if (s == NULL) {
        return;
    }
    gfxGlyphOutlineFree(&s->outline);
    gfxPathFree(&s->path);
}

/* Contract: NULL-safe and idempotent. */
void gfxGlyphImageFree(GfxGlyphImage *g) {
    if (g == NULL) {
        return;
    }
    if (g->mask.data != NULL && g->alloc != NULL) {
        g->alloc->free(g->alloc->ctx, g->mask.data, g->allocSize);
    }
    memset(g, 0, sizeof *g);
}

/* floor/ceil without libm; the input is clamped to +-2^24 first so the cast cannot overflow. */
static int32_t floorI(float v) {
    if (!(v > -16777216.0f)) {
        v = -16777216.0f; /* also catches NaN */
    }
    if (v > 16777216.0f) {
        v = 16777216.0f;
    }
    int32_t i = (int32_t)v;
    if ((float)i > v) {
        i--;
    }
    return i;
}

static int32_t ceilI(float v) {
    if (!(v > -16777216.0f)) {
        v = -16777216.0f;
    }
    if (v > 16777216.0f) {
        v = 16777216.0f;
    }
    int32_t i = (int32_t)v;
    if ((float)i < v) {
        i++;
    }
    return i;
}

/* Contract: may allocate (scratch and the mask); never sleeps; see gfx-font.h. */
Status gfxFontRenderGlyph(const GfxFont *f, uint16_t glyph, uint32_t sizeQ6, uint32_t bin,
                          GfxGlyphScratch *s, const GfxAllocator *a, GfxGlyphImage *out) {
    if (f == NULL || f->data == NULL || s == NULL || out == NULL) {
        return STATUS_ERR_INVALID;
    }
    memset(out, 0, sizeof *out);
    if (sizeQ6 < GFX_FONT_MIN_SIZE_Q6 || sizeQ6 > GFX_FONT_MAX_SIZE_Q6 ||
        bin >= GFX_FONT_SUBPIXEL_BINS) {
        return STATUS_ERR_INVALID;
    }
    if (a == NULL) {
        a = gfxAllocatorDefault();
    }
    const float scale = (float)sizeQ6 / ((float)f->unitsPerEm * 64.0f);
    /* font y-up becomes pixel y-down, the baseline at y = 0; the bin shifts right by bin/4 px */
    const GfxFontXform xf = {scale, 0.0f, 0.0f, -scale, (float)bin * 0.25f, 0.0f};
    GfxGlyphOutline *o = &s->outline;
    Status st = gfxFontGlyphOutline(f, glyph, &xf, o);
    if (st != STATUS_OK) {
        return st;
    }
    if (o->nPoints == 0) {
        return STATUS_OK; /* an empty glyph */
    }
    /* the control box contains every quadratic curve of the outline */
    float minX = o->xy[0], maxX = o->xy[0], minY = o->xy[1], maxY = o->xy[1];
    for (uint32_t i = 1; i < o->nPoints; i++) {
        const float x = o->xy[2u * i], y = o->xy[2u * i + 1u];
        minX = x < minX ? x : minX;
        maxX = x > maxX ? x : maxX;
        minY = y < minY ? y : minY;
        maxY = y > maxY ? y : maxY;
    }
    const int32_t ix0 = floorI(minX), iy0 = floorI(minY);
    int32_t ix1 = ceilI(maxX), iy1 = ceilI(maxY);
    if (ix1 == ix0) {
        ix1++;
    }
    if (iy1 == iy0) {
        iy1++;
    }
    const int64_t w = (int64_t)ix1 - ix0, h = (int64_t)iy1 - iy0;
    if (w > GFX_FONT_MAX_GLYPH_DIM || h > GFX_FONT_MAX_GLYPH_DIM) {
        return STATUS_ERR_UNSUPPORTED;
    }
    gfxPathReset(&s->path);
    st = gfxGlyphOutlineToPath(o, -(float)ix0, -(float)iy0, &s->path);
    if (st != STATUS_OK) {
        return st;
    }
    const size_t bytes = (size_t)(w * h);
    uint8_t *px = a->alloc(a->ctx, bytes);
    if (px == NULL) {
        return STATUS_ERR_NO_MEMORY;
    }
    memset(px, 0, bytes);
    GfxMask m = {px, (int32_t)w, (int32_t)h, (int32_t)w};
    st = gfxFillPathMask(&m, &s->path, GFX_FILL_NONZERO, a);
    if (st != STATUS_OK) {
        a->free(a->ctx, px, bytes);
        return st;
    }
    out->mask = m;
    out->left = ix0;
    out->top = iy0;
    out->allocSize = bytes;
    out->alloc = a;
    return STATUS_OK;
}
