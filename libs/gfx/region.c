/* See gfx-region.h. */
#include "gfx/gfx-region.h"

#include "gfx/gfx-internal.h"

#include <string.h>

void gfxRegionInit(GfxRegion *g, GfxRect limit) {
    memset(g, 0, sizeof(*g));
    g->limit = limit;
}

void gfxRegionClear(GfxRegion *g) {
    g->count = 0;
}

bool gfxRegionIsEmpty(const GfxRegion *g) {
    return g->count == 0;
}

/* Positive-area overlap (touching edges do not count). */
static bool overlaps(GfxRect a, GfxRect b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

static bool contains(GfxRect outer, GfxRect inner) {
    return outer.x0 <= inner.x0 && outer.y0 <= inner.y0 && outer.x1 >= inner.x1 &&
           outer.y1 >= inner.y1;
}

/* Rect area, saturating at UINT64_MAX (extents can reach 2^32 each). */
static uint64_t area(GfxRect r) {
    uint64_t w = (uint64_t)((int64_t)r.x1 - r.x0), h = (uint64_t)((int64_t)r.y1 - r.y0);
    if (w != 0 && h > UINT64_MAX / w) {
        return UINT64_MAX;
    }
    return w * h;
}

static void removeAt(GfxRegion *g, uint32_t i) {
    g->r[i] = g->r[g->count - 1];
    g->count--;
}

/* Merges any two overlapping rects into their bounding box until none overlap. */
static void resolveOverlaps(GfxRegion *g) {
    bool again = true;
    while (again) {
        again = false;
        for (uint32_t i = 0; i < g->count && !again; i++) {
            for (uint32_t j = i + 1; j < g->count; j++) {
                if (overlaps(g->r[i], g->r[j])) {
                    g->r[i] = gfxRectUnion(g->r[i], g->r[j]);
                    removeAt(g, j);
                    again = true;
                    break;
                }
            }
        }
    }
}

void gfxRegionAdd(GfxRegion *g, GfxRect r) {
    r = gfxRectIntersect(r, g->limit);
    if (gfxRectIsEmpty(r)) {
        return;
    }
    for (uint32_t i = 0; i < g->count; i++) {
        if (contains(g->r[i], r)) {
            return; /* already covered */
        }
    }
    /* Drop rects the new one swallows; merge with any it overlaps (the merged box may then
     * overlap others, so rescan until stable). */
    bool again = true;
    while (again) {
        again = false;
        for (uint32_t i = 0; i < g->count; i++) {
            if (contains(r, g->r[i])) {
                removeAt(g, i);
                again = true;
                break;
            }
            if (overlaps(r, g->r[i])) {
                r = gfxRectUnion(r, g->r[i]);
                removeAt(g, i);
                again = true;
                break;
            }
        }
    }
    g->r[g->count++] = r; /* may be the 17th (the array has one spare slot); merged below */
    while (g->count > GFX_REGION_MAX_RECTS) {
        /* Merge the pair whose bounding box wastes the least area, then re-disjoin. */
        uint32_t bi = 0, bj = 1;
        uint64_t best = UINT64_MAX;
        for (uint32_t i = 0; i < g->count; i++) {
            for (uint32_t j = i + 1; j < g->count; j++) {
                uint64_t u = area(gfxRectUnion(g->r[i], g->r[j]));
                uint64_t sum = area(g->r[i]) + area(g->r[j]);
                uint64_t waste = u > sum ? u - sum : 0;
                if (waste < best) {
                    best = waste;
                    bi = i;
                    bj = j;
                }
            }
        }
        g->r[bi] = gfxRectUnion(g->r[bi], g->r[bj]);
        removeAt(g, bj);
        resolveOverlaps(g);
    }
}

void gfxRegionUnion(GfxRegion *dst, const GfxRegion *src) {
    GfxRegion copy = *src; /* dst == src is allowed */
    for (uint32_t i = 0; i < copy.count; i++) {
        gfxRegionAdd(dst, copy.r[i]);
    }
}

void gfxRegionIntersectRect(GfxRegion *g, GfxRect clip) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g->count; i++) {
        GfxRect x = gfxRectIntersect(g->r[i], clip);
        if (!gfxRectIsEmpty(x)) {
            g->r[n++] = x;
        }
    }
    g->count = n;
}

static int32_t sat(int64_t v) {
    return v > INT32_MAX ? INT32_MAX : (v < INT32_MIN ? INT32_MIN : (int32_t)v);
}

static GfxRect shifted(GfxRect r, int32_t dx, int32_t dy) {
    GfxRect t = {sat((int64_t)r.x0 + dx), sat((int64_t)r.y0 + dy), sat((int64_t)r.x1 + dx),
                 sat((int64_t)r.y1 + dy)};
    return t;
}

void gfxRegionTranslate(GfxRegion *g, int32_t dx, int32_t dy) {
    g->limit = shifted(g->limit, dx, dy);
    uint32_t n = 0;
    for (uint32_t i = 0; i < g->count; i++) {
        GfxRect t = shifted(g->r[i], dx, dy);
        if (!gfxRectIsEmpty(t)) { /* saturation at the extremes can collapse a rect */
            g->r[n++] = t;
        }
    }
    g->count = n;
    /* Saturating shifts can push distinct rects onto each other at the extremes. */
    resolveOverlaps(g);
}

GfxRect gfxRegionBounds(const GfxRegion *g) {
    GfxRect b = {0, 0, 0, 0};
    for (uint32_t i = 0; i < g->count; i++) {
        b = gfxRectUnion(b, g->r[i]);
    }
    return b;
}
