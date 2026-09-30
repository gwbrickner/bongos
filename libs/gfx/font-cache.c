/* Fallback face stack and glyph cache (M12.3, D-155). Entries live in one array and are linked by
 * index (-1 = none): a doubly linked LRU list, a singly linked hash chain per bucket, and a free
 * list threaded through lruNext. The hash is a pure function of the key (no pointers), so the
 * output depends only on the call sequence. The only allocations are the two tables in Init and
 * the masks made by gfxFontRenderGlyph, which always happens before the cache is touched. */
#include "gfx/font-internal.h"

#include <string.h>

#define NIL (-1)

/* Contract: pure. splitmix64's finalizer, truncated to 32 bits. */
uint32_t fontCacheHash(uint64_t key) {
    uint64_t z = key;
    z ^= z >> 30;
    z *= 0xBF58476D1CE4E5B9ull;
    z ^= z >> 27;
    z *= 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (uint32_t)z;
}

static size_t entriesBytes(uint32_t capacity) {
    return (size_t)capacity * sizeof(GfxGlyphCacheEntry);
}

static size_t bucketsBytes(uint32_t bucketMask) {
    return ((size_t)bucketMask + 1) * sizeof(int32_t);
}

/* Contract: no locks, may not sleep. On failure `s` is zeroed and owns nothing. */
Status gfxFontStackInit(GfxFontStack *s, const GfxFont *const *faces, uint32_t nFaces,
                        size_t cacheBytes, const GfxAllocator *a) {
    if (s == NULL) {
        return STATUS_ERR_INVALID;
    }
    memset(s, 0, sizeof *s);
    if (faces == NULL || nFaces < 1 || nFaces > GFX_FONT_STACK_MAX_FACES) {
        return STATUS_ERR_INVALID;
    }
    for (uint32_t i = 0; i < nFaces; i++) {
        if (faces[i] == NULL || faces[i]->data == NULL) {
            return STATUS_ERR_INVALID;
        }
    }
    if (a == NULL) {
        a = gfxAllocatorDefault();
    }
    size_t budget = GFX_GLYPH_CACHE_DEFAULT_BYTES;
    if (cacheBytes != 0) {
        budget = cacheBytes < GFX_GLYPH_CACHE_MIN_BYTES   ? GFX_GLYPH_CACHE_MIN_BYTES
                 : cacheBytes > GFX_GLYPH_CACHE_MAX_BYTES ? GFX_GLYPH_CACHE_MAX_BYTES
                                                          : cacheBytes;
    }
    /* Every entry costs at least GFX_GLYPH_CACHE_ENTRY_COST, so this many can never be exceeded. */
    uint32_t capacity = GFX_GLYPH_CACHE_MAX_ENTRIES;
    if (budget / GFX_GLYPH_CACHE_ENTRY_COST < capacity) {
        capacity = (uint32_t)(budget / GFX_GLYPH_CACHE_ENTRY_COST);
    }
    uint32_t nBuckets = 1;
    while (nBuckets < capacity) {
        nBuckets <<= 1;
    }

    GfxGlyphCacheEntry *entries = a->alloc(a->ctx, entriesBytes(capacity));
    if (entries == NULL) {
        return STATUS_ERR_NO_MEMORY;
    }
    int32_t *buckets = a->alloc(a->ctx, bucketsBytes(nBuckets - 1));
    if (buckets == NULL) {
        a->free(a->ctx, entries, entriesBytes(capacity));
        return STATUS_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < nBuckets; i++) {
        buckets[i] = NIL;
    }
    memset(entries, 0, entriesBytes(capacity));
    for (uint32_t i = 0; i < capacity; i++) {
        entries[i].lruNext = i + 1 < capacity ? (int32_t)(i + 1) : NIL;
        entries[i].lruPrev = NIL;
        entries[i].hashNext = NIL;
    }

    for (uint32_t i = 0; i < nFaces; i++) {
        s->faces[i] = faces[i];
    }
    s->nFaces = nFaces;
    s->alloc = a;
    s->entries = entries;
    s->buckets = buckets;
    s->bucketMask = nBuckets - 1;
    s->capacity = capacity;
    s->lruHead = NIL;
    s->lruTail = NIL;
    s->freeHead = 0;
    s->budget = budget;
    gfxGlyphScratchInit(&s->scratch, a);
    return STATUS_OK;
}

/* Contract: NULL-safe; no locks, may not sleep. Zeroes *s, so a second call is a no-op. */
void gfxFontStackDestroy(GfxFontStack *s) {
    if (s == NULL) {
        return;
    }
    if (s->entries != NULL) {
        for (int32_t i = s->lruHead; i != NIL; i = s->entries[i].lruNext) {
            gfxGlyphImageFree(&s->entries[i].img);
        }
        gfxGlyphImageFree(&s->temp);
        gfxGlyphScratchFree(&s->scratch);
        s->alloc->free(s->alloc->ctx, s->buckets, bucketsBytes(s->bucketMask));
        s->alloc->free(s->alloc->ctx, s->entries, entriesBytes(s->capacity));
    }
    memset(s, 0, sizeof *s);
}

