/* Private to libs/gfx (and its host tests): pixel math and span blending. Integer-only. */
#ifndef LIBS_GFX_INTERNAL_H
#define LIBS_GFX_INTERNAL_H

#include "gfx/gfx-image.h"
#include "gfx/gfx-path.h"
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

/* A flattened edge in 24.8 fixed device coordinates; y0 < y1 always (horizontal edges are dropped),
 * and `dir` is +1 if the original edge went down (y increasing), -1 if up. */
typedef struct {
    int32_t x0, y0, x1, y1;
    int32_t dir;
} GfxEdge;

typedef struct {
    GfxEdge *edges;
    uint32_t count, cap;
    const GfxAllocator *alloc;
    GfxRect clip; /* device pixels: edges wholly above or below it are dropped */
    Status error;
} GfxEdgeList;

void gfxEdgeListInit(GfxEdgeList *l, const GfxAllocator *a, GfxRect clip);
void gfxEdgeListFree(GfxEdgeList *l);

/* Appends the edge (x0,y0)-(x1,y1), 24.8 device coordinates. Sets l->error to NO_MEMORY or
 * UNSUPPORTED (over GFX_RASTER_MAX_EDGES) and ignores further edges after a failure. */
void gfxEdgeListAdd(GfxEdgeList *l, int32_t x0, int32_t y0, int32_t x1, int32_t y1);

/* Flattens `p` (curves subdivided to 0.1 px) into `l`, translated by (originX, originY) device
 * pixels. Every subpath is closed implicitly. Returns l->error. */
Status gfxFlattenPath(const GfxPath *p, int32_t originX, int32_t originY, GfxEdgeList *l);

/* Cubic subdivision to 0.1 px (shared by the fill flattener and the stroker). `fn` is called for
 * each point after (x0,y0); the last call is exactly (x3,y3). */
typedef void (*GfxPointFn)(void *ctx, float x, float y);
void gfxSubdivideCubic(float x0, float y0, float x1, float y1, float x2, float y2, float x3,
                       float y3, GfxPointFn fn, void *ctx);

/* Called once per output row segment: coverage `cov[0..n)` (0..255) for pixels x..x+n-1 of row y.
 */
typedef void (*GfxSpanFn)(void *ctx, int32_t y, int32_t x, const uint8_t *cov, int32_t n);

/* Exact-area anti-aliased scan conversion of `edges` clipped to `clip` (device pixels). Grows
 * (*scratch, *scratchSize) as needed with `a`; the caller frees it. NO_MEMORY on failure;
 * UNSUPPORTED if nEdges > GFX_RASTER_MAX_EDGES (which the cell arithmetic's int32 bound needs).
 * Every edge coordinate must be within +-2^30 (gfxFlattenPath guarantees it), which keeps the
 * int64 interpolation products below 2^62. */
Status gfxRasterFill(const GfxAllocator *a, void **scratch, size_t *scratchSize,
                     const GfxEdge *edges, uint32_t nEdges, GfxRect clip, GfxFillRule rule,
                     GfxSpanFn fn, void *ctx);

/* Decoder allocation accounting: every decoder allocation goes through here so `maxTotalBytes`
 * bounds the PEAK of all live temporaries plus the output. */
typedef struct {
    const GfxAllocator *a;
    GfxDecodeLimits lim;
    uint64_t live;
    bool limitHit; /* the last failed gfxDecAlloc was the byte budget, not the allocator */
} GfxDecodeCtx;

void gfxDecodeCtxInit(GfxDecodeCtx *d, const GfxDecodeLimits *lim, const GfxAllocator *a);
void *gfxDecAlloc(GfxDecodeCtx *d, size_t n); /* NULL if over budget (limitHit) or on failure */
void gfxDecFree(GfxDecodeCtx *d, void *p, size_t n);
/* NO_MEMORY or (after a budget hit) UNSUPPORTED, for a NULL from gfxDecAlloc. */
Status gfxDecAllocStatus(const GfxDecodeCtx *d);
/* INVALID if w or h is zero; UNSUPPORTED if either is over its limit or w*h exceeds maxPixels. */
Status gfxDecCheckDims(const GfxDecodeCtx *d, uint64_t w, uint64_t h);
/* Allocates out->pixels (w*h*4 bytes, uninitialized) through the accounting; fills the fields. */
Status gfxDecAllocImage(GfxDecodeCtx *d, uint32_t w, uint32_t h, GfxImage *out);

#endif
