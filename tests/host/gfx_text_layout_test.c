/* Host tests for libs/gfx's paragraph layout and text drawing (M12.3, D-156). Hand-computed cases
 * on synth-grid (1000 units per em; visible ASCII advances 500 units, so 8 px at 16 px), kerning
 * from the synthetic and shipped fonts, the Python layout reference in layout.cases (advances and
 * kerning from an independent font parse), structural invariants over random text, the size and
 * coordinate limits, allocation-failure sweeps, and pixel comparisons of drawing against a manual
 * render-and-fill. */
#include "font_testutil.h"
#include "framework/test.h"
#include "gfx/gfx-text.h"
#include "gfx_golden.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

#define PX(n) ((uint32_t)(n) * 64u)

/* A stack plus the layout it made. */
typedef struct {
    GfxFontStack s;
    GfxTextLayout l;
} Fx;

static int fxInit(Fx *fx, const int *fonts, uint32_t n, size_t cache, const GfxAllocator *a) {
    const GfxFont *faces[8];
    for (uint32_t i = 0; i < n; i++) {
        faces[i] = fontOf(fonts[i]);
        CHK(faces[i] != NULL);
    }
    memset(&fx->l, 0, sizeof fx->l);
    CHK(gfxFontStackInit(&fx->s, faces, n, cache, a) == STATUS_OK);
    return 1;
}

static void fxFree(Fx *fx) {
    gfxTextLayoutFree(&fx->l);
    gfxFontStackDestroy(&fx->s);
}

static Status fxLay(Fx *fx, const char *text, uint32_t sizeQ6, int32_t maxW, int32_t tab,
                    uint32_t flags) {
    GfxTextStyle st = {sizeQ6, maxW, tab, flags};
    gfxTextLayoutFree(&fx->l);
    return gfxTextLayout(&fx->s, (const uint8_t *)text, strlen(text), &st, NULL, &fx->l);
}

static const int GRID[] = {FTU_SYNTH_GRID};

/* ---- hand-computed cases on synth-grid ----------------------------------------------------- */

