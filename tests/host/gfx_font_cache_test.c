/* Host tests for libs/gfx's fallback stack and glyph cache (M12.3, D-155). The cache is checked
 * against three independent things: a direct render (an evicting cache must give the same pixels as
 * no cache), a move-to-front model of the LRU list (hit/miss, entries, bytes, evictions and the
 * full list order), and a structural invariant checker that walks the hash chains and free list. */
#include "font_testutil.h"
#include "framework/test.h"
#include "gfx/font-internal.h"
#include "gfx/gfx-font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHK(c)                                                                                     \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                         \
            return 0;                                                                              \
        }                                                                                          \
    } while (0)

static const GfxFont *fontOf(int which) {
    static GfxFont fonts[FTU_COUNT];
    static int state[FTU_COUNT];
    if (state[which] == 0) {
        size_t n = 0;
        const uint8_t *d = ftuFont(which, &n);
        state[which] = d != NULL && gfxFontInit(&fonts[which], d, n) == STATUS_OK ? 1 : 2;
    }
    return state[which] == 1 ? &fonts[which] : NULL;
}

/* The stack the tests mostly use: Sans, Mono, then the synthetic CJK/PUA font. */
static int stdFaces(const GfxFont *out[3]) {
    out[0] = fontOf(FTU_SANS);
    out[1] = fontOf(FTU_MONO);
    out[2] = fontOf(FTU_SYNTH_FALLBACK);
    return out[0] != NULL && out[1] != NULL && out[2] != NULL;
}

static uint64_t rngState;
static uint32_t rnd(void) {
    uint64_t x = rngState;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rngState = x;
    return (uint32_t)(x >> 16);
}

/* ---- structural invariants ---------------------------------------------------------------- */

static int checkInvariants(const GfxFontStack *s) {
    CHK(s->entries != NULL && s->buckets != NULL);
    uint32_t n = 0;
    size_t bytes = 0;
    int32_t prev = -1;
    for (int32_t i = s->lruHead; i != -1; i = s->entries[i].lruNext) {
        CHK(i >= 0 && (uint32_t)i < s->capacity);
        CHK(n < s->capacity);
        const GfxGlyphCacheEntry *e = &s->entries[i];
        CHK(e->lruPrev == prev);
        CHK(e->cost >= GFX_GLYPH_CACHE_ENTRY_COST);
        CHK(e->cost == (uint32_t)e->img.mask.width * (uint32_t)e->img.mask.height + 64u);
        bytes += e->cost;
        /* in exactly its own chain */
        const uint32_t b = fontCacheHash(e->key) & s->bucketMask;
        int found = 0;
        uint32_t guard = 0;
        for (int32_t j = s->buckets[b]; j != -1; j = s->entries[j].hashNext) {
            CHK(++guard <= s->capacity);
            found += j == i;
            if (j != i) {
                CHK(s->entries[j].key != e->key);
            }
        }
        CHK(found == 1);
        prev = i;
        n++;
    }
    CHK(s->lruTail == prev);
    CHK(n == s->stats.entries);
    CHK(n <= s->capacity);
    CHK(bytes == s->stats.bytes);
    CHK(bytes <= s->budget);
    /* every chain entry is on the LRU list: count chain members overall */
    uint32_t chained = 0;
    for (uint32_t b = 0; b <= s->bucketMask; b++) {
        uint32_t guard = 0;
        for (int32_t j = s->buckets[b]; j != -1; j = s->entries[j].hashNext) {
            CHK(++guard <= s->capacity);
            CHK((fontCacheHash(s->entries[j].key) & s->bucketMask) == b);
            chained++;
        }
    }
    CHK(chained == n);
    /* free list */
    uint32_t nfree = 0;
    for (int32_t i = s->freeHead; i != -1; i = s->entries[i].lruNext) {
        CHK(i >= 0 && (uint32_t)i < s->capacity);
        CHK(++nfree <= s->capacity);
        CHK(s->entries[i].img.mask.data == NULL && s->entries[i].key == 0);
    }
    CHK(nfree == s->capacity - n);
    return 1;
}

/* ---- direct-render comparison ------------------------------------------------------------- */

static int sameImage(const GfxGlyphImage *a, const GfxGlyphImage *b) {
    CHK(a->mask.width == b->mask.width && a->mask.height == b->mask.height);
    CHK(a->left == b->left && a->top == b->top);
    for (int32_t y = 0; y < a->mask.height; y++) {
        CHK(memcmp(a->mask.data + (size_t)y * (size_t)a->mask.stride,
                   b->mask.data + (size_t)y * (size_t)b->mask.stride, (size_t)a->mask.width) == 0);
    }
    return 1;
}

/* The cached image must equal what a fresh render gives (or be empty when the render fails). */
static int matchesDirect(const GfxFontStack *s, uint32_t face, uint16_t glyph, uint32_t size,
                         uint32_t bin, const GfxGlyphImage *img) {
    GfxGlyphScratch sc;
    gfxGlyphScratchInit(&sc, NULL);
    GfxGlyphImage ref;
    const Status st = gfxFontRenderGlyph(s->faces[face], glyph, size, bin, &sc, NULL, &ref);
    int ok;
    if (st == STATUS_OK) {
        ok = sameImage(img, &ref);
        gfxGlyphImageFree(&ref);
    } else {
        ok = img->mask.data == NULL && img->mask.width == 0 && img->mask.height == 0;
    }
    gfxGlyphScratchFree(&sc);
    return ok;
}

