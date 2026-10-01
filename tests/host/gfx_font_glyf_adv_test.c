/* Adversarial host tests for libs/gfx's glyf loader, outline -> path conversion and glyph renderer
 * (M12.3, D-151/D-153), written by the step 3-5 bug sweep (docs/sweeps/M12.3.md). Fonts are built
 * from scratch with the glyf table as the last bytes of an exact-size buffer, so ASan sees any
 * read past it. Expected outlines come from the construction (an independent encoder), every limit
 * is pinned at N and N+1, truncation runs at every byte, a strict allocator checks every free size
 * and leak, and a seeded mutation fuzz renders mutated glyf/loca/hmtx/head tables of the fixtures
 * and both shipped fonts under allocation failure. */
#include "font_testutil.h"
#include "framework/test.h"
#include "gfx/font-internal.h"
#include "gfx/gfx-font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- PRNG (xorshift64*) and a big-endian byte buffer ---------------------------------------- */

static uint64_t rngState = 1;

static void rngSeed(uint64_t s) {
    rngState = s != 0 ? s : 0x9E3779B97F4A7C15ull;
}

static uint32_t rng(void) {
    rngState ^= rngState >> 12;
    rngState ^= rngState << 25;
    rngState ^= rngState >> 27;
    return (uint32_t)((rngState * 0x2545F4914F6CDD1Dull) >> 32);
}

typedef struct {
    uint8_t *d;
    size_t n, cap;
} Buf;

static void put8(Buf *b, uint32_t v) {
    if (b->n + 1 > b->cap) {
        b->cap = b->cap != 0 ? b->cap * 2 : 256;
        b->d = realloc(b->d, b->cap);
    }
    b->d[b->n++] = (uint8_t)v;
}

static void put16(Buf *b, uint32_t v) {
    put8(b, v >> 8);
    put8(b, v);
}

static void putBuf(Buf *b, const Buf *src) {
    for (size_t i = 0; i < src->n; i++) {
        put8(b, src->d[i]);
    }
}

static void bufFree(Buf *b) {
    free(b->d);
    memset(b, 0, sizeof *b);
}

/* ---- a strict allocator: fails the Nth allocation, checks every free's size ------------------ */

typedef struct {
    void *p;
    size_t n;
} Blk;

typedef struct {
    Blk blk[64];
    int nBlk, count, failAt, bad;
    size_t live, peak;
} Strict;

static void *stAlloc(void *ctx, size_t n) {
    Strict *s = ctx;
    if (s->count++ == s->failAt) {
        return NULL;
    }
    if (s->nBlk >= 64) {
        s->bad++;
        return NULL;
    }
    void *p = malloc(n != 0 ? n : 1);
    s->blk[s->nBlk++] = (Blk){p, n};
    s->live += n;
    s->peak = s->live > s->peak ? s->live : s->peak;
    return p;
}

static void stFree(void *ctx, void *p, size_t n) {
    Strict *s = ctx;
    if (p == NULL) {
        return;
    }
    for (int i = 0; i < s->nBlk; i++) {
        if (s->blk[i].p == p) {
            if (s->blk[i].n != n) {
                s->bad++; /* freed with a different size than it was allocated with */
            }
            s->live -= s->blk[i].n;
            s->blk[i] = s->blk[--s->nBlk];
            free(p);
            return;
        }
    }
    s->bad++; /* a foreign or double free */
}

static void strictInit(Strict *s, GfxAllocator *a, int failAt) {
    memset(s, 0, sizeof *s);
    s->failAt = failAt;
    a->alloc = stAlloc;
    a->free = stFree;
    a->ctx = s;
}

/* ---- glyph encoders (independent of the loader) --------------------------------------------- */

typedef struct {
    int32_t x, y;
    uint8_t on;
} Pt;

/* A simple glyph from absolute points (deltas must fit int16) and contour end indices; `instr`
 * instruction bytes; `rle` packs runs of equal flags with the repeat bit. */
static void encSimple(Buf *b, const Pt *p, uint32_t np, const uint32_t *ends, uint32_t nc,
                      uint32_t instr, int rle) {
    put16(b, nc);
    for (int i = 0; i < 4; i++) {
        put16(b, 0);
    }
    for (uint32_t i = 0; i < nc; i++) {
        put16(b, ends[i]);
    }
    put16(b, instr);
    for (uint32_t i = 0; i < instr; i++) {
        put8(b, 0xB0u + i % 7u);
    }
    uint8_t *fl = malloc(np + 1);
    Buf xs = {0}, ys = {0};
    int32_t px = 0, py = 0;
    for (uint32_t i = 0; i < np; i++) {
        int32_t dx = p[i].x - px, dy = p[i].y - py;
        uint8_t f = p[i].on ? 1u : 0u;
        if (dx == 0) {
            f |= 0x10u;
        } else if (dx >= -255 && dx <= 255) {
            f |= 0x02u | (dx > 0 ? 0x10u : 0u);
            put8(&xs, (uint32_t)(dx > 0 ? dx : -dx));
        } else {
            put16(&xs, (uint32_t)(uint16_t)(int16_t)dx);
        }
        if (dy == 0) {
            f |= 0x20u;
        } else if (dy >= -255 && dy <= 255) {
            f |= 0x04u | (dy > 0 ? 0x20u : 0u);
            put8(&ys, (uint32_t)(dy > 0 ? dy : -dy));
        } else {
            put16(&ys, (uint32_t)(uint16_t)(int16_t)dy);
        }
        fl[i] = f;
        px = p[i].x;
        py = p[i].y;
    }
    for (uint32_t i = 0; i < np;) {
        uint32_t run = 1;
        while (rle && i + run < np && fl[i + run] == fl[i] && run < 256) {
            run++;
        }
        if (run > 1) {
            put8(b, fl[i] | 0x08u);
            put8(b, run - 1);
        } else {
            put8(b, fl[i]);
        }
        i += run;
    }
    putBuf(b, &xs);
    putBuf(b, &ys);
    bufFree(&xs);
    bufFree(&ys);
    free(fl);
}

/* np points at the origin in one contour (all "same" coordinates: no coordinate bytes) */
static void encDots(Buf *b, uint32_t np, uint32_t nc) {
    put16(b, nc);
    for (int i = 0; i < 4; i++) {
        put16(b, 0);
    }
    for (uint32_t i = 0; i < nc; i++) {
        put16(b, nc == 1 ? np - 1 : i + (np - nc) * (i == nc - 1));
    }
    put16(b, 0);
    for (uint32_t i = 0; i < np; i += 256) {
        uint32_t run = np - i < 256 ? np - i : 256;
        if (run > 1) {
            put8(b, 0x31u | 0x08u);
            put8(b, run - 1);
        } else {
            put8(b, 0x31u);
        }
    }
}

enum { XF_NONE, XF_SCALE, XF_XY, XF_2X2 };

typedef struct {
    uint16_t gid;
    int32_t dx, dy;
    int words, kind;
    int16_t m[4]; /* F2Dot14 raw: scale m[0]; xy m[0],m[1]; 2x2 a,b,c,d in file order */
    uint16_t extra;
} Comp;

static void encComposite(Buf *b, const Comp *c, uint32_t n) {
    put16(b, 0xFFFFu);
    for (int i = 0; i < 4; i++) {
        put16(b, 0);
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t fl = 0x0002u | c[i].extra;
        int words =
            c[i].words || c[i].dx < -128 || c[i].dx > 127 || c[i].dy < -128 || c[i].dy > 127;
        if (words) {
            fl |= 0x0001u;
        }
        fl |= c[i].kind == XF_SCALE ? 0x0008u
              : c[i].kind == XF_XY  ? 0x0040u
              : c[i].kind == XF_2X2 ? 0x0080u
                                    : 0u;
        if (i + 1 < n) {
            fl |= 0x0020u;
        }
        put16(b, fl);
        put16(b, c[i].gid);
        if (words) {
            put16(b, (uint32_t)(uint16_t)c[i].dx);
            put16(b, (uint32_t)(uint16_t)c[i].dy);
        } else {
            put8(b, (uint32_t)(uint8_t)c[i].dx);
            put8(b, (uint32_t)(uint8_t)c[i].dy);
        }
        int nm = c[i].kind == XF_SCALE ? 1 : c[i].kind == XF_XY ? 2 : c[i].kind == XF_2X2 ? 4 : 0;
        for (int k = 0; k < nm; k++) {
            put16(b, (uint32_t)(uint16_t)c[i].m[k]);
        }
    }
}

/* A font (on top of synth-bad's cmap/head/hhea) whose glyphs are `g[0..n)`, long loca, glyf last
 * and exact. `lastLen` (if not SIZE_MAX) truncates the last glyph to that many bytes. */
