/* libs/gfx image decoders (M12.2, D-146): PNG and BMP into premultiplied ARGB32 (D-141). The
 * decoders take untrusted input: every size is computed in 64 bits and checked against the limits
 * BEFORE anything is allocated, every offset is bounds-checked, and every allocation goes through
 * an accounting wrapper that enforces `maxTotalBytes` across all live temporaries.
 *
 * Status mapping, for every decoder:
 *   STATUS_ERR_INVALID     malformed, truncated, or a checksum/CRC error
 *   STATUS_ERR_UNSUPPORTED valid but out of scope (see each decoder) OR a limit was exceeded
 *   STATUS_ERR_NO_MEMORY   the allocator failed
 * On every failure `*out` is zeroed and every temporary is freed. Pure, reentrant, never sleeps. */
#ifndef LIBS_GFX_IMAGE_H
#define LIBS_GFX_IMAGE_H

#include "gfx/gfx.h"

typedef struct {
    uint32_t width, height;
    uint32_t *pixels; /* premultiplied ARGB, stride == width; NULL after gfxImageFree */
    const GfxAllocator *alloc;
    size_t allocSize; /* bytes of `pixels`, for the allocator's free */
} GfxImage;

typedef struct {
    uint32_t maxWidth, maxHeight;
    uint64_t maxPixels;     /* width * height */
    uint64_t maxTotalBytes; /* peak of all live allocations during a decode */
} GfxDecodeLimits;

#define GFX_DECODE_DEFAULT_MAX_DIM    16384u
#define GFX_DECODE_DEFAULT_MAX_PIXELS ((uint64_t)1 << 26)
#define GFX_DECODE_DEFAULT_MAX_BYTES  ((uint64_t)512 << 20)

/* `lim` may be NULL (the defaults above); `a` may be NULL (the default allocator). The allocator
 * must outlive the returned image: gfxImageFree uses it.
 *
 * PNG scope: color types 0/2/3/4/6 at every legal bit depth (1/2/4/8/16), PLTE and tRNS, all five
 * row filters, non-interlaced and Adam7. Ancillary chunks are CRC-checked and skipped (no color
 * management: gAMA/iCCP/sRGB are ignored); APNG is not animated (the default image is decoded);
 * an unknown critical chunk is UNSUPPORTED. 16-bit samples are reduced to 8 bits with rounding.
 * Data after IEND is ignored.
 *
 * BMP scope: "BM" files with a 40/52/56/108/124-byte DIB header; 1/4/8-bit palettes,
 * 16/24/32-bit; BI_RGB, BI_BITFIELDS and BI_ALPHABITFIELDS; bottom-up and top-down. RLE4/RLE8,
 * embedded JPEG/PNG, OS/2 headers, and planes != 1 are UNSUPPORTED. */
Status gfxPngDecode(const uint8_t *data, size_t size, const GfxDecodeLimits *lim,
                    const GfxAllocator *a, GfxImage *out);
Status gfxBmpDecode(const uint8_t *data, size_t size, const GfxDecodeLimits *lim,
                    const GfxAllocator *a, GfxImage *out);

/* Sniffs the PNG signature or "BM" and dispatches; UNSUPPORTED for anything else. */
Status gfxImageDecode(const uint8_t *data, size_t size, const GfxDecodeLimits *lim,
                      const GfxAllocator *a, GfxImage *out);

void gfxImageFree(GfxImage *img);                /* NULL-safe; idempotent (zeroes *img) */
GfxSurface gfxImageSurface(const GfxImage *img); /* a view of the pixels for gfxBlit */

#endif
