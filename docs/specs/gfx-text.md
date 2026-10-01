# libs/gfx text: UTF-8, line breaking, layout, draw (M12.3, D-156)

Source of truth for `libs/gfx/gfx-text.h` and `text-*.c`. Written from the `architect` consult of
2026-10-01. The tests' oracles (`tests/data/font/gen.py`) are written from THIS document, not from
the C tables. Status names are `STATUS_OK` and `STATUS_ERR_{INVALID,UNSUPPORTED,NO_MEMORY}`.

## Choices D-156 states (beyond its row)
- A final hard break produces a trailing empty line. SHY (U+00AD) is invisible and shows no hyphen
  at a break. Thai is class AL (no dictionary). Hangul syllables are ID. Bad UTF-8 draws face 0's
  `.notdef` (Liberation has no U+FFFD).
- `GFX_TEXT_NO_SUBPIXEL`: every scaled advance, kern and tab width is rounded half away from zero to
  a multiple of 64 before it is accumulated, so `bin` is always 0.
- Steps 7 and 8 take untrusted text and do size arithmetic and allocation: **risky**, one step sweep
  over 7+8 after step 8.

## UTF-8
```c
#define GFX_TEXT_MAX_BYTES (1u << 20)
#define GFX_UTF8_END 0xFFFFFFFFu
uint32_t gfxUtf8Decode(const uint8_t *s, size_t len, size_t *consumed);
size_t gfxUtf8Encode(uint32_t cp, uint8_t out[4]); /* surrogate or > 0x10FFFF encodes U+FFFD */
size_t gfxUtf8Count(const uint8_t *s, size_t len, size_t *nReplaced); /* nReplaced may be NULL */
typedef struct { const uint8_t *s; size_t len, pos; } GfxUtf8Iter;
void gfxUtf8IterInit(GfxUtf8Iter *it, const uint8_t *s, size_t len);
bool gfxUtf8Next(GfxUtf8Iter *it, uint32_t *cp, size_t *offset); /* false at the end */
```
Decode: `consumed` is not NULL. `len == 0`: return `GFX_UTF8_END`, `*consumed = 0`. Never read past
`s[len-1]`. Unicode Table 3-7:
- b0 < 0x80: itself, consume 1 (U+0000 passes through).
- C2..DF: one trail in 80..BF.
- E0: next A0..BF. E1..EC and EE..EF: next 80..BF. ED: next 80..9F. Then one more 80..BF.
- F0: next 90..BF. F1..F3: next 80..BF. F4: next 80..8F. Then two more 80..BF.
- Any other lead (80..C1, F5..FF): U+FFFD, consume 1.
- Trail byte k >= 1 missing or out of range: U+FFFD, consume k (the lead plus the trails accepted so
  far, never the bad byte). U+FFFE and U+FFFF pass through. Matches Python's `replace` decoder.

## Line classes (text-break.c)
```c
typedef enum { GFX_LB_AL, GFX_LB_BK, GFX_LB_CR, GFX_LB_LF, GFX_LB_NL, GFX_LB_SP, GFX_LB_ZW,
  GFX_LB_WJ, GFX_LB_GL, GFX_LB_BA, GFX_LB_HY, GFX_LB_B2, GFX_LB_OP, GFX_LB_NS, GFX_LB_CM,
  GFX_LB_NU, GFX_LB_ID, GFX_LB_COUNT } GfxLineClass;
typedef enum { GFX_BREAK_NONE, GFX_BREAK_ALLOWED, GFX_BREAK_MANDATORY } GfxBreak;
typedef struct { uint8_t prev2, prev, lastNonSp; bool started; } GfxTextBreakState;
GfxLineClass gfxTextLineClass(uint32_t cp);
bool gfxTextIsInvisible(uint32_t cp);
void gfxTextBreakInit(GfxTextBreakState *st);
GfxBreak gfxTextBreakNext(GfxTextBreakState *st, uint32_t cp); /* the break BEFORE cp */
```
Lookup order: (1) binary search in the "specific" table (sorted, non-overlapping); (2) binary search
in the ID table; (3) AL.