static uint8_t *glyphFont(const Buf *g, uint32_t n, uint32_t upem, size_t lastLen, size_t *outN) {
    size_t sn;
    const uint8_t *src = ftuFont(FTU_SYNTH_BAD, &sn);
    uint32_t hl, ml, hhl;
    uint32_t ho = ftuTable(src, sn, "head", &hl), mo = ftuTable(src, sn, "maxp", &ml),
             hho = ftuTable(src, sn, "hhea", &hhl);
    uint8_t *head = malloc(hl), *maxp = malloc(ml), *hhea = malloc(hhl);
    memcpy(head, src + ho, hl);
    memcpy(maxp, src + mo, ml);
    memcpy(hhea, src + hho, hhl);
    ftuPut16(head, 18, upem);
    ftuPut16(head, 50, 1);
    ftuPut16(maxp, 4, n);
    ftuPut16(hhea, 34, n);
    uint8_t *hmtx = calloc(4u * n, 1);
    uint8_t *loca = malloc(4u * (n + 1));
    Buf glyf = {0};
    for (uint32_t i = 0; i < n; i++) {
        ftuPut32(loca, 4 * i, (uint32_t)glyf.n);
        size_t len = i + 1 == n && lastLen != (size_t)-1 ? lastLen : g[i].n;
        for (size_t k = 0; k < len; k++) {
            put8(&glyf, g[i].d[k]);
        }
    }
    ftuPut32(loca, 4 * n, (uint32_t)glyf.n);
    const FtuTable ov[] = {{"head", head, hl},           {"maxp", maxp, ml},
                           {"hhea", hhea, hhl},          {"hmtx", hmtx, 4u * n},
                           {"loca", loca, 4u * (n + 1)}, {"glyf", glyf.d, (uint32_t)glyf.n}};
    uint8_t *d = ftuRebuild(src, ov, 6, outN);
    free(head);
    free(maxp);
    free(hhea);
    free(hmtx);
    free(loca);
    bufFree(&glyf);
    return d;
}

/* ---- expected outlines by construction ------------------------------------------------------- */

typedef struct {
    double a, b, c, d, e, f;
} Mat;

static Mat matMul(Mat m, Mat k) { /* m o k */
    Mat r = {m.a * k.a + m.c * k.b, m.b * k.a + m.d * k.b,       m.a * k.c + m.c * k.d,
             m.b * k.c + m.d * k.d, m.a * k.e + m.c * k.f + m.e, m.b * k.e + m.d * k.f + m.f};
    return r;
}

static int ptIs(const GfxGlyphOutline *o, uint32_t i, Mat m, double x, double y) {
    double ex = m.a * x + m.c * y + m.e, ey = m.b * x + m.d * y + m.f;
    return (double)o->xy[2 * i] == ex && (double)o->xy[2 * i + 1] == ey;
}

/* ---- simple glyphs --------------------------------------------------------------------------- */

TEST(glyfAdvSimpleRoundTripRandom) {
    enum { N = 240 };
    static Pt pts[N][128];
    static uint32_t ends[N][8];
    static uint32_t np[N], nc[N];
    Buf g[N + 1];
    memset(g, 0, sizeof g);
    rngSeed(0x61796C66ull);
    for (uint32_t k = 0; k < N; k++) {
        nc[k] = 1 + rng() % 6;
        np[k] = 0;
        int32_t x = 0, y = 0;
        for (uint32_t c = 0; c < nc[k]; c++) {
            uint32_t len = 1 + rng() % 20;
            for (uint32_t i = 0; i < len; i++) {
                int32_t d[2];
                for (int j = 0; j < 2; j++) {
                    switch (rng() % 4) {
                        case 0:
                            d[j] = 0;
                            break;
                        case 1: {
                            const int32_t mag = (int32_t)(1 + rng() % 255);
                            d[j] = mag * (rng() & 1 ? 1 : -1);
                        } break;
                        case 2: {
                            const int32_t mag = (int32_t)(256 + rng() % 32512);
                            d[j] = mag * (rng() & 1 ? 1 : -1);
                        } break;
                        default: /* the int16 extremes and the short/long boundary */
                        {
                            static const int32_t ex[] = {32767, -32768, 255, -255,
                                                         256,   -256,   1,   -1};
                            d[j] = ex[rng() % 8];
                        }
                    }
                }
                x += d[0];
                y += d[1];
                pts[k][np[k]++] = (Pt){x, y, (uint8_t)(rng() & 1)};
            }
            ends[k][c] = np[k] - 1;
        }
        const uint32_t instr = rng() % 3 == 0 ? rng() % 40 : 0;
        const int rle = (int)(rng() & 1);
        encSimple(&g[k], pts[k], np[k], ends[k], nc[k], instr, rle);
    }
    /* N: a composite of glyphs 0, 1 and 2 */
    Comp cc[3] = {{.gid = 0, .dx = 7, .dy = -9},
                  {.gid = 1, .dx = -300, .dy = 1000, .kind = XF_SCALE, .m = {0x2000}},
                  {.gid = 2, .dx = 0, .dy = 0, .kind = XF_2X2, .m = {0, 0x4000, -0x4000, 0}}};
    encComposite(&g[N], cc, 3);
    size_t n;
    uint8_t *font = glyphFont(g, N + 1, 1000, (size_t)-1, &n);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, font, n), STATUS_OK);
    GfxGlyphOutline o;
    gfxGlyphOutlineInit(&o, NULL);
    static const Mat xfs[] = {{1, 0, 0, 1, 0, 0}, {2, 0, 0, -0.5, 3, -4}, {0, 1, -1, 0, 0.5, 0}};
    for (int t = 0; t < 3; t++) {
        const GfxFontXform xf = {(float)xfs[t].a, (float)xfs[t].b, (float)xfs[t].c,
                                 (float)xfs[t].d, (float)xfs[t].e, (float)xfs[t].f};
        for (uint32_t k = 0; k < N; k++) {
            ASSERT_EQ(gfxFontGlyphOutline(&f, (uint16_t)k, &xf, &o), STATUS_OK);
            ASSERT_EQ(o.nPoints, np[k]);
            ASSERT_EQ(o.nContours, nc[k]);
            for (uint32_t c = 0; c < nc[k]; c++) {
                ASSERT_EQ(o.contourEnd[c], ends[k][c]);
            }
            for (uint32_t i = 0; i < np[k]; i++) {
                if (!ptIs(&o, i, xfs[t], pts[k][i].x, pts[k][i].y)) {
                    fprintf(stderr, "  glyph %u point %u xf %d: (%g,%g) want (%d,%d)\n", k, i, t,
                            (double)o.xy[2 * i], (double)o.xy[2 * i + 1], (int)pts[k][i].x,
                            (int)pts[k][i].y);
                }
                ASSERT_TRUE(ptIs(&o, i, xfs[t], pts[k][i].x, pts[k][i].y));
                ASSERT_EQ(o.onCurve[i], pts[k][i].on);
            }
        }
        /* the composite: each component's points under M o comp, contour ends rebased */
        ASSERT_EQ(gfxFontGlyphOutline(&f, N, &xf, &o), STATUS_OK);
        ASSERT_EQ(o.nPoints, np[0] + np[1] + np[2]);
        ASSERT_EQ(o.nContours, nc[0] + nc[1] + nc[2]);
        static const Mat comp[3] = {
            {1, 0, 0, 1, 7, -9}, {0.5, 0, 0, 0.5, -300, 1000}, {0, 1, -1, 0, 0, 0}};
        uint32_t base = 0, cbase = 0;
        for (int c = 0; c < 3; c++) {
            Mat m = matMul(xfs[t], comp[c]);
            for (uint32_t i = 0; i < np[c]; i++) {
                ASSERT_TRUE(ptIs(&o, base + i, m, pts[c][i].x, pts[c][i].y));
            }
            for (uint32_t i = 0; i < nc[c]; i++) {
                ASSERT_EQ(o.contourEnd[cbase + i], base + ends[c][i]);
            }
            base += np[c];
            cbase += nc[c];
        }
    }
    gfxGlyphOutlineFree(&o);
    for (uint32_t k = 0; k <= N; k++) {
        bufFree(&g[k]);
    }
    free(font);
}

/* Truncating the last glyph of an exact-size font at every length: 0 is an empty glyph, the full
 * length loads, anything in between is INVALID (and never reads past the buffer). */
