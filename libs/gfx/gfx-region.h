/* libs/gfx damage regions (M12.2, D-144): a small fixed set of disjoint rectangles that always
 * covers everything added to it. Compositors collect per-window damage here and repaint only the
 * covered area. No allocation; every operation is O(16^2) at worst and never fails. */
#ifndef LIBS_GFX_REGION_H
#define LIBS_GFX_REGION_H

#include "gfx/gfx.h"

#define GFX_REGION_MAX_RECTS 16

/* Invariants after every operation:
 *  - every rect is non-empty and inside `limit`
 *  - rects are pairwise NON-OVERLAPPING (touching is allowed), so a repaint that paints each
 *    rect once composites every pixel exactly once (no double blending)
 *  - the union of the rects is a SUPERSET of everything added (overlapping additions merge into
 *    their bounding box, and past 16 rects the cheapest pair is merged: the region may
 *    over-cover, never under-cover)
 *  - count <= GFX_REGION_MAX_RECTS */
typedef struct {
    GfxRect r[GFX_REGION_MAX_RECTS + 1]; /* the extra slot is scratch while a 17th rect is merged */
    uint32_t count;
    GfxRect limit;
} GfxRegion;

/* Empty region that will clip everything added to `limit` (e.g. the screen). */
void gfxRegionInit(GfxRegion *g, GfxRect limit);
void gfxRegionClear(GfxRegion *g);

void gfxRegionAdd(GfxRegion *g, GfxRect r);
void gfxRegionUnion(GfxRegion *dst, const GfxRegion *src); /* adds every rect of src */
void gfxRegionIntersectRect(GfxRegion *g, GfxRect clip);
void gfxRegionTranslate(GfxRegion *g, int32_t dx, int32_t dy); /* saturating; moves `limit` too */
GfxRect gfxRegionBounds(const GfxRegion *g);                   /* empty rect if the region is */
bool gfxRegionIsEmpty(const GfxRegion *g);

#endif