**ASCII**

| Code points | Class |
|---|---|
| 09 | BA |
| 0A | LF |
| 0B, 0C | BK |
| 0D | CR |
| 20 | SP |
| 28, 5B, 7B | OP |
| 21, 29, 2C, 2E, 3A, 3B, 3F, 5D, 7D | NS |
| 2D | HY |
| 30-39 | NU |
| 7C | BA |
| other 00-1F, and 7F | CM |
| everything else | AL |

**Specific table (non-ASCII)**
- NL: 0085
- CM: 0080-0084, 0086-009F, 0300-034E, 0350-036F, 1AB0-1AFF, 1DC0-1DFF, 200C-200F, 202A-202E,
  20D0-20FF, 302A-302F, 3099-309A, FE00-FE0F, FE20-FE2F, E0001, E0020-E007F, E0100-E01EF
- GL: 00A0, 034F, 2007, 2011, 202F
- BA: 00AD, 1680, 2000-2006, 2008-200A, 2010, 2012-2013, 205F, 3000
- ZW: 200B
- B2: 2014, 2E3A-2E3B
- OP: 2018, 201C, 3008, 300A, 300C, 300E, 3010, 3014, 3016, 3018, 301A, 301D, FF08, FF3B, FF5B,
  FF5F, FF62
- NS: quotes and CJK punctuation 2019, 201D, 3001-3002, 3005, 3009, 300B, 300D, 300F, 3011, 3015,
  3017, 3019, 301B, 301C, 301E-301F, 303B; small hiragana 3041, 3043, 3045, 3047, 3049, 3063, 3083,
  3085, 3087, 308E, 3095-3096; kana marks 309B-309E, 30A0; small katakana 30A1, 30A3, 30A5, 30A7,
  30A9, 30C3, 30E3, 30E5, 30E7, 30EE, 30F5-30F6; 30FB-30FE, 31F0-31FF; fullwidth punctuation FF01,
  FF09, FF0C, FF0E, FF1A-FF1B, FF1F, FF3D, FF5D, FF60-FF61, FF63-FF64
- BK: 2028-2029
- WJ: 2060, FEFF

**ID table:** 2E80-2FFF, 3000-31FF, 3200-4DBF, 4E00-9FFF, A000-A4CF, AC00-D7A3, F900-FAFF,
FE30-FE4F, FF00-FF60, FFE0-FFE6, 1F000-1FAFF, 20000-2FFFD, 30000-3FFFD. Where a specific entry
overlaps an ID range, the specific entry wins.

**Invisible set:** 0000-001F, 007F-009F, 00AD, 034F, 061C, 115F-1160, 17B4-17B5, 180B-180F,
200B-200F, 2028-202E, 2060-206F, 3164, FE00-FE0F, FEFF, FFA0, FFF9-FFFB, 1BCA0-1BCA3, 1D173-1D17A,
E0000-E0FFF.

