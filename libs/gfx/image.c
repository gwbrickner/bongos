/* See gfx-image.h: decode limits, allocation accounting, image ownership, format sniffing. */
#include "gfx/gfx-internal.h"

#include <string.h>

void gfxDecodeCtxInit(GfxDecodeCtx *d, const GfxDecodeLimits *lim, const GfxAllocator *a) {
    memset(d, 0, sizeof(*d));
    d->a = a != NULL ? a : gfxAllocatorDefault();
    if (lim != NULL) {
        d->lim = *lim;
    } else {
        d->lim.maxWidth = GFX_DECODE_DEFAULT_MAX_DIM;
        d->lim.maxHeight = GFX_DECODE_DEFAULT_MAX_DIM;
        d->lim.maxPixels = GFX_DECODE_DEFAULT_MAX_PIXELS;
        d->lim.maxTotalBytes = GFX_DECODE_DEFAULT_MAX_BYTES;
    }
}

void *gfxDecAlloc(GfxDecodeCtx *d, size_t n) {
    d->limitHit = false;
    if ((uint64_t)n > d->lim.maxTotalBytes || d->live > d->lim.maxTotalBytes - (uint64_t)n) {
        d->limitHit = true;
        return NULL;
    }
    void *p = d->a->alloc(d->a->ctx, n);
    if (p != NULL) {
        d->live += n;
    }
    return p;
}

void gfxDecFree(GfxDecodeCtx *d, void *p, size_t n) {
    if (p != NULL) {
        d->a->free(d->a->ctx, p, n);
        d->live -= n;
    }
}

Status gfxDecAllocStatus(const GfxDecodeCtx *d) {
    return d->limitHit ? STATUS_ERR_UNSUPPORTED : STATUS_ERR_NO_MEMORY;
}

Status gfxDecCheckDims(const GfxDecodeCtx *d, uint64_t w, uint64_t h) {
    if (w == 0 || h == 0) {
        return STATUS_ERR_INVALID;
    }
    if (w > d->lim.maxWidth || h > d->lim.maxHeight || w * h > d->lim.maxPixels) {
        return STATUS_ERR_UNSUPPORTED;
    }
    return STATUS_OK;
}

Status gfxDecAllocImage(GfxDecodeCtx *d, uint32_t w, uint32_t h, GfxImage *out) {
    uint64_t bytes = (uint64_t)w * (uint64_t)h * 4u;
    if (bytes > (uint64_t)SIZE_MAX) {
        return STATUS_ERR_UNSUPPORTED;
    }
    uint32_t *px = gfxDecAlloc(d, (size_t)bytes);
    if (px == NULL) {
        return gfxDecAllocStatus(d);
    }
    out->width = w;
    out->height = h;
    out->pixels = px;
    out->alloc = d->a;
    out->allocSize = (size_t)bytes;
    return STATUS_OK;
}

void gfxImageFree(GfxImage *img) {
    if (img == NULL) {
        return;
    }
    if (img->pixels != NULL && img->alloc != NULL) {
        img->alloc->free(img->alloc->ctx, img->pixels, img->allocSize);
    }
    memset(img, 0, sizeof(*img));
}

GfxSurface gfxImageSurface(const GfxImage *img) {
    GfxSurface s = {img->pixels, (int32_t)img->width, (int32_t)img->height, (int32_t)img->width};
    return s;
}

static const uint8_t PNG_SIG[8] = {137, 80, 78, 71, 13, 10, 26, 10};

Status gfxImageDecode(const uint8_t *data, size_t size, const GfxDecodeLimits *lim,
                      const GfxAllocator *a, GfxImage *out) {
    memset(out, 0, sizeof(*out));
    if (size >= 8 && memcmp(data, PNG_SIG, 8) == 0) {
        return gfxPngDecode(data, size, lim, a, out);
    }
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') {
        return gfxBmpDecode(data, size, lim, a, out);
    }
    return STATUS_ERR_UNSUPPORTED;
}
