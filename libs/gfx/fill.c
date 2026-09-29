/* See gfx-path.h: gfxFillPath and gfxFillPathMask, glue between flatten.c and raster.c. */
#include "gfx/gfx-internal.h"

typedef struct {
    GfxCanvas *c;
    GfxColor col;
    GfxOp op;
} CanvasSink;

static void canvasSpan(void *ctx, int32_t y, int32_t x, const uint8_t *cov, int32_t n) {
    CanvasSink *s = ctx;
    uint32_t *row = s->c->surf.pixels + (size_t)y * (size_t)s->c->surf.stride + (size_t)x;
    gfxBlendSpanCov(row, cov, (size_t)n, s->col, s->op);
}

Status gfxFillPath(GfxCanvas *c, const GfxPath *p, GfxFillRule rule, GfxColor col, GfxOp op) {
    if (p->error != STATUS_OK) {
        return p->error;
    }
    GfxRect clip = c->clip[c->clipDepth - 1];
    if (gfxRectIsEmpty(clip) || p->nVerbs == 0) {
        return STATUS_OK;
    }
    GfxEdgeList l;
    gfxEdgeListInit(&l, c->alloc, clip);
    Status st = gfxFlattenPath(p, c->originX, c->originY, &l);
    if (st == STATUS_OK) {
        CanvasSink sink = {c, gfxColorSanitize(col), op};
        st = gfxRasterFill(c->alloc, &c->scratch, &c->scratchSize, l.edges, l.count, clip, rule,
                           canvasSpan, &sink);
    }
    gfxEdgeListFree(&l);
    return st;
}

typedef struct {
    GfxMask *m;
} MaskSink;

static void maskSpan(void *ctx, int32_t y, int32_t x, const uint8_t *cov, int32_t n) {
    MaskSink *s = ctx;
    uint8_t *row = s->m->data + (size_t)y * (size_t)s->m->stride + (size_t)x;
    for (int32_t i = 0; i < n; i++) {
        if (cov[i] != 0) {
            row[i] = (uint8_t)(cov[i] + gfxMulDiv255(row[i], 255u - cov[i]));
        }
    }
}

Status gfxFillPathMask(GfxMask *m, const GfxPath *p, GfxFillRule rule, const GfxAllocator *a) {
    if (p->error != STATUS_OK) {
        return p->error;
    }
    if (m->data == NULL || m->width <= 0 || m->height <= 0 || m->stride < m->width) {
        return STATUS_ERR_INVALID;
    }
    if (a == NULL) {
        a = gfxAllocatorDefault();
    }
    GfxRect clip = {0, 0, m->width, m->height};
    GfxEdgeList l;
    gfxEdgeListInit(&l, a, clip);
    Status st = gfxFlattenPath(p, 0, 0, &l);
    void *scratch = NULL;
    size_t scratchSize = 0;
    if (st == STATUS_OK) {
        MaskSink sink = {m};
        st =
            gfxRasterFill(a, &scratch, &scratchSize, l.edges, l.count, clip, rule, maskSpan, &sink);
    }
    if (scratch != NULL) {
        a->free(a->ctx, scratch, scratchSize);
    }
    gfxEdgeListFree(&l);
    return st;
}
