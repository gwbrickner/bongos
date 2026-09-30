/* See gfx-image.h: the JPEG decoder (ITU-T T.81). Scaffold: replaced step by step in M12.7. */
#include "gfx/gfx-internal.h"

#include <string.h>

Status gfxJpegDecode(const uint8_t *data, size_t size, const GfxDecodeLimits *lim,
                     const GfxAllocator *a, GfxImage *out) {
    (void)data;
    (void)size;
    (void)lim;
    (void)a;
    memset(out, 0, sizeof(*out));
    return STATUS_ERR_UNSUPPORTED;
}
