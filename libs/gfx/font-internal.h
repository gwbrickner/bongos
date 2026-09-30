/* Private to libs/gfx's font engine (and its host tests): bounded big-endian readers and the
 * hooks between font-sfnt.c, font-cmap.c and font-kern.c (D-150..D-154). Every reader takes the
 * end of the *enclosing* structure and returns 0 past it, so no read can leave the buffer; callers
 * that need to tell "0" from "out of bounds" check fontFits first. Offset sums are done in
 * uint64_t so they cannot wrap. */
#ifndef LIBS_GFX_FONT_INTERNAL_H
#define LIBS_GFX_FONT_INTERNAL_H

#include "gfx/gfx-font.h"

/* True when [off, off+n) lies inside [0, end). */
static inline bool fontFits(uint64_t off, uint64_t n, uint64_t end) {
    return off <= end && n <= end - off;
}

static inline uint32_t fontRd8(const uint8_t *d, uint64_t off, uint64_t end) {
    return fontFits(off, 1, end) ? d[off] : 0u;
}

static inline uint32_t fontRd16(const uint8_t *d, uint64_t off, uint64_t end) {
    return fontFits(off, 2, end) ? ((uint32_t)d[off] << 8) | d[off + 1] : 0u;
}

static inline int32_t fontRdS16(const uint8_t *d, uint64_t off, uint64_t end) {
    return (int32_t)(int16_t)(uint16_t)fontRd16(d, off, end);
}

static inline uint32_t fontRd32(const uint8_t *d, uint64_t off, uint64_t end) {
    return fontFits(off, 4, end) ? ((uint32_t)d[off] << 24) | ((uint32_t)d[off + 1] << 16) |
                                       ((uint32_t)d[off + 2] << 8) | d[off + 3]
                                 : 0u;
}

/* font-cmap.c: picks the Unicode subtable (D-151) and validates it. INVALID or UNSUPPORTED. */
Status fontCmapSelect(GfxFont *f);

/* font-kern.c: resolves GPOS 'kern' subtables, else the 'kern' table, into *f (D-154). Never
 * fails: a malformed optional table just leaves kerning off. `kernOff/kernLen` is the 'kern'
 * table (0/0 when absent); f->gposOff/gposLen must be set. */
void fontKernResolve(GfxFont *f, uint32_t kernOff, uint32_t kernLen);

/* Offsets of the glyph's data inside glyf, from loca. INVALID when the entry is out of range or
 * non-monotonic. `*start` and `*end` are relative to glyfOff; equal means an empty glyph. */
Status fontGlyphRange(const GfxFont *f, uint16_t glyph, uint32_t *start, uint32_t *end);

#endif
