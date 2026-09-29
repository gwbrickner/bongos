/* See gfx-internal.h: exact-area anti-aliased scan conversion in the style of FreeType's "gray"
 * rasterizer, written from scratch in pure integers (D-143).
 *
 * Coordinates are 24.8 fixed point. The clip is processed in bands of up to 16 pixel rows. Each
 * band has, per cell (pixel), a signed `cover` (the y-extent of edge crossing that cell's row,
 * in 1/256 px) and an `area` (that extent times the sum of the crossing's entry and exit x
 * fractions within the cell). A left-to-right sweep accumulates cover; the coverage of a pixel is
 * then (acc*512 - area) / 131072 (full coverage), mapped through the fill rule. Every edge is
 * split at row and cell boundaries by exact integer arithmetic, so the pieces of a closed contour
 * cancel exactly and shared boundaries leave no seams.
 *
 * Bounds: one piece adds at most 256 to a cell's cover and 256*512 to its area, and at most
 * GFX_RASTER_MAX_EDGES (8192) edges exist, so both fit int32 (area <= 2^17 * 2^13 = 2^30). */
#include "gfx/gfx-internal.h"

#include <string.h>

#define BAND_ROWS      16
#define BAND_MAX_CELLS 131072 /* caps the band buffers at 1 MiB of cover+area */
#define FULL           131072 /* coverage of a whole pixel in (acc*512 - area) units */

/* floor(v / 256) for any sign, without relying on implementation-defined negative shifts. */
static int64_t floorDiv256(int64_t v) {
    return v >= 0 ? v / 256 : -((-v + 255) / 256);
}

/* x of the edge at fixed y (y0 <= y <= y1); exact at both ends, monotone in between. */
static int64_t edgeXAt(const GfxEdge *e, int64_t y) {
    if (y == e->y0) {
        return e->x0;
    }
    if (y == e->y1) {
        return e->x1;
    }
    return e->x0 + ((y - e->y0) * ((int64_t)e->x1 - e->x0)) / ((int64_t)e->y1 - e->y0);
}

/* y at fixed x along the segment (xa,ya)-(xb,yb) with xa != xb, xa..xb in either order. Exact at
 * both ends. */
static int64_t segYAtX(int64_t xa, int64_t ya, int64_t xb, int64_t yb, int64_t x) {
    if (x == xa) {
        return ya;
    }
    if (x == xb) {
        return yb;
    }
    return ya + ((x - xa) * (yb - ya)) / (xb - xa);
}

/* Adds one row-local piece: from (xa,ya) to (xb,yb), both y within [0,256], x relative to the
 * band's left edge in 24.8; `w` = band width in cells; `dir` = +-1.
 *
 * Every crossing y (at a cell boundary, at x = 0 and at x = W) is evaluated on the one segment
 * (xa,ya)-(xb,yb) as given, never on a re-anchored sub-segment: a translation of x by a whole
 * number of cells (a different band origin, i.e. a different clip) then leaves every crossing
 * unchanged, so a fill under a clip is bit-identical to the unclipped fill inside it. */
static void addRowPiece(int32_t *cover, int32_t *area, int32_t w, int64_t xa, int64_t ya,
                        int64_t xb, int64_t yb, int32_t dir) {
    const int64_t W = (int64_t)w * 256;
    /* Walk left to right; the y extent of each piece is |dy|. */
    if (xa > xb) {
        int64_t t = xa;
        xa = xb;
        xb = t;
        t = ya;
        ya = yb;
        yb = t;
    }
    /* Wholly left of the band: all of its coverage lands on cell 0's left boundary (cover only,
     * x fraction 0 contributes no area). */
    if (xb <= 0) {
        cover[0] += dir * (int32_t)(yb >= ya ? yb - ya : ya - yb);
        return;
    }
    if (xa >= W) {
        return; /* wholly right of the band: affects no visible cell */
    }
    if (xa == xb) {
        int64_t c = xa >> 8; /* 0 < xa < W here */
        int32_t dy = (int32_t)(yb >= ya ? yb - ya : ya - yb);
        cover[c] += dir * dy;
        area[c] += dir * dy * (int32_t)((xa & 255) * 2);
        return;
    }
    /* The part left of x = 0 (if any) is cover on cell 0; the part right of x = W is dropped. */
    int64_t lo = xa, hi = xb < W ? xb : W;
    if (xa < 0) {
        int64_t yc = segYAtX(xa, ya, xb, yb, 0);
        cover[0] += dir * (int32_t)(yc >= ya ? yc - ya : ya - yc);
        lo = 0;
    }
    int64_t c0 = lo >> 8;       /* lo >= 0 */
    int64_t c1 = (hi - 1) >> 8; /* the cell holding the points just left of hi; hi > lo */
    for (int64_t c = c0; c <= c1; c++) {
        int64_t xl = lo > c * 256 ? lo : c * 256;
        int64_t xr = hi < (c + 1) * 256 ? hi : (c + 1) * 256;
        int64_t yl = segYAtX(xa, ya, xb, yb, xl);
        int64_t yr = segYAtX(xa, ya, xb, yb, xr);
        int32_t dy = (int32_t)(yr >= yl ? yr - yl : yl - yr);
        cover[c] += dir * dy;
        area[c] += dir * dy * (int32_t)((xl - c * 256) + (xr - c * 256));
    }
}

