/* See gfx-internal.h: turns a GfxPath into 24.8 fixed-point edges. This is one of the few float
 * files in libs/gfx (D-142): contraction is off so results don't depend on the target's FMA, and
 * there is no libm. */
#pragma STDC FP_CONTRACT OFF

#include "gfx/gfx-internal.h"

#include <string.h>

#define FLATTEN_TOLERANCE    0.1f /* device pixels */
#define FLATTEN_MAX_SEGMENTS 256
#define FIXED_LIMIT          ((int64_t)1 << 30) /* device coordinates in 24.8, i.e. +-4M pixels */

void gfxEdgeListInit(GfxEdgeList *l, const GfxAllocator *a, GfxRect clip) {
    memset(l, 0, sizeof(*l));
    l->alloc = a != NULL ? a : gfxAllocatorDefault();
    l->clip = clip;
}

void gfxEdgeListFree(GfxEdgeList *l) {
    if (l->edges != NULL) {
        l->alloc->free(l->alloc->ctx, l->edges, l->cap * sizeof(GfxEdge));
    }
    l->edges = NULL;
    l->count = l->cap = 0;
}

void gfxEdgeListAdd(GfxEdgeList *l, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (l->error != STATUS_OK || y0 == y1) {
        return; /* failed already, or horizontal (contributes no coverage) */
    }
    int32_t dir = 1;
    if (y0 > y1) {
        int32_t t = x0;
        x0 = x1;
        x1 = t;
        t = y0;
        y0 = y1;
        y1 = t;
        dir = -1;
    }
    /* Wholly above or below the clip: no visible coverage and no effect on visible cells. */
    if ((int64_t)y1 <= (int64_t)l->clip.y0 * 256 || (int64_t)y0 >= (int64_t)l->clip.y1 * 256) {
        return;
    }
    if (l->count >= GFX_RASTER_MAX_EDGES) {
        l->error = STATUS_ERR_UNSUPPORTED;
        return;
    }
    if (l->count == l->cap) {
        uint32_t cap = l->cap != 0 ? l->cap * 2 : 256;
        GfxEdge *e = l->alloc->alloc(l->alloc->ctx, cap * sizeof(GfxEdge));
        if (e == NULL) {
            l->error = STATUS_ERR_NO_MEMORY;
            return;
        }
        if (l->edges != NULL) {
            memcpy(e, l->edges, l->count * sizeof(GfxEdge));
            l->alloc->free(l->alloc->ctx, l->edges, l->cap * sizeof(GfxEdge));
        }
        l->edges = e;
        l->cap = cap;
    }
    l->edges[l->count++] = (GfxEdge){x0, y0, x1, y1, dir};
}

/* Float user coordinate -> 24.8 device coordinate: clamp first (a float-to-int cast of an
 * out-of-range value is undefined), round half away from zero, add the integer origin, clamp. */
static int32_t toFixed(float v, int32_t origin) {
    if (v > GFX_COORD_MAX) {
        v = GFX_COORD_MAX;
    } else if (v < -GFX_COORD_MAX) {
        v = -GFX_COORD_MAX;
    }
    int64_t f = (int64_t)(v * 256.0f + (v >= 0.0f ? 0.5f : -0.5f)) + (int64_t)origin * 256;
    if (f > FIXED_LIMIT) {
        f = FIXED_LIMIT;
    } else if (f < -FIXED_LIMIT) {
        f = -FIXED_LIMIT;
    }
    return (int32_t)f;
}

typedef struct {
    GfxEdgeList *l;
    int32_t ox, oy;
    int32_t curX, curY;     /* current point, fixed */
    int32_t startX, startY; /* subpath start, fixed */
    bool open;
} Flat;

static void flatLine(Flat *f, int32_t x, int32_t y) {
    gfxEdgeListAdd(f->l, f->curX, f->curY, x, y);
    f->curX = x;
    f->curY = y;
}

static void flatClose(Flat *f) {
    if (f->open) {
        flatLine(f, f->startX, f->startY);
        f->open = false;
    }
}

