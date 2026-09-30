/* libs/gfx font engine (M12.3, D-150..D-156): TrueType (`glyf`) fonts, outlines, glyph rendering,
 * a fallback chain with a glyph cache. Userland/host only, never built into the kernel; no libc
 * beyond <string.h>, no global mutable state, and the GfxAllocator hook is the only path to memory.
 *
 * Fonts are untrusted input. Every read is bounds-checked against the enclosing table, every loop
 * and recursion has a hard limit (below), and a malformed *optional* table (kern, GPOS) is ignored
 * rather than failing the font. Status mapping (D-146): INVALID = malformed or truncated input,
 * UNSUPPORTED = out of scope (CFF, collections, point-matching composites) or over a limit,
 * NO_MEMORY = the allocator failed. TrueType instructions are never executed (D-153).
 *
 * Units: "Q6" is 26.6 fixed point (1/64 px); a size is pixels-per-em in Q6.
 *
 * Bounded memory (no byte-budget wrapper is needed, every allocation has a static bound): an
 * outline is at most ~164 KB once built (~238 KB peak while it grows), a glyph path 65536
 * verbs, the raster edges 8192 x 20 B, a mask 4 MiB (2048 x 2048), the cache its budget plus
 * ~200 KB of tables, a layout at most 24 B per codepoint plus 28 B per line with the input
 * capped at 1 MiB. */
#ifndef LIBS_GFX_FONT_H
#define LIBS_GFX_FONT_H

#include "gfx/gfx-path.h"
#include "gfx/gfx.h"

#define GFX_FONT_MAX_FILE_BYTES      (64u << 20)
#define GFX_FONT_MAX_POINTS          16384u /* whole glyph after composite expansion */
#define GFX_FONT_MAX_CONTOURS        4096u
#define GFX_FONT_MAX_COMPOSITE_DEPTH 8u
#define GFX_FONT_MAX_COMPONENTS      256u /* total over the whole component tree */
#define GFX_FONT_MAX_GPOS_SCRIPTS    16u  /* candidate scripts examined */
#define GFX_FONT_MAX_GPOS_FEATURES                                                                 \
    256u /* feature indices per LangSys, lookup indices per feature */
#define GFX_FONT_MAX_KERN_LOOKUPS     32u
#define GFX_FONT_MAX_KERN_SUBTABLES   256u
#define GFX_FONT_MAX_KERN_TABLES      16u /* 'kern' subtables walked */
#define GFX_FONT_MAX_GLYPH_DIM        2048
#define GFX_FONT_SUBPIXEL_BINS        4u
#define GFX_FONT_STACK_MAX_FACES      8u
#define GFX_FONT_MIN_SIZE_Q6          64u          /* 1 px per em */
#define GFX_FONT_MAX_SIZE_Q6          (512u * 64u) /* 512 px per em */
#define GFX_GLYPH_CACHE_DEFAULT_BYTES (2u << 20)
#define GFX_GLYPH_CACHE_MIN_BYTES     (64u << 10)
#define GFX_GLYPH_CACHE_MAX_ENTRIES   4096u
#define GFX_GLYPH_CACHE_ENTRY_COST    64u

/* A parsed font: offsets into caller-owned bytes. Zero-copy and allocation-free; there is no free
 * function. Once initialized it is immutable, so it may be shared read-only across threads. */
typedef struct GfxFont {
    const uint8_t *data;
    uint32_t size;
    uint16_t unitsPerEm, numGlyphs, numHMetrics;
    bool locaLong;
    int16_t ascender, descender, lineGap; /* D-152: ascender >= 0, descender <= 0, lineGap >= 0 */
    uint32_t glyfOff, glyfLen, locaOff, locaLen, hmtxOff, hmtxLen, cmapOff, cmapLen;
    uint32_t cmapSub; /* absolute offset of the chosen cmap subtable */
    uint16_t cmapFormat;
    bool cmapSymbol;
    uint32_t kernPairsOff, kernPairs; /* 'kern' format 0 pair array; kernPairs == 0: unused */
    uint32_t gposOff, gposLen, nKernSub;
    uint32_t kernSubOff[GFX_FONT_MAX_KERN_SUBTABLES];   /* absolute PairPos subtable offsets */
    uint8_t kernSubLookup[GFX_FONT_MAX_KERN_SUBTABLES]; /* ordinal of the owning lookup */
} GfxFont;

