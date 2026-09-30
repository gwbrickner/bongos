/* Host tests for libs/gfx damage regions (M12.2, D-144). */
#include "framework/test.h"
#include "gfx/gfx-region.h"
#include "gfx/gfx.h"

#include <string.h>

static uint32_t rngState = 1;

static void rngSeed(uint32_t s) {
    rngState = s != 0 ? s : 1;
}

static uint32_t rngNext(void) {
    uint32_t x = rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rngState = x;
    return x;
}

#define GRID 48

static bool rectsOverlap(GfxRect a, GfxRect b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

static bool invariantsHold(const GfxRegion *g) {
    if (g->count > GFX_REGION_MAX_RECTS) {
        return false;
    }
    for (uint32_t i = 0; i < g->count; i++) {
        GfxRect r = g->r[i];
        if (gfxRectIsEmpty(r) || r.x0 < g->limit.x0 || r.y0 < g->limit.y0 || r.x1 > g->limit.x1 ||
            r.y1 > g->limit.y1) {
            return false;
        }
        for (uint32_t j = i + 1; j < g->count; j++) {
            if (rectsOverlap(r, g->r[j])) {
                return false;
            }
        }
    }
    return true;
}

/* Paints `r` (clipped to the grid) into a bitmap. */
static void paint(uint8_t *bm, GfxRect r) {
    for (int32_t y = r.y0 < 0 ? 0 : r.y0; y < r.y1 && y < GRID; y++) {
        for (int32_t x = r.x0 < 0 ? 0 : r.x0; x < r.x1 && x < GRID; x++) {
            bm[y * GRID + x] = 1;
        }
    }
}

static bool covers(const GfxRegion *g, const uint8_t *bm) {
    for (int32_t y = 0; y < GRID; y++) {
        for (int32_t x = 0; x < GRID; x++) {
            if (!bm[y * GRID + x]) {
                continue;
            }
            bool in = false;
            for (uint32_t i = 0; i < g->count; i++) {
                in = in || (x >= g->r[i].x0 && x < g->r[i].x1 && y >= g->r[i].y0 && y < g->r[i].y1);
            }
            if (!in) {
                return false;
            }
        }
    }
    return true;
}

static GfxRect randRect(void) {
    int32_t x = (int32_t)(rngNext() % (GRID + 20)) - 10,
            y = (int32_t)(rngNext() % (GRID + 20)) - 10;
    int32_t w = (int32_t)(rngNext() % 14), h = (int32_t)(rngNext() % 14);
    GfxRect r = {x, y, x + w, y + h};
    return r;
}

TEST(gfxRegionRandomAddsKeepInvariantsAndCover) {
    rngSeed(0xD00D);
    for (int iter = 0; iter < 300; iter++) {
        GfxRegion g;
        gfxRegionInit(&g, (GfxRect){0, 0, GRID, GRID});
        uint8_t bm[GRID * GRID];
        memset(bm, 0, sizeof(bm));
        int adds = 1 + (int)(rngNext() % 60);
        for (int i = 0; i < adds; i++) {
            GfxRect r = randRect();
            gfxRegionAdd(&g, r);
            paint(bm, r);
            ASSERT_TRUE(invariantsHold(&g));
            ASSERT_TRUE(covers(&g, bm));
        }
        /* the region never exceeds the bounding box of what was added */
        GfxRect b = gfxRegionBounds(&g);
        ASSERT_TRUE(b.x0 >= 0 && b.y0 >= 0 && b.x1 <= GRID && b.y1 <= GRID);
    }
}

/* While nothing overlaps and there are <= 16 rects, the region is exact: no over-coverage. */
TEST(gfxRegionDisjointAddsAreExact) {
    GfxRegion g;
    gfxRegionInit(&g, (GfxRect){0, 0, GRID, GRID});
    uint8_t bm[GRID * GRID];
    memset(bm, 0, sizeof(bm));
    for (int i = 0; i < 12; i++) { /* a 4x3 lattice of 8x8 tiles with gaps */
        GfxRect r = {(i % 4) * 11, (i / 4) * 11, (i % 4) * 11 + 8, (i / 4) * 11 + 8};
        gfxRegionAdd(&g, r);
        paint(bm, r);
    }
    ASSERT_EQ(g.count, (uint32_t)12);
    uint8_t exact[GRID * GRID];
    memset(exact, 0, sizeof(exact));
    for (uint32_t i = 0; i < g.count; i++) {
        paint(exact, g.r[i]);
    }
    ASSERT_EQ(memcmp(exact, bm, sizeof(bm)), 0);
    /* touching (not overlapping) rects stay separate */
    GfxRegion t;
    gfxRegionInit(&t, (GfxRect){0, 0, GRID, GRID});
    gfxRegionAdd(&t, (GfxRect){0, 0, 10, 10});
    gfxRegionAdd(&t, (GfxRect){10, 0, 20, 10});
    ASSERT_EQ(t.count, (uint32_t)2);
}

TEST(gfxRegionAddSemantics) {
    GfxRegion g;
    gfxRegionInit(&g, (GfxRect){0, 0, 100, 100});
    gfxRegionAdd(&g, (GfxRect){10, 10, 10, 20});   /* empty */
    gfxRegionAdd(&g, (GfxRect){-50, -50, -5, -5}); /* wholly outside the limit */
    ASSERT_TRUE(gfxRegionIsEmpty(&g));
    gfxRegionAdd(&g, (GfxRect){-10, -10, 20, 20}); /* clipped to the limit */
    ASSERT_EQ(g.count, (uint32_t)1);
    ASSERT_EQ(g.r[0].x0, 0);
    ASSERT_EQ(g.r[0].x1, 20);
    gfxRegionAdd(&g, (GfxRect){5, 5, 10, 10}); /* contained: no change */
    ASSERT_EQ(g.count, (uint32_t)1);
    gfxRegionAdd(&g, (GfxRect){15, 15, 40, 40}); /* overlaps: merges to a bounding box */
    ASSERT_EQ(g.count, (uint32_t)1);
    ASSERT_EQ(g.r[0].x1, 40);
    ASSERT_EQ(g.r[0].y1, 40);
    gfxRegionAdd(&g, (GfxRect){0, 0, 100, 100}); /* swallows everything */
    ASSERT_EQ(g.count, (uint32_t)1);
    ASSERT_EQ(g.r[0].x1, 100);
    gfxRegionClear(&g);
    ASSERT_TRUE(gfxRegionIsEmpty(&g));
    GfxRect none = gfxRegionBounds(&g);
    ASSERT_TRUE(gfxRectIsEmpty(none));
}

/* 17+ disjoint rects force merging; the region stays <= 16 and covers all of them. */
TEST(gfxRegionOverflowMergesButStillCovers) {
    GfxRegion g;
    gfxRegionInit(&g, (GfxRect){0, 0, GRID, GRID});
    uint8_t bm[GRID * GRID];
    memset(bm, 0, sizeof(bm));
    for (int i = 0; i < 40; i++) {
        GfxRect r = {(i % 8) * 6, (i / 8) * 9, (i % 8) * 6 + 3, (i / 8) * 9 + 3};
        gfxRegionAdd(&g, r);
        paint(bm, r);
        ASSERT_TRUE(invariantsHold(&g));
        ASSERT_TRUE(covers(&g, bm));
    }
    ASSERT_TRUE(g.count <= GFX_REGION_MAX_RECTS);
}

TEST(gfxRegionUnionIntersectTranslate) {
    GfxRegion a, b;
    gfxRegionInit(&a, (GfxRect){0, 0, 100, 100});
    gfxRegionInit(&b, (GfxRect){0, 0, 100, 100});
    gfxRegionAdd(&a, (GfxRect){0, 0, 10, 10});
    gfxRegionAdd(&b, (GfxRect){50, 50, 60, 60});
    gfxRegionAdd(&b, (GfxRect){5, 5, 15, 15}); /* overlaps a's rect */
    gfxRegionUnion(&a, &b);
    ASSERT_TRUE(invariantsHold(&a));
    ASSERT_EQ(a.count, (uint32_t)2);
    gfxRegionUnion(&a, &a); /* self-union is a no-op */
    ASSERT_EQ(a.count, (uint32_t)2);
    gfxRegionIntersectRect(&a, (GfxRect){8, 8, 55, 55});
    ASSERT_TRUE(invariantsHold(&a));
    GfxRect bnd = gfxRegionBounds(&a);
    ASSERT_EQ(bnd.x0, 8);
    ASSERT_EQ(bnd.x1, 55);
    gfxRegionIntersectRect(&a, (GfxRect){200, 200, 300, 300});
    ASSERT_TRUE(gfxRegionIsEmpty(&a));

    GfxRegion t;
    gfxRegionInit(&t, (GfxRect){0, 0, 50, 50});
    gfxRegionAdd(&t, (GfxRect){10, 10, 20, 20});
    gfxRegionTranslate(&t, -5, 7);
    ASSERT_EQ(t.r[0].x0, 5);
    ASSERT_EQ(t.r[0].y0, 17);
    ASSERT_EQ(t.limit.x1, 45);
    ASSERT_EQ(t.limit.y0, 7);
    ASSERT_TRUE(invariantsHold(&t));
}

TEST(gfxRegionExtremeCoordinatesAreSafe) {
    GfxRegion g;
    gfxRegionInit(&g, (GfxRect){INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX});
    gfxRegionAdd(&g, (GfxRect){INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX});
    gfxRegionAdd(&g, (GfxRect){-5, -5, 5, 5});
    ASSERT_EQ(g.count, (uint32_t)1);
    ASSERT_TRUE(invariantsHold(&g));
    gfxRegionTranslate(&g, INT32_MAX, INT32_MIN);
    gfxRegionTranslate(&g, INT32_MIN, INT32_MAX);
    ASSERT_TRUE(g.count <= GFX_REGION_MAX_RECTS);
    /* many huge rects merge without overflowing the area arithmetic */
    GfxRegion h;
    gfxRegionInit(&h, (GfxRect){INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX});
    rngSeed(5);
    for (int i = 0; i < 100; i++) {
        int32_t x = (int32_t)rngNext(), y = (int32_t)rngNext();
        int32_t x1 = x > INT32_MAX - 1000 ? INT32_MAX : x + 1000;
        int32_t y1 = y > INT32_MAX - 1000 ? INT32_MAX : y + 1000;
        gfxRegionAdd(&h, (GfxRect){x, y, x1, y1});
        ASSERT_TRUE(invariantsHold(&h));
    }
}

/* Past 16 rects the pair whose bounding box wastes the least area merges: 16 unit squares 10 px
 * apart plus one touching the first merge into a 2x1 box, leaving exactly the 17 px added. */
TEST(gfxRegionOverflowMergesCheapestPair) {
    GfxRegion g;
    gfxRegionInit(&g, (GfxRect){-1000, -1000, 1000, 1000});
    for (int32_t i = 0; i < 16; i++) {
        gfxRegionAdd(&g, (GfxRect){i * 10, 0, i * 10 + 1, 1});
    }
    gfxRegionAdd(&g, (GfxRect){1, 0, 2, 1}); /* touches (0,0)-(1,1): a free merge */
    ASSERT_EQ(g.count, (uint32_t)16);
    ASSERT_TRUE(invariantsHold(&g));
    int64_t area = 0;
    for (uint32_t i = 0; i < g.count; i++) {
        area += (int64_t)(g.r[i].x1 - g.r[i].x0) * (g.r[i].y1 - g.r[i].y0);
    }
    ASSERT_EQ(area, (int64_t)17);
}

/* Translation saturates at the int32 range: a rect that crosses the edge is cut there, one that
 * lands wholly on it collapses and is dropped. */
TEST(gfxRegionTranslateSaturates) {
    GfxRegion g;
    gfxRegionInit(&g, (GfxRect){INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX});
    gfxRegionAdd(&g, (GfxRect){-10, 0, 10, 10});
    gfxRegionTranslate(&g, INT32_MAX, 0);
    ASSERT_EQ(g.count, (uint32_t)1);
    ASSERT_EQ(g.r[0].x0, INT32_MAX - 10);
    ASSERT_EQ(g.r[0].x1, INT32_MAX);
    ASSERT_EQ(g.r[0].y1, 10);
    ASSERT_TRUE(invariantsHold(&g));
    gfxRegionTranslate(&g, 0, INT32_MIN); /* y [0,10) -> [INT32_MIN, INT32_MIN + 10): kept */
    ASSERT_EQ(g.count, (uint32_t)1);
    gfxRegionTranslate(&g, 0, INT32_MIN); /* both y edges saturate to INT32_MIN: collapses */
    ASSERT_EQ(g.count, (uint32_t)0);
    GfxRegion h;
    gfxRegionInit(&h, (GfxRect){INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX});
    gfxRegionAdd(&h, (GfxRect){0, 0, 10, 10});
    gfxRegionTranslate(&h, INT32_MAX, 0); /* [INT32_MAX, INT32_MAX): empty */
    ASSERT_EQ(h.count, (uint32_t)0);
    ASSERT_TRUE(gfxRegionIsEmpty(&h));
}

/* An empty limit swallows everything. */
TEST(gfxRegionEmptyLimit) {
    GfxRegion g;
    gfxRegionInit(&g, (GfxRect){5, 5, 5, 20});
    gfxRegionAdd(&g, (GfxRect){0, 0, 100, 100});
    ASSERT_TRUE(gfxRegionIsEmpty(&g));
}
