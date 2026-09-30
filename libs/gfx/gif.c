/* See gfx-image.h: the GIF decoder and animation compositor. Scaffold: replaced in M12.7. */
#include "gfx/gfx-internal.h"

#include <string.h>

Status gfxGifDecode(const uint8_t *data, size_t size, const GfxDecodeLimits *lim,
                    const GfxAllocator *a, GfxImage *out) {
    (void)data;
    (void)size;
    (void)lim;
    (void)a;
    memset(out, 0, sizeof(*out));
    return STATUS_ERR_UNSUPPORTED;
}

Status gfxGifOpen(const uint8_t *data, size_t size, const GfxDecodeLimits *lim,
                  const GfxAllocator *a, GfxGif **out) {
    (void)data;
    (void)size;
    (void)lim;
    (void)a;
    *out = NULL;
    return STATUS_ERR_UNSUPPORTED;
}

GfxGifInfo gfxGifGetInfo(const GfxGif *g) {
    (void)g;
    GfxGifInfo i = {0, 0, 0, -1};
    return i;
}

Status gfxGifNextFrame(GfxGif *g, GfxGifFrame *f) {
    (void)g;
    memset(f, 0, sizeof(*f));
    return STATUS_ERR_NOT_FOUND;
}

void gfxGifRewind(GfxGif *g) {
    (void)g;
}

void gfxGifClose(GfxGif *g) {
    (void)g;
}