typedef enum { GFX_FONT_KERN_NONE, GFX_FONT_KERN_TABLE, GFX_FONT_KERN_GPOS } GfxFontKernSource;

/* Parses `data` (an sfnt with TrueType outlines) into *f. Never allocates. *f is zeroed on failure.
 * `data` must stay valid and unmodified while `f`, or any stack or layout using it, is alive.
 * Failure modes: INVALID (malformed, truncated, missing required table), UNSUPPORTED (CFF, TTC,
 * WOFF, over GFX_FONT_MAX_FILE_BYTES, no usable Unicode cmap). Never sleeps. */
Status gfxFontInit(GfxFont *f, const uint8_t *data, size_t size);

/* Glyph for a codepoint; 0 (.notdef) when unmapped or out of range. Never fails. */
uint16_t gfxFontGlyphIndex(const GfxFont *f, uint32_t cp);

/* Advance width in font units; 0 when glyph >= numGlyphs. */
uint16_t gfxFontAdvanceUnits(const GfxFont *f, uint16_t glyph);

/* Pair kerning in font units (added to the left glyph's advance); 0 when none. */
int32_t gfxFontKernUnits(const GfxFont *f, uint16_t left, uint16_t right);
GfxFontKernSource gfxFontKernSource(const GfxFont *f);

/* units * sizeQ6 / unitsPerEm, rounded half away from zero (D-152); the result is Q6. */
int32_t gfxFontScaleQ6(const GfxFont *f, int32_t units, uint32_t sizeQ6);

/* Whole-pixel metrics at a size. ascent and descent round up; lineGap rounds; lineHeight is
 * max(1, ascent + descent + lineGap). INVALID for a size outside [MIN_SIZE_Q6, MAX_SIZE_Q6]. */
typedef struct {
    int32_t ascent, descent, lineGap, lineHeight;
} GfxFontMetricsPx;
Status gfxFontMetrics(const GfxFont *f, uint32_t sizeQ6, GfxFontMetricsPx *out);

/* ---- outlines ---------------------------------------------------------------------------- */

/* x' = (a*x + c*y) + e; y' = (b*x + d*y) + f */
typedef struct {
    float a, b, c, d, e, f;
} GfxFontXform;

typedef struct {
    float *xy;
    uint8_t *onCurve;
    uint32_t *contourEnd; /* index of each contour's last point */
    uint32_t nPoints, nContours, capPoints, capContours;
    const GfxAllocator *alloc;
} GfxGlyphOutline;

/* `a` may be NULL (default allocator) and must outlive the outline. No allocation until used. */
void gfxGlyphOutlineInit(GfxGlyphOutline *o, const GfxAllocator *a);
void gfxGlyphOutlineFree(GfxGlyphOutline *o); /* NULL-safe; leaves an empty, reusable outline */

/* Loads glyph `glyph` (composites expanded) transformed by *xf (NULL: identity) into `o`, which is
 * reset first. Failure modes: INVALID (malformed or out-of-range glyph data), UNSUPPORTED (a limit,
 * point-matching composite), NO_MEMORY. On failure `o` can still be freed; its contents are
 * unspecified. An empty glyph (a space) is OK with nPoints == 0. */
Status gfxFontGlyphOutline(const GfxFont *f, uint16_t glyph, const GfxFontXform *xf,
                           GfxGlyphOutline *o);

/* Appends the outline to `p` as closed quadratic subpaths, translated by (dx, dy); returns the
 * path's sticky error. Contours with fewer than 2 points are skipped. */
Status gfxGlyphOutlineToPath(const GfxGlyphOutline *o, float dx, float dy, GfxPath *p);

/* ---- glyph images ------------------------------------------------------------------------- */

