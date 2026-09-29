/* Private to libs/gfx (and its host tests): pixel math and span blending. Integer-only. */
#ifndef LIBS_GFX_INTERNAL_H
#define LIBS_GFX_INTERNAL_H

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

/* Called once per output row segment: coverage `cov[0..n)` (0..255) for pixels x..x+n-1 of row y.
 */
typedef void (*GfxSpanFn)(void *ctx, int32_t y, int32_t x, const uint8_t *cov, int32_t n);

/* Exact-area anti-aliased scan conversion of `edges` clipped to `clip` (device pixels). Grows
 * (*scratch, *scratchSize) as needed with `a`; the caller frees it. NO_MEMORY on failure;
 * UNSUPPORTED if nEdges > GFX_RASTER_MAX_EDGES (which the cell arithmetic's int32 bound needs). */
Status gfxRasterFill(const GfxAllocator *a, void **scratch, size_t *scratchSize,
                     const GfxEdge *edges, uint32_t nEdges, GfxRect clip, GfxFillRule rule,
                     GfxSpanFn fn, void *ctx);

#endif