static float absf(float v) {
    return v < 0.0f ? -v : v;
}

/* Cubic from (x0,y0): the segment count n is the smallest integer with 4*n^2*tol >= 3*M, where M
 * bounds the second difference (the flatness error of n uniform pieces is <= 0.75*M/n^2). Points
 * are evaluated directly in Bernstein form (no forward differencing, which accumulates error). */
static void flatCubic(Flat *f, float x0, float y0, float x1, float y1, float x2, float y2, float x3,
                      float y3) {
    float m1 = absf(x0 - 2.0f * x1 + x2) + absf(y0 - 2.0f * y1 + y2);
    float m2 = absf(x1 - 2.0f * x2 + x3) + absf(y1 - 2.0f * y2 + y3);
    float m = m1 > m2 ? m1 : m2;
    int n = 1;
    while (n < FLATTEN_MAX_SEGMENTS && (float)(n * n) * FLATTEN_TOLERANCE * 4.0f < 3.0f * m) {
        n++;
    }
    for (int i = 1; i < n; i++) {
        float t = (float)i / (float)n;
        float u = 1.0f - t;
        float b0 = u * u * u, b1 = 3.0f * u * u * t, b2 = 3.0f * u * t * t, b3 = t * t * t;
        float px = b0 * x0 + b1 * x1 + b2 * x2 + b3 * x3;
        float py = b0 * y0 + b1 * y1 + b2 * y2 + b3 * y3;
        flatLine(f, toFixed(px, f->ox), toFixed(py, f->oy));
    }
    flatLine(f, toFixed(x3, f->ox), toFixed(y3, f->oy));
}

Status gfxFlattenPath(const GfxPath *p, int32_t originX, int32_t originY, GfxEdgeList *l) {
    if (p->error != STATUS_OK) {
        return l->error = p->error;
    }
    Flat f = {l, originX, originY, 0, 0, 0, 0, false};
    float cx = 0.0f, cy = 0.0f; /* current point, float (curve control math needs it exact) */
    float sx = 0.0f, sy = 0.0f;
    const float *pt = p->pts;
    for (uint32_t i = 0; i < p->nVerbs && l->error == STATUS_OK; i++) {
        switch (p->verbs[i]) {
            case GFX_VERB_MOVE:
                flatClose(&f);
                cx = sx = pt[0];
                cy = sy = pt[1];
                f.curX = f.startX = toFixed(cx, originX);
                f.curY = f.startY = toFixed(cy, originY);
                f.open = true;
                pt += 2;
                break;
            case GFX_VERB_LINE:
                flatLine(&f, toFixed(pt[0], originX), toFixed(pt[1], originY));
                cx = pt[0];
                cy = pt[1];
                pt += 2;
                break;
            case GFX_VERB_QUAD: {
                /* elevate to a cubic: c1 = p0 + 2/3 (q - p0), c2 = p3 + 2/3 (q - p3) */
                float qx = pt[0], qy = pt[1], ex = pt[2], ey = pt[3];
                float c1x = cx + (2.0f / 3.0f) * (qx - cx), c1y = cy + (2.0f / 3.0f) * (qy - cy);
                float c2x = ex + (2.0f / 3.0f) * (qx - ex), c2y = ey + (2.0f / 3.0f) * (qy - ey);
                flatCubic(&f, cx, cy, c1x, c1y, c2x, c2y, ex, ey);
                cx = ex;
                cy = ey;
                pt += 4;
                break;
            }
            case GFX_VERB_CUBIC:
                flatCubic(&f, cx, cy, pt[0], pt[1], pt[2], pt[3], pt[4], pt[5]);
                cx = pt[4];
                cy = pt[5];
                pt += 6;
                break;
            case GFX_VERB_CLOSE:
                flatClose(&f);
                cx = sx;
                cy = sy;
                break;
            default:
                return l->error = STATUS_ERR_INVALID;
        }
    }
    flatClose(&f);
    return l->error;
}