typedef struct {
    GfxMask mask;
    int32_t left, top; /* mask (0,0) lands at (penX + left, baselineY + top) */
    size_t allocSize;
    const GfxAllocator *alloc;
} GfxGlyphImage;

/* Reusable scratch for rendering (outline + path). Must not be copied by value. */
typedef struct {
    GfxGlyphOutline outline;
    GfxPath path;
} GfxGlyphScratch;
void gfxGlyphScratchInit(GfxGlyphScratch *s, const GfxAllocator *a);
void gfxGlyphScratchFree(GfxGlyphScratch *s);

/* Renders `glyph` at `sizeQ6` into a new A8 mask, positioned `bin`/4 px to the right of the pen
 * (bin < GFX_FONT_SUBPIXEL_BINS). No hinting, linear coverage. An empty glyph is OK with a mask
 * of data == NULL and width == height == 0. Failure modes: INVALID (bad size or bin, malformed
 * glyph), UNSUPPORTED (a limit, mask over GFX_FONT_MAX_GLYPH_DIM, over the raster's edge limit),
 * NO_MEMORY. `a` owns the mask: free it with gfxGlyphImageFree. */
Status gfxFontRenderGlyph(const GfxFont *f, uint16_t glyph, uint32_t sizeQ6, uint32_t bin,
                          GfxGlyphScratch *s, const GfxAllocator *a, GfxGlyphImage *out);
void gfxGlyphImageFree(GfxGlyphImage *g); /* NULL-safe and idempotent */

/* ---- fallback chain + glyph cache --------------------------------------------------------- */

typedef struct {
    uint64_t key;
    int32_t lruPrev, lruNext, hashNext;
    GfxGlyphImage img;
    uint32_t cost;
} GfxGlyphCacheEntry;

typedef struct {
    uint64_t hits, misses, evictions, bad;
    uint32_t entries;
    size_t bytes;
} GfxGlyphCacheStats;

/* Must not be copied by value. The stack does not own its fonts: every face and its bytes must
 * outlive the stack. Not thread-safe. */
typedef struct {
    const GfxFont *faces[GFX_FONT_STACK_MAX_FACES];
    uint32_t nFaces;
    const GfxAllocator *alloc;
    GfxGlyphScratch scratch;
    GfxGlyphCacheEntry *entries;
    int32_t *buckets;
    uint32_t bucketMask;
    int32_t lruHead, lruTail, freeHead;
    size_t budget;
    GfxGlyphCacheStats stats;
    GfxGlyphImage temp; /* an oversized glyph, freed at the next Glyph call / Destroy */
} GfxFontStack;

/* `cacheBytes` 0 = default, otherwise clamped to at least GFX_GLYPH_CACHE_MIN_BYTES. INVALID for
 * nFaces 0 or > GFX_FONT_STACK_MAX_FACES or a NULL face; NO_MEMORY if the tables can't be
 * allocated. `a` may be NULL and must outlive the stack. On failure the stack owns nothing. */
Status gfxFontStackInit(GfxFontStack *s, const GfxFont *const *faces, uint32_t nFaces,
                        size_t cacheBytes, const GfxAllocator *a);
void gfxFontStackDestroy(GfxFontStack *s); /* frees everything; safe after a failed Init */

/* The first face (in stack order) with a nonzero glyph for `cp`; if none has one, face 0 and
 * glyph 0 (.notdef). Never fails. */
void gfxFontStackPick(const GfxFontStack *s, uint32_t cp, uint32_t *face, uint16_t *glyph);

/* The cached image for (face, glyph, sizeQ6, bin); *out stays valid until the next gfxFontStack*
 * call on `s`. A glyph that is INVALID or UNSUPPORTED is negatively cached and returns OK with an
 * empty image (stats.bad++). INVALID for a bad face, bin or size; NO_MEMORY is returned and not
 * cached. Never sleeps. */
Status gfxFontStackGlyph(GfxFontStack *s, uint32_t face, uint16_t glyph, uint32_t sizeQ6,
                         uint32_t bin, const GfxGlyphImage **out);

#endif
