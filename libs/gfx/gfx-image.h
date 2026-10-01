/* libs/gfx image decoders (M12.2, D-146; JPEG and GIF added in M12.7, D-160..D-164): PNG, BMP,
 * JPEG and GIF into premultiplied ARGB32 (D-141). The
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

/* Sniffs the PNG signature, "BM", FF D8 FF (JPEG) or GIF87a/GIF89a and dispatches; UNSUPPORTED for
 * anything else. GIF yields frame 0. */
Status gfxImageDecode(const uint8_t *data, size_t size, const GfxDecodeLimits *lim,
                      const GfxAllocator *a, GfxImage *out);

/* ---- JPEG and GIF (M12.7, D-160..D-164) -------------------------------------------------- */

/* JPEG scope: SOF0/SOF1/SOF2 (baseline, extended sequential and progressive), Huffman coding,
 * 8-bit samples, 1 component (gray) or 3 (YCbCr, or RGB per the JFIF/Adobe/component-id rule),
 * every sampling factor 1..4, restart intervals, up to 128 scans. CMYK/YCCK, arithmetic coding,
 * 12-bit, lossless and hierarchical files are UNSUPPORTED. Strict: the EOI marker is required
 * (a truncated file is INVALID, never a partial image), restarts must be exactly RSTn in order
 * (no resync). EXIF orientation and ICC profiles are ignored. The output is opaque and exactly
 * reproducible on every host: an integer "islow" IDCT, replicated chroma, libjpeg's fixed-point
 * YCbCr conversion. Output pixels are opaque, so already premultiplied.
 * Budget: whole-image int16 coefficients (about 7 bytes/pixel for 4:2:0, 10 for 4:4:4) plus the
 * output count against `maxTotalBytes`; the sum (which includes the output pass's strips) is
 * checked before anything image-sized is allocated. */
Status gfxJpegDecode(const uint8_t *data, size_t size, const GfxDecodeLimits *lim,
                     const GfxAllocator *a, GfxImage *out);

/* GIF87a/GIF89a. gfxGifDecode returns frame 0 composited onto a transparent W x H logical screen.
 * Animation uses the streaming compositor below: one W x H canvas (plus one save buffer when any
 * frame uses disposal 3), so memory does not depend on the frame count; there is no frame limit
 * (each NextFrame call is bounded by the frame area, which is <= maxPixels).
 * Semantics: the trailer is required; LZW minimum code size 2..11 (1 is INVALID); a full table
 * keeps decoding with the last code width (deferred clear); the LZW stream may end early (the rest
 * of the frame is left as it was); frames are clipped to the logical screen; the logical screen's
 * background color is ignored (transparent); a pixel index beyond the palette (or with no palette)
 * is opaque black; disposal 4..7 behave as 0. */
Status gfxGifDecode(const uint8_t *data, size_t size, const GfxDecodeLimits *lim,
                    const GfxAllocator *a, GfxImage *out);

typedef struct GfxGif GfxGif; /* opaque; allocated through the accounting; not thread-safe */

typedef struct {
    uint32_t width, height; /* logical screen */
    uint32_t frameCount;    /* >= 1, exact (from the open-time pre-scan) */
    int32_t loopCount;      /* -1: no NETSCAPE2.0/ANIMEXTS1.0 extension; 0: forever; n: raw count */
} GfxGifInfo;

typedef struct {
    GfxSurface canvas; /* owned by the GfxGif; valid until the next NextFrame/Rewind/Close */
    uint32_t index;
    uint32_t delayMs;  /* raw GCE delay * 10 (0 without a GCE); callers apply their own minimum */
    uint32_t disposal; /* raw 0..7 of THIS frame (applied at the start of the next call) */
    GfxRect rect;      /* this frame's rectangle clipped to the canvas (may be empty): the damage */
} GfxGifFrame;

/* Validates the whole container except the LZW data (see gfxGifDecode) and allocates the canvas.
 * `data` must outlive the GfxGif. */
Status gfxGifOpen(const uint8_t *data, size_t size, const GfxDecodeLimits *lim,
                  const GfxAllocator *a, GfxGif **out);
GfxGifInfo gfxGifGetInfo(const GfxGif *g);
/* Applies the previous frame's disposal, draws the next frame, returns a view of the canvas.
 * NOT_FOUND after the last frame; INVALID on an LZW error (sticky until gfxGifRewind). On any
 * failure `*f` is zeroed. */
Status gfxGifNextFrame(GfxGif *g, GfxGifFrame *f);
void gfxGifRewind(GfxGif *g); /* back to frame 0 with a cleared canvas; clears a sticky error */
void gfxGifClose(GfxGif *g);  /* NULL-safe */

void gfxImageFree(GfxImage *img);                /* NULL-safe; idempotent (zeroes *img) */
GfxSurface gfxImageSurface(const GfxImage *img); /* a view of the pixels for gfxBlit */

#endif
