/* libs/gfx text: UTF-8 decoding, simplified UAX #14 line breaking, paragraph layout and drawing
 * (M12.3, D-156). The exact rules, class tables and the layout algorithm are in
 * docs/specs/gfx-text.md. Nothing here sleeps. */
#ifndef LIBS_GFX_TEXT_H
#define LIBS_GFX_TEXT_H

#include "gfx/gfx-font.h"
#include "gfx/gfx.h"

#define GFX_TEXT_MAX_BYTES (1u << 20)
#define GFX_UTF8_END       0xFFFFFFFFu

/* ---- UTF-8 -------------------------------------------------------------------------------- */

/* Decodes one codepoint at s[0..len). `consumed` must not be NULL; it receives at least 1 byte
 * (0 only when len == 0, which returns GFX_UTF8_END). Never reads past s[len-1]. Malformed input
 * gives U+FFFD per maximal subpart (Unicode Table 3-7, as Python's "replace"): the lead byte plus
 * the trail bytes accepted so far are consumed, never the offending byte. U+0000, U+FFFE and
 * U+FFFF are returned as themselves. Pure; never fails. */
uint32_t gfxUtf8Decode(const uint8_t *s, size_t len, size_t *consumed);

/* Writes 1-4 bytes and returns the count. A surrogate or a value above 0x10FFFF encodes U+FFFD. */
size_t gfxUtf8Encode(uint32_t cp, uint8_t out[4]);

/* The number of codepoints gfxUtf8Decode yields over s[0..len); *nReplaced (may be NULL) gets how
 * many of them were U+FFFD substitutions for malformed input. */
size_t gfxUtf8Count(const uint8_t *s, size_t len, size_t *nReplaced);

typedef struct {
    const uint8_t *s;
    size_t len, pos;
} GfxUtf8Iter;
/* Starts an iteration over s[0..len) (s may be NULL when len is 0). Pure apart from *it. */
void gfxUtf8IterInit(GfxUtf8Iter *it, const uint8_t *s, size_t len);
/* The next codepoint (as gfxUtf8Decode) and its byte offset; false at the end, with *cp and
 * *offset untouched. */
bool gfxUtf8Next(GfxUtf8Iter *it, uint32_t *cp, size_t *offset);

/* ---- line breaking ------------------------------------------------------------------------ */

typedef enum {
    GFX_LB_AL,
    GFX_LB_BK,
    GFX_LB_CR,
    GFX_LB_LF,
    GFX_LB_NL,
    GFX_LB_SP,
    GFX_LB_ZW,
    GFX_LB_WJ,
    GFX_LB_GL,
    GFX_LB_BA,
    GFX_LB_HY,
    GFX_LB_B2,
    GFX_LB_OP,
    GFX_LB_NS,
    GFX_LB_CM,
    GFX_LB_NU,
    GFX_LB_ID,
    GFX_LB_COUNT
} GfxLineClass;

typedef enum { GFX_BREAK_NONE, GFX_BREAK_ALLOWED, GFX_BREAK_MANDATORY } GfxBreak;

/* prev2/prev/lastNonSp hold a GfxLineClass, or GFX_LB_COUNT for "none". */
typedef struct {
    uint8_t prev2, prev, lastNonSp;
    bool started;
} GfxTextBreakState;

/* The line class of a codepoint (AL for anything not in the tables, including values above
 * 0x10FFFF and surrogates). Pure. */
GfxLineClass gfxTextLineClass(uint32_t cp);

/* True for codepoints that are never drawn and have no advance (controls, format characters,
 * variation selectors, tags; U+00AD, U+200B and the like). Pure. */
bool gfxTextIsInvisible(uint32_t cp);

/* Resets *st to the start of a text (nothing seen yet). */
void gfxTextBreakInit(GfxTextBreakState *st);

/* Feeds the next codepoint and returns the break opportunity BEFORE it (the first codepoint is
 * always NONE). Mandatory after BK, LF, NL and CR (except CR LF). Pure apart from *st. */
GfxBreak gfxTextBreakNext(GfxTextBreakState *st, uint32_t cp);

/* ---- layout ------------------------------------------------------------------------------- */

#define GFX_TEXT_NO_KERNING   1u
#define GFX_TEXT_NO_SUBPIXEL  2u /* every advance, kern and tab is rounded to whole pixels first */
#define GFX_TEXT_MAX_COORD_Q6 (1 << 30) /* 2^24 px: the bound on x and y */

typedef struct {
    uint32_t sizeQ6;
    int32_t maxWidthQ6; /* 0 = no wrapping */
    int32_t tabQ6;      /* 0 = default (8 spaces of face 0) */
    uint32_t flags;
} GfxTextStyle;