static void truncationSweep(const Buf *g, uint32_t n, uint32_t wantPts) {
    const Buf *last = &g[n - 1];
    GfxGlyphOutline o;
    gfxGlyphOutlineInit(&o, NULL);
    for (size_t len = 0; len <= last->n; len++) {
        size_t fn;
        uint8_t *font = glyphFont(g, n, 1000, len, &fn);
        GfxFont f;
        ASSERT_EQ(gfxFontInit(&f, font, fn), STATUS_OK);
        Status st = gfxFontGlyphOutline(&f, (uint16_t)(n - 1), NULL, &o);
        Status want = len == last->n ? STATUS_OK : len == 0 ? STATUS_OK : STATUS_ERR_INVALID;
        if (st != want) {
            fprintf(stderr, "  truncated to %zu of %zu: status %d\n", len, last->n, (int)st);
        }
        free(font);
        ASSERT_EQ(st, want);
        if (len == 0 || len == last->n) { /* on failure the contents are unspecified */
            ASSERT_EQ(o.nPoints, len == 0 ? 0u : wantPts);
        }
    }
    gfxGlyphOutlineFree(&o);
}

TEST(glyfAdvTruncationAtEveryByte) {
    static const Pt tri[] = {{0, 0, 1}, {100, 0, 1}, {0, 100, 0}};
    static const uint32_t triEnd[] = {2};
    /* long and short coordinates, instructions, a repeat run */
    static const Pt mix[] = {{0, 0, 1},         {300, 0, 1}, {300, -40, 0}, {300, -80, 0},
                             {300, -120, 0},    {-5, 7, 1},  {-5, 7, 1},    {-5, 7, 1},
                             {32000, 30000, 0}, {1, 1, 1}};
    static const uint32_t mixEnd[] = {4, 9};
    Buf g[2];
    memset(g, 0, sizeof g);
    encSimple(&g[0], tri, 3, triEnd, 1, 0, 0);
    encSimple(&g[1], mix, 10, mixEnd, 2, 5, 1);
    truncationSweep(g, 2, 10);
    bufFree(&g[1]);
    /* every composite argument and transform form, the last component ending the glyph */
    Comp cc[5] = {{.gid = 0, .dx = -128, .dy = 127},
                  {.gid = 0, .dx = 1, .dy = 2, .words = 1, .kind = XF_SCALE, .m = {0x2000}},
                  {.gid = 0, .dx = -32768, .dy = 32767, .kind = XF_XY, .m = {0x4000, -0x8000}},
                  {.gid = 0, .dx = 3, .dy = 4, .kind = XF_2X2, .m = {0, 0x4000, -0x4000, 0}},
                  {.gid = 0, .dx = 5, .dy = 6, .extra = 0x0800, .kind = XF_SCALE, .m = {0x7FFF}}};
    encComposite(&g[1], cc, 5);
    truncationSweep(g, 2, 15);
    bufFree(&g[0]);
    bufFree(&g[1]);
}

TEST(glyfAdvFlagRepeatAndInstructionBounds) {
    /* hand-written simple glyphs: 1 contour of 3 points, every coordinate "same" (no x/y bytes) */
    static const struct {
        uint8_t bytes[24];
        uint32_t n;
        Status want;
    } cases[] = {
        /* repeat exactly to the last point */
        {{0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0x39, 2}, 16, STATUS_OK},
        /* one repeat too many */
        {{0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0x39, 3}, 16, STATUS_ERR_INVALID},
        /* repeat 0 is a single flag, then two more */
        {{0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0x39, 0, 0x31, 0x31}, 18, STATUS_OK},
        /* the repeat byte missing */
        {{0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0x31, 0x31, 0x39}, 17, STATUS_ERR_INVALID},
        /* instructions that end exactly at the glyph end leave no room for flags */
        {{0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 3, 1, 2, 3}, 17, STATUS_ERR_INVALID},
        /* instructions one byte past the end */
        {{0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 4, 1, 2, 3}, 17, STATUS_ERR_INVALID},
        /* instruction length 0xFFFF */
        {{0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0xFF, 0xFF, 0x39, 2}, 16, STATUS_ERR_INVALID},
        /* contour end 65535: 65536 points is over the limit before anything else is read */
        {{0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF}, 12, STATUS_ERR_UNSUPPORTED},
        /* equal contour ends are not increasing */
        {{0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 0, 0x39, 2}, 18, STATUS_ERR_INVALID},
        /* the contour-end array itself truncated */
        {{0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}, 12, STATUS_ERR_INVALID},
        /* a 1-byte x after a short-x flag is the last byte: fine */
        {{0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x33, 9}, 16, STATUS_OK},
        /* a long x (2 bytes) with only 1 left */
        {{0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x21, 9}, 16, STATUS_ERR_INVALID},
        /* a long y (2 bytes) with only 1 left */
        {{0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 9}, 16, STATUS_ERR_INVALID},
        /* a short y with nothing left */
        {{0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x15}, 15, STATUS_ERR_INVALID},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Buf g[1] = {{0}};
        for (uint32_t k = 0; k < cases[i].n; k++) {
            put8(&g[0], cases[i].bytes[k]);
        }
        size_t fn;
        uint8_t *font = glyphFont(g, 1, 1000, (size_t)-1, &fn);
        GfxFont f;
        ASSERT_EQ(gfxFontInit(&f, font, fn), STATUS_OK);
        GfxGlyphOutline o;
        gfxGlyphOutlineInit(&o, NULL);
        Status st = gfxFontGlyphOutline(&f, 0, NULL, &o);
        if (st != cases[i].want) {
            fprintf(stderr, "  case %zu: status %d\n", i, (int)st);
        }
        ASSERT_EQ(st, cases[i].want);
        if (st == STATUS_OK && i == 10) {
            ASSERT_TRUE(o.xy[0] == 9.0f && o.xy[1] == 0.0f); /* +9 (flag 0x10 = positive) */
        }
        gfxGlyphOutlineFree(&o);
        free(font);
        bufFree(&g[0]);
    }
}

/* ---- limits ---------------------------------------------------------------------------------- */