/* Contract: pure read; no locks, never sleeps, never fails. A NULL output pointer is skipped. */
void gfxFontStackPick(const GfxFontStack *s, uint32_t cp, uint32_t *face, uint16_t *glyph) {
    uint32_t f = 0;
    uint16_t g = 0;
    if (s != NULL) {
        for (uint32_t i = 0; i < s->nFaces; i++) {
            const uint16_t gi = gfxFontGlyphIndex(s->faces[i], cp);
            if (gi != 0) {
                f = i;
                g = gi;
                break;
            }
        }
    }
    if (face != NULL) {
        *face = f;
    }
    if (glyph != NULL) {
        *glyph = g;
    }
}

static void lruUnlink(GfxFontStack *s, int32_t i) {
    GfxGlyphCacheEntry *e = &s->entries[i];
    if (e->lruPrev != NIL) {
        s->entries[e->lruPrev].lruNext = e->lruNext;
    } else {
        s->lruHead = e->lruNext;
    }
    if (e->lruNext != NIL) {
        s->entries[e->lruNext].lruPrev = e->lruPrev;
    } else {
        s->lruTail = e->lruPrev;
    }
}

static void lruPushFront(GfxFontStack *s, int32_t i) {
    GfxGlyphCacheEntry *e = &s->entries[i];
    e->lruPrev = NIL;
    e->lruNext = s->lruHead;
    if (s->lruHead != NIL) {
        s->entries[s->lruHead].lruPrev = i;
    } else {
        s->lruTail = i;
    }
    s->lruHead = i;
}

/* Removes the LRU tail: unlinks it from its hash chain and the LRU list, frees its image and
 * pushes the slot on the free list. */
static void evictTail(GfxFontStack *s) {
    const int32_t i = s->lruTail;
    GfxGlyphCacheEntry *e = &s->entries[i];
    int32_t *link = &s->buckets[fontCacheHash(e->key) & s->bucketMask];
    while (*link != i) {
        link = &s->entries[*link].hashNext; /* the entry is always in its chain */
    }
    *link = e->hashNext;
    lruUnlink(s, i);
    gfxGlyphImageFree(&e->img);
    s->stats.bytes -= e->cost;
    s->stats.entries--;
    s->stats.evictions++;
    memset(e, 0, sizeof *e);
    e->hashNext = NIL;
    e->lruPrev = NIL;
    e->lruNext = s->freeHead;
    s->freeHead = i;
}

/* Contract: no locks, may not sleep, not thread-safe. Allocation happens only inside the render,
 * before any cache state changes, so NO_MEMORY leaves the cache exactly as it was. */
Status gfxFontStackGlyph(GfxFontStack *s, uint32_t face, uint16_t glyph, uint32_t sizeQ6,
                         uint32_t bin, const GfxGlyphImage **out) {
    if (out == NULL) {
        return STATUS_ERR_INVALID;
    }
    *out = NULL;
    if (s == NULL || s->entries == NULL) {
        return STATUS_ERR_INVALID;
    }
    gfxGlyphImageFree(&s->temp);
    if (face >= s->nFaces || bin >= GFX_FONT_SUBPIXEL_BINS || sizeQ6 < GFX_FONT_MIN_SIZE_Q6 ||
        sizeQ6 > GFX_FONT_MAX_SIZE_Q6) {
        return STATUS_ERR_INVALID;
    }

    const uint64_t key = fontCacheKey(face, glyph, sizeQ6, bin);
    const uint32_t bucket = fontCacheHash(key) & s->bucketMask;
    for (int32_t i = s->buckets[bucket]; i != NIL; i = s->entries[i].hashNext) {
        if (s->entries[i].key == key) {
            if (s->lruHead != i) {
                lruUnlink(s, i);
                lruPushFront(s, i);
            }
            s->stats.hits++;
            *out = &s->entries[i].img;
            return STATUS_OK;
        }
    }

    s->stats.misses++;
    GfxGlyphImage img;
    memset(&img, 0, sizeof img);
    Status st = gfxFontRenderGlyph(s->faces[face], glyph, sizeQ6, bin, &s->scratch, s->alloc, &img);
    if (st == STATUS_ERR_INVALID || st == STATUS_ERR_UNSUPPORTED) {
        gfxGlyphImageFree(&img);
        s->stats.bad++; /* cached negatively as an empty image */
    } else if (st != STATUS_OK) {
        gfxGlyphImageFree(&img);
        return st;
    }

    const uint32_t cost =
        (uint32_t)img.mask.width * (uint32_t)img.mask.height + GFX_GLYPH_CACHE_ENTRY_COST;
    if (cost > s->budget / 8) {
        s->temp = img;
        *out = &s->temp;
        return STATUS_OK;
    }
    while (s->lruTail != NIL &&
           (s->stats.entries >= s->capacity || s->stats.bytes + cost > s->budget)) {
        evictTail(s);
    }
    const int32_t i = s->freeHead;
    GfxGlyphCacheEntry *e = &s->entries[i];
    s->freeHead = e->lruNext;
    e->key = key;
    e->img = img;
    e->cost = cost;
    e->hashNext = s->buckets[bucket];
    s->buckets[bucket] = i;
    lruPushFront(s, i);
    s->stats.bytes += cost;
    s->stats.entries++;
    *out = &e->img;
    return STATUS_OK;
}
