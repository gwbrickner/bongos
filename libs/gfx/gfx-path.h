/* libs/gfx paths and fills (M12.2, D-142/D-143). Path geometry is float; the rasterizer behind it
 * is integer-only (24.8 fixed point, exact-area coverage). Coordinates are clamped to
 * +-GFX_COORD_MAX and non-finite values are rejected when they enter a path. */
#ifndef LIBS_GFX_PATH_H
#define LIBS_GFX_PATH_H

#include "gfx/gfx.h"

typedef enum { GFX_FILL_NONZERO, GFX_FILL_EVENODD } GfxFillRule;

enum { GFX_VERB_MOVE = 0, GFX_VERB_LINE, GFX_VERB_QUAD, GFX_VERB_CUBIC, GFX_VERB_CLOSE };

#define GFX_PATH_MAX_VERBS 65536u

/* A path builder. Errors are sticky: the first failure is kept in `error` and every later call
 * returns it, so a whole sequence of appends can be checked once (or at the fill). Every open
 * subpath is closed implicitly when filled. */
typedef struct {
    uint8_t *verbs;
    float *pts;
    uint32_t nVerbs, nPts, capVerbs, capPts;
    Status error;
    const GfxAllocator *alloc;
    float startX, startY;
    bool hasCurrent; /* a moveTo has happened */
    bool open;       /* a subpath is open (not closed since the last moveTo) */
} GfxPath;

/* `a` may be NULL (default allocator). No allocation until the first verb. */
void gfxPathInit(GfxPath *p, const GfxAllocator *a);
void gfxPathReset(GfxPath *p); /* empties the path and clears the error; keeps the buffers */
void gfxPathFree(GfxPath *p);

/* All may return INVALID (a non-finite coordinate, or lineTo/quadTo/cubicTo/close with no current
 * point), NO_MEMORY (allocation failure, or more than GFX_PATH_MAX_VERBS verbs). */
Status gfxPathMoveTo(GfxPath *p, float x, float y);
Status gfxPathLineTo(GfxPath *p, float x, float y);
Status gfxPathQuadTo(GfxPath *p, float cx, float cy, float x, float y);
Status gfxPathCubicTo(GfxPath *p, float c1x, float c1y, float c2x, float c2y, float x, float y);
Status gfxPathClose(GfxPath *p);

/* Closed subpaths, all wound the same way (right then down: clockwise on screen). A negative w or
 * h is normalized. `r` is clamped to [0, min(w,h)/2]. */
Status gfxPathAddRect(GfxPath *p, float x, float y, float w, float h);
Status gfxPathAddRoundedRect(GfxPath *p, float x, float y, float w, float h, float r);
Status gfxPathAddEllipse(GfxPath *p, float cx, float cy, float rx, float ry);

#define GFX_RASTER_MAX_EDGES 8192u

/* Fills `p` with `col` (premultiplied; sanitized on entry) using `rule`, anti-aliased, clipped to
 * the canvas clip and translated by its origin. Failure modes: the path's sticky error, NO_MEMORY
 * (scratch), UNSUPPORTED (more than GFX_RASTER_MAX_EDGES flattened edges are not wholly above or
 * below the clip; edges left or right of it still count).
 * On failure nothing has been drawn. May allocate; never sleeps.
 *
 * Known limit (inherent to cover/area rasterizers, FreeType's included): where a path overlaps
 * itself *within a single pixel* with opposite winding, that pixel's coverage is the signed sum
 * rather than the union. Simple and same-direction shapes are exact. Strokes avoid it by
 * emitting every piece with one winding (D-143). */
Status gfxFillPath(GfxCanvas *c, const GfxPath *p, GfxFillRule rule, GfxColor col, GfxOp op);

/* Composites the path's coverage into `m` (union: cov = new + old*(255-new)/255), with (0,0) at
 * the mask's top-left. Same failure modes as gfxFillPath. The mask may be wider than
 * GFX_SURFACE_MAX_DIM; past 131072 columns the scratch is one row, 9 bytes per column. */
Status gfxFillPathMask(GfxMask *m, const GfxPath *p, GfxFillRule rule, const GfxAllocator *a);

typedef enum { GFX_CAP_BUTT, GFX_CAP_SQUARE, GFX_CAP_ROUND } GfxCap;
typedef enum { GFX_JOIN_BEVEL, GFX_JOIN_MITER, GFX_JOIN_ROUND } GfxJoin;

typedef struct {
    float width; /* total width; <= 0 draws nothing */
    GfxCap cap;
    GfxJoin join;
    float miterLimit; /* SVG semantics: miter length / width; a sharper join falls back to bevel */
} GfxStroke;

/* Builds the outline of stroking `in` into `out` (which is reset first): one closed polygon per
 * segment, join and cap, ALL WOUND THE SAME WAY, to be filled together with GFX_FILL_NONZERO so
 * shared edges cancel and no seams appear (D-143). Curves are flattened to 0.1 px. Round joins
 * and caps are short arc fans, not full circles, to keep the edge count down. A subpath with no
 * length draws a dot for round caps, a square for square caps, and nothing for butt caps.
 * Failure modes: `in`'s sticky error, INVALID (a non-finite stroke width or miter limit),
 * NO_MEMORY. */
Status gfxStrokeToPath(const GfxPath *in, const GfxStroke *s, GfxPath *out);

/* gfxStrokeToPath + gfxFillPath (nonzero, SRC_OVER). Same failure modes as both. */
Status gfxStrokePath(GfxCanvas *c, const GfxPath *p, const GfxStroke *s, GfxColor col);

/* A butt-capped, bevel-joined line segment of the given width. */
Status gfxStrokeLine(GfxCanvas *c, float x0, float y0, float x1, float y1, float width,
                     GfxColor col);

#endif