/* ---- the LRU model ------------------------------------------------------------------------ */

typedef struct {
    uint32_t face, size, bin;
    uint16_t glyph;
} Op;

typedef struct {
    uint64_t key;
    uint32_t cost;
} ModelEnt;

typedef struct {
    ModelEnt e[4200];
    uint32_t n;
    size_t bytes;
    uint64_t hits, misses, evictions;
} Model;

static int modelStep(Model *m, const GfxFontStack *s, uint64_t key, uint32_t cost) {
    for (uint32_t i = 0; i < m->n; i++) {
        if (m->e[i].key == key) {
            const ModelEnt t = m->e[i];
            memmove(&m->e[1], &m->e[0], i * sizeof t);
            m->e[0] = t;
            m->hits++;
            return 1;
        }
    }
    m->misses++;
    if (cost > s->budget / 8) {
        return 1; /* bypasses the cache */
    }
    while (m->n > 0 && (m->n >= s->capacity || m->bytes + cost > s->budget)) {
        m->bytes -= m->e[m->n - 1].cost;
        m->n--;
        m->evictions++;
    }
    CHK(m->n < sizeof m->e / sizeof m->e[0]);
    memmove(&m->e[1], &m->e[0], m->n * sizeof m->e[0]);
    m->e[0].key = key;
    m->e[0].cost = cost;
    m->n++;
    m->bytes += cost;
    return 1;
}

static int modelMatches(const Model *m, const GfxFontStack *s) {
    CHK(s->stats.hits == m->hits && s->stats.misses == m->misses);
    CHK(s->stats.evictions == m->evictions);
    CHK(s->stats.entries == m->n && s->stats.bytes == m->bytes);
    uint32_t k = 0;
    for (int32_t i = s->lruHead; i != -1; i = s->entries[i].lruNext) {
        CHK(k < m->n && s->entries[i].key == m->e[k].key);
        k++;
    }
    CHK(k == m->n);
    return 1;
}

/* Runs `ops` against `s` with the model `m` (which carries over between calls) and (optionally)
 * the direct-render comparison. */
static int runOpsWith(Model *m, GfxFontStack *s, const Op *ops, uint32_t nOps, int checkPixels,
                      int everyStep) {
    int ok = 1;
    for (uint32_t i = 0; ok && i < nOps; i++) {
        const Op *o = &ops[i];
        const GfxGlyphImage *img = NULL;
        ok = gfxFontStackGlyph(s, o->face, o->glyph, o->size, o->bin, &img) == STATUS_OK &&
             img != NULL;
        if (!ok) {
            fprintf(stderr, "  FAIL op %u: Glyph failed\n", i);
            break;
        }
        const uint32_t cost = (uint32_t)img->mask.width * (uint32_t)img->mask.height + 64u;
        ok = modelStep(m, s, fontCacheKey(o->face, o->glyph, o->size, o->bin), cost);
        if (ok && checkPixels) {
            ok = matchesDirect(s, o->face, o->glyph, o->size, o->bin, img);
        }
        if (ok && (everyStep || i + 1 == nOps)) {
            ok = modelMatches(m, s) && checkInvariants(s);
        }
    }
    return ok;
}

/* runOpsWith on a fresh model. */
static int runOps(GfxFontStack *s, const Op *ops, uint32_t nOps, int checkPixels, int everyStep) {
    Model *m = calloc(1, sizeof *m);
    CHK(m != NULL);
    const int ok = runOpsWith(m, s, ops, nOps, checkPixels, everyStep);
    free(m);
    return ok;
}

/* ---- tests -------------------------------------------------------------------------------- */

static uint32_t testCps[] = {' ',    'A',    'W',    'g',    'l',      'i',    '@',    'e',
                             'a',    'q',    '0',    '%',    '~',      '.',    0x4E00, 0x4E05,
                             0x4E0F, 0x3002, 0xE000, 0xE002, 0x110000, 0xFFFF0};

static void makeRandomOps(Op *ops, uint32_t n, const GfxFontStack *s) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t face;
        uint16_t glyph;
        const uint32_t r = rnd();
        if (r % 10 == 0) { /* a glyph index past the end of a font: a negative entry */
            face = rnd() % s->nFaces;
            glyph = (uint16_t)(60000 + rnd() % 40);
        } else {
            gfxFontStackPick(s, testCps[rnd() % (sizeof testCps / sizeof testCps[0])], &face,
                             &glyph);
        }
        ops[i].face = face;
        ops[i].glyph = glyph;
        /* mostly a few popular sizes (so there are hits), sometimes any size (so there is churn) */
        ops[i].size =
            rnd() % 10 < 7 ? (12 + rnd() % 3 * 6) * 64 : (8 + rnd() % 41) * 64 + rnd() % 64;
        ops[i].bin = rnd() % 4;
    }
}