TEST(glyfAdvLimitsAreExact) {
    enum {
        DOTS_MAX,
        DOTS_OVER,
        CONT_MAX,
        CONT_OVER,
        HALF,
        HALF1,
        C2048,
        C2049,
        EMPTY,
        TRI,
        /* composites from here */
        SUM_MAX,
        SUM_OVER,
        SUM_TRI_OVER,
        CSUM_MAX,
        CSUM_OVER,
        COMP256,
        COMP257,
        FAN15,
        TREE256,
        TREE257,
        NG
    };
    Buf g[NG];
    memset(g, 0, sizeof g);
    encDots(&g[DOTS_MAX], GFX_FONT_MAX_POINTS, 1);
    encDots(&g[DOTS_OVER], GFX_FONT_MAX_POINTS + 1, 1);
    encDots(&g[CONT_MAX], GFX_FONT_MAX_CONTOURS, GFX_FONT_MAX_CONTOURS);
    encDots(&g[CONT_OVER], GFX_FONT_MAX_CONTOURS + 1, GFX_FONT_MAX_CONTOURS + 1);
    encDots(&g[HALF], GFX_FONT_MAX_POINTS / 2, 1);
    encDots(&g[HALF1], GFX_FONT_MAX_POINTS / 2 + 1, 1);
    encDots(&g[C2048], GFX_FONT_MAX_CONTOURS / 2, GFX_FONT_MAX_CONTOURS / 2);
    encDots(&g[C2049], GFX_FONT_MAX_CONTOURS / 2 + 1, GFX_FONT_MAX_CONTOURS / 2 + 1);
    put16(&g[EMPTY], 0);
    for (int i = 0; i < 4; i++) {
        put16(&g[EMPTY], 0);
    }
    static const Pt tri[] = {{0, 0, 1}, {100, 0, 1}, {0, 100, 1}};
    static const uint32_t triEnd[] = {2};
    encSimple(&g[TRI], tri, 3, triEnd, 1, 0, 0);
    Comp two[3] = {{.gid = HALF}, {.gid = HALF}, {.gid = TRI}};
    encComposite(&g[SUM_MAX], two, 2);
    two[1].gid = HALF1;
    encComposite(&g[SUM_OVER], two, 2);
    two[1].gid = HALF;
    encComposite(&g[SUM_TRI_OVER], two, 3);
    Comp cs[2] = {{.gid = C2048}, {.gid = C2048}};
    encComposite(&g[CSUM_MAX], cs, 2);
    cs[1].gid = C2049;
    encComposite(&g[CSUM_OVER], cs, 2);
    static Comp many[300];
    for (int i = 0; i < 300; i++) {
        many[i] = (Comp){.gid = TRI, .dx = i % 100};
    }
    encComposite(&g[COMP256], many, GFX_FONT_MAX_COMPONENTS);
    encComposite(&g[COMP257], many, GFX_FONT_MAX_COMPONENTS + 1);
    for (int i = 0; i < 15; i++) {
        many[i].gid = EMPTY;
    }
    encComposite(&g[FAN15], many, 15);
    Comp tree[17];
    for (int i = 0; i < 17; i++) {
        tree[i] = (Comp){.gid = i < 16 ? FAN15 : EMPTY};
    }
    encComposite(&g[TREE256], tree, 16); /* 16 + 16 * 15 = 256 components */
    encComposite(&g[TREE257], tree, 17); /* one more */
    size_t fn;
    uint8_t *font = glyphFont(g, NG, 1000, (size_t)-1, &fn);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, font, fn), STATUS_OK);
    static const struct {
        int gid;
        Status want;
        uint32_t pts, contours;
    } cases[] = {
        {DOTS_MAX, STATUS_OK, GFX_FONT_MAX_POINTS, 1},
        {DOTS_OVER, STATUS_ERR_UNSUPPORTED, 0, 0},
        {CONT_MAX, STATUS_OK, GFX_FONT_MAX_CONTOURS, GFX_FONT_MAX_CONTOURS},
        {CONT_OVER, STATUS_ERR_UNSUPPORTED, 0, 0},
        {SUM_MAX, STATUS_OK, GFX_FONT_MAX_POINTS, 2},
        {SUM_OVER, STATUS_ERR_UNSUPPORTED, 0, 0},
        {SUM_TRI_OVER, STATUS_ERR_UNSUPPORTED, 0, 0},
        {CSUM_MAX, STATUS_OK, GFX_FONT_MAX_CONTOURS, GFX_FONT_MAX_CONTOURS},
        {CSUM_OVER, STATUS_ERR_UNSUPPORTED, 0, 0},
        {COMP256, STATUS_OK, 3 * GFX_FONT_MAX_COMPONENTS, GFX_FONT_MAX_COMPONENTS},
        {COMP257, STATUS_ERR_UNSUPPORTED, 0, 0},
        {TREE256, STATUS_OK, 0, 0},
        {TREE257, STATUS_ERR_UNSUPPORTED, 0, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Strict st;
        GfxAllocator a;
        strictInit(&st, &a, -1);
        GfxGlyphOutline o;
        gfxGlyphOutlineInit(&o, &a);
        Status s = gfxFontGlyphOutline(&f, (uint16_t)cases[i].gid, NULL, &o);
        if (s != cases[i].want) {
            fprintf(stderr, "  limit case %zu (glyph %d): status %d\n", i, cases[i].gid, (int)s);
        }
        ASSERT_EQ(s, cases[i].want);
        if (s == STATUS_OK) {
            ASSERT_EQ(o.nPoints, cases[i].pts);
            ASSERT_EQ(o.nContours, cases[i].contours);
            ASSERT_TRUE(o.capPoints <= GFX_FONT_MAX_POINTS && o.capPoints >= o.nPoints);
            ASSERT_TRUE(o.capContours <= GFX_FONT_MAX_CONTOURS && o.capContours >= o.nContours);
            for (uint32_t c = 0; c < o.nContours; c++) {
                ASSERT_TRUE(o.contourEnd[c] < o.nPoints);
                ASSERT_TRUE(c == 0 || o.contourEnd[c] > o.contourEnd[c - 1]);
            }
        }
        /* the arrays never outgrow the documented bound (~164 KB), growth included */
        ASSERT_TRUE(st.peak <= 9u * GFX_FONT_MAX_POINTS + 4u * GFX_FONT_MAX_CONTOURS +
                                   9u * GFX_FONT_MAX_POINTS / 2u);
        gfxGlyphOutlineFree(&o);
        ASSERT_EQ(st.live, (size_t)0);
        ASSERT_EQ(st.bad, 0);
    }
    free(font);
    for (int i = 0; i < NG; i++) {
        bufFree(&g[i]);
    }
}

/* ---- composite transforms ---------------------------------------------------------------------
 */

TEST(glyfAdvCompositeTransformsByConstruction) {
    enum { P, C0, NCOMP = 14, Q = C0 + NCOMP, R, BADMID, NG };
    static const Pt pp[] = {{100, 0, 1}, {0, 50, 0}, {-30, -70, 1}};
    static const uint32_t ppEnd[] = {2};
    Buf g[NG];
    memset(g, 0, sizeof g);
    encSimple(&g[P], pp, 3, ppEnd, 1, 0, 0);
    /* one-component composites of P, and the matrix + offset each must produce */
    static const struct {
        Comp c;
        Mat want;
    } cc[NCOMP] = {
        {{.gid = P, .dx = -128, .dy = 127}, {1, 0, 0, 1, -128, 127}},
        {{.gid = P, .dx = -32768, .dy = 32767}, {1, 0, 0, 1, -32768, 32767}},
        {{.gid = P, .dx = 1, .dy = -1, .words = 1}, {1, 0, 0, 1, 1, -1}},
        {{.gid = P, .dx = 10, .dy = 20, .kind = XF_SCALE, .m = {-0x8000}}, {-2, 0, 0, -2, 10, 20}},
        {{.gid = P, .dx = 10, .dy = 20, .kind = XF_SCALE, .m = {-0x8000}, .extra = 0x0800},
         {-2, 0, 0, -2, -20, -40}},
        {{.gid = P, .dx = 10, .dy = 20, .kind = XF_SCALE, .m = {-0x8000}, .extra = 0x1800},
         {-2, 0, 0, -2, 10, 20}},
        {{.gid = P, .dx = 10, .dy = 20, .kind = XF_SCALE, .m = {-0x8000}, .extra = 0x1000},
         {-2, 0, 0, -2, 10, 20}},
        {{.gid = P, .dx = 8, .dy = 4, .kind = XF_XY, .m = {0x2000, 0x7000}, .extra = 0x0800},
         {0.5, 0, 0, 1.75, 4, 7}},
        {{.gid = P, .dx = 8, .dy = 4, .kind = XF_2X2, .m = {0, 0x4000, -0x4000, 0}},
         {0, 1, -1, 0, 8, 4}},
        {{.gid = P,
          .dx = 8,
          .dy = 4,
          .kind = XF_2X2,
          .m = {0, 0x4000, -0x4000, 0},
          .extra = 0x0800},
         {0, 1, -1, 0, -4, 8}},
        {{.gid = P, .dx = 3, .dy = 5, .kind = XF_2X2, .m = {0x2000, 0x1000, -0x6000, 0x7FFF}},
         {0.5, 0.25, -1.5, 32767.0 / 16384.0, 3, 5}},
        /* scale beside X_AND_Y_SCALE and TWO_BY_TWO: WE_HAVE_A_SCALE wins, 2 bytes are read */
        {{.gid = P, .dx = 0, .dy = 0, .kind = XF_SCALE, .m = {0x6000}, .extra = 0x00C0},
         {1.5, 0, 0, 1.5, 0, 0}},
        /* X_AND_Y_SCALE beside TWO_BY_TWO: X_AND_Y wins, 4 bytes are read */
        {{.gid = P, .dx = 0, .dy = 0, .kind = XF_XY, .m = {0x6000, 0x2000}, .extra = 0x0080},
         {1.5, 0, 0, 0.5, 0, 0}},
        /* flags that are ignored: ROUND_XY_TO_GRID, USE_MY_METRICS, OVERLAP_COMPOUND, instructions
         * (0x0100: after the last component, never read) */
        {{.gid = P, .dx = 1, .dy = 2, .extra = 0x0004 | 0x0200 | 0x0400 | 0x0100},
         {1, 0, 0, 1, 1, 2}},
    };
    for (int i = 0; i < NCOMP; i++) {
        encComposite(&g[C0 + i], &cc[i].c, 1);
    }
    /* nested: Q = P scaled 0.5 moved by (100, 0); R = Q rotated, offset (10, 20) scaled */
    Comp q = {.gid = P, .dx = 100, .dy = 0, .kind = XF_SCALE, .m = {0x2000}};
    encComposite(&g[Q], &q, 1);
    Comp r = {.gid = Q,
              .dx = 10,
              .dy = 20,
              .kind = XF_2X2,
              .m = {0, 0x4000, -0x4000, 0},
              .extra = 0x0800};
    encComposite(&g[R], &r, 1);
    /* a point-matching component after a loaded one */
    Buf *bm = &g[BADMID];
    Comp ok = {.gid = P, .dx = 1, .dy = 1};
    encComposite(bm, &ok, 1);
    bm->d[11] |= 0x20; /* MORE_COMPONENTS in the first component's flags */
    put16(bm, 0x0000); /* args are point numbers */
    put16(bm, P);
    put8(bm, 0);
    put8(bm, 1);
    size_t fn;
    uint8_t *font = glyphFont(g, NG, 1000, (size_t)-1, &fn);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, font, fn), STATUS_OK);
    GfxGlyphOutline o;
    gfxGlyphOutlineInit(&o, NULL);
    static const Mat roots[] = {{1, 0, 0, 1, 0, 0}, {0.5, 0, 0, -2, 1, 2}};
    for (int t = 0; t < 2; t++) {
        const GfxFontXform xf = {(float)roots[t].a, (float)roots[t].b, (float)roots[t].c,
                                 (float)roots[t].d, (float)roots[t].e, (float)roots[t].f};
        for (int i = 0; i < NCOMP; i++) {
            Status st = gfxFontGlyphOutline(&f, (uint16_t)(C0 + i), &xf, &o);
            ASSERT_EQ(st, STATUS_OK);
            ASSERT_EQ(o.nPoints, 3u);
            Mat m = matMul(roots[t], cc[i].want);
            for (uint32_t k = 0; k < 3; k++) {
                if (!ptIs(&o, k, m, pp[k].x, pp[k].y)) {
                    fprintf(stderr, "  composite case %d root %d point %u: (%g, %g)\n", i, t, k,
                            (double)o.xy[2 * k], (double)o.xy[2 * k + 1]);
                }
                ASSERT_TRUE(ptIs(&o, k, m, pp[k].x, pp[k].y));
            }
        }
        ASSERT_EQ(gfxFontGlyphOutline(&f, R, &xf, &o), STATUS_OK);
        Mat m =
            matMul(roots[t], matMul((Mat){0, 1, -1, 0, -20, 10}, (Mat){0.5, 0, 0, 0.5, 100, 0}));
        for (uint32_t k = 0; k < 3; k++) {
            ASSERT_TRUE(ptIs(&o, k, m, pp[k].x, pp[k].y));
        }
    }
    /* a partial load that fails leaves a reusable outline */
    ASSERT_EQ(gfxFontGlyphOutline(&f, BADMID, NULL, &o), STATUS_ERR_UNSUPPORTED);
    ASSERT_EQ(gfxFontGlyphOutline(&f, P, NULL, &o), STATUS_OK);
    ASSERT_EQ(o.nPoints, 3u);
    ASSERT_EQ(o.nContours, 1u);
    ASSERT_EQ(o.contourEnd[0], 2u);
    gfxGlyphOutlineFree(&o);
    free(font);
    for (int i = 0; i < NG; i++) {
        bufFree(&g[i]);
    }
}

