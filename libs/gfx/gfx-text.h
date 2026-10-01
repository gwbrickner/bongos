/* libs/gfx text: UTF-8 decoding and simplified UAX #14 line breaking (M12.3, D-156). The exact
 * rules, class tables and the layout algorithm are in docs/specs/gfx-text.md. Pure functions: no
 * allocation, no state beyond what the caller passes, never sleeps. */
#ifndef LIBS_GFX_TEXT_H
#define LIBS_GFX_TEXT_H

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
void gfxUtf8IterInit(GfxUtf8Iter *it, const uint8_t *s, size_t len);
/* The next codepoint and its byte offset; false at the end. */
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

void gfxTextBreakInit(GfxTextBreakState *st);

/* Feeds the next codepoint and returns the break opportunity BEFORE it (the first codepoint is
 * always NONE). Mandatory after BK, LF, NL and CR (except CR LF). Pure apart from *st. */
GfxBreak gfxTextBreakNext(GfxTextBreakState *st, uint32_t cp);

#endif
