/* libs/gfx core (M12.2, D-141..D-143): a 2D software rasterizer over caller-owned pixel memory.
 * Userland/host only, never built into the kernel. Pixels are premultiplied ARGB32, one native
 * uint32_t 0xAARRGGBB each (B,G,R,A in memory -- the GOP BGRX / virtio-gpu B8G8R8A8 layout), and
 * every channel is <= A. All drawing entry points sanitize incoming colors and source pixels
 * (each channel clamped to A) so a straight-alpha value passed by mistake cannot break that
 * invariant or overflow a channel; the one exception is an exact opaque SRC copy in gfxBlit,
 * which moves the source pixels unchanged.
 *
 * Contracts, unless a function says otherwise: not thread-safe per canvas, never sleeps, no
 * global state, and no libc beyond <string.h> (the allocator hook is the only path to malloc). */
#ifndef LIBS_GFX_H
#define LIBS_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "uapi/status.h"

typedef uint32_t GfxColor; /* premultiplied 0xAARRGGBB */

/* Builds a premultiplied color from straight (non-premultiplied) 0xAARRGGBB, rounding half up. */
GfxColor gfxColorPremul(uint32_t straightArgb);

/* Half-open: covers x in [x0,x1), y in [y0,y1); empty iff x0 >= x1 || y0 >= y1. */
typedef struct {
    int32_t x0, y0, x1, y1;
} GfxRect;

bool gfxRectIsEmpty(GfxRect r);
GfxRect gfxRectIntersect(GfxRect a, GfxRect b); /* empty result is {0,0,0,0} */
GfxRect gfxRectUnion(GfxRect a, GfxRect b);     /* bounding box; an empty operand is ignored */

/* Caller-owned pixels; `stride` is in pixels (>= width). */
typedef struct {
    uint32_t *pixels;
    int32_t width, height, stride;
} GfxSurface;

/* An 8-bit coverage mask (blur input/output, glyphs, shadows). */
typedef struct GfxMask {
    uint8_t *data;
    int32_t width, height, stride; /* stride in bytes (>= width) */
} GfxMask;

/* Allocation hook for scratch memory and decoded images. `alloc` returns NULL on failure; `free`
 * gets the size `alloc` was asked for. */
typedef struct GfxAllocator {
    void *(*alloc)(void *ctx, size_t size);
    void (*free)(void *ctx, void *ptr, size_t size);
    void *ctx;
} GfxAllocator;

/* malloc/free. The only place in libs/gfx that touches <stdlib.h> is alloc.c. */
const GfxAllocator *gfxAllocatorDefault(void);

typedef enum {
    GFX_OP_SRC_OVER = 0, /* the source is composited over the destination */
    GFX_OP_SRC = 1,      /* the source replaces the destination (scaled by coverage) */
} GfxOp;

#define GFX_CLIP_DEPTH      32
#define GFX_SURFACE_MAX_DIM 65536
#define GFX_COORD_MAX       1048576.0f /* path coordinates are clamped to +-this; non-finite rejected */

typedef struct {
    GfxSurface surf;
    int32_t originX, originY;     /* integer translation applied to every drawing coordinate */
    GfxRect clip[GFX_CLIP_DEPTH]; /* in surface coordinates; clip[0] is always the surface bounds */
    uint32_t clipDepth;
    const GfxAllocator *alloc; /* raster scratch */
    void *scratch;             /* grown on demand and reused across fills */
    size_t scratchSize;
} GfxCanvas;

/* Failure modes: INVALID for a NULL surface, non-positive or over-GFX_SURFACE_MAX_DIM dimensions,
 * or stride < width (or a stride*height that overflows). Call gfxCanvasDestroy first if the canvas
 * already owns scratch (re-initializing would leak it). `a` may be NULL (the default allocator
 * is used). Does not allocate. */
Status gfxCanvasInit(GfxCanvas *c, GfxSurface s, const GfxAllocator *a);
void gfxCanvasDestroy(GfxCanvas *c); /* frees the scratch buffer */

void gfxCanvasSetOrigin(GfxCanvas *c, int32_t x, int32_t y);
void gfxCanvasTranslate(GfxCanvas *c, int32_t dx, int32_t dy); /* saturating */

/* Pushes `r` (in canvas coordinates: the origin is applied) intersected with the current clip.
 * UNSUPPORTED at GFX_CLIP_DEPTH. Popping below the base clip is INVALID. */
Status gfxCanvasPushClip(GfxCanvas *c, GfxRect r);
Status gfxCanvasPopClip(GfxCanvas *c);
GfxRect gfxCanvasClipBounds(const GfxCanvas *c); /* current clip in canvas coordinates */

/* Integer-aligned fill, no anti-aliasing. */
void gfxFillRect(GfxCanvas *c, GfxRect r, GfxColor col, GfxOp op);

/* Copies/composites `srcRect` of `src` (clamped to src's bounds) to (dx, dy), scaled by the
 * global `alpha` (255 = opaque). `src` may be the canvas surface itself (the same `pixels` and
 * `stride`): the copy direction is chosen so an overlapping move reads every source pixel before
 * overwriting it, for either op. A different GfxSurface viewing the same memory (a sub-rect view)
 * is not detected and must not overlap the destination. An invalid `src` (NULL, non-positive size,
 * stride < width) draws nothing. */
void gfxBlit(GfxCanvas *c, int32_t dx, int32_t dy, const GfxSurface *src, GfxRect srcRect, GfxOp op,
             uint8_t alpha);

/* SRC_OVER of `col` through the mask's coverage, with the mask's (0,0) at (dx, dy). An invalid mask
 * (NULL data, non-positive size, stride < width) draws nothing. */
void gfxFillMask(GfxCanvas *c, int32_t dx, int32_t dy, const GfxMask *m, GfxColor col);

#endif