TEST(layoutMetricsAndAdvances) {
    Fx fx;
    ASSERT_TRUE(fxInit(&fx, GRID, 1, 0, NULL));
    ASSERT_EQ(fxLay(&fx, "abc", PX(16), 0, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.ascent, 13); /* 800/1000 * 16 = 12.8 rounds up */
    ASSERT_EQ(fx.l.descent, 4); /* 200/1000 * 16 = 3.2 rounds up */
    ASSERT_EQ(fx.l.lineHeight, 17);
    ASSERT_EQ(fx.l.nGlyphs, 3u);
    ASSERT_EQ(fx.l.nLines, 1u);
    ASSERT_EQ(fx.l.height, 17);
    ASSERT_EQ(fx.l.lines[0].baseline, 13);
    ASSERT_EQ(fx.l.lines[0].widthQ6, 1536);
    ASSERT_EQ(fx.l.widthQ6, 1536);
    ASSERT_EQ(fx.l.lines[0].end, (uint8_t)GFX_TEXT_LINE_END_TEXT);
    for (uint32_t i = 0; i < 3; i++) {
        ASSERT_EQ(fx.l.glyphs[i].xQ6, (int32_t)(512 * i));
        ASSERT_EQ(fx.l.glyphs[i].x, (int32_t)(8 * i));
        ASSERT_EQ(fx.l.glyphs[i].bin, (uint8_t)0);
        ASSERT_EQ(fx.l.glyphs[i].y, 13);
        ASSERT_EQ(fx.l.glyphs[i].offset, i);
    }
    fxFree(&fx);
}

TEST(layoutSubpixelBinsAndNoSubpixel) {
    Fx fx;
    ASSERT_TRUE(fxInit(&fx, GRID, 1, 0, NULL));
    /* 16.25 px: an advance of 8.125 px = 520 Q6; pen 0, 520, 1040 */
    ASSERT_EQ(fxLay(&fx, "abc", 1040, 0, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.glyphs[1].xQ6, 520);
    ASSERT_EQ(fx.l.glyphs[2].xQ6, 1040);
    /* floor((xQ6 + 8) / 16) quarter pixels: 33 -> 8 px + bin 1; 65 -> 16 px + bin 1 */
    ASSERT_TRUE(fx.l.glyphs[0].x == 0 && fx.l.glyphs[0].bin == 0);
    ASSERT_TRUE(fx.l.glyphs[1].x == 8 && fx.l.glyphs[1].bin == 1);
    ASSERT_TRUE(fx.l.glyphs[2].x == 16 && fx.l.glyphs[2].bin == 1);
    /* NO_SUBPIXEL rounds every advance to whole pixels first: 520 -> 512 */
    ASSERT_EQ(fxLay(&fx, "abc", 1040, 0, 0, GFX_TEXT_NO_SUBPIXEL), STATUS_OK);
    for (uint32_t i = 0; i < 3; i++) {
        ASSERT_EQ(fx.l.glyphs[i].xQ6, (int32_t)(512 * i));
        ASSERT_EQ(fx.l.glyphs[i].bin, (uint8_t)0);
    }
    /* a quarter-pixel rounding edge: 0.125 px rounds the bin up (xQ6 8 -> q 1) */
    ASSERT_EQ(fxLay(&fx, "aaaa", 1040 + 16, 0, 0, 0), STATUS_OK); /* advance 528 Q6 */
    ASSERT_EQ(fx.l.glyphs[1].xQ6, 528);
    ASSERT_TRUE(fx.l.glyphs[1].x == 8 && fx.l.glyphs[1].bin == 1);
    fxFree(&fx);
}

TEST(layoutWrapsAtTheExactWidth) {
    Fx fx;
    ASSERT_TRUE(fxInit(&fx, GRID, 1, 0, NULL));
    /* "aaa bbb": three glyphs are exactly 1536 Q6; the space hangs; b's go to the next line */
    ASSERT_EQ(fxLay(&fx, "aaa bbb", PX(16), 1536, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.nLines, 2u);
    ASSERT_EQ(fx.l.lines[0].count, 4u);
    ASSERT_EQ(fx.l.lines[0].widthQ6, 1536);
    ASSERT_EQ(fx.l.lines[0].end, (uint8_t)GFX_TEXT_LINE_END_SOFT);
    ASSERT_TRUE(fx.l.glyphs[3].flags & GFX_TEXT_GLYPH_HANGING);
    ASSERT_EQ(fx.l.lines[1].first, 4u);
    ASSERT_EQ(fx.l.lines[1].widthQ6, 1536);
    ASSERT_EQ(fx.l.lines[1].baseline, 13 + 17);
    ASSERT_EQ(fx.l.lines[1].byteStart, 4u);
    ASSERT_EQ(fx.l.lines[0].byteEnd, 4u);
    ASSERT_EQ(fx.l.glyphs[4].xQ6, 0);
    ASSERT_EQ(fx.l.glyphs[4].y, 30);
    ASSERT_EQ(fx.l.height, 34);

    /* one Q6 unit narrower: the words no longer fit, so there are emergency breaks too */
    ASSERT_EQ(fxLay(&fx, "aaa bbb", PX(16), 1535, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.nLines, 4u); /* "aa" | "a " | "bb" | "b" */
    ASSERT_EQ(fx.l.lines[0].count, 2u);
    ASSERT_EQ(fx.l.lines[0].end, (uint8_t)GFX_TEXT_LINE_END_EMERGENCY);
    ASSERT_EQ(fx.l.lines[1].count, 2u);
    ASSERT_EQ(fx.l.lines[1].end, (uint8_t)GFX_TEXT_LINE_END_SOFT);
    ASSERT_EQ(fx.l.lines[2].count, 2u);
    ASSERT_EQ(fx.l.lines[2].end, (uint8_t)GFX_TEXT_LINE_END_EMERGENCY);
    ASSERT_EQ(fx.l.lines[3].count, 1u);
    ASSERT_EQ(fx.l.lines[3].end, (uint8_t)GFX_TEXT_LINE_END_TEXT);
    fxFree(&fx);
}

TEST(layoutHangingSpacesAndWideGlyphs) {
    Fx fx;
    ASSERT_TRUE(fxInit(&fx, GRID, 1, 0, NULL));
    ASSERT_EQ(fxLay(&fx, "ab   cd", PX(16), 1024, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.nLines, 2u);
    ASSERT_EQ(fx.l.lines[0].count, 5u);
    ASSERT_EQ(fx.l.lines[0].widthQ6, 1024); /* the three spaces hang past it */
    for (uint32_t i = 2; i < 5; i++) {
        ASSERT_TRUE(fx.l.glyphs[i].flags & GFX_TEXT_GLYPH_HANGING);
    }
    ASSERT_TRUE(!(fx.l.glyphs[1].flags & GFX_TEXT_GLYPH_HANGING));
    /* an ideograph (1024 Q6 wide) is wider than a 512 line: it sits alone and does not loop */
    ASSERT_EQ(fxLay(&fx, "\xE4\xB8\x80\xE4\xB8\x80", PX(16), 512, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.nLines, 2u);
    ASSERT_EQ(fx.l.lines[0].count, 1u);
    ASSERT_EQ(fx.l.lines[0].widthQ6, 1024); /* 1000 units at 16 px, wider than the 512 line */
    ASSERT_EQ(fx.l.lines[0].end, (uint8_t)GFX_TEXT_LINE_END_SOFT);
    ASSERT_EQ(fxLay(&fx, "a", PX(16), 100, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.nLines, 1u);
    fxFree(&fx);
}

TEST(layoutCombiningMarksStayWithTheirBase) {
    Fx fx;
    ASSERT_TRUE(fxInit(&fx, GRID, 1, 0, NULL));
    /* the grid font has no U+0301: .notdef, advance 500; the class is CM all the same */
    ASSERT_EQ(fxLay(&fx, "aab\xCC\x81", PX(16), 1024, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.nLines, 2u); /* "aa" | "b" + mark */
    ASSERT_EQ(fx.l.lines[1].first, 2u);
    ASSERT_EQ(fx.l.lines[1].count, 2u);
    ASSERT_EQ(fxLay(&fx, "aa\xCC\x81", PX(16), 1024, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.nLines, 2u); /* the mark overflows: its base goes with it */
    ASSERT_EQ(fx.l.lines[0].count, 1u);
    ASSERT_EQ(fx.l.lines[1].count, 2u);
    /* the base is the first glyph of the line: step forward past the whole run, no break */
    ASSERT_EQ(fxLay(&fx, "a\xCC\x81\xCC\x81", PX(16), 512, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.nLines, 1u);
    ASSERT_EQ(fx.l.lines[0].count, 3u);
    /* base, run of marks, then more text: the break comes after the run */
    ASSERT_EQ(fxLay(&fx,
                    "a\xCC\x81\xCC\x81"
                    "b",
                    PX(16), 512, 0, 0),
              STATUS_OK);
    ASSERT_EQ(fx.l.nLines, 2u);
    ASSERT_EQ(fx.l.lines[0].count, 3u);
    ASSERT_EQ(fx.l.lines[0].end, (uint8_t)GFX_TEXT_LINE_END_EMERGENCY);
    /* after stepping over the run, the next record is treated normally: a hard break stays on the
     * line (no line holding only the LF), a space hangs, an allowed break is a soft break */
    ASSERT_EQ(fxLay(&fx, "a\xCC\x81\nb", PX(16), 512, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.nLines, 2u);
    ASSERT_EQ(fx.l.lines[0].count, 3u);
    ASSERT_EQ(fx.l.lines[0].end, (uint8_t)GFX_TEXT_LINE_END_HARD);
    ASSERT_EQ(fx.l.lines[0].widthQ6, 1024);
    ASSERT_EQ(fxLay(&fx, "a\xCC\x81\r\n", PX(16), 512, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.nLines, 2u);
    ASSERT_EQ(fx.l.lines[0].count, 4u);
    ASSERT_EQ(fx.l.lines[1].count, 0u);
    ASSERT_EQ(fxLay(&fx, "a\xCC\x81  b", PX(16), 512, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.nLines, 2u);
    ASSERT_EQ(fx.l.lines[0].count, 4u);
    ASSERT_EQ(fx.l.lines[0].end, (uint8_t)GFX_TEXT_LINE_END_SOFT);
    ASSERT_EQ(fx.l.lines[0].widthQ6, 1024);
    ASSERT_TRUE(fx.l.glyphs[2].flags & fx.l.glyphs[3].flags & GFX_TEXT_GLYPH_HANGING);
    ASSERT_EQ(fx.l.glyphs[4].xQ6, 0);
    ASSERT_EQ(fxLay(&fx, "a\xCC\x81 ", PX(16), 512, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.nLines, 1u);
    ASSERT_EQ(fx.l.lines[0].end, (uint8_t)GFX_TEXT_LINE_END_TEXT);
    ASSERT_EQ(fxLay(&fx, "a\xCC\x81\xE4\xB8\x80", PX(16), 512, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.nLines, 2u); /* the ideograph's allowed break: a soft break after the run */
    ASSERT_EQ(fx.l.lines[0].count, 2u);
    ASSERT_EQ(fx.l.lines[0].end, (uint8_t)GFX_TEXT_LINE_END_SOFT);
    fxFree(&fx);
}

TEST(layoutHardBreaksAndEmptyLines) {
    Fx fx;
    ASSERT_TRUE(fxInit(&fx, GRID, 1, 0, NULL));
    ASSERT_EQ(fxLay(&fx, "", PX(16), 0, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.nGlyphs, 0u);
    ASSERT_EQ(fx.l.nLines, 1u);
    ASSERT_TRUE(fx.l.glyphs == NULL && fx.l.lines != NULL);
    ASSERT_EQ(fx.l.lines[0].count, 0u);
    ASSERT_EQ(fx.l.lines[0].widthQ6, 0);

    struct {
        const char *text;
        uint32_t lines;
    } cases[] = {
        {"a\nb", 2},
        {"a\r\nb", 2},
        {"a\rb", 2},
        {"a\xC2\x85"
         "b",
         2},
        {"a\xE2\x80\xA8"
         "b",
         2},
        {"a\xE2\x80\xA9"
         "b",
         2},
        {"a\x0B"
         "b",
         2},
        {"a\n", 2},
        {"\n\n", 3},
        {"a\r\n", 2},
        {"a\r\n\r\nb", 3},
        {"abc", 1},
        {"a\r", 2},
        {"a\xC2\x85", 2},
        {"a\xE2\x80\xA8", 2},
        {"a\xE2\x80\xA9", 2},
        {"a\x0B", 2},
        {"a\x0C", 2},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ASSERT_EQ(fxLay(&fx, cases[i].text, PX(16), 0, 0, 0), STATUS_OK);
        if (fx.l.nLines != cases[i].lines) {
            fprintf(stderr, "  text %zu: %u lines, want %u\n", i, fx.l.nLines, cases[i].lines);
            ASSERT_TRUE(0);
        }
        /* every line but the last ends with its hard break, also the one the text ends in */
        for (uint32_t li = 0; li + 1 < fx.l.nLines; li++) {
            ASSERT_EQ(fx.l.lines[li].end, (uint8_t)GFX_TEXT_LINE_END_HARD);
        }
        ASSERT_EQ(fx.l.lines[fx.l.nLines - 1].end, (uint8_t)GFX_TEXT_LINE_END_TEXT);
    }
    /* the lines of "a\r\nb": CR and LF both belong to line 0; byte ranges tile the text */
    ASSERT_EQ(fxLay(&fx, "a\r\nb", PX(16), 0, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.lines[0].count, 3u);
    ASSERT_EQ(fx.l.lines[0].end, (uint8_t)GFX_TEXT_LINE_END_HARD);
    ASSERT_EQ(fx.l.lines[0].byteEnd, 3u);
    ASSERT_EQ(fx.l.lines[1].byteStart, 3u);
    ASSERT_EQ(fx.l.lines[1].byteEnd, 4u);
    ASSERT_EQ(fx.l.lines[0].widthQ6, 512); /* the break characters add no width */
    /* a final hard break adds an empty last line at the end of the text */
    ASSERT_EQ(fxLay(&fx, "a\n", PX(16), 0, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.lines[1].count, 0u);
    ASSERT_EQ(fx.l.lines[1].byteStart, 2u);
    ASSERT_EQ(fx.l.lines[1].byteEnd, 2u);
    ASSERT_EQ(fx.l.lines[1].baseline, 13 + 17);
    ASSERT_EQ(fx.l.height, 34);
    fxFree(&fx);
}

TEST(layoutTabStops) {
    Fx fx;
    ASSERT_TRUE(fxInit(&fx, GRID, 1, 0, NULL));
    ASSERT_EQ(fxLay(&fx, "ab\tc", PX(16), 0, 2048, 0), STATUS_OK);
    ASSERT_EQ(fx.l.glyphs[2].xQ6, 1024); /* the tab starts at the pen */
    ASSERT_TRUE(fx.l.glyphs[2].flags & GFX_TEXT_GLYPH_TAB);
    ASSERT_EQ(fx.l.glyphs[3].xQ6, 2048); /* and ends at the next stop */
    /* exactly on a stop: the next stop, not the same one */
    ASSERT_EQ(fxLay(&fx, "abcd\te", PX(16), 0, 2048, 0), STATUS_OK);
    ASSERT_EQ(fx.l.glyphs[5].xQ6, 4096);
    /* the default stop is 8 spaces of the first face: 8 * 512 */
    ASSERT_EQ(fxLay(&fx, "a\tb", PX(16), 0, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.glyphs[2].xQ6, 4096);
    /* stops are measured from the line start, not the text start */
    ASSERT_EQ(fxLay(&fx, "aaaa\nb\tc", PX(16), 0, 2048, 0), STATUS_OK);
    ASSERT_EQ(fx.l.glyphs[7].xQ6, 2048);
    ASSERT_EQ(fx.l.glyphs[7].y, 30);
    /* NO_SUBPIXEL rounds the stop to whole pixels */
    ASSERT_EQ(fxLay(&fx, "a\tb", PX(16), 0, 2080, GFX_TEXT_NO_SUBPIXEL), STATUS_OK);
    ASSERT_EQ(fx.l.glyphs[2].xQ6, 2112); /* 2080 -> 2112 (round half away from zero at 32) */
    fxFree(&fx);
}

/* ---- kerning ------------------------------------------------------------------------------- */

TEST(layoutKerningOnLiberation) {
    static const int SANS[] = {FTU_SANS};
    Fx fx;
    ASSERT_TRUE(fxInit(&fx, SANS, 1, 0, NULL));
    const GfxFont *f = fontOf(FTU_SANS);
    const uint16_t gA = gfxFontGlyphIndex(f, 'A'), gV = gfxFontGlyphIndex(f, 'V');
    const int32_t kernUnits = gfxFontKernUnits(f, gA, gV);
    ASSERT_TRUE(kernUnits < 0);
    const int32_t advA = gfxFontScaleQ6(f, gfxFontAdvanceUnits(f, gA), PX(16));
    const int32_t kern = gfxFontScaleQ6(f, kernUnits, PX(16));
    ASSERT_EQ(fxLay(&fx, "AV", PX(16), 0, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.glyphs[1].xQ6, advA + kern);
    ASSERT_EQ(fxLay(&fx, "AV", PX(16), 0, 0, GFX_TEXT_NO_KERNING), STATUS_OK);
    ASSERT_EQ(fx.l.glyphs[1].xQ6, advA);
    /* kerning does not cross a zero-width invisible, a tab or a line start */
    ASSERT_EQ(fxLay(&fx,
                    "A\xE2\x80\x8B"
                    "V",
                    PX(16), 0, 0, 0),
              STATUS_OK);
    ASSERT_EQ(fx.l.glyphs[2].xQ6, advA);
    ASSERT_EQ(fxLay(&fx, "A\nV", PX(16), 0, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.glyphs[2].xQ6, 0);
    const int32_t tab =
        8 * gfxFontScaleQ6(f, gfxFontAdvanceUnits(f, gfxFontGlyphIndex(f, ' ')), PX(16));
    ASSERT_TRUE(tab > advA);
    ASSERT_EQ(fxLay(&fx, "A\tV", PX(16), 0, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.glyphs[2].xQ6, tab); /* exactly on the stop: no kern */
    /* an invisible codepoint is face 0, glyph 0, even where the font maps it (Sans has U+00AD);
     * U+3000 is not a SPACE record (only U+0020 hangs) */
    ASSERT_TRUE(gfxFontGlyphIndex(f, 0xAD) != 0);
    ASSERT_EQ(fxLay(&fx, "A\xC2\xAD\xE3\x80\x80", PX(16), 0, 0, 0), STATUS_OK);
    ASSERT_TRUE(fx.l.glyphs[1].flags & GFX_TEXT_GLYPH_INVISIBLE);
    ASSERT_TRUE(fx.l.glyphs[1].face == 0 && fx.l.glyphs[1].glyph == 0);
    ASSERT_TRUE(!(fx.l.glyphs[2].flags & (GFX_TEXT_GLYPH_SPACE | GFX_TEXT_GLYPH_HANGING)));
    const uint16_t gV2 = gfxFontGlyphIndex(f, 'V');
    const int32_t fitsKerned = advA + kern + gfxFontScaleQ6(f, gfxFontAdvanceUnits(f, gV2), PX(16));
    ASSERT_EQ(fxLay(&fx, "AV", PX(16), fitsKerned, 0, 0), STATUS_OK); /* fits only kerned */
    ASSERT_EQ(fx.l.nLines, 1u);
    ASSERT_EQ(fxLay(&fx, "AV", PX(16), fitsKerned, 0, GFX_TEXT_NO_KERNING), STATUS_OK);
    ASSERT_EQ(fx.l.nLines, 2u); /* ... and not without kerning, so the break is emergency */
    fxFree(&fx);
}

TEST(layoutKerningStaysInsideAFace) {
    /* synth-fallback kerns its glyph pair (1, 2) by -100 units; U+4E00 and U+4E01 are those */
    static const int FB[] = {FTU_SYNTH_FALLBACK};
    Fx fx;
    ASSERT_TRUE(fxInit(&fx, FB, 1, 0, NULL));
    ASSERT_EQ(fxLay(&fx, "\xE4\xB8\x80\xE4\xB8\x81", PX(16), 0, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.glyphs[1].xQ6, 1024 - 102); /* -100 * 1024/1000 = -102.4 rounds to -102 */
    ASSERT_EQ(fxLay(&fx, "\xE4\xB8\x80\xE4\xB8\x81", PX(16), 0, 0, GFX_TEXT_NO_KERNING), STATUS_OK);
    ASSERT_EQ(fx.l.glyphs[1].xQ6, 1024);
    ASSERT_EQ(fxLay(&fx, "\xE4\xB8\x80\xE2\x80\x8B\xE4\xB8\x81", PX(16), 0, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.glyphs[2].xQ6, 1024); /* a ZWSP in between: no kerning */
    ASSERT_EQ(fxLay(&fx, "\xE4\xB8\x80\xE4\xB8\x81", PX(16), 1024, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.nLines, 2u);       /* the second is wider than the line: it wraps ... */
    ASSERT_EQ(fx.l.glyphs[1].xQ6, 0); /* ... and kerning does not reach across the break */
    fxFree(&fx);

    /* across a face change: '!' is glyph 1 of the grid font (face 1), U+4E01 is glyph 2 of the
     * fallback font (face 0). If the engine asked the current face for the pair (1, 2) it would
     * find -100 units; between different faces it must not kern at all. */
    static const int TWO[] = {FTU_SYNTH_FALLBACK, FTU_SYNTH_GRID};
    ASSERT_TRUE(fxInit(&fx, TWO, 2, 0, NULL));
    ASSERT_EQ(fxLay(&fx, "!\xE4\xB8\x81", PX(16), 0, 0, 0), STATUS_OK);
    ASSERT_EQ(fx.l.glyphs[0].face, (uint8_t)1);
    ASSERT_EQ(fx.l.glyphs[1].face, (uint8_t)0);
    ASSERT_EQ(fx.l.glyphs[1].glyph, (uint16_t)2);
    ASSERT_EQ(fx.l.glyphs[1].xQ6, 512);
    fxFree(&fx);
}

/* ---- negative pens, from patched fonts ------------------------------------------------------ */

/* A private copy of fixture `which` with 16-bit values patched (table tag, offset in the table,
 * value), initialized into *f. Free the bytes after the stack and font are done with. */
typedef struct {
    const char *tag;
    uint32_t off;
    uint16_t val;
} Patch;
static uint8_t *patchedFont(int which, const Patch *p, int n, GfxFont *f) {
    size_t size = 0;
    const uint8_t *src = ftuFont(which, &size);
    uint8_t *d = src != NULL ? malloc(size) : NULL;
    if (d == NULL) {
        return NULL;
    }
    memcpy(d, src, size);
    for (int i = 0; i < n; i++) {
        uint32_t len = 0;
        const uint32_t t = ftuTable(d, size, p[i].tag, &len);
        if (t == 0 || p[i].off + 2 > len) {
            free(d);
            return NULL;
        }
        ftuPut16(d, t + p[i].off, p[i].val);
    }
    if (gfxFontInit(f, d, size) != STATUS_OK) {
        free(d);
        return NULL;
    }
    return d;
}

/* synth-fallback: U+4E00 is glyph 1, U+4E01 glyph 2 (hmtx entries 1 and 2), and its only 'kern'
 * pair (1, 2) has its value at byte 22 of the table */
static uint8_t *fallbackKern(uint16_t adv1, uint16_t adv2, int16_t kern, GfxFont *f) {
    const Patch p[] = {{"hmtx", 4, adv1}, {"hmtx", 8, adv2}, {"kern", 22, (uint16_t)kern}};
    return patchedFont(FTU_SYNTH_FALLBACK, p, 3, f);
}

static Status layOne(const GfxFont *f, const char *text, size_t len, uint32_t sizeQ6, int32_t tab,
                     uint32_t flags, GfxTextLayout *l) {
    GfxFontStack s;
    const GfxFont *faces[1] = {f};
    if (gfxFontStackInit(&s, faces, 1, 0, NULL) != STATUS_OK) {
        return STATUS_ERR_NO_MEMORY;
    }
    GfxTextStyle st = {sizeQ6, 0, tab, flags};
    const Status r = gfxTextLayout(&s, (const uint8_t *)text, len, &st, NULL, l);
    gfxFontStackDestroy(&s); /* only l->stack is left dangling, and it is not used here */
    return r;
}

#define YI   "\xE4\xB8\x80" /* U+4E00, glyph 1 */
#define DING "\xE4\xB8\x81" /* U+4E01, glyph 2 */

TEST(layoutNegativePens) {
    GfxFont f;
    GfxTextLayout l;
    /* a kern larger than the advance before it: a negative x, rounded down (never truncated
     * towards zero): xQ6 -102 -> q = floor(-94 / 16) = -6 -> x = -2, bin 2 */
    uint8_t *d = fallbackKern(0, 1000, -100, &f);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ(layOne(&f, YI DING, 6, PX(16), 0, 0, &l), STATUS_OK);
    ASSERT_EQ(l.glyphs[1].xQ6, -102);
    ASSERT_EQ(l.glyphs[1].x, -2);
    ASSERT_EQ(l.glyphs[1].bin, (uint8_t)2);
    ASSERT_EQ(l.lines[0].widthQ6, 1024 - 102);
    gfxTextLayoutFree(&l);
    free(d);

    /* the line ends left of its start: width 0; a tab from a negative pen goes to stop 0 */
    d = fallbackKern(0, 0, -100, &f);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ(layOne(&f, YI DING, 6, PX(16), 0, 0, &l), STATUS_OK);
    ASSERT_EQ(l.lines[0].widthQ6, 0);
    ASSERT_EQ(l.widthQ6, 0);
    gfxTextLayoutFree(&l);
    ASSERT_EQ(layOne(&f, YI DING "\tx", 8, PX(16), 2048, 0, &l), STATUS_OK);
    ASSERT_EQ(l.glyphs[2].xQ6, -102);
    ASSERT_EQ(l.glyphs[3].xQ6, 0);
    gfxTextLayoutFree(&l);
    free(d);

    /* NO_SUBPIXEL rounds a negative half pixel away from zero: -32 Q6 -> -64 */
    d = fallbackKern(1000, 1000, -125, &f);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ(layOne(&f, YI DING, 6, 256, 0, 0, &l), STATUS_OK);
    ASSERT_EQ(l.glyphs[1].xQ6, 256 - 32);
    gfxTextLayoutFree(&l);
    ASSERT_EQ(layOne(&f, YI DING, 6, 256, 0, GFX_TEXT_NO_SUBPIXEL, &l), STATUS_OK);
    ASSERT_EQ(l.glyphs[1].xQ6, 256 - 64);
    gfxTextLayoutFree(&l);
    free(d);

    /* the negative bound, also when only the kern itself crosses it: at 512 px a pair is kern
     * -2^20 then advance +2^19, so after j pairs the pen is -j * 2^19 and the kern of pair j + 1
     * dips 2^20 lower. 2047 pairs touch -2^30 exactly (allowed); the 2048th kern goes below it
     * (UNSUPPORTED), although its advance would bring the pen back to -2^30. */
    d = fallbackKern(0, 16000, -32000, &f);
    ASSERT_TRUE(d != NULL);
    char *big = malloc(2048 * 6);
    ASSERT_TRUE(big != NULL);
    for (int i = 0; i < 2048; i++) {
        memcpy(big + 6 * i, YI DING, 6);
    }
    ASSERT_EQ(layOne(&f, big, 2047 * 6, 32768, 0, 0, &l), STATUS_OK);
    ASSERT_EQ(l.glyphs[2 * 2047 - 1].xQ6, -GFX_TEXT_MAX_COORD_Q6);
    ASSERT_EQ(l.lines[0].widthQ6, 0);
    gfxTextLayoutFree(&l);
    memset(&l, 0x5A, sizeof l);
    ASSERT_EQ(layOne(&f, big, 2048 * 6, 32768, 0, 0, &l), STATUS_ERR_UNSUPPORTED);
    ASSERT_TRUE(l.glyphs == NULL && l.lines == NULL && l.nGlyphs == 0);
    free(big);
    free(d);
}

/* synth-grid's U+0020 is glyph 1 (hmtx entry 1): the default tab from a tiny or zero space */
TEST(layoutDefaultTabFallbacks) {
    GfxFont f;
    GfxTextLayout l;
    /* space 50 units: 3 Q6 at 1 px, a tab of 24 Q6, raised to the 64 minimum */
    const Patch tiny[] = {{"hmtx", 4, 50}};
    uint8_t *d = patchedFont(FTU_SYNTH_GRID, tiny, 1, &f);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ(layOne(&f, "a\tb", 3, 64, 0, 0, &l), STATUS_OK);
    ASSERT_EQ(l.glyphs[1].xQ6, 32);
    ASSERT_EQ(l.glyphs[2].xQ6, 64);
    gfxTextLayoutFree(&l);
    free(d);
}

/* ---- the Python reference ------------------------------------------------------------------ */

TEST(layoutMatchesPythonReference) {
    const char *text = ftuOracle("layout.cases");
    ASSERT_TRUE(text != NULL);
    size_t pos = 0, cases = 0;
    char line[4096];
    while (ftuNextLine(text, &pos, line, sizeof line)) {
        char name[8];
        unsigned size;
        int maxw;
        int used = 0;
        ASSERT_EQ(sscanf(line, "%7s %u %d : %n", name, &size, &maxw, &used), 3);
        const char *p = line + used;
        uint8_t utf8[256];
        size_t n = 0;
        if (*p == '-') {
            p += 1;
        }
        while (*p != ':' && *p != 0) {
            char *end;
            const uint32_t cp = (uint32_t)strtoul(p, &end, 16);
            if (end == p) {
                break;
            }
            n += gfxUtf8Encode(cp, utf8 + n);
            p = *end == ' ' ? end + 1 : end;
        }
        ASSERT_TRUE(*p == ':' || (*p == ' ' && p[1] == ':'));
        p = strchr(p, ':') + 2;
        const char *recsStart = p;
        const char *widthsStart = strstr(recsStart, " : ");
        ASSERT_TRUE(widthsStart != NULL);
        widthsStart += 3;

        const int fonts[1] = {strcmp(name, "sans") == 0   ? FTU_SANS
                              : strcmp(name, "mono") == 0 ? FTU_MONO
                                                          : FTU_SYNTH_GRID};
        Fx fx;
        ASSERT_TRUE(fxInit(&fx, fonts, 1, 0, NULL));
        GfxTextStyle st = {size, maxw, 0, 0};
        ASSERT_EQ(gfxTextLayout(&fx.s, utf8, n, &st, NULL, &fx.l), STATUS_OK);
        /* per-codepoint line and x */
        uint32_t k = 0;
        for (const char *q = recsStart; q < widthsStart - 3 && *q != '-' && k < fx.l.nGlyphs;) {
            char *end;
            const unsigned wantLine = (unsigned)strtoul(q, &end, 10);
            const long wantX = strtol(end + 1, &end, 10);
            uint32_t gotLine = 0;
            for (uint32_t li = 0; li < fx.l.nLines; li++) {
                if (k >= fx.l.lines[li].first && k < fx.l.lines[li].first + fx.l.lines[li].count) {
                    gotLine = li;
                }
            }
            if (gotLine != wantLine || fx.l.glyphs[k].xQ6 != wantX) {
                fprintf(stderr, "  case %zu (%s) glyph %u: line %u x %d, want line %u x %ld\n",
                        cases, line, k, gotLine, fx.l.glyphs[k].xQ6, wantLine, wantX);
                fxFree(&fx);
                ASSERT_TRUE(0);
            }
            k++;
            q = *end == ' ' ? end + 1 : end;
        }
        ASSERT_EQ(k, fx.l.nGlyphs);
        /* per-line width */
        uint32_t li = 0;
        for (const char *q = widthsStart; *q != 0 && li < fx.l.nLines;) {
            char *end;
            const long w = strtol(q, &end, 10);
            if (fx.l.lines[li].widthQ6 != w) {
                fprintf(stderr, "  case %zu (%s) line %u width %d, want %ld\n", cases, line, li,
                        fx.l.lines[li].widthQ6, w);
                fxFree(&fx);
                ASSERT_TRUE(0);
            }
            li++;
            q = *end == ' ' ? end + 1 : end;
        }
        ASSERT_EQ(li, fx.l.nLines);
        fxFree(&fx);
        cases++;
    }
    ASSERT_TRUE(cases >= 700);
}

/* ---- structural invariants over random text ------------------------------------------------ */

static uint64_t rngState;
static uint32_t rnd(void) {
    uint64_t x = rngState;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rngState = x;
    return (uint32_t)(x >> 16);
}

static int64_t fdiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

static int checkLayout(const GfxTextLayout *l, const uint8_t *text, size_t len,
                       const GfxTextStyle *st) {
    CHK(l->nGlyphs == gfxUtf8Count(text, len, NULL));
    CHK(l->nLines >= 1 && l->lines != NULL);
    CHK(l->height == (int32_t)(l->nLines * (uint32_t)l->lineHeight));
    uint32_t next = 0;
    int32_t widest = 0;
    uint32_t prevByteEnd = 0;
    for (uint32_t li = 0; li < l->nLines; li++) {
        const GfxTextLine *ln = &l->lines[li];
        CHK(ln->first == next); /* the lines tile the glyph records */
        next += ln->count;
        CHK(ln->baseline == l->ascent + (int32_t)li * l->lineHeight);
        CHK(ln->widthQ6 >= 0);
        if (ln->widthQ6 > widest) {
            widest = ln->widthQ6;
        }
        CHK(ln->byteStart == prevByteEnd && ln->byteEnd >= ln->byteStart && ln->byteEnd <= len);
        prevByteEnd = ln->byteEnd;
        if (ln->count > 0) {
            CHK(ln->byteStart == l->glyphs[ln->first].offset);
        }
        if (li > 0 && ln->count > 0) { /* every break is an opportunity, mandatory or emergency */
            const GfxTextLine *pv = &l->lines[li - 1];
            const uint32_t brk = (l->glyphs[ln->first].flags & GFX_TEXT_GLYPH_BREAK_MASK) >> 2;
            {
                CHK(brk != GFX_BREAK_NONE || pv->end == GFX_TEXT_LINE_END_EMERGENCY);
                CHK((pv->end == GFX_TEXT_LINE_END_HARD) == (brk == GFX_BREAK_MANDATORY));
                /* a line starts with a hard break character only after a hard break */
                CHK(pv->end == GFX_TEXT_LINE_END_HARD ||
                    !(l->glyphs[ln->first].flags & GFX_TEXT_GLYPH_HARD));
            }
        }
        /* hanging flags only on trailing spaces */
        int tail = 1;
        for (uint32_t j = ln->first + ln->count; j > ln->first; j--) {
            const GfxTextGlyph *r = &l->glyphs[j - 1];
            if (r->flags & GFX_TEXT_GLYPH_HARD) {
                continue;
            }
            if (tail && (r->flags & GFX_TEXT_GLYPH_SPACE)) {
                CHK(r->flags & GFX_TEXT_GLYPH_HANGING);
            } else {
                tail = 0;
                CHK(!(r->flags & GFX_TEXT_GLYPH_HANGING));
            }
        }
        uint32_t prevOff = 0;
        for (uint32_t j = ln->first; j < ln->first + ln->count; j++) {
            const GfxTextGlyph *r = &l->glyphs[j];
            CHK(r->y == ln->baseline);
            CHK(j == ln->first || r->offset > prevOff);
            prevOff = r->offset;
            const int64_t q = fdiv((int64_t)r->xQ6 + 8, 16);
            CHK((int64_t)r->x * 4 + r->bin == q && r->bin < 4);
            if (st->flags & GFX_TEXT_NO_SUBPIXEL) {
                CHK(r->bin == 0);
            }
            CHK(r->face < l->stack->nFaces);
            CHK(r->xQ6 >= -GFX_TEXT_MAX_COORD_Q6 && r->xQ6 <= GFX_TEXT_MAX_COORD_Q6);
        }
    }
    CHK(next == l->nGlyphs);
    CHK(prevByteEnd == len);
    CHK(widest == l->widthQ6);
    return 1;
}

TEST(layoutInvariantsOnRandomText) {
    static const uint32_t pool[] = {
        'a',    'b',    'W',    'A',    'V',   ' ',    ' ',    ' ',     '\n',    '\r',   '\t',
        '-',    '(',    ')',    '.',    0x301, 0x200B, 0xA0,   0x2014,  0x4E00,  0x4E05, 0x3002,
        0xFF08, 0xE000, 0xE004, 0x2028, 0x85,  0xAD,   0x2060, 0x1F600, 0x10FFFF};
    static const int SETS[3][2] = {{FTU_SYNTH_GRID, FTU_COUNT},
                                   {FTU_SANS, FTU_SYNTH_FALLBACK},
                                   {FTU_SYNTH_FALLBACK, FTU_SYNTH_GRID}};
    rngState = 0xC0FFEE1234567ull;
    for (int iter = 0; iter < 1500; iter++) {
        const int *set = SETS[iter % 3];
        Fx fx;
        ASSERT_TRUE(fxInit(&fx, set, set[1] == FTU_COUNT ? 1 : 2, 64u << 10, NULL));
        uint8_t buf[200];
        size_t n = 0;
        const uint32_t len = rnd() % 50;
        for (uint32_t i = 0; i < len && n + 4 < sizeof buf; i++) {
            if (rnd() % 17 == 0) {
                buf[n++] = (uint8_t)rnd(); /* a stray byte: malformed UTF-8 */
            } else {
                n += gfxUtf8Encode(pool[rnd() % (sizeof pool / sizeof pool[0])], buf + n);
            }
        }
        GfxTextStyle st;
        st.sizeQ6 = PX(6 + rnd() % 40) + rnd() % 64;
        const uint32_t w = rnd() % 4;
        st.maxWidthQ6 = w == 0 ? 0 : w == 1 ? (int32_t)(rnd() % 200) : (int32_t)(rnd() % 6000);
        st.tabQ6 = rnd() % 3 == 0 ? (int32_t)(64 + rnd() % 3000) : 0;
        st.flags = rnd() % 4;
        ASSERT_EQ(gfxTextLayout(&fx.s, buf, n, &st, NULL, &fx.l), STATUS_OK);
        if (!checkLayout(&fx.l, buf, n, &st)) {
            fprintf(stderr, "  iteration %d, %zu bytes, size %u width %d flags %u\n", iter, n,
                    st.sizeQ6, st.maxWidthQ6, st.flags);
            fxFree(&fx);
            ASSERT_TRUE(0);
        }
        fxFree(&fx);
    }
}

/* ---- arguments, limits, allocation failure ------------------------------------------------- */

TEST(layoutArgumentsAndLimits) {
    Fx fx;
    ASSERT_TRUE(fxInit(&fx, GRID, 1, 0, NULL));
    GfxTextStyle st = {PX(16), 0, 0, 0};
    GfxTextLayout l;
    memset(&l, 0xA5, sizeof l);
    ASSERT_EQ(gfxTextLayout(NULL, (const uint8_t *)"a", 1, &st, NULL, &l), STATUS_ERR_INVALID);
    ASSERT_TRUE(l.glyphs == NULL && l.lines == NULL && l.nLines == 0); /* zeroed first */
    ASSERT_EQ(gfxTextLayout(&fx.s, (const uint8_t *)"a", 1, NULL, NULL, &l), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxTextLayout(&fx.s, (const uint8_t *)"a", 1, &st, NULL, NULL), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxTextLayout(&fx.s, NULL, 1, &st, NULL, &l), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxTextLayout(&fx.s, NULL, 0, &st, NULL, &l), STATUS_OK); /* empty text, no pointer */
    gfxTextLayoutFree(&l);
    GfxFontStack zero;
    memset(&zero, 0, sizeof zero);
    ASSERT_EQ(gfxTextLayout(&zero, (const uint8_t *)"a", 1, &st, NULL, &l), STATUS_ERR_INVALID);
    GfxFontStack nine; /* a corrupt stack, only for the argument check */
    memcpy(&nine, &fx.s, sizeof nine);
    nine.nFaces = GFX_FONT_STACK_MAX_FACES + 1;
    ASSERT_EQ(gfxTextLayout(&nine, (const uint8_t *)"a", 1, &st, NULL, &l), STATUS_ERR_INVALID);
    nine.nFaces = GFX_FONT_STACK_MAX_FACES;
    for (uint32_t i = 1; i < GFX_FONT_STACK_MAX_FACES; i++) {
        nine.faces[i] = fx.s.faces[0];
    }
    ASSERT_EQ(gfxTextLayout(&nine, (const uint8_t *)"a", 1, &st, NULL, &l), STATUS_OK); /* 8 ok */
    gfxTextLayoutFree(&l);
    const GfxTextStyle bad[] = {
        {PX(16), 0, 0, 4},  {PX(16), -1, 0, 0},  {PX(16), GFX_TEXT_MAX_COORD_Q6 + 1, 0, 0},
        {PX(16), 0, 63, 0}, {PX(16), 0, -64, 0}, {PX(16), 0, GFX_TEXT_MAX_COORD_Q6 + 1, 0},
        {63, 0, 0, 0},      {32769, 0, 0, 0},    {0, 0, 0, 0}};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        ASSERT_EQ(gfxTextLayout(&fx.s, (const uint8_t *)"a", 1, &bad[i], NULL, &l),
                  STATUS_ERR_INVALID);
    }
    const GfxTextStyle ok[] = {{PX(16), GFX_TEXT_MAX_COORD_Q6, 0, 0},
                               {PX(16), 0, GFX_TEXT_MAX_COORD_Q6, 0},
                               {PX(16), 0, 64, 3},
                               {64, 0, 0, 0},
                               {32768, 0, 0, 0}};
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        ASSERT_EQ(gfxTextLayout(&fx.s, (const uint8_t *)"a", 1, &ok[i], NULL, &l), STATUS_OK);
        gfxTextLayoutFree(&l);
    }
    gfxTextLayoutFree(&l);
    gfxTextLayoutFree(&l); /* idempotent */
    gfxTextLayoutFree(NULL);

    /* the byte limit: exactly 1 MiB lays out, one more byte is refused with no allocation */
    FtuAlloc fa;
    GfxAllocator al;
    ftuAllocInit(&fa, &al, -1);
    uint8_t *big = malloc((size_t)GFX_TEXT_MAX_BYTES + 1);
    ASSERT_TRUE(big != NULL);
    memset(big, 'a', (size_t)GFX_TEXT_MAX_BYTES + 1);
    ASSERT_EQ(gfxTextLayout(&fx.s, big, (size_t)GFX_TEXT_MAX_BYTES + 1, &st, &al, &l),
              STATUS_ERR_UNSUPPORTED);
    ASSERT_EQ(fa.count, 0);
    ASSERT_TRUE(l.glyphs == NULL);
    st.sizeQ6 = 64;
    ASSERT_EQ(gfxTextLayout(&fx.s, big, (size_t)GFX_TEXT_MAX_BYTES, &st, &al, &l), STATUS_OK);
    ASSERT_EQ(l.nGlyphs, GFX_TEXT_MAX_BYTES);
    ASSERT_EQ(fa.count, 2);
    gfxTextLayoutFree(&l);
    ASSERT_EQ(fa.live, 0);

    /* the coordinate limit: at 512 px an 'a' is 16384 Q6 wide; 65536 of them end exactly on
     * 2^30 (allowed), one more is beyond 2^24 px */
    st.sizeQ6 = 32768;
    ASSERT_EQ(gfxTextLayout(&fx.s, big, 65536, &st, &al, &l), STATUS_OK);
    ASSERT_EQ(l.glyphs[65535].xQ6, 65535 * 16384);
    ASSERT_EQ(l.widthQ6, GFX_TEXT_MAX_COORD_Q6);
    gfxTextLayoutFree(&l);
    ASSERT_EQ(gfxTextLayout(&fx.s, big, 65537, &st, &al, &l), STATUS_ERR_UNSUPPORTED);
    ASSERT_TRUE(l.glyphs == NULL && l.lines == NULL);
    ASSERT_EQ(fa.live, 0);
    /* the vertical limit: a baseline beyond 2^24 px */
    memset(big, '\n', 40000);
    ASSERT_EQ(gfxTextLayout(&fx.s, big, 40000, &st, &al, &l), STATUS_ERR_UNSUPPORTED);
    ASSERT_EQ(fa.live, 0);
    /* ... exactly: a baseline at 2^24 px is allowed, one line more is not */
    uint32_t edgeSize = 0, k = 0;
    for (uint32_t q = 1024; q <= GFX_FONT_MAX_SIZE_Q6 && edgeSize == 0; q++) {
        GfxFontMetricsPx m;
        ASSERT_EQ(gfxFontMetrics(fontOf(FTU_SYNTH_GRID), q, &m), STATUS_OK);
        if (((1 << 24) - m.ascent) % m.lineHeight == 0) {
            edgeSize = q;
            k = (uint32_t)(((1 << 24) - m.ascent) / m.lineHeight);
        }
    }
    ASSERT_TRUE(edgeSize != 0 && k < GFX_TEXT_MAX_BYTES);
    memset(big, '\n', k + 1);
    st.sizeQ6 = edgeSize;
    ASSERT_EQ(gfxTextLayout(&fx.s, big, k, &st, &al, &l), STATUS_OK); /* k + 1 lines */
    ASSERT_EQ(l.lines[k].baseline, 1 << 24);
    gfxTextLayoutFree(&l);
    ASSERT_EQ(gfxTextLayout(&fx.s, big, k + 1, &st, &al, &l), STATUS_ERR_UNSUPPORTED);
    ASSERT_EQ(fa.live, 0);
    free(big);
    fxFree(&fx);
}

/* The work bound (O(n), each codepoint placed a few times): a megabyte of the inputs that would be
 * quadratic with a naive emergency break, each well under a second even under ASan. A quadratic
 * regression takes hours on these, so the CPU-time bound only turns a hang into a failure. */
TEST(layoutWorkIsLinear) {
    Fx fx;
    ASSERT_TRUE(fxInit(&fx, GRID, 1, 0, NULL));
    const size_t n = GFX_TEXT_MAX_BYTES;
    uint8_t *b = malloc(n);
    ASSERT_TRUE(b != NULL);
    for (int kind = 0; kind < 6; kind++) {
        size_t len = n;
        int32_t w = 1;
        for (size_t i = 0; i < n; i++) {
            switch (kind) {
                case 0: /* a base, then marks: the step-forward case */
                    b[i] = i == 0 ? 'a' : (i % 2 ? 0xCC : 0x81);
                    break;
                case 1: /* two bases, then marks: an emergency break, then the step forward */
                    b[i] = i < 2 ? 'a' : (i % 2 ? 0x81 : 0xCC);
                    break;
                case 2: /* bases each with a run of marks wider than the line */
                    b[i] = i % 64 == 0 ? 'x' : (i % 2 ? 0xCC : 0x81);
                    w = 64 * 3;
                    break;
                case 3: /* one long word: an emergency break at every glyph */
                    b[i] = 'a';
                    break;
                case 4: /* a long word with one break opportunity at its start */
                    b[i] = i == 1 ? ' ' : 'a';
                    w = 64 * 40;
                    break;
                default: /* spaces: none of them overflows */
                    b[i] = ' ';
                    break;
            }
        }
        if (kind == 0) {
            len = n - 1; /* whole marks only */
        }
        GfxTextStyle st = {64, w, 0, 0};
        const clock_t t0 = clock();
        ASSERT_EQ(gfxTextLayout(&fx.s, b, len, &st, NULL, &fx.l), STATUS_OK);
        const double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        if (secs > 20.0) {
            fprintf(stderr, "  input %d took %.1f s\n", kind, secs);
            ASSERT_TRUE(0);
        }
        ASSERT_TRUE(checkLayout(&fx.l, b, len, &st));
        gfxTextLayoutFree(&fx.l);
    }
    free(b);
    fxFree(&fx);
}

TEST(layoutAllocationFailureSweep) {
    static const int SETS[] = {FTU_SANS, FTU_SYNTH_FALLBACK};
    Fx fx;
    ASSERT_TRUE(fxInit(&fx, SETS, 2, 0, NULL));
    const char *text = "Wrap this text\n\xE4\xB8\x80\xE4\xB8\x81 and tabs\there, all of it.";
    GfxTextStyle st = {PX(14), PX(60), 0, 0};
    int failures = 0;
    for (int failAt = 0; failAt < 20; failAt++) {
        FtuAlloc fa;
        GfxAllocator al;
        ftuAllocInit(&fa, &al, failAt);
        GfxTextLayout l;
        memset(&l, 0xA5, sizeof l);
        const Status s = gfxTextLayout(&fx.s, (const uint8_t *)text, strlen(text), &st, &al, &l);
        if (s == STATUS_OK) {
            ASSERT_TRUE(l.nLines > 1);
            gfxTextLayoutFree(&l);
        } else {
            ASSERT_EQ(s, STATUS_ERR_NO_MEMORY);
            ASSERT_TRUE(l.glyphs == NULL && l.lines == NULL && l.nGlyphs == 0);
            failures++;
        }
        ASSERT_EQ(fa.live, 0);
        ASSERT_EQ(fa.liveBytes, (size_t)0);
    }
    ASSERT_EQ(failures, 2); /* exactly two allocations: the glyphs and the lines */
    fxFree(&fx);
}

/* ---- drawing ------------------------------------------------------------------------------- */

typedef struct {
    uint32_t *px;
    GfxSurface s;
    GfxCanvas c;
} Canvas;

static int canvasInit(Canvas *cv, int w, int h, uint32_t bg) {
    cv->px = malloc((size_t)w * (size_t)h * 4);
    CHK(cv->px != NULL);
    cv->s = (GfxSurface){cv->px, w, h, w};
    CHK(gfxCanvasInit(&cv->c, cv->s, NULL) == STATUS_OK);
    gfxFillRect(&cv->c, (GfxRect){0, 0, w, h}, bg, GFX_OP_SRC);
    return 1;
}

static void canvasFree(Canvas *cv) {
    gfxCanvasDestroy(&cv->c);
    free(cv->px);
}

/* The reference: every glyph rendered straight from the font and filled, no cache. */
static int drawManual(Canvas *cv, const GfxTextLayout *l, uint32_t firstLine, uint32_t nLines,
                      int32_t ox, int32_t oy, GfxColor col) {
    GfxGlyphScratch sc;
    gfxGlyphScratchInit(&sc, NULL);
    for (uint32_t li = firstLine; li < l->nLines && li - firstLine < nLines; li++) {
        for (uint32_t j = l->lines[li].first; j < l->lines[li].first + l->lines[li].count; j++) {
            const GfxTextGlyph *r = &l->glyphs[j];
            if (r->flags & GFX_TEXT_GLYPH_INVISIBLE) {
                continue;
            }
            GfxGlyphImage img;
            if (gfxFontRenderGlyph(l->stack->faces[r->face], r->glyph, l->sizeQ6, r->bin, &sc, NULL,
                                   &img) != STATUS_OK) {
                continue;
            }
            if (img.mask.data != NULL) {
                gfxFillMask(&cv->c, ox + r->x + img.left, oy + r->y + img.top, &img.mask, col);
            }
            gfxGlyphImageFree(&img);
        }
    }
    gfxGlyphScratchFree(&sc);
    return 1;
}

static int samePixels(const Canvas *a, const Canvas *b) {
    CHK(a->s.width == b->s.width && a->s.height == b->s.height);
    return memcmp(a->px, b->px, (size_t)a->s.width * (size_t)a->s.height * 4) == 0;
}

static const char *const SAMPLE =
    "Kerning: AVATAR To Wa. Wrapping text with 10-20 and (parentheses)\nand a hard break,\n\n"
    "\xE4\xB8\x80\xE4\xB8\x81\xE3\x80\x82 mixed \xEF\xBF\xBD\xFF done.";

TEST(drawMatchesManualRenderAndIsCacheIndependent) {
    static const int SETS[] = {FTU_SANS, FTU_SYNTH_FALLBACK};
    const GfxColor cols[] = {gfxColorPremul(0xFFFFFFFFu), gfxColorPremul(0x80FFC040u)};
    for (int variant = 0; variant < 4; variant++) {
        Fx small, big;
        ASSERT_TRUE(fxInit(&small, SETS, 2, 64u << 10, NULL));
        ASSERT_TRUE(fxInit(&big, SETS, 2, 8u << 20, NULL));
        static const uint32_t sizesPx[4] = {13, 17, 90, 100};
        GfxTextStyle st = {PX(sizesPx[variant]) + (uint32_t)variant * 5, PX(150), 0,
                           variant == 3 ? GFX_TEXT_NO_SUBPIXEL : 0u};
        ASSERT_EQ(
            gfxTextLayout(&small.s, (const uint8_t *)SAMPLE, strlen(SAMPLE), &st, NULL, &small.l),
            STATUS_OK);
        ASSERT_EQ(gfxTextLayout(&big.s, (const uint8_t *)SAMPLE, strlen(SAMPLE), &st, NULL, &big.l),
                  STATUS_OK);
        Canvas ref, a, b;
        ASSERT_TRUE(canvasInit(&ref, 190, 420, 0xFF203050u));
        ASSERT_TRUE(canvasInit(&a, 190, 420, 0xFF203050u));
        ASSERT_TRUE(canvasInit(&b, 190, 420, 0xFF203050u));
        const GfxColor col = cols[variant & 1];
        ASSERT_TRUE(drawManual(&ref, &small.l, 0, small.l.nLines, 7, 3, col));
        ASSERT_EQ(gfxTextDraw(&a.c, &small.s, &small.l, 7, 3, col), STATUS_OK);
        ASSERT_EQ(gfxTextDraw(&b.c, &big.s, &big.l, 7, 3, col), STATUS_OK);
        ASSERT_TRUE(samePixels(&ref, &a));
        ASSERT_TRUE(samePixels(&ref, &b));
        ASSERT_TRUE(small.s.stats.evictions > 0 || variant < 2); /* large sizes: it really evicts */
        ASSERT_EQ(big.s.stats.evictions, 0u);
        /* something was actually drawn */
        int changed = 0;
        for (int i = 0; i < 190 * 420; i++) {
            changed += a.px[i] != 0xFF203050u;
        }
        ASSERT_TRUE(changed > 500);

        /* a subset of lines, with the count clamped */
        Canvas sub, subRef;
        ASSERT_TRUE(canvasInit(&sub, 190, 420, 0xFF203050u));
        ASSERT_TRUE(canvasInit(&subRef, 190, 420, 0xFF203050u));
        ASSERT_TRUE(small.l.nLines >= 4);
        ASSERT_TRUE(drawManual(&subRef, &small.l, 1, 2, 7, 3, col));
        ASSERT_EQ(gfxTextDrawLines(&sub.c, &small.s, &small.l, 1, 2, 7, 3, col), STATUS_OK);
        ASSERT_TRUE(samePixels(&subRef, &sub));
        Canvas none;
        ASSERT_TRUE(canvasInit(&none, 190, 420, 0xFF203050u));
        ASSERT_EQ(gfxTextDrawLines(&none.c, &small.s, &small.l, small.l.nLines, 5, 0, 0, col),
                  STATUS_OK);
        ASSERT_EQ(gfxTextDrawLines(&none.c, &small.s, &small.l, 0, 0, 0, 0, col), STATUS_OK);
        for (int i = 0; i < 190 * 420; i++) {
            ASSERT_EQ(none.px[i], 0xFF203050u);
        }
        Canvas all;
        ASSERT_TRUE(canvasInit(&all, 190, 420, 0xFF203050u));
        ASSERT_EQ(gfxTextDrawLines(&all.c, &small.s, &small.l, 0, 0xFFFFFFFFu, 7, 3, col),
                  STATUS_OK);
        ASSERT_TRUE(samePixels(&ref, &all));
        canvasFree(&ref);
        canvasFree(&a);
        canvasFree(&b);
        canvasFree(&sub);
        canvasFree(&subRef);
        canvasFree(&none);
        canvasFree(&all);
        fxFree(&small);
        fxFree(&big);
    }
}

TEST(drawArgumentsAndExtremeOrigins) {
    static const int SETS[] = {FTU_SANS};
    Fx fx, other;
    ASSERT_TRUE(fxInit(&fx, SETS, 1, 0, NULL));
    ASSERT_TRUE(fxInit(&other, SETS, 1, 0, NULL));
    ASSERT_EQ(fxLay(&fx, "Hello\nworld", PX(16), 0, 0, 0), STATUS_OK);
    Canvas cv, cv2;
    ASSERT_TRUE(canvasInit(&cv, 64, 64, 0xFF000000u));
    const GfxColor white = gfxColorPremul(0xFFFFFFFFu);
    ASSERT_EQ(gfxTextDraw(NULL, &fx.s, &fx.l, 0, 0, white), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxTextDraw(&cv.c, NULL, &fx.l, 0, 0, white), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxTextDraw(&cv.c, &fx.s, NULL, 0, 0, white), STATUS_ERR_INVALID);
    ASSERT_EQ(gfxTextDraw(&cv.c, &other.s, &fx.l, 0, 0, white), STATUS_ERR_INVALID);
    const int32_t ext[] = {INT32_MIN, INT32_MIN + 1, -1000000, 0,
                           1000000,   INT32_MAX - 1, INT32_MAX};
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        for (size_t j = 0; j < sizeof ext / sizeof ext[0]; j++) {
            ASSERT_EQ(gfxTextDraw(&cv.c, &fx.s, &fx.l, ext[i], ext[j], white), STATUS_OK);
        }
    }
    /* a glyph whose position is beyond int32 is skipped, never wrapped: with the canvas origin at
     * INT32_MAX, a wrapped x would land back on the canvas */
    ASSERT_TRUE(canvasInit(&cv2, 64, 64, 0xFF000000u));
    gfxCanvasSetOrigin(&cv2.c, INT32_MAX, 0);
    ASSERT_EQ(gfxTextDraw(&cv2.c, &fx.s, &fx.l, INT32_MAX, 0, white), STATUS_OK);
    gfxCanvasSetOrigin(&cv2.c, 0, INT32_MAX);
    ASSERT_EQ(gfxTextDraw(&cv2.c, &fx.s, &fx.l, 0, INT32_MAX, white), STATUS_OK);
    for (int i = 0; i < 64 * 64; i++) {
        ASSERT_EQ(cv2.px[i], 0xFF000000u);
    }
    gfxCanvasSetOrigin(&cv2.c, 0, 0); /* and the same text does draw at (0, 0) */
    ASSERT_EQ(gfxTextDraw(&cv2.c, &fx.s, &fx.l, 0, 0, white), STATUS_OK);
    int drawn = 0;
    for (int i = 0; i < 64 * 64; i++) {
        drawn += cv2.px[i] != 0xFF000000u;
    }
    ASSERT_TRUE(drawn > 50);
    canvasFree(&cv2);
    /* an error other than NO_MEMORY from the cache stops the draw and is returned (a record with a
     * face the stack does not have: only a corrupt layout can do that) */
    const uint8_t face0 = fx.l.glyphs[0].face;
    fx.l.glyphs[0].face = 7;
    ASSERT_EQ(gfxTextDraw(&cv.c, &fx.s, &fx.l, 0, 0, white), STATUS_ERR_INVALID);
    fx.l.glyphs[0].face = face0;
    /* everything above was far off the 64x64 canvas except the origin (0, 0) rows: only check that
     * it did not crash and an empty layout draws nothing */
    ASSERT_EQ(fxLay(&fx, "", PX(16), 0, 0, 0), STATUS_OK);
    ASSERT_EQ(gfxTextDraw(&cv.c, &fx.s, &fx.l, 0, 0, white), STATUS_OK);
    canvasFree(&cv);
    fxFree(&fx);
    fxFree(&other);
}

TEST(drawAllocationFailureSweep) {
    static const int SETS[] = {FTU_SANS, FTU_SYNTH_FALLBACK};
    const GfxColor col = gfxColorPremul(0xC0FFFFFFu);
    Fx refFx;
    ASSERT_TRUE(fxInit(&refFx, SETS, 2, 0, NULL));
    GfxTextStyle st = {PX(15), PX(90), 0, 0};
    ASSERT_EQ(gfxTextLayout(&refFx.s, (const uint8_t *)SAMPLE, strlen(SAMPLE), &st, NULL, &refFx.l),
              STATUS_OK);
    Canvas ref;
    ASSERT_TRUE(canvasInit(&ref, 100, 160, 0xFF102030u));
    ASSERT_TRUE(drawManual(&ref, &refFx.l, 0, refFx.l.nLines, 2, 2, col));
    int noMem = 0;
    for (int failAt = 0; failAt < 400; failAt++) {
        FtuAlloc fa;
        GfxAllocator al;
        ftuAllocInit(&fa, &al, failAt);
        Fx fx;
        const GfxFont *faces[2] = {fontOf(FTU_SANS), fontOf(FTU_SYNTH_FALLBACK)};
        memset(&fx.l, 0, sizeof fx.l);
        if (gfxFontStackInit(&fx.s, faces, 2, 64u << 10, &al) != STATUS_OK) {
            ASSERT_EQ(fa.live, 0);
            continue;
        }
        ASSERT_EQ(gfxTextLayout(&fx.s, (const uint8_t *)SAMPLE, strlen(SAMPLE), &st, NULL, &fx.l),
                  STATUS_OK);
        Canvas cv;
        ASSERT_TRUE(canvasInit(&cv, 100, 160, 0xFF102030u));
        Status s = gfxTextDraw(&cv.c, &fx.s, &fx.l, 2, 2, col);
        if (s == STATUS_ERR_NO_MEMORY) {
            noMem++;
            /* one allocation failed, so exactly one glyph was skipped and the draw went on: the
             * pixels differ from the reference only inside one glyph's box */
            int x0 = 100, y0 = 160, x1 = -1, y1 = -1;
            for (int y = 0; y < 160; y++) {
                for (int x = 0; x < 100; x++) {
                    if (cv.px[y * 100 + x] != ref.px[y * 100 + x]) {
                        x0 = x < x0 ? x : x0;
                        y0 = y < y0 ? y : y0;
                        x1 = x > x1 ? x : x1;
                        y1 = y > y1 ? y : y1;
                    }
                }
            }
            if (x1 - x0 >= 24 || y1 - y0 >= 24) {
                fprintf(stderr, "  failAt %d: pixels differ in (%d,%d)-(%d,%d)\n", failAt, x0, y0,
                        x1, y1);
                ASSERT_TRUE(0);
            }
            /* repaint over a cleared background: the result must be the reference */
            gfxFillRect(&cv.c, (GfxRect){0, 0, 100, 160}, 0xFF102030u, GFX_OP_SRC);
            s = gfxTextDraw(&cv.c, &fx.s, &fx.l, 2, 2, col);
        }
        ASSERT_EQ(s, STATUS_OK);
        ASSERT_TRUE(samePixels(&ref, &cv));
        canvasFree(&cv);
        fxFree(&fx);
        ASSERT_EQ(fa.live, 0);
        ASSERT_EQ(fa.liveBytes, (size_t)0);
        if (fa.count <= failAt) {
            break;
        }
    }
    ASSERT_TRUE(noMem > 20);
    canvasFree(&ref);
    fxFree(&refFx);
}

/* ---- goldens ------------------------------------------------------------------------------- */

TEST(textGoldenParagraph) {
    static const int SETS[] = {FTU_SANS};
    Fx fx;
    ASSERT_TRUE(fxInit(&fx, SETS, 1, 0, NULL));
    const char *text =
        "Typography: AVATAR waves, To Wa Yo \xE2\x80\x94 kerning pairs sit tight.\n"
        "Ranges like 10-20 and well-known (parenthetical) text wrap at the break opportunities.\n\n"
        "A second paragraph after a blank line, with an em dash\xE2\x80\x94here, and a last line.";
    ASSERT_EQ(fxLay(&fx, text, PX(16), PX(320), 0, 0), STATUS_OK);
    Canvas cv;
    ASSERT_TRUE(canvasInit(&cv, 340, fx.l.height + 20, 0xFFFFFFFFu));
    ASSERT_EQ(gfxTextDraw(&cv.c, &fx.s, &fx.l, 10, 10, gfxColorPremul(0xFF101820u)), STATUS_OK);
    ASSERT_TRUE(goldenCheck("font_text_paragraph", &cv.s));
    canvasFree(&cv);
    fxFree(&fx);
}

TEST(textGoldenMonospace) {
    static const int SETS[] = {FTU_MONO};
    Fx fx;
    ASSERT_TRUE(fxInit(&fx, SETS, 1, 0, NULL));
    const char *text = "col1\tcol2\tcol3\r\nab\tcd\r\n\r\nABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
                       "abcdefghijklmnopqrstuvwxyz and then words that wrap";
    ASSERT_EQ(fxLay(&fx, text, PX(15), PX(270), 0, GFX_TEXT_NO_SUBPIXEL), STATUS_OK);
    Canvas cv;
    ASSERT_TRUE(canvasInit(&cv, 290, fx.l.height + 20, 0xFF1E1E1Eu));
    ASSERT_EQ(gfxTextDraw(&cv.c, &fx.s, &fx.l, 10, 10, gfxColorPremul(0xFFB5E8A0u)), STATUS_OK);
    ASSERT_TRUE(goldenCheck("font_text_mono", &cv.s));
    canvasFree(&cv);
    fxFree(&fx);
}

TEST(textGoldenFallback) {
    static const int SETS[] = {FTU_SANS, FTU_SYNTH_FALLBACK};
    Fx fx;
    ASSERT_TRUE(fxInit(&fx, SETS, 2, 0, NULL));
    const char *text = "Latin text \xE4\xB8\x80\xE4\xB8\x81\xE4\xB8\x82\xE4\xB8\x83\xE4\xB8\x84"
                       "\xE4\xB8\x85\xE4\xB8\x86\xE3\x80\x82\xE4\xB8\x87\xE4\xB8\x88\xE3\x80\x82"
                       " NB\xC2\xA0space bad\xFF\xC0 pua\xEE\x80\x84 end \xEE\x80\x80.";
    ASSERT_EQ(fxLay(&fx, text, PX(18), PX(150), 0, 0), STATUS_OK);
    Canvas cv;
    ASSERT_TRUE(canvasInit(&cv, 170, fx.l.height + 20, 0xFFF4F0E8u));
    ASSERT_EQ(gfxTextDraw(&cv.c, &fx.s, &fx.l, 10, 10, gfxColorPremul(0xFF301030u)), STATUS_OK);
    ASSERT_TRUE(goldenCheck("font_text_fallback", &cv.s));
    canvasFree(&cv);
    fxFree(&fx);
}

TEST(textGoldenSizesAndClip) {
    static const int SETS[] = {FTU_SANS};
    Fx fx;
    ASSERT_TRUE(fxInit(&fx, SETS, 1, 0, NULL));
    Canvas cv;
    ASSERT_TRUE(canvasInit(&cv, 300, 250, 0xFF2A5A8Au));
    const uint32_t sizes[] = {9, 12, 16, 24, 36};
    int32_t y = 8;
    for (size_t i = 0; i < 5; i++) {
        ASSERT_EQ(fxLay(&fx, "Hamburgefonstiv 0123", PX(sizes[i]), 0, 0, 0), STATUS_OK);
        const GfxColor col = gfxColorPremul(i % 2 ? 0xB0FFE0A0u : 0xC0FFFFFFu);
        if (i == 3) { /* one line cut by a clip */
            ASSERT_EQ(gfxCanvasPushClip(&cv.c, (GfxRect){20, y, 150, y + fx.l.height - 6}),
                      STATUS_OK);
        }
        ASSERT_EQ(gfxTextDraw(&cv.c, &fx.s, &fx.l, 10, y, col), STATUS_OK);
        if (i == 3) {
            ASSERT_EQ(gfxCanvasPopClip(&cv.c), STATUS_OK);
        }
        y += fx.l.height + 6;
    }
    ASSERT_TRUE(goldenCheck("font_text_sizes_clip", &cv.s));
    canvasFree(&cv);
    fxFree(&fx);
}