TEST(cacheDifferential) {
    const GfxFont *faces[3];
    ASSERT_TRUE(stdFaces(faces));
    enum { N = 5000 };
    static Op ops[N];
    rngState = 0x9E3779B97F4A7C15ull;
    GfxFontStack s;
    ASSERT_EQ(gfxFontStackInit(&s, faces, 3, 64u << 10, NULL), STATUS_OK);
    makeRandomOps(ops, N, &s);
    ASSERT_TRUE(runOps(&s, ops, N, 1, 0));
    ASSERT_TRUE(s.stats.evictions > 100); /* the small cache really did evict */
    ASSERT_TRUE(s.stats.hits > 100 && s.stats.bad > 0);
    gfxFontStackDestroy(&s);

    GfxFontStack big;
    ASSERT_EQ(gfxFontStackInit(&big, faces, 3, 8u << 20, NULL), STATUS_OK);
    ASSERT_TRUE(runOps(&big, ops, N, 1, 0));
    ASSERT_EQ(big.stats.evictions, 0u);
    ASSERT_TRUE(big.stats.hits > s.stats.hits); /* ...and the big one evicted nothing */
    gfxFontStackDestroy(&big);
}

TEST(cacheLruOracle) {
    const GfxFont *faces[3];
    ASSERT_TRUE(stdFaces(faces));
    rngState = 0x1234567887654321ull;
    GfxFontStack s;
    ASSERT_EQ(gfxFontStackInit(&s, faces, 3, 64u << 10, NULL), STATUS_OK);
    /* a universe of 400 keys, walked at random with a locality bias so both hits and evictions
     * are common */
    enum { U = 400, N = 6000 };
    static Op uni[U], ops[N];
    makeRandomOps(uni, U, &s);
    for (uint32_t i = 0; i < U; i++) {
        uni[i].size = (24 + i % 24) * 64 + i / 24;
    }
    for (uint32_t i = 0; i < N; i++) {
        const uint32_t r = rnd();
        ops[i] = uni[r % 4 == 0 ? r / 4 % U : r / 4 % 40];
    }
    ASSERT_TRUE(runOps(&s, ops, N, 0, 1));
    ASSERT_TRUE(s.stats.evictions > 50 && s.stats.hits > 500);
    gfxFontStackDestroy(&s);
}

TEST(cacheHashPinned) {
    ASSERT_EQ(fontCacheKey(0, 1, 64, 0), 0x400001ull);
    ASSERT_EQ(fontCacheKey(3, 0xFFFF, 32768, 3), 0xf8000ffffull);
    ASSERT_EQ(fontCacheKey(1, 65, 768, 2), 0x603000041ull);
    ASSERT_EQ(fontCacheHash(0x400001ull), 0x09e88d8au);
    ASSERT_EQ(fontCacheHash(0xf8000ffffull), 0xd11c60bbu);
    ASSERT_EQ(fontCacheHash(0x603000041ull), 0xe2ab8b1fu);
}

TEST(cacheCollisions) {
    const GfxFont *faces[3];
    ASSERT_TRUE(stdFaces(faces));
    GfxFontStack s;
    ASSERT_EQ(gfxFontStackInit(&s, faces, 3, 64u << 10, NULL), STATUS_OK);
    const uint32_t mask = s.bucketMask;
    /* real keys (Sans 'A' at many sizes and bins) that share the first key's bucket, plus
     * negative keys (glyph past the end) in the same bucket */
    const uint16_t gA = gfxFontGlyphIndex(faces[0], 'A');
    ASSERT_TRUE(gA != 0);
    enum { MAXC = 200 };
    static Op col[MAXC];
    uint32_t nc = 0;
    const uint32_t target = fontCacheHash(fontCacheKey(0, gA, 12 * 64, 0)) & mask;
    uint32_t nReal = 0, nNeg = 0;
    for (uint32_t size = 8 * 64; size <= 48 * 64 && nc < MAXC; size++) {
        for (uint32_t bin = 0; bin < 4 && nc < MAXC; bin++) {
            if ((fontCacheHash(fontCacheKey(0, gA, size, bin)) & mask) == target) {
                col[nc++] = (Op){0, size, bin, gA};
                nReal++;
            }
        }
    }
    for (uint32_t g = 5000; g < 65000 && nNeg < 40; g++) {
        if ((fontCacheHash(fontCacheKey(0, (uint16_t)g, 64, 0)) & mask) == target) {
            col[nc++] = (Op){0, 64, 0, (uint16_t)g};
            nNeg++;
        }
    }
    ASSERT_TRUE(nReal >= 6 && nNeg >= 30);
    /* fillers in other buckets push the cache to its entry capacity and past it */
    enum { F = 1300, N = 9000 };
    static Op all[MAXC + F], ops[N];
    memcpy(all, col, nc * sizeof col[0]);
    uint32_t na = nc;
    for (uint32_t i = 0; i < F; i++) {
        all[na++] = (Op){1, 64 + i % 7 * 64, i % 4, (uint16_t)(3000 + i)};
    }
    rngState = 0xDEADBEEFCAFEF00Dull;
    for (uint32_t i = 0; i < N; i++) {
        const uint32_t r = rnd();
        ops[i] = r % 2 == 0 ? all[r / 2 % nc] : all[nc + r / 2 % F];
    }
    ASSERT_TRUE(runOps(&s, ops, N, 1, 0));
    ASSERT_TRUE(s.stats.evictions > 500);
    ASSERT_TRUE(checkInvariants(&s));
    gfxFontStackDestroy(&s);
}

/* Two keys that differ in exactly one field and share a bucket must still be two entries: the
 * lookup compares the whole key, not only the bits that pick the bucket. One pair per field. */