static Status ensureScratch(const GfxAllocator *a, void **scratch, size_t *size, size_t need) {
    if (*size >= need) {
        return STATUS_OK;
    }
    void *n = a->alloc(a->ctx, need);
    if (n == NULL) {
        return STATUS_ERR_NO_MEMORY;
    }
    if (*scratch != NULL) {
        a->free(a->ctx, *scratch, *size);
    }
    *scratch = n;
    *size = need;
    return STATUS_OK;
}

Status gfxRasterFill(const GfxAllocator *a, void **scratch, size_t *scratchSize,
                     const GfxEdge *edges, uint32_t nEdges, GfxRect clip, GfxFillRule rule,
                     GfxSpanFn fn, void *ctx) {
    if (nEdges == 0 || gfxRectIsEmpty(clip)) {
        return STATUS_OK;
    }
    if (nEdges > GFX_RASTER_MAX_EDGES) {
        return STATUS_ERR_UNSUPPORTED;
    }
    /* Bounding box of the edges (in pixels), clipped. */
    int64_t minX = edges[0].x0, maxX = edges[0].x0, minY = edges[0].y0, maxY = edges[0].y1;
    for (uint32_t i = 0; i < nEdges; i++) {
        int64_t lo = edges[i].x0 < edges[i].x1 ? edges[i].x0 : edges[i].x1;
        int64_t hi = edges[i].x0 < edges[i].x1 ? edges[i].x1 : edges[i].x0;
        minX = lo < minX ? lo : minX;
        maxX = hi > maxX ? hi : maxX;
        minY = edges[i].y0 < minY ? edges[i].y0 : minY;
        maxY = edges[i].y1 > maxY ? edges[i].y1 : maxY;
    }
    int64_t bx0 = floorDiv256(minX), bx1 = -floorDiv256(-maxX);
    int64_t by0 = floorDiv256(minY), by1 = -floorDiv256(-maxY);
    bx0 = bx0 > clip.x0 ? bx0 : clip.x0;
    bx1 = bx1 < clip.x1 ? bx1 : clip.x1;
    by0 = by0 > clip.y0 ? by0 : clip.y0;
    by1 = by1 < clip.y1 ? by1 : clip.y1;
    if (bx0 >= bx1 || by0 >= by1) {
        return STATUS_OK;
    }
    int32_t bw = (int32_t)(bx1 - bx0);
    int32_t bandRows = BAND_MAX_CELLS / bw;
    bandRows = bandRows > BAND_ROWS ? BAND_ROWS : bandRows;
    if (bandRows < 1) {
        bandRows = 1;
    }
    size_t cells = (size_t)bandRows * (size_t)bw;
    size_t need = cells * 2 * sizeof(int32_t) + (size_t)bw;
    Status st = ensureScratch(a, scratch, scratchSize, need);
    if (st != STATUS_OK) {
        return st;
    }
    int32_t *cover = *scratch;
    int32_t *area = cover + cells;
    uint8_t *covRow = (uint8_t *)(area + cells);

    for (int64_t by = by0; by < by1; by += bandRows) {
        int32_t rows = (int32_t)(by1 - by < bandRows ? by1 - by : bandRows);
        memset(cover, 0, (size_t)rows * (size_t)bw * sizeof(int32_t));
        memset(area, 0, (size_t)rows * (size_t)bw * sizeof(int32_t));
        int64_t bandTop = by * 256, bandBot = (by + rows) * 256;
        for (uint32_t i = 0; i < nEdges; i++) {
            const GfxEdge *e = &edges[i];
            if ((int64_t)e->y1 <= bandTop || (int64_t)e->y0 >= bandBot) {
                continue;
            }
            int64_t ya0 = e->y0 > bandTop ? e->y0 : bandTop;
            int64_t yb1 = e->y1 < bandBot ? e->y1 : bandBot;
            for (int64_t r = floorDiv256(ya0); r * 256 < yb1; r++) {
                int64_t rowTop = r * 256;
                int64_t ya = ya0 > rowTop ? ya0 : rowTop;
                int64_t yb = yb1 < rowTop + 256 ? yb1 : rowTop + 256;
                if (ya >= yb) {
                    continue;
                }
                int64_t xa = edgeXAt(e, ya) - bx0 * 256;
                int64_t xb = edgeXAt(e, yb) - bx0 * 256;
                size_t off = (size_t)(r - by) * (size_t)bw;
                addRowPiece(cover + off, area + off, bw, xa, ya - rowTop, xb, yb - rowTop, e->dir);
            }
        }
        for (int32_t r = 0; r < rows; r++) {
            const int32_t *cv = cover + (size_t)r * (size_t)bw;
            const int32_t *ar = area + (size_t)r * (size_t)bw;
            int64_t acc = 0;
            for (int32_t x = 0; x < bw; x++) {
                acc += cv[x];
                int64_t v = acc * 512 - ar[x];
                int64_t m = v < 0 ? -v : v;
                if (rule == GFX_FILL_NONZERO) {
                    m = m > FULL ? FULL : m;
                } else {
                    m %= 2 * FULL;
                    m = m > FULL ? 2 * FULL - m : m;
                }
                covRow[x] = (uint8_t)((m * 255 + FULL / 2) / FULL);
            }
            fn(ctx, (int32_t)(by + r), (int32_t)bx0, covRow, bw);
        }
    }
    return STATUS_OK;
}