enum {
    GFX_TEXT_GLYPH_INVISIBLE = 0x01,
    GFX_TEXT_GLYPH_HANGING = 0x02,    /* a trailing space: past the line's width, not wrapped for */
    GFX_TEXT_GLYPH_BREAK_MASK = 0x0C, /* GfxBreak << 2: the break opportunity before this glyph */
    GFX_TEXT_GLYPH_TAB = 0x10,
    GFX_TEXT_GLYPH_SPACE = 0x20, /* U+0020 */
    GFX_TEXT_GLYPH_CM = 0x40,
    GFX_TEXT_GLYPH_HARD = 0x80 /* BK, CR, LF or NL */
};

/* One per decoded codepoint, in text order, so `offset` maps straight to a caret position. */
typedef struct {
    int32_t xQ6;     /* pen position in the line, Q6 */
    int32_t x, y;    /* whole-pixel pen x and the line's baseline */
    uint32_t offset; /* byte offset of the codepoint in the text */
    uint16_t glyph;
    uint8_t face, bin, flags, pad[3]; /* bin: quarter-pixel phase of x */
} GfxTextGlyph;
_Static_assert(sizeof(GfxTextGlyph) == 24, "GfxTextGlyph is 24 bytes");

enum {
    GFX_TEXT_LINE_END_TEXT,
    GFX_TEXT_LINE_END_HARD,
    GFX_TEXT_LINE_END_SOFT,
    GFX_TEXT_LINE_END_EMERGENCY
};

typedef struct {
    uint32_t first, count;       /* glyph records of the line */
    uint32_t byteStart, byteEnd; /* the line's text, [start, end) */
    int32_t baseline, widthQ6;   /* widthQ6 excludes hanging spaces and the hard break */
    uint8_t end, pad[3];
} GfxTextLine;
_Static_assert(sizeof(GfxTextLine) == 28, "GfxTextLine is 28 bytes");

typedef struct {
    GfxTextGlyph *glyphs;
    GfxTextLine *lines;
    uint32_t nGlyphs, nLines, sizeQ6, flags;
    int32_t ascent, descent, lineHeight; /* whole pixels, from face 0 */
    int32_t widthQ6, height;             /* the widest line, and nLines * lineHeight */
    const GfxFontStack *stack;
    const GfxAllocator *alloc;
} GfxTextLayout;

/* Lays `text` (UTF-8, `len` bytes, malformed input becomes U+FFFD) out with the face stack `s`:
 * per-codepoint fallback (gfxFontStackPick), kerning between adjacent glyphs of the same face,
 * tab stops, line breaking at the opportunities of gfxTextBreakNext (and an emergency break at a
 * glyph boundary when a word alone is wider than the line). `a` may be NULL (default) and must
 * outlive the layout, as must `s` and its faces. *out is zeroed first and stays zeroed on failure.
 * Failure modes: INVALID (NULL arguments, text NULL with len > 0, nFaces 0, unknown flags,
 * maxWidthQ6 or tabQ6 out of range, a size outside the font limits), UNSUPPORTED (len over
 * GFX_TEXT_MAX_BYTES, a coordinate beyond 2^24 px), NO_MEMORY. Allocates 24 bytes per codepoint
 * and 28 per line. Reads only `s`' faces (never its cache), but must not race with a
 * gfxFontStackGlyph or gfxFontStackDestroy on `s`. */
Status gfxTextLayout(const GfxFontStack *s, const uint8_t *text, size_t len, const GfxTextStyle *st,
                     const GfxAllocator *a, GfxTextLayout *out);

/* NULL-safe and idempotent; zeroes *l. */
void gfxTextLayoutFree(GfxTextLayout *l);

/* ---- drawing ------------------------------------------------------------------------------ */

/* Draws every line (or `nLines` from `firstLine`, clamped) with its origin at (ox, oy), through
 * the stack's glyph cache and gfxFillMask (so the canvas clip applies). `s` must be the stack the
 * layout was made with, and must not have been destroyed. Failure modes: INVALID (NULL or a
 * different stack); NO_MEMORY after drawing everything else (a glyph that could not be rendered is
 * skipped; redraw over a cleared background, since drawing twice darkens antialiased edges). */
Status gfxTextDraw(GfxCanvas *c, GfxFontStack *s, const GfxTextLayout *l, int32_t ox, int32_t oy,
                   GfxColor col);
Status gfxTextDrawLines(GfxCanvas *c, GfxFontStack *s, const GfxTextLayout *l, uint32_t firstLine,
                        uint32_t nLines, int32_t ox, int32_t oy, GfxColor col);

#endif