TEST(cacheKeyFieldsAllCompared) {
    const GfxFont *faces[3];
    ASSERT_TRUE(stdFaces(faces));
    GfxFontStack s;
    ASSERT_EQ(gfxFontStackInit(&s, faces, 3, 0, NULL), STATUS_OK);
    const uint32_t mask = s.bucketMask;
    enum { PAIRS = 3 }; /* per field */
    static Op ops[4 * PAIRS * 4];
    uint32_t nOps = 0;
    for (uint32_t field = 0; field < 4; field++) {
        uint32_t found = 0;
        /* glyphs 3..400 exist in both Liberations (Sans and Mono share indices, not outlines) */
        for (uint32_t g = 3; g < 400 && found < PAIRS; g++) {
            for (uint32_t size = 10 * 64; size < 40 * 64 && found < PAIRS; size += 7) {
                const uint32_t bin = g % 4;
                Op a = {0, size, bin, (uint16_t)g}, b = a;
                if (field == 0) {
                    b.face = 1;
                } else if (field == 1) {
                    b.bin = (bin + 1 + size % 3) % 4;
                } else if (field == 2) {
                    b.size = size + 1 + g % 13;
                } else {
                    b.glyph = (uint16_t)(g + 1 + size % 5);
                }
                if ((fontCacheHash(fontCacheKey(a.face, a.glyph, a.size, a.bin)) & mask) !=
                    (fontCacheHash(fontCacheKey(b.face, b.glyph, b.size, b.bin)) & mask)) {
                    continue;
                }
                /* a, b, a, b: two misses, then two hits on the right entries */
                ops[nOps++] = a;
                ops[nOps++] = b;
                ops[nOps++] = a;
                ops[nOps++] = b;
                found++;
            }
        }
        ASSERT_EQ(found, (uint32_t)PAIRS);
    }
    /* the model (whole-key compare) checks every hit and miss, and the pixels must match */
    ASSERT_TRUE(runOps(&s, ops, nOps, 1, 1));
    ASSERT_TRUE(s.stats.misses >= 4 * PAIRS && s.stats.hits >= 4 * PAIRS);
    gfxFontStackDestroy(&s);
}

/* A budget whose entry capacity is not a power of two (100000 / 64 = 1562 entries, 2048
 * buckets): every bucket index has to come from the mask, never from the capacity. Keys are
 * forced into buckets at and above the capacity, then churned past the entry limit. */
TEST(cacheNonPowerOfTwoCapacity) {
    const GfxFont *faces[3];
    ASSERT_TRUE(stdFaces(faces));
    GfxFontStack s;
    ASSERT_EQ(gfxFontStackInit(&s, faces, 3, 100000, NULL), STATUS_OK);
    ASSERT_EQ(s.capacity, 1562u);
    ASSERT_EQ(s.bucketMask, 2047u);
    enum { HIGH = 300, LOW = 4000, N = 12000 };
    static Op uni[HIGH + LOW], ops[N];
    uint32_t nHigh = 0, nLow = 0;
    /* negative keys (glyph past the end, cost 64) so the cache is entry-bound: 1562 * 64 <= 100000
     */
    for (uint32_t g = 5000; g < 65000 && (nHigh < HIGH || nLow < LOW); g++) {
        for (uint32_t face = 0; face < 3; face++) {
            const uint32_t b = fontCacheHash(fontCacheKey(face, (uint16_t)g, 64, 0)) & s.bucketMask;
            if (b >= s.capacity && nHigh < HIGH) {
                uni[nHigh++] = (Op){face, 64, 0, (uint16_t)g};
            } else if (b < s.capacity && nLow < LOW) {
                uni[HIGH + nLow++] = (Op){face, 64, 0, (uint16_t)g};
            }
        }
    }
    ASSERT_TRUE(nHigh == HIGH && nLow == LOW);
    rngState = 0x5EED5EED12345678ull;
    for (uint32_t i = 0; i < N; i++) {
        const uint32_t r = rnd();
        /* half the traffic to the high buckets so they are hit, evicted and reused */
        ops[i] = r % 2 == 0 ? uni[r / 2 % HIGH] : uni[HIGH + r / 2 % LOW];
    }
    ASSERT_TRUE(runOps(&s, ops, N, 0, 0));
    ASSERT_TRUE(s.stats.evictions > 1000 && s.stats.hits > 1000);
    ASSERT_EQ(s.stats.entries, 1562u);
    ASSERT_TRUE(checkInvariants(&s));
    /* real glyphs on top: the same budget, now byte-bound */
    static Op real[3000];
    rngState = 0xABCDEF0123456789ull;
    makeRandomOps(real, 3000, &s);
    Model *m = calloc(1, sizeof *m);
    ASSERT_TRUE(m != NULL);
    /* carry the cache's current state into a fresh model: replay the LRU order, oldest first */
    int32_t order[1562];
    uint32_t n = 0;
    for (int32_t j = s.lruHead; j != -1; j = s.entries[j].lruNext) {
        order[n++] = j;
    }
    for (uint32_t k = n; k-- > 0;) {
        memmove(&m->e[1], &m->e[0], m->n * sizeof m->e[0]);
        m->e[0].key = s.entries[order[k]].key;
        m->e[0].cost = s.entries[order[k]].cost;
        m->n++;
        m->bytes += s.entries[order[k]].cost;
    }
    m->hits = s.stats.hits;
    m->misses = s.stats.misses;
    m->evictions = s.stats.evictions;
    ASSERT_TRUE(runOpsWith(m, &s, real, 3000, 1, 0));
    free(m);
    gfxFontStackDestroy(&s);
}