/* ---- allocation failure with a strict allocator, then reuse -----------------------------------
 */

TEST(glyfAdvAllocationFailureThenReuse) {
    enum { BIG, HALF, TRI, SUM, CONT, CHAIN0, NG = CHAIN0 + 4 };
    Buf g[NG];
    memset(g, 0, sizeof g);
    encDots(&g[BIG], GFX_FONT_MAX_POINTS, 1);
    encDots(&g[HALF], 5000, 1);
    static const Pt tri[] = {{0, 0, 1}, {100, 0, 1}, {0, 100, 1}};
    static const uint32_t triEnd[] = {2};
    encSimple(&g[TRI], tri, 3, triEnd, 1, 0, 0);
    Comp two[3] = {{.gid = TRI}, {.gid = HALF}, {.gid = HALF}};
    encComposite(&g[SUM], two, 3);
    encDots(&g[CONT], 3000, 3000);
    for (int i = 0; i < 4; i++) {
        Comp c[2] = {{.gid = i < 3 ? CHAIN0 + i + 1 : CONT}, {.gid = TRI}};
        encComposite(&g[CHAIN0 + i], c, 2);
    }
    size_t fn;
    uint8_t *font = glyphFont(g, NG, 1000, (size_t)-1, &fn);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, font, fn), STATUS_OK);
    static const int gids[] = {BIG, SUM, CONT, CHAIN0};
    for (size_t k = 0; k < sizeof gids / sizeof gids[0]; k++) {
        int sawOk = 0;
        for (int failAt = 0; failAt < 64 && !sawOk; failAt++) {
            Strict st;
            GfxAllocator a;
            strictInit(&st, &a, failAt);
            GfxGlyphOutline o;
            gfxGlyphOutlineInit(&o, &a);
            /* a small glyph first, so the failing load has old arrays to keep */
            Status s0 = gfxFontGlyphOutline(&f, TRI, NULL, &o);
            Status s = gfxFontGlyphOutline(&f, (uint16_t)gids[k], NULL, &o);
            ASSERT_TRUE(s0 == STATUS_OK || s0 == STATUS_ERR_NO_MEMORY);
            ASSERT_TRUE(s == STATUS_OK || s == STATUS_ERR_NO_MEMORY);
            sawOk = s0 == STATUS_OK && s == STATUS_OK;
            /* whatever happened, the outline is usable again once memory is back */
            st.failAt = -1;
            ASSERT_EQ(gfxFontGlyphOutline(&f, TRI, NULL, &o), STATUS_OK);
            ASSERT_EQ(o.nPoints, 3u);
            ASSERT_EQ(o.contourEnd[0], 2u);
            ASSERT_TRUE(o.xy[2] == 100.0f && o.xy[5] == 100.0f);
            ASSERT_EQ(gfxFontGlyphOutline(&f, (uint16_t)gids[k], NULL, &o), STATUS_OK);
            gfxGlyphOutlineFree(&o);
            gfxGlyphOutlineFree(&o); /* idempotent */
            ASSERT_EQ(st.live, (size_t)0);
            ASSERT_EQ(st.bad, 0);
        }
        ASSERT_TRUE(sawOk);
    }
    free(font);
    for (int i = 0; i < NG; i++) {
        bufFree(&g[i]);
    }
}

/* ---- outline -> path with hostile outlines -----------------------------------------------------
 */

static void hostileOutline(GfxGlyphOutline *o, float *xy, uint8_t *on, uint32_t np, uint32_t *ends,
                           uint32_t nc) {
    gfxGlyphOutlineInit(o, NULL);
    o->xy = xy;
    o->onCurve = on;
    o->contourEnd = ends;
    o->nPoints = np;
    o->nContours = nc;
}

TEST(glyfAdvOutlineToPathHostile) {
    GfxGlyphOutline o;
    GfxPath p;
    gfxPathInit(&p, NULL);
    /* midpoints of huge off-curve points overflow to inf: rejected, no UB */
    float big[] = {3e38f, 3e38f, 3e38f, -3e38f, 3e38f, 3e38f};
    uint8_t off3[] = {0, 0, 0};
    uint32_t e2[] = {2};
    hostileOutline(&o, big, off3, 3, e2, 1);
    ASSERT_EQ(gfxGlyphOutlineToPath(&o, 0.0f, 0.0f, &p), STATUS_ERR_INVALID);
    /* a finite translation that overflows */
    gfxPathReset(&p);
    uint8_t on3[] = {1, 1, 1};
    hostileOutline(&o, big, on3, 3, e2, 1);
    ASSERT_EQ(gfxGlyphOutlineToPath(&o, 3e38f, 0.0f, &p), STATUS_ERR_INVALID);
    /* NaN in the outline, or as the translation */
    gfxPathReset(&p);
    float nan[] = {0.0f, 0.0f, __builtin_nanf(""), 1.0f, 2.0f, 2.0f};
    hostileOutline(&o, nan, on3, 3, e2, 1);
    ASSERT_EQ(gfxGlyphOutlineToPath(&o, 0.0f, 0.0f, &p), STATUS_ERR_INVALID);
    gfxPathReset(&p);
    float fine[] = {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f};
    hostileOutline(&o, fine, on3, 3, e2, 1);
    ASSERT_EQ(gfxGlyphOutlineToPath(&o, __builtin_inff(), 0.0f, &p), STATUS_ERR_INVALID);
    /* a sticky path error is returned as is */
    gfxPathReset(&p);
    p.error = STATUS_ERR_NO_MEMORY;
    ASSERT_EQ(gfxGlyphOutlineToPath(&o, 0.0f, 0.0f, &p), STATUS_ERR_NO_MEMORY);
    ASSERT_EQ(p.nVerbs, 0u);
    /* contour ends: decreasing, repeated, past nPoints, UINT32_MAX */
    static const struct {
        uint32_t ends[3];
        uint32_t nc;
        Status want;
        uint32_t verbs;
    } ec[] = {
        {{2, 1}, 2, STATUS_ERR_INVALID, 0},
        {{1, 1}, 2, STATUS_ERR_INVALID, 0},
        {{3}, 1, STATUS_ERR_INVALID, 0},
        {{0xFFFFFFFFu}, 1, STATUS_ERR_INVALID, 0},
        {{2}, 1, STATUS_OK, 4},
        {{1, 2}, 2, STATUS_OK, 3}, /* 2 points + 1 point */
        {{0, 1, 2}, 3, STATUS_OK, 0},
        {{0}, 0, STATUS_OK, 0},
    };
    for (size_t i = 0; i < sizeof ec / sizeof ec[0]; i++) {
        gfxPathReset(&p);
        uint32_t ends[3];
        memcpy(ends, ec[i].ends, sizeof ends);
        hostileOutline(&o, fine, on3, 3, ends, ec[i].nc);
        ASSERT_EQ(gfxGlyphOutlineToPath(&o, 0.0f, 0.0f, &p), ec[i].want);
        if (ec[i].want == STATUS_OK) {
            ASSERT_EQ(p.nVerbs, ec[i].verbs);
        }
    }
    /* the second and later contours start where the previous one ended */
    gfxPathReset(&p);
    float six[] = {0, 0, 4, 0, 4, 4, 10, 10, 14, 10, 14, 14};
    uint8_t on6[] = {1, 1, 1, 1, 1, 1};
    uint32_t e6[] = {2, 5};
    hostileOutline(&o, six, on6, 6, e6, 2);
    ASSERT_EQ(gfxGlyphOutlineToPath(&o, 0.0f, 0.0f, &p), STATUS_OK);
    ASSERT_EQ(p.nVerbs, 8u);
    ASSERT_TRUE(p.pts[6] == 10.0f && p.pts[7] == 10.0f); /* second moveTo: point 3 */
    /* two-point contours: on/off gives move, quad back to the start, close */
    gfxPathReset(&p);
    float two[] = {0, 0, 5, 5};
    uint8_t onOff[] = {1, 0};
    uint32_t e1[] = {1};
    hostileOutline(&o, two, onOff, 2, e1, 1);
    ASSERT_EQ(gfxGlyphOutlineToPath(&o, 0.0f, 0.0f, &p), STATUS_OK);
    ASSERT_EQ(p.nVerbs, 3u);
    ASSERT_EQ(p.verbs[1], (uint8_t)GFX_VERB_QUAD);
    ASSERT_TRUE(p.pts[2] == 5.0f && p.pts[4] == 0.0f);
    /* off/on: starts at the last point, quad through the first back to it */
    gfxPathReset(&p);
    uint8_t offOn[] = {0, 1};
    hostileOutline(&o, two, offOn, 2, e1, 1);
    ASSERT_EQ(gfxGlyphOutlineToPath(&o, 0.0f, 0.0f, &p), STATUS_OK);
    ASSERT_EQ(p.nVerbs, 3u);
    ASSERT_TRUE(p.pts[0] == 5.0f && p.pts[2] == 0.0f && p.pts[4] == 5.0f);
    /* onCurve values other than 0/1 count as on-curve */
    gfxPathReset(&p);
    uint8_t onJunk[] = {0x80, 0xFF, 2};
    hostileOutline(&o, fine, onJunk, 3, e2, 1);
    ASSERT_EQ(gfxGlyphOutlineToPath(&o, 0.0f, 0.0f, &p), STATUS_OK);
    ASSERT_EQ(p.nVerbs, 4u);
    gfxPathFree(&p);
}