**Pair rules** (first match wins; `p2`, `p`, `L` are the state's `prev2`, `prev`, `lastNonSp`):
1. First codepoint: NONE; set `L = p = cur` (if `cur` is SP, `L` is "none").
2. `p` in {BK, LF, NL}, or `p == CR && cur != LF`: MANDATORY.
3. `p == CR && cur == LF`: NONE.
4. `cur` in {BK, CR, LF, NL, SP, ZW}: NONE.
5. `L == ZW`: ALLOWED.
6. `p == WJ || cur == WJ`: NONE.
7. `p == GL`: NONE. `cur == GL && p` not in {SP, BA, HY}: NONE.
8. `cur` in {NS, CM}: NONE.
9. `L == OP`: NONE.
10. `L == B2 && cur == B2`: NONE.
11. `p == SP`: ALLOWED.
12. `cur` in {BA, HY}: NONE.
13. `p == HY && (cur == NU || (cur == AL && p2` in {none, BK, CR, LF, NL, SP, ZW, GL}`))`: NONE.
14. `p` in {BA, HY, B2} `|| cur == B2`: ALLOWED.
15. `p == ID || cur == ID`: ALLOWED.
16. Otherwise NONE.

After each call: `p2 = p`, `p = cur`, `L = cur` unless `cur` is SP.

**Hand-checked expectations** (`|` allowed, `!` mandatory; asserted in C and in gen.py):
`a |b`, `a  |b`, `well-|known`, `10-20`, `a |-b`, `x|—|y`, `x|——|y`, `f(x) |g`, `( a`,
`一|二。|三`, `（一）`, `a<NBSP>b |c`, `a<ZWSP>|b`, `a\r\n!b`, `a\r!b`, `a\n!\n!b`,
`a<U+0301> |b`, `e.g. |x`, `a\t|b`.

## Layout (text-layout.c)
```c
#define GFX_TEXT_NO_KERNING 1u
#define GFX_TEXT_NO_SUBPIXEL 2u
#define GFX_TEXT_MAX_COORD_Q6 (1 << 30) /* 2^24 px, for x and y */
typedef struct { uint32_t sizeQ6; int32_t maxWidthQ6; /* 0 = no wrap */ int32_t tabQ6; /* 0 = default */
                 uint32_t flags; } GfxTextStyle;
enum { GFX_TEXT_GLYPH_INVISIBLE = 0x01, GFX_TEXT_GLYPH_HANGING = 0x02,
       GFX_TEXT_GLYPH_BREAK_MASK = 0x0C /* GfxBreak << 2, the break before */,
       GFX_TEXT_GLYPH_TAB = 0x10, GFX_TEXT_GLYPH_SPACE = 0x20 /* U+0020 */,
       GFX_TEXT_GLYPH_CM = 0x40, GFX_TEXT_GLYPH_HARD = 0x80 /* BK CR LF NL */ };
typedef struct { int32_t xQ6, x, y; uint32_t offset; uint16_t glyph; uint8_t face, bin, flags,
                 pad[3]; } GfxTextGlyph; /* 24 B */
enum { GFX_TEXT_LINE_END_TEXT, GFX_TEXT_LINE_END_HARD, GFX_TEXT_LINE_END_SOFT,
       GFX_TEXT_LINE_END_EMERGENCY };
typedef struct { uint32_t first, count, byteStart, byteEnd; int32_t baseline, widthQ6;
                 uint8_t end, pad[3]; } GfxTextLine; /* 28 B */
typedef struct { GfxTextGlyph *glyphs; GfxTextLine *lines; uint32_t nGlyphs, nLines, sizeQ6, flags;
                 int32_t ascent, descent, lineHeight, widthQ6, height;
                 const GfxFontStack *stack; const GfxAllocator *alloc; } GfxTextLayout;
Status gfxTextLayout(const GfxFontStack *s, const uint8_t *text, size_t len, const GfxTextStyle *st,
                     const GfxAllocator *a, GfxTextLayout *out);
void gfxTextLayoutFree(GfxTextLayout *l); /* NULL-safe, idempotent, zeroes */
```
`_Static_assert` on both struct sizes. Exactly one record per decoded codepoint, so offsets map
straight to carets.

Steps:
1. Zero `*out`.
2. INVALID if: `s`, `st` or `out` NULL, or `text` NULL with `len > 0`; `s->nFaces` 0 or above 8;
   unknown flag bits; `maxWidthQ6` outside [0, MAX_COORD]; `tabQ6` neither 0 nor in
   [64, MAX_COORD]; `gfxFontMetrics(s->faces[0], sizeQ6)` fails.
3. `len > GFX_TEXT_MAX_BYTES`: UNSUPPORTED, no allocation.
4. `n = gfxUtf8Count`. Allocate `n*24`; `n == 0`: `glyphs` NULL.
5. Pass 1: decode each codepoint; store `offset`, the flags and `BreakNext << 2`. Invisible
   codepoint: face 0, glyph 0, INVISIBLE. Otherwise the result of `gfxFontStackPick`.
6. Pass 2 in count mode gives `nLines` (UNSUPPORTED checked here); allocate `nLines*28`; run pass 2
   again in fill mode. On any failure free everything and leave `*out` zeroed.

Pass 2, per line: `pen = 0` (int64, Q6), no previous glyph.
- Mandatory break: record `i > lineStart` with MANDATORY closes the line as HARD.
- Invisible, not a tab: `xQ6 = pen`, zero advance, reset the previous glyph.
- Tab: `xQ6 = pen`, `pen = (floorDiv(pen, tab) + 1) * tab` from the line start; reset the previous
  glyph. Default `tab` = 8 x the scaled advance of face 0's U+0020 glyph; if 0, `sizeQ6*4`; minimum
  64.
- Visible glyph: if kerning is on and the previous glyph is from the same face,
  `pen += ScaleQ6(face, gfxFontKernUnits(prev, g))`; then `xQ6 = pen`,
  `pen += ScaleQ6(face, AdvanceUnits(g))`. With NO_SUBPIXEL round each term to a multiple of 64
  first. Kerning never crosses a line start, an invisible codepoint, a tab or a face change.
- Bounds: `|pen| > GFX_TEXT_MAX_COORD_Q6`: UNSUPPORTED.
- Overflow when `maxWidthQ6 > 0`, `pen > maxWidthQ6`, `i > lineStart`, and the record is not SPACE,
  not HARD and not a zero-width invisible. Then: if the largest ALLOWED break `o` with
  `lineStart < o <= i` exists, close the line before `o` (SOFT) and restart at `o`; otherwise an
  emergency break before `i` (if `i` is CM, step back to the last non-CM in (lineStart, i), which
  may be a SPACE: a space followed by combining marks is their base, a standalone diacritic, so it
  moves to the next line with them and does not hang; if none,
  step forward past the CM run: the marks stay on the line without further overflow checks, and the
  record after the run is then checked like any other, so a space after it hangs, a hard break
  after it ends the line as HARD, and a visible glyph after it breaks before itself).
- Line width: `widthQ6 = max(0, pen after the last record that is not SPACE, not HARD and not a
  zero-width invisible)`. The SPACE records after that record are HANGING (looking through a final
  HARD and through zero-width invisibles, so `ab ` ZWSP, SHY or LRM keeps its space hanging and the
  line no longer than `maxWidthQ6`; none of these records can trigger an overflow either). A line
  is wider than `maxWidthQ6` only when the last record that counts is its first record or one of
  the combining marks directly after it.
- Glyph position: `q = floorDiv(xQ6 + 8, 16)`, `x = floorDiv(q, 4)`, `bin = q & 3`, in int64, with
  a helper (never right-shift a negative number).
- Vertical: `y = baseline = ascent + k*lineHeight` from face 0; above 2^24 px: UNSUPPORTED.
- End of text: `n == 0`, or the last record is HARD: add an empty last line with
  `byteStart = byteEnd = len`.
- Work bound: each codepoint placed at most about 3 times; O(n).

## Draw (text-draw.c)
```c
Status gfxTextDraw(GfxCanvas *c, GfxFontStack *s, const GfxTextLayout *l, int32_t ox, int32_t oy,
                   GfxColor col);
Status gfxTextDrawLines(GfxCanvas *c, GfxFontStack *s, const GfxTextLayout *l, uint32_t firstLine,
                        uint32_t nLines, int32_t ox, int32_t oy, GfxColor col); /* clamped */
```
1. INVALID if any pointer is NULL or `l->stack != s`.
2. Per non-INVISIBLE record: `gfxFontStackGlyph(s, face, glyph, l->sizeQ6, bin, &img)`, then at once
   `gfxFillMask(c, X, Y, &img->mask, col)` with `X = ox + x + left`, `Y = oy + y + top` in int64
   (outside int32: skip the glyph, never wrap). The check is made before `gfxFillMask` adds the
   canvas origin, so a skipped glyph stays skipped even when the origin would bring it back on
   screen: since `x + left` and `y + top` stay within about +-2^25 px, callers keep `|ox|` and
   `|oy|` below `2^31 - 2^26` and put any larger translation in the canvas origin. Never hold `img`
   across another Glyph call.
3. NO_MEMORY: remember, skip the glyph, continue, return NO_MEMORY at the end (a retry must repaint
   over a cleared background: SRC_OVER twice darkens AA edges). Any other error: return at once.

No culling (clipping is `gfxFillMask`'s); callers cull with `DrawLines`. A layout is invalid once its
stack is destroyed.

## Tests
Step 7 (`gfx_text_utf8_test.c`): every `utf8.cases` line from an exact-size malloc copy; `Count`
and iterator offsets agree; encode/decode round trip for every scalar value (surrogates encode as
FFFD); gen.py `utf8.digest` (seeded 1 MiB LCG stream: count and FNV-1a-64 of Python's decoded
codepoints; the C side reproduces the LCG); gen.py `linebreak.ranges` (the full 0..10FFFF class
partition plus the invisible set, written from this spec, not copied from C; the C test checks every
codepoint and that both C tables are sorted and non-overlapping); gen.py `break.cases` (hex
codepoints then one 0/1/2 digit per boundary: the hand cases above asserted inside gen.py plus 2000
random strings over ~25 representative codepoints).

Step 8 (`gfx_text_layout_test.c`, draw tests): hand-computed cases on synth-grid (16 px gives an 8 px
advance): bins at 16.25 px and NO_SUBPIXEL, exact fit vs `maxWidth - 1` Q6, hanging spaces,
emergency breaks, a glyph wider than `maxWidth` alone on its line, CM not orphaned,
BK/CR/LF/CRLF/NEL/LS/PS, the trailing empty line, empty text, tab stops after a wrap. Kerning on
Liberation ("AV"); none across a face change, ZWSP or a line break; the NO_KERNING flag. gen.py
`layout.cases` from a Python layout reference. Its scope is deliberately narrow: one face (Sans,
Mono or the grid), no tabs, no combining marks, no invisible other than LF and no flags; it is
independent of the C in its font parse, breaks and Q6 scaling. Tabs, marks, invisibles, fallback,
kerning across faces, the flags and negative pens are covered by the hand-computed cases and the
structural invariants above. Invariants: lines partition [0, len), offsets
monotonic, every break is an opportunity or EMERGENCY. Limits: exactly 1 MiB OK; 1 MiB + 1
UNSUPPORTED with no allocation; coordinate overflow UNSUPPORTED. Allocation-failure sweep with a
leak check. Draw: pixel-identical to a manual `gfxFontRenderGlyph` + `gfxFillMask` composition and
identical between a 64 KiB and an 8 MiB cache; a draw allocation-failure sweep (NO_MEMORY or OK,
no leak, redraw over a cleared canvas matches); mismatched stack INVALID; INT32-extreme origins.

Goldens (D-147: exact; promote by hand after viewing; record in the log): `font_text_paragraph`
(Sans 16 px wrapped at 320 px; kerning, `10-20`, an em dash, parentheses, a hard LF),
`font_text_mono` (Mono 15 px NO_SUBPIXEL; tabs, CRLF, a blank line, an emergency-broken long
token), `font_text_fallback` (stack {Sans, synth-fallback}; ideographic breaks, `。` never at a line
start, NBSP, bad UTF-8 and U+E004 both showing `.notdef`), `font_text_sizes_clip` (9/12/16/24/36 px,
translucent color on a colored background, one line cut by `gfxCanvasPushClip`).

Step 9: mutation fuzz of random bytes through Layout and Draw, with tiny or zero widths and all
flags, under ASan/UBSan.

## Owner questions
1. A final hard break adds an empty line, and SHY never shows a hyphen at a break: acceptable?
2. Invalid UTF-8 and missing glyphs show face 0's `.notdef` box (Liberation has no U+FFFD):
   acceptable, or add a U+FFFD fallback face later?
3. Thai breaks only at spaces and Korean breaks between syllables (the UAX #14 default): defer
   both to M16.3?