TEST(cacheBudgetExact) {
    const GfxFont *faces[3];
    ASSERT_TRUE(stdFaces(faces));
    GfxFontStack s;
    ASSERT_EQ(gfxFontStackInit(&s, faces, 3, 1, NULL), STATUS_OK);
    ASSERT_EQ(s.budget, (size_t)GFX_GLYPH_CACHE_MIN_BYTES);
    ASSERT_EQ(s.capacity, 1024u);
    const GfxGlyphImage *img;
    /* 1024 negative entries (cost 64 each) fill 64 KiB exactly */
    for (uint32_t i = 0; i < 1024; i++) {
        ASSERT_EQ(gfxFontStackGlyph(&s, 2, (uint16_t)(100 + i), 64, 0, &img), STATUS_OK);
        ASSERT_TRUE(img->mask.data == NULL);
    }
    ASSERT_EQ(s.stats.evictions, 0u);
    ASSERT_EQ(s.stats.bytes, (size_t)65536);
    ASSERT_EQ(s.stats.entries, 1024u);
    ASSERT_EQ(s.stats.bad, 1024u);
    ASSERT_EQ(gfxFontStackGlyph(&s, 2, 5000, 64, 0, &img), STATUS_OK);
    ASSERT_EQ(s.stats.evictions, 1u);
    ASSERT_EQ(s.stats.entries, 1024u);
    ASSERT_TRUE(checkInvariants(&s));
    /* the oldest one (glyph 100) is gone, the second oldest is still a hit */
    const uint64_t hits = s.stats.hits;
    ASSERT_EQ(gfxFontStackGlyph(&s, 2, 101, 64, 0, &img), STATUS_OK);
    ASSERT_EQ(s.stats.hits, hits + 1);
    ASSERT_EQ(gfxFontStackGlyph(&s, 2, 100, 64, 0, &img), STATUS_OK);
    ASSERT_EQ(s.stats.hits, hits + 1);
    gfxFontStackDestroy(&s);

    /* the default 2 MiB budget is entry-bound at 4096 */
    ASSERT_EQ(gfxFontStackInit(&s, faces, 3, 0, NULL), STATUS_OK);
    ASSERT_EQ(s.budget, (size_t)GFX_GLYPH_CACHE_DEFAULT_BYTES);
    ASSERT_EQ(s.capacity, 4096u);
    for (uint32_t i = 0; i < 5000; i++) {
        ASSERT_EQ(gfxFontStackGlyph(&s, 2, (uint16_t)(100 + i % 1000), 64 + i / 1000 * 64, 0, &img),
                  STATUS_OK);
    }
    ASSERT_EQ(s.stats.entries, 4096u);
    ASSERT_EQ(s.stats.evictions, 904u);
    ASSERT_EQ(s.stats.bytes, (size_t)4096 * 64);
    ASSERT_TRUE(checkInvariants(&s));
    gfxFontStackDestroy(&s);
}

TEST(cacheTempBoundary) {
    const GfxFont *faces[3];
    ASSERT_TRUE(stdFaces(faces));
    const uint16_t gW = gfxFontGlyphIndex(faces[0], 'W');
    const uint32_t size = 200 * 64;
    GfxGlyphScratch sc;
    gfxGlyphScratchInit(&sc, NULL);
    GfxGlyphImage ref;
    ASSERT_EQ(gfxFontRenderGlyph(faces[0], gW, size, 0, &sc, NULL, &ref), STATUS_OK);
    const size_t c = (size_t)ref.mask.width * (size_t)ref.mask.height + 64;
    ASSERT_TRUE(c >= 8192 && c * 8 <= GFX_GLYPH_CACHE_MAX_BYTES);

    FtuAlloc fa;
    GfxAllocator al;
    GfxFontStack s;
    const GfxGlyphImage *img;
    /* cost == budget/8: cached */
    ftuAllocInit(&fa, &al, -1);
    ASSERT_EQ(gfxFontStackInit(&s, faces, 3, c * 8, &al), STATUS_OK);
    ASSERT_EQ(gfxFontStackGlyph(&s, 0, gW, size, 0, &img), STATUS_OK);
    ASSERT_TRUE(sameImage(img, &ref));
    ASSERT_EQ(s.stats.entries, 1u);
    ASSERT_EQ(s.stats.bytes, c);
    gfxFontStackDestroy(&s);
    ASSERT_EQ(fa.live, 0);

    /* one byte less budget: cost > budget/8, so it is served from the temp slot */
    ftuAllocInit(&fa, &al, -1);
    ASSERT_EQ(gfxFontStackInit(&s, faces, 3, c * 8 - 1, &al), STATUS_OK);
    ASSERT_EQ(gfxFontStackGlyph(&s, 0, gW, size, 0, &img), STATUS_OK);
    ASSERT_TRUE(sameImage(img, &ref));
    ASSERT_TRUE(img == &s.temp);
    ASSERT_EQ(s.stats.entries, 0u);
    ASSERT_EQ(s.stats.bytes, (size_t)0);
    ASSERT_EQ(s.stats.misses, 1u);
    const int withTemp = fa.live;
    /* the temp mask is freed by the next Glyph call, even one that fails */
    ASSERT_EQ(gfxFontStackGlyph(&s, 0, gW, size, 9, &img), STATUS_ERR_INVALID);
    ASSERT_TRUE(img == NULL);
    ASSERT_EQ(fa.live, withTemp - 1);
    ASSERT_TRUE(s.temp.mask.data == NULL);
    /* the same glyph is never cached: a second request is another miss */
    ASSERT_EQ(gfxFontStackGlyph(&s, 0, gW, size, 0, &img), STATUS_OK);
    ASSERT_EQ(s.stats.misses, 2u);
    ASSERT_EQ(fa.live, withTemp);
    ASSERT_TRUE(checkInvariants(&s));
    gfxFontStackDestroy(&s); /* frees the temp slot too */
    ASSERT_EQ(fa.live, 0);
    ASSERT_EQ(fa.liveBytes, (size_t)0);

    gfxGlyphImageFree(&ref);
    gfxGlyphScratchFree(&sc);
}