/* ---- rendering
 * ----------------------------------------------------------------------------------- */

/* The glyph's control box as the renderer computes it (same transform, same float math). */
static int renderBox(const GfxFont *f, uint16_t gid, uint32_t sizeQ6, float *minX, float *maxX,
                     float *minY, float *maxY) {
    const float scale = (float)sizeQ6 / ((float)f->unitsPerEm * 64.0f);
    const GfxFontXform xf = {scale, 0.0f, 0.0f, -scale, 0.0f, 0.0f};
    GfxGlyphOutline o;
    gfxGlyphOutlineInit(&o, NULL);
    if (gfxFontGlyphOutline(f, gid, &xf, &o) != STATUS_OK || o.nPoints == 0) {
        gfxGlyphOutlineFree(&o);
        return 0;
    }
    *minX = *maxX = o.xy[0];
    *minY = *maxY = o.xy[1];
    for (uint32_t i = 1; i < o.nPoints; i++) {
        float x = o.xy[2 * i], y = o.xy[2 * i + 1];
        *minX = x < *minX ? x : *minX;
        *maxX = x > *maxX ? x : *maxX;
        *minY = y < *minY ? y : *minY;
        *maxY = y > *maxY ? y : *maxY;
    }
    gfxGlyphOutlineFree(&o);
    return 1;
}

TEST(renderAdvGlyphsBeyondTheFloatRange) {
    /* Composite offsets and scales put a glyph millions of pixels from its origin (upem 16 at
     * 512 px is 32 px per unit). The mask must still contain the whole glyph or be refused. */
    enum { LEAF, FAR0, FAR_END = FAR0 + 8, STR0, STR_END = STR0 + 5, NEGFAR, NG };
    static const Pt leaf[] = {{0, 0, 1}, {32767, 0, 1}, {0, 1, 1}};
    static const uint32_t leafEnd[] = {2};
    Buf g[NG];
    memset(g, 0, sizeof g);
    encSimple(&g[LEAF], leaf, 3, leafEnd, 1, 0, 0);
    /* FAR0: 8 levels of scale ~2 with offsets (32767, 32767): every point is past 2^24 px */
    for (int i = 0; i < 8; i++) {
        Comp c = {.gid = (uint16_t)(i < 7 ? FAR0 + i + 1 : LEAF),
                  .dx = 32767,
                  .dy = 32767,
                  .kind = XF_SCALE,
                  .m = {0x7FFF}};
        encComposite(&g[FAR0 + i], &c, 1);
    }
    /* STR0: 5 levels of scale ~2; only the deepest offset is nonzero, plus a small root offset:
     * the box starts a few hundred px below 2^24 px and ends ~3e7 px further right */
    for (int i = 0; i < 5; i++) {
        Comp c = {.gid = (uint16_t)(i < 4 ? STR0 + i + 1 : LEAF),
                  .dx = i == 4   ? 32767
                        : i == 0 ? 50
                                 : 0,
                  .dy = 0,
                  .words = 1,
                  .kind = XF_SCALE,
                  .m = {0x7FFF}};
        encComposite(&g[STR0 + i], &c, 1);
    }
    /* NEGFAR: the FAR chain from its second level, mirrored by -2: every point below -2^24 px */
    Comp nf = {.gid = FAR0 + 1, .dx = 0, .dy = 0, .kind = XF_SCALE, .m = {-0x8000}};
    encComposite(&g[NEGFAR], &nf, 1);
    size_t fn;
    uint8_t *font = glyphFont(g, NG, 16, (size_t)-1, &fn);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, font, fn), STATUS_OK);
    float x0, x1, y0, y1;
    const uint32_t size = GFX_FONT_MAX_SIZE_Q6;
    ASSERT_TRUE(renderBox(&f, FAR0, size, &x0, &x1, &y0, &y1));
    ASSERT_TRUE(x0 > 16777216.0f && y1 < -16777216.0f); /* the precondition of the case */
    ASSERT_TRUE(renderBox(&f, STR0, size, &x0, &x1, &y0, &y1));
    ASSERT_TRUE(x0 < 16777216.0f && x0 > 16777216.0f - 2000.0f && x1 > 16777216.0f + 1e6f);
    ASSERT_TRUE(renderBox(&f, NEGFAR, size, &x0, &x1, &y0, &y1));
    ASSERT_TRUE(x1 < -16777216.0f);
    static const int gids[] = {FAR0, STR0, NEGFAR};
    for (size_t i = 0; i < sizeof gids / sizeof gids[0]; i++) {
        Strict st;
        GfxAllocator a;
        strictInit(&st, &a, -1);
        GfxGlyphScratch sc;
        gfxGlyphScratchInit(&sc, &a);
        GfxGlyphImage img;
        Status s = gfxFontRenderGlyph(&f, (uint16_t)gids[i], size, 0, &sc, &a, &img);
        if (s == STATUS_OK) {
            fprintf(stderr, "  glyph %d: OK with a %dx%d mask at (%d, %d)\n", gids[i],
                    (int)img.mask.width, (int)img.mask.height, (int)img.left, (int)img.top);
        }
        ASSERT_EQ(s, STATUS_ERR_UNSUPPORTED);
        ASSERT_TRUE(img.mask.data == NULL);
        gfxGlyphImageFree(&img);
        gfxGlyphScratchFree(&sc);
        ASSERT_EQ(st.live, (size_t)0);
        ASSERT_EQ(st.bad, 0);
    }
    /* the same glyphs at 1 px are near enough (the leaf's 32767 units still make them too wide) */
    GfxGlyphScratch sc;
    gfxGlyphScratchInit(&sc, NULL);
    GfxGlyphImage img;
    ASSERT_EQ(gfxFontRenderGlyph(&f, LEAF, 64, 0, &sc, NULL, &img), STATUS_OK);
    ASSERT_EQ(img.left, 0);
    ASSERT_EQ(img.mask.width, 2048);
    gfxGlyphImageFree(&img);
    gfxGlyphScratchFree(&sc);
    free(font);
    for (int i = 0; i < NG; i++) {
        bufFree(&g[i]);
    }
}