TEST(cacheNegative) {
    const GfxFont *faces[4];
    ASSERT_TRUE(stdFaces(faces));
    faces[3] = fontOf(FTU_SYNTH_BAD);
    ASSERT_TRUE(faces[3] != NULL);
    GfxFontStack s;
    ASSERT_EQ(gfxFontStackInit(&s, faces, 4, 0, NULL), STATUS_OK);
    const GfxGlyphImage *img;
    /* synth-bad glyph 1 is INVALID, glyph 2 UNSUPPORTED (bad.oracle); a glyph past numGlyphs */
    const uint16_t bads[] = {1, 2, 60000};
    for (size_t i = 0; i < 3; i++) {
        ASSERT_EQ(gfxFontStackGlyph(&s, 3, bads[i], 16 * 64, 0, &img), STATUS_OK);
        ASSERT_TRUE(img != NULL && img->mask.data == NULL && img->mask.width == 0);
        ASSERT_EQ(s.stats.bad, (uint64_t)i + 1);
        ASSERT_EQ(s.stats.misses, (uint64_t)i + 1);
        /* the second request is a hit on the negative entry, and is not counted as bad again */
        ASSERT_EQ(gfxFontStackGlyph(&s, 3, bads[i], 16 * 64, 0, &img), STATUS_OK);
        ASSERT_EQ(s.stats.bad, (uint64_t)i + 1);
        ASSERT_EQ(s.stats.hits, (uint64_t)i + 1);
    }
    /* a space is an empty glyph but not a bad one */
    const uint16_t gSpace = gfxFontGlyphIndex(faces[0], ' ');
    const uint64_t bad = s.stats.bad;
    ASSERT_EQ(gfxFontStackGlyph(&s, 0, gSpace, 16 * 64, 0, &img), STATUS_OK);
    ASSERT_EQ(s.stats.bad, bad);
    ASSERT_TRUE(img->mask.data == NULL);
    ASSERT_TRUE(checkInvariants(&s));
    gfxFontStackDestroy(&s);
}

static int allocSweepScript(GfxFontStack *s, const GfxFont **faces) {
    (void)faces;
    /* cache hits, evictions (64 KiB cache), negative entries and one temp-sized glyph */
    static Op script[80];
    rngState = 0x0123456789ABCDEFull;
    makeRandomOps(script, 70, s);
    for (uint32_t i = 70; i < 80; i++) {
        script[i] = (Op){0, 200 * 64 + (i & 1) * 64, i & 3, gfxFontGlyphIndex(faces[0], 'W')};
    }
    for (uint32_t i = 0; i < 80; i++) {
        const Op *o = &script[i];
        const GfxGlyphImage *img;
        GfxGlyphCacheStats before = s->stats;
        int32_t order[1100];
        uint32_t nOrder = 0;
        for (int32_t j = s->lruHead; j != -1 && nOrder < 1100; j = s->entries[j].lruNext) {
            order[nOrder++] = j;
        }
        Status st = gfxFontStackGlyph(s, o->face, o->glyph, o->size, o->bin, &img);
        if (st == STATUS_ERR_NO_MEMORY) {
            /* nothing changed apart from the miss, and no temp image is held */
            CHK(img == NULL);
            CHK(s->stats.entries == before.entries && s->stats.bytes == before.bytes);
            CHK(s->stats.evictions == before.evictions && s->stats.bad == before.bad);
            CHK(s->stats.hits == before.hits && s->stats.misses == before.misses + 1);
            CHK(s->temp.mask.data == NULL);
            uint32_t k = 0;
            for (int32_t j = s->lruHead; j != -1; j = s->entries[j].lruNext) {
                CHK(k < nOrder && j == order[k]);
                k++;
            }
            CHK(k == nOrder);
            CHK(checkInvariants(s));
            /* retrying gives the reference pixels: NO_MEMORY was not cached */
            st = gfxFontStackGlyph(s, o->face, o->glyph, o->size, o->bin, &img);
        }
        CHK(st == STATUS_OK && img != NULL);
        CHK(matchesDirect(s, o->face, o->glyph, o->size, o->bin, img));
        CHK(checkInvariants(s));
    }
    return 1;
}

TEST(cacheAllocSweep) {
    const GfxFont *faces[3];
    ASSERT_TRUE(stdFaces(faces));
    int sawInitFail = 0, sawGlyphFail = 0;
    for (int failAt = 0; failAt < 4000; failAt++) {
        FtuAlloc fa;
        GfxAllocator al;
        ftuAllocInit(&fa, &al, failAt);
        GfxFontStack s;
        memset(&s, 0xA5, sizeof s); /* a failed Init must zero the struct, not find it zeroed */
        const Status st = gfxFontStackInit(&s, faces, 3, 64u << 10, &al);
        if (st != STATUS_OK) {
            ASSERT_EQ(st, STATUS_ERR_NO_MEMORY);
            sawInitFail++;
            ASSERT_EQ(fa.live, 0);
            ASSERT_EQ(fa.liveBytes, (size_t)0); /* freed with the size it was allocated with */
            GfxFontStack zero;
            memset(&zero, 0, sizeof zero);
            ASSERT_TRUE(memcmp(&s, &zero, sizeof s) == 0);
            uint32_t f = 9;
            uint16_t g = 9;
            gfxFontStackPick(&s, 'A', &f, &g);
            ASSERT_TRUE(f == 0 && g == 0);
            const GfxGlyphImage *img;
            ASSERT_EQ(gfxFontStackGlyph(&s, 0, 1, 64, 0, &img), STATUS_ERR_INVALID);
            gfxFontStackDestroy(&s); /* a no-op after a failed Init */
            ASSERT_EQ(fa.live, 0);
            /* the same struct initializes fine once allocation works */
            ASSERT_EQ(gfxFontStackInit(&s, faces, 3, 64u << 10, &al), STATUS_OK);
            gfxFontStackDestroy(&s);
            ASSERT_EQ(fa.live, 0);
            continue;
        }
        ASSERT_TRUE(allocSweepScript(&s, faces));
        gfxFontStackDestroy(&s);
        ASSERT_EQ(fa.live, 0);
        ASSERT_EQ(fa.liveBytes, (size_t)0);
        if (fa.count <= failAt) {
            break; /* the failure point is past every allocation: the sweep is complete */
        }
        sawGlyphFail++;
    }
    ASSERT_EQ(sawInitFail, 2);
    ASSERT_TRUE(sawGlyphFail > 50);
}

TEST(cacheArgs) {
    const GfxFont *faces[3];
    ASSERT_TRUE(stdFaces(faces));
    GfxFontStack s;
    const GfxGlyphImage *img = (const GfxGlyphImage *)&s;
    ASSERT_EQ(gfxFontStackInit(&s, faces, 3, 0, NULL), STATUS_OK);
    ASSERT_EQ(gfxFontStackGlyph(&s, 0, 1, 4 * 64, 0, &img), STATUS_OK); /* warm up */
    const GfxGlyphCacheStats st = s.stats;
    ASSERT_EQ(gfxFontStackGlyph(&s, 3, 1, 64, 0, &img), STATUS_ERR_INVALID);
    ASSERT_TRUE(img == NULL);
    ASSERT_EQ(gfxFontStackGlyph(&s, 0, 1, 64, 4, &img), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontStackGlyph(&s, 0, 1, 63, 0, &img), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontStackGlyph(&s, 0, 1, 32769, 0, &img), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxFontStackGlyph(&s, 0, 1, 64, 0, NULL), STATUS_ERR_INVALID);
    ASSERT_TRUE(memcmp(&st, &s.stats, sizeof st) == 0);
    /* the smallest and largest legal sizes are accepted */
    ASSERT_EQ(gfxFontStackGlyph(&s, 0, 1, GFX_FONT_MIN_SIZE_Q6, 0, &img), STATUS_OK);
    ASSERT_EQ(gfxFontStackGlyph(&s, 0, 1, GFX_FONT_MAX_SIZE_Q6, 3, &img), STATUS_OK);
    ASSERT_TRUE(checkInvariants(&s));
    gfxFontStackDestroy(&s);
    gfxFontStackDestroy(&s); /* twice */

    /* a destroyed (zeroed) stack refuses work and picks (0, 0) */
    ASSERT_EQ(gfxFontStackGlyph(&s, 0, 1, 64, 0, &img), STATUS_ERR_INVALID);
    uint32_t f = 7;
    uint16_t g = 7;
    gfxFontStackPick(&s, 'A', &f, &g);
    ASSERT_TRUE(f == 0 && g == 0);
    gfxFontStackPick(NULL, 'A', &f, &g);
    gfxFontStackPick(&s, 'A', NULL, NULL);
    gfxFontStackDestroy(NULL);

    /* Init argument checks (each leaves the struct zeroed, whatever it held before) */
    GfxFont zeroFont;
    memset(&zeroFont, 0, sizeof zeroFont);
    const GfxFont *bad[9] = {faces[0], faces[0], faces[0], faces[0], faces[0],
                             faces[0], faces[0], faces[0], faces[0]};
    const GfxFont *withNull[2] = {faces[0], NULL};
    const GfxFont *withZero[2] = {faces[0], &zeroFont};
    GfxFontStack zero;
    memset(&zero, 0, sizeof zero);
    for (int k = 0; k < 5; k++) {
        memset(&s, 0xA5, sizeof s);
        const GfxFont *const *fs = k == 0   ? faces
                                   : k == 1 ? bad
                                   : k == 2 ? NULL
                                   : k == 3 ? withNull
                                            : withZero;
        const uint32_t nf = k == 0 ? 0 : k == 1 ? 9 : k == 2 ? 3 : 2;
        ASSERT_EQ(gfxFontStackInit(&s, fs, nf, 0, NULL), STATUS_ERR_INVALID);
        ASSERT_TRUE(memcmp(&s, &zero, sizeof s) == 0);
    }
    ASSERT_EQ(gfxFontStackInit(&s, bad, 8, 0, NULL), STATUS_OK);
    gfxFontStackDestroy(&s);
    ASSERT_EQ(gfxFontStackInit(NULL, faces, 3, 0, NULL), STATUS_ERR_INVALID);

    /* budget clamps */
    ASSERT_EQ(gfxFontStackInit(&s, faces, 3, 1, NULL), STATUS_OK);
    ASSERT_EQ(s.budget, (size_t)GFX_GLYPH_CACHE_MIN_BYTES);
    gfxFontStackDestroy(&s);
    ASSERT_EQ(gfxFontStackInit(&s, faces, 3, SIZE_MAX, NULL), STATUS_OK);
    ASSERT_EQ(s.budget, (size_t)GFX_GLYPH_CACHE_MAX_BYTES);
    ASSERT_EQ(s.capacity, (uint32_t)GFX_GLYPH_CACHE_MAX_ENTRIES);
    gfxFontStackDestroy(&s);

    /* a hit allocates nothing; Init makes exactly two allocations */
    FtuAlloc fa;
    GfxAllocator al;
    ftuAllocInit(&fa, &al, -1);
    ASSERT_EQ(gfxFontStackInit(&s, faces, 3, 0, &al), STATUS_OK);
    ASSERT_EQ(fa.count, 2);
    ASSERT_EQ(gfxFontStackGlyph(&s, 0, gfxFontGlyphIndex(faces[0], 'g'), 14 * 64, 1, &img),
              STATUS_OK);
    const int count = fa.count;
    ASSERT_EQ(gfxFontStackGlyph(&s, 0, gfxFontGlyphIndex(faces[0], 'g'), 14 * 64, 1, &img),
              STATUS_OK);
    ASSERT_EQ(fa.count, count);
    ASSERT_EQ(s.stats.hits, 1u);
    gfxFontStackDestroy(&s);
    ASSERT_EQ(fa.live, 0);

    /* the scratch grows through the stack's allocator too: the first miss makes exactly the
     * allocations of a direct render with fresh scratch (outline, path, edges, then the mask) */
    FtuAlloc fd;
    GfxAllocator ald;
    ftuAllocInit(&fd, &ald, -1);
    GfxGlyphScratch sc;
    gfxGlyphScratchInit(&sc, &ald);
    GfxGlyphImage ref;
    ASSERT_EQ(
        gfxFontRenderGlyph(faces[0], gfxFontGlyphIndex(faces[0], 'g'), 14 * 64, 1, &sc, &ald, &ref),
        STATUS_OK);
    ASSERT_TRUE(fd.count > 1);
    gfxGlyphImageFree(&ref);
    gfxGlyphScratchFree(&sc);
    ASSERT_EQ(fd.live, 0);
    ftuAllocInit(&fa, &al, -1);
    ASSERT_EQ(gfxFontStackInit(&s, faces, 3, 0, &al), STATUS_OK);
    ASSERT_EQ(gfxFontStackGlyph(&s, 0, gfxFontGlyphIndex(faces[0], 'g'), 14 * 64, 1, &img),
              STATUS_OK);
    ASSERT_EQ(fa.count, 2 + fd.count);
    gfxFontStackDestroy(&s);
    ASSERT_EQ(fa.live, 0);
}