TEST(renderAdvEachFarBoundIsRefusedAlone) {
    /* Four glyphs, each beyond +-2^24 px on exactly one side (and less than 2^25 px out, and only
     * ~1024 px across): up, down, right, left. Each clause of the renderer's range check must
     * refuse its glyph on its own; a clamped control box would give a bogus 1-px mask. Chain:
     * 5 levels of scale ~2 with 32767-unit offsets on the deepest two, upem 16 at 512 px. */
    enum { SMALL, UP0, RIGHT0 = UP0 + 5, DOWN = RIGHT0 + 5, LEFT, NG };
    static const Pt small[] = {{0, 0, 1}, {1, 0, 1}, {0, 1, 1}};
    static const uint32_t smallEnd[] = {2};
    Buf g[NG];
    memset(g, 0, sizeof g);
    encSimple(&g[SMALL], small, 3, smallEnd, 1, 0, 0);
    for (int axis = 0; axis < 2; axis++) {
        const int base = axis == 0 ? UP0 : RIGHT0;
        for (int i = 0; i < 5; i++) {
            const int32_t off = i >= 3 ? 32767 : 0;
            Comp c = {.gid = (uint16_t)(i < 4 ? base + i + 1 : SMALL),
                      .dx = axis == 1 ? off : 0,
                      .dy = axis == 0 ? off : 0,
                      .words = 1,
                      .kind = XF_SCALE,
                      .m = {0x7FFF}};
            encComposite(&g[base + i], &c, 1);
        }
    }
    Comp down = {.gid = UP0 + 1, .kind = XF_SCALE, .m = {-0x8000}};
    encComposite(&g[DOWN], &down, 1);
    Comp left = {.gid = RIGHT0 + 1, .kind = XF_SCALE, .m = {-0x8000}};
    encComposite(&g[LEFT], &left, 1);
    size_t fn;
    uint8_t *font = glyphFont(g, NG, 16, (size_t)-1, &fn);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, font, fn), STATUS_OK);
    const uint32_t size = GFX_FONT_MAX_SIZE_Q6;
    const float lim = 16777216.0f, lim2 = 33554432.0f;
    static const int gids[] = {UP0, DOWN, RIGHT0, LEFT};
    for (int k = 0; k < 4; k++) {
        float x0, x1, y0, y1;
        ASSERT_TRUE(renderBox(&f, (uint16_t)gids[k], size, &x0, &x1, &y0, &y1));
        /* the precondition: out on one side only, by less than 2^25 px, small across */
        const int out = (x0 < -lim) + (x1 > lim) * 2 + (y0 < -lim) * 4 + (y1 > lim) * 8;
        static const int wantOut[] = {4, 8, 2, 1};
        ASSERT_EQ(out, wantOut[k]);
        ASSERT_TRUE(x0 > -lim2 && x1 < lim2 && y0 > -lim2 && y1 < lim2);
        ASSERT_TRUE(x1 - x0 < 2048.0f && y1 - y0 < 2048.0f);
        Strict st;
        GfxAllocator a;
        strictInit(&st, &a, -1);
        GfxGlyphScratch sc;
        gfxGlyphScratchInit(&sc, &a);
        GfxGlyphImage img;
        Status s = gfxFontRenderGlyph(&f, (uint16_t)gids[k], size, 0, &sc, &a, &img);
        if (s != STATUS_ERR_UNSUPPORTED) {
            fprintf(stderr, "  case %d: status %d, %dx%d mask at (%d, %d)\n", k, (int)s,
                    (int)img.mask.width, (int)img.mask.height, (int)img.left, (int)img.top);
        }
        ASSERT_EQ(s, STATUS_ERR_UNSUPPORTED);
        ASSERT_TRUE(img.mask.data == NULL);
        gfxGlyphImageFree(&img);
        gfxGlyphScratchFree(&sc);
        ASSERT_EQ(st.live, (size_t)0);
        ASSERT_EQ(st.bad, 0);
    }
    free(font);
    for (int i = 0; i < NG; i++) {
        bufFree(&g[i]);
    }
}

TEST(glyfAdvFreeLeavesAReusableOutline) {
    size_t n;
    const uint8_t *d = ftuFont(FTU_SANS, &n);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    const uint16_t gid = gfxFontGlyphIndex(&f, 0xE9); /* e acute: a composite */
    Strict st;
    GfxAllocator a;
    strictInit(&st, &a, -1);
    GfxGlyphOutline o;
    gfxGlyphOutlineInit(&o, &a);
    ASSERT_EQ(gfxFontGlyphOutline(&f, gid, NULL, &o), STATUS_OK);
    const uint32_t np = o.nPoints, nc = o.nContours;
    ASSERT_TRUE(np > 8 && nc >= 2);
    float *xy = malloc(sizeof(float) * 2 * np);
    memcpy(xy, o.xy, sizeof(float) * 2 * np);
    gfxGlyphOutlineFree(&o);
    ASSERT_TRUE(o.xy == NULL && o.onCurve == NULL && o.contourEnd == NULL);
    ASSERT_TRUE(o.nPoints == 0 && o.nContours == 0 && o.capPoints == 0 && o.capContours == 0);
    ASSERT_TRUE(o.alloc == &a);
    ASSERT_EQ(st.live, (size_t)0);
    /* reused after the free: the same glyph again, through the same allocator */
    ASSERT_EQ(gfxFontGlyphOutline(&f, gid, NULL, &o), STATUS_OK);
    ASSERT_EQ(o.nPoints, np);
    ASSERT_EQ(o.nContours, nc);
    ASSERT_TRUE(memcmp(xy, o.xy, sizeof(float) * 2 * np) == 0);
    gfxGlyphOutlineFree(&o);
    gfxGlyphOutlineFree(NULL);
    ASSERT_EQ(st.live, (size_t)0);
    ASSERT_EQ(st.bad, 0);
    free(xy);
}

TEST(renderAdvScratchIsReusableAfterFree) {
    size_t n;
    const uint8_t *d = ftuFont(FTU_SANS, &n);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, d, n), STATUS_OK);
    const uint16_t gid = gfxFontGlyphIndex(&f, 'g');
    Strict st;
    GfxAllocator a;
    strictInit(&st, &a, -1);
    GfxGlyphScratch sc;
    gfxGlyphScratchInit(&sc, &a);
    GfxGlyphImage img[2];
    for (int k = 0; k < 2; k++) {
        ASSERT_EQ(gfxFontRenderGlyph(&f, gid, 30 * 64, 1, &sc, &a, &img[k]), STATUS_OK);
        gfxGlyphScratchFree(&sc); /* between the renders: the second one starts from nothing */
        gfxGlyphScratchFree(&sc);
    }
    ASSERT_EQ(img[0].mask.width, img[1].mask.width);
    ASSERT_EQ(img[0].mask.height, img[1].mask.height);
    ASSERT_TRUE(img[0].left == img[1].left && img[0].top == img[1].top);
    ASSERT_TRUE(memcmp(img[0].mask.data, img[1].mask.data, img[0].allocSize) == 0);
    gfxGlyphImageFree(&img[0]);
    gfxGlyphImageFree(&img[1]);
    gfxGlyphImageFree(&img[1]); /* idempotent */
    gfxGlyphScratchFree(NULL);
    gfxGlyphImageFree(NULL);
    ASSERT_EQ(st.live, (size_t)0);
    ASSERT_EQ(st.bad, 0);
}

TEST(glyfAdvOutlineToPathAllOffStartsAtTheMidpoint) {
    /* all off-curve, with first and last differing in both x and y */
    float xy[] = {0, 0, 10, 2, 4, 8};
    uint8_t off[] = {0, 0, 0};
    uint32_t ends[] = {2};
    GfxGlyphOutline o;
    hostileOutline(&o, xy, off, 3, ends, 1);
    GfxPath p;
    gfxPathInit(&p, NULL);
    ASSERT_EQ(gfxGlyphOutlineToPath(&o, 1.0f, -1.0f, &p), STATUS_OK);
    ASSERT_EQ(p.nVerbs, 5u); /* move, 3 quads, close */
    static const float want[] = {
        3,  3,        /* move: mid(last (4,8), first (0,0)) + (1,-1) */
        1,  -1, 6, 0, /* quad: control first, to mid(first, second) */
        11, 1,  8, 4, /* quad: control second, to mid(second, third) */
        5,  7,  3, 3, /* quad: control third (the last), back to the start */
    };
    for (int i = 0; i < 14; i++) {
        if (p.pts[i] != want[i]) {
            fprintf(stderr, "  pts[%d] = %g, want %g\n", i, (double)p.pts[i], (double)want[i]);
        }
        ASSERT_TRUE(p.pts[i] == want[i]);
    }
    gfxPathFree(&p);
}