TEST(stackPick) {
    const GfxFont *sans = fontOf(FTU_SANS), *mono = fontOf(FTU_MONO),
                  *fb = fontOf(FTU_SYNTH_FALLBACK);
    ASSERT_TRUE(sans != NULL && mono != NULL && fb != NULL);
    ASSERT_TRUE(gfxFontGlyphIndex(sans, 0x4E00) == 0 && gfxFontGlyphIndex(mono, 0x4E00) == 0);
    ASSERT_TRUE(gfxFontGlyphIndex(fb, 'A') == 0);
    GfxFontStack s;
    uint32_t face;
    uint16_t glyph;

    const GfxFont *order1[3] = {sans, mono, fb};
    ASSERT_EQ(gfxFontStackInit(&s, order1, 3, 0, NULL), STATUS_OK);
    gfxFontStackPick(&s, 0x4E00, &face, &glyph); /* only the third face has it */
    ASSERT_TRUE(face == 2 && glyph == 1);
    gfxFontStackPick(&s, 0xE000, &face, &glyph);
    ASSERT_TRUE(face == 2 && glyph == 18);
    gfxFontStackPick(&s, 'A', &face, &glyph); /* both Liberations have it: the first wins */
    ASSERT_TRUE(face == 0 && glyph == gfxFontGlyphIndex(sans, 'A'));
    gfxFontStackPick(&s, 0x10FFFF, &face, &glyph); /* unmapped everywhere: face 0 .notdef */
    ASSERT_TRUE(face == 0 && glyph == 0);
    gfxFontStackPick(&s, 0x110000, &face, &glyph);
    ASSERT_TRUE(face == 0 && glyph == 0);
    gfxFontStackDestroy(&s);

    const GfxFont *order2[3] = {fb, mono, sans};
    ASSERT_EQ(gfxFontStackInit(&s, order2, 3, 0, NULL), STATUS_OK);
    gfxFontStackPick(&s, 'A', &face, &glyph); /* order matters: Mono now comes first */
    ASSERT_TRUE(face == 1 && glyph == gfxFontGlyphIndex(mono, 'A'));
    gfxFontStackPick(&s, 0x4E05, &face, &glyph);
    ASSERT_TRUE(face == 0 && glyph == 6);
    gfxFontStackDestroy(&s);
}