TEST(renderAdvDegenerateGlyphs) {
    enum { DOT, LINE, SAME, NG };
    Buf g[NG];
    memset(g, 0, sizeof g);
    static const Pt dot[] = {{500, 500, 1}};
    static const uint32_t e0[] = {0};
    encSimple(&g[DOT], dot, 1, e0, 1, 0, 0);
    static const Pt line[] = {{0, 0, 1}, {1000, 0, 1}};
    static const uint32_t e1[] = {1};
    encSimple(&g[LINE], line, 2, e1, 1, 0, 0);
    static const Pt same[] = {{250, 250, 1}, {250, 250, 0}, {250, 250, 1}};
    static const uint32_t e2[] = {2};
    encSimple(&g[SAME], same, 3, e2, 1, 0, 0);
    size_t fn;
    uint8_t *font = glyphFont(g, NG, 1000, (size_t)-1, &fn);
    GfxFont f;
    ASSERT_EQ(gfxFontInit(&f, font, fn), STATUS_OK);
    GfxGlyphScratch sc;
    gfxGlyphScratchInit(&sc, NULL);
    static const struct {
        int gid;
        int32_t w, h, left, top;
    } want[] = {{DOT, 1, 1, 10, -10}, {LINE, 20, 1, 0, 0}, {SAME, 1, 1, 5, -5}};
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        GfxGlyphImage img;
        ASSERT_EQ(gfxFontRenderGlyph(&f, (uint16_t)want[i].gid, 20 * 64, 0, &sc, NULL, &img),
                  STATUS_OK);
        ASSERT_EQ(img.mask.width, want[i].w);
        ASSERT_EQ(img.mask.height, want[i].h);
        ASSERT_EQ(img.left, want[i].left);
        ASSERT_EQ(img.top, want[i].top);
        for (int32_t k = 0; k < img.mask.width * img.mask.height; k++) {
            ASSERT_EQ((int)img.mask.data[k], 0); /* no area, no coverage */
        }
        gfxGlyphImageFree(&img);
    }
    gfxGlyphScratchFree(&sc);
    free(font);
    for (int i = 0; i < NG; i++) {
        bufFree(&g[i]);
    }
}

/* ---- seeded mutation fuzz: load, path and render mutated fonts under allocation failure ------ */

#ifndef FONT_FUZZ_SCALE
#define FONT_FUZZ_SCALE 1u
#endif

static int statusAllowed(Status s) {
    return s == STATUS_OK || s == STATUS_ERR_INVALID || s == STATUS_ERR_UNSUPPORTED ||
           s == STATUS_ERR_NO_MEMORY;
}

/* Renders `gid` at a few sizes and bins with allocation failure injected; checks the image. */
static void fuzzRender(const GfxFont *f, uint16_t gid, int *rendered) {
    static const uint32_t sizes[] = {64, 11 * 64 + 7, 40 * 64, GFX_FONT_MAX_SIZE_Q6};
    for (int k = 0; k < 3; k++) {
        uint32_t size = sizes[rng() % 4], bin = rng() % 4;
        int failAt = rng() % 3 == 0 ? (int)(rng() % 12) : -1;
        Strict st;
        GfxAllocator a;
        strictInit(&st, &a, failAt);
        GfxGlyphScratch sc;
        gfxGlyphScratchInit(&sc, &a);
        GfxGlyphImage img;
        Status s = gfxFontRenderGlyph(f, gid, size, bin, &sc, &a, &img);
        if (!statusAllowed(s)) {
            fprintf(stderr, "  render status %d\n", (int)s);
        }
        ASSERT_TRUE(statusAllowed(s));
        if (s == STATUS_OK && img.mask.data != NULL) {
            (*rendered)++;
            ASSERT_TRUE(img.mask.width >= 1 && img.mask.width <= GFX_FONT_MAX_GLYPH_DIM);
            ASSERT_TRUE(img.mask.height >= 1 && img.mask.height <= GFX_FONT_MAX_GLYPH_DIM);
            ASSERT_EQ(img.allocSize, (size_t)img.mask.width * (size_t)img.mask.height);
            volatile uint32_t sum = 0;
            for (size_t i = 0; i < img.allocSize; i++) {
                sum += img.mask.data[i];
            }
        } else {
            ASSERT_TRUE(img.mask.data == NULL);
        }
        /* the outline in the scratch (if any) converts safely too */
        if (s == STATUS_OK) {
            GfxPath p;
            gfxPathInit(&p, &a);
            Status ps = gfxGlyphOutlineToPath(&sc.outline, 0.5f, -0.5f, &p);
            ASSERT_TRUE(ps == STATUS_OK || ps == STATUS_ERR_NO_MEMORY);
            gfxPathFree(&p);
        }
        gfxGlyphImageFree(&img);
        gfxGlyphScratchFree(&sc);
        ASSERT_EQ(st.live, (size_t)0);
        ASSERT_EQ(st.bad, 0);
    }
}

TEST(renderAdvMutationFuzz) {
    rngSeed(0x72656E64ull);
    static const struct {
        int font;
        uint32_t runs;
    } plan[] = {{FTU_SYNTH_FALLBACK, 700}, {FTU_SYNTH_BAD, 700}, {FTU_SANS, 350}, {FTU_MONO, 250}};
    static const char *const tags[] = {"glyf", "glyf", "glyf", "loca", "hmtx", "head"};
    int accepted = 0, rendered = 0, loaded = 0;
    const int failuresBefore = hostTestFailures;
    for (size_t p = 0; p < sizeof plan / sizeof plan[0]; p++) {
        size_t n;
        const uint8_t *src = ftuFont(plan[p].font, &n);
        ASSERT_TRUE(src != NULL);
        uint8_t *d = malloc(n);
        for (uint32_t run = 0; run < plan[p].runs * FONT_FUZZ_SCALE; run++) {
            memcpy(d, src, n);
            const char *tag = tags[rng() % 6];
            uint32_t len, off = ftuTable(d, n, tag, &len);
            uint32_t lo = off, span = len;
            if (strcmp(tag, "head") == 0) { /* upem, the bbox, indexToLocFormat */
                static const uint32_t fields[] = {18, 19, 36, 38, 40, 42, 50, 51};
                lo = off + fields[rng() % 8];
                span = 1;
            }
            GfxFont pre;
            ASSERT_EQ(gfxFontInit(&pre, src, n), STATUS_OK);
            uint16_t target = (uint16_t)(rng() % pre.numGlyphs);
            if (strcmp(tag, "glyf") == 0 && rng() % 4 != 0) {
                /* aim at one glyph's bytes */
                uint32_t s, e;
                if (fontGlyphRange(&pre, target, &s, &e) == STATUS_OK && e > s) {
                    lo = off + s;
                    span = e - s;
                }
            }
            if (span == 0) {
                continue;
            }
            int edits = 1 + (int)(rng() % 4);
            for (int k = 0; k < edits; k++) {
                static const uint8_t special[] = {0x00, 0xFF, 0x7F, 0x80, 0x01, 0x08, 0x3F};
                uint32_t at = lo + rng() % span;
                d[at] = rng() % 3 == 0 ? special[rng() % sizeof special] : (uint8_t)rng();
            }
            GfxFont f;
            Status st = gfxFontInit(&f, d, n);
            ASSERT_TRUE(st == STATUS_OK || st == STATUS_ERR_INVALID ||
                        st == STATUS_ERR_UNSUPPORTED);
            if (st != STATUS_OK) {
                continue;
            }
            accepted++;
            GfxGlyphOutline o;
            gfxGlyphOutlineInit(&o, NULL);
            Status ls = gfxFontGlyphOutline(&f, target, NULL, &o);
            ASSERT_TRUE(statusAllowed(ls) && ls != STATUS_ERR_NO_MEMORY);
            if (ls == STATUS_OK) {
                loaded++;
                ASSERT_TRUE(o.nPoints <= GFX_FONT_MAX_POINTS);
                ASSERT_TRUE(o.nContours <= GFX_FONT_MAX_CONTOURS);
                for (uint32_t c = 0; c < o.nContours; c++) {
                    ASSERT_TRUE(o.contourEnd[c] < o.nPoints);
                    ASSERT_TRUE(c == 0 || o.contourEnd[c] > o.contourEnd[c - 1]);
                }
                for (uint32_t i = 0; i < o.nPoints; i++) {
                    ASSERT_TRUE(o.onCurve[i] <= 1u);
                    ASSERT_TRUE(__builtin_isfinite(o.xy[2 * i]) &&
                                __builtin_isfinite(o.xy[2 * i + 1]));
                }
            }
            gfxGlyphOutlineFree(&o);
            fuzzRender(&f, target, &rendered);
            if (hostTestFailures != failuresBefore) {
                fprintf(stderr, "  fuzz font %d run %u tag %s glyph %u\n", plan[p].font, run, tag,
                        target);
                free(d);
                return;
            }
        }
        free(d);
    }
    ASSERT_TRUE(accepted > 1000);
    ASSERT_TRUE(loaded > 600);
    ASSERT_TRUE(rendered > 600);
}
