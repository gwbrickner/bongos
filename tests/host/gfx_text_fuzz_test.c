/* Seeded fuzz of the whole font pipeline (M12.3, D-158): random UTF-8-ish bytes with random styles
 * through layout and drawing, over both the fixture fonts and mutated copies of them, with
 * allocation failure injected at a random point and a leak check after every iteration. It checks
 * that nothing crashes or leaks under ASan/UBSan/LSan and that the layout's structure is sound; the
 * exact behavior is covered by the other font tests. A deeper run: -DFONT_FUZZ_SCALE=50. */
#include "font_testutil.h"
#include "framework/test.h"
#include "gfx/gfx-text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FONT_FUZZ_SCALE
#define FONT_FUZZ_SCALE 1u
#endif

static uint64_t rngState;
static uint32_t rnd(void) {
    uint64_t x = rngState;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rngState = x;
    return (uint32_t)(x >> 16);
}

static size_t randomText(uint8_t *buf, size_t cap) {
    static const uint32_t pool[] = {'A',    'V',    'a',    'W',      ' ',     ' ',    '\n',
                                    '\r',   '\t',   '-',    '(',      '.',     0x301,  0x200B,
                                    0xA0,   0x2014, 0x4E00, 0x4E01,   0x3002,  0xE000, 0xE004,
                                    0x2028, 0x85,   0xAD,   0x10FFFF, 0x1F600, 0xFF08, '!'};
    size_t n = 0;
    const uint32_t len = rnd() % 70;
    for (uint32_t i = 0; i < len && n + 4 < cap; i++) {
        const uint32_t r = rnd() % 20;
        if (r == 0) {
            buf[n++] = (uint8_t)rnd(); /* a stray byte */
        } else if (r == 1 && n + 8 < cap) {
            buf[n++] = (uint8_t)(0xE0 | (rnd() & 0xF)); /* a truncated sequence */
        } else {
            n += gfxUtf8Encode(pool[rnd() % (sizeof pool / sizeof pool[0])], buf + n);
        }
    }
    return n;
}

static int structureIsSound(const GfxTextLayout *l, const uint8_t *text, size_t len,
                            int32_t maxWidthQ6) {
    if (l->nGlyphs != gfxUtf8Count(text, len, NULL) || l->nLines < 1) {
        return 0;
    }
    uint32_t next = 0, prevEnd = 0;
    for (uint32_t li = 0; li < l->nLines; li++) {
        const GfxTextLine *ln = &l->lines[li];
        if (ln->first != next || ln->byteStart != prevEnd || ln->byteEnd < ln->byteStart ||
            ln->byteEnd > len || ln->widthQ6 < 0) {
            return 0;
        }
        for (uint32_t j = ln->first; j < ln->first + ln->count; j++) {
            if (l->glyphs[j].y != ln->baseline || l->glyphs[j].face >= l->stack->nFaces ||
                l->glyphs[j].bin > 3) {
                return 0;
            }
        }
        /* the spaces before the end (looking through a hard break and zero-width invisibles)
         * hang and nothing else does; a line over the wrap width holds one base and its marks */
        uint32_t last = UINT32_MAX;
        for (uint32_t j = ln->first + ln->count; j > ln->first; j--) {
            const uint8_t f = l->glyphs[j - 1].flags;
            const int skip = (f & GFX_TEXT_GLYPH_HARD) ||
                             ((f & GFX_TEXT_GLYPH_INVISIBLE) && !(f & GFX_TEXT_GLYPH_TAB));
            if (last == UINT32_MAX && (skip || (f & GFX_TEXT_GLYPH_SPACE))) {
                if (!!(f & GFX_TEXT_GLYPH_HANGING) != !!(f & GFX_TEXT_GLYPH_SPACE)) {
                    return 0;
                }
                continue;
            }
            if (last == UINT32_MAX) {
                last = j - 1;
            }
            if (f & GFX_TEXT_GLYPH_HANGING) {
                return 0;
            }
        }
        if (maxWidthQ6 > 0 && ln->widthQ6 > maxWidthQ6) {
            for (uint32_t j = ln->first + 1; last != UINT32_MAX && j <= last; j++) {
                if (!(l->glyphs[j].flags & GFX_TEXT_GLYPH_CM)) {
                    return 0;
                }
            }
        }
        next += ln->count;
        prevEnd = ln->byteEnd;
    }
    return next == l->nGlyphs && prevEnd == len;
}

/* One iteration: build a stack over `faces`, lay out random text and draw it, all through a
 * failing allocator; returns 0 on a crash-free-but-wrong result. */
static int oneRun(const GfxFont *const *faces, uint32_t nFaces) {
    FtuAlloc fa;
    GfxAllocator al;
    const int failAt = rnd() % 3 == 0 ? -1 : (int)(rnd() % 60);
    ftuAllocInit(&fa, &al, failAt);
    GfxFontStack s;
    int ok = 1;
    const size_t cache = rnd() % 2 ? 64u << 10 : 0;
    if (gfxFontStackInit(&s, faces, nFaces, cache, &al) == STATUS_OK) {
        uint8_t buf[320];
        const size_t n = randomText(buf, sizeof buf);
        GfxTextStyle st;
        st.sizeQ6 = 64 + rnd() % (40 * 64);
        const uint32_t w = rnd() % 4;
        st.maxWidthQ6 = w == 0 ? 0 : w == 1 ? (int32_t)(rnd() % 100) : (int32_t)(rnd() % 8000);
        st.tabQ6 = rnd() % 3 == 0 ? (int32_t)(64 + rnd() % 5000) : 0;
        st.flags = rnd() % 4;
        GfxTextLayout l;
        const Status ls = gfxTextLayout(&s, buf, n, &st, &al, &l);
        if (ls == STATUS_OK) {
            ok = structureIsSound(&l, buf, n, st.maxWidthQ6);
            uint32_t px[64 * 64];
            memset(px, 0, sizeof px);
            GfxCanvas c;
            if (gfxCanvasInit(&c, (GfxSurface){px, 64, 64, 64}, &al) == STATUS_OK) {
                const Status ds = gfxTextDraw(&c, &s, &l, (int32_t)(rnd() % 40) - 10,
                                              (int32_t)(rnd() % 40) - 10, 0xFFFFFFFFu);
                ok = ok && (ds == STATUS_OK || ds == STATUS_ERR_NO_MEMORY);
                gfxCanvasDestroy(&c);
            }
            gfxTextLayoutFree(&l);
        } else {
            ok = ok && (ls == STATUS_ERR_NO_MEMORY || ls == STATUS_ERR_UNSUPPORTED) &&
                 l.glyphs == NULL && l.lines == NULL;
        }
        gfxFontStackDestroy(&s);
    }
    return ok && fa.live == 0 && fa.liveBytes == 0;
}

static const GfxFont *fixture(int which) {
    static GfxFont fonts[FTU_COUNT];
    static int state[FTU_COUNT];
    if (state[which] == 0) {
        size_t n = 0;
        const uint8_t *d = ftuFont(which, &n);
        state[which] = d != NULL && gfxFontInit(&fonts[which], d, n) == STATUS_OK ? 1 : 2;
    }
    return state[which] == 1 ? &fonts[which] : NULL;
}

TEST(textFuzzFixtureFonts) {
    rngState = 0x7E57F0A7C0DEull;
    static const int ids[] = {FTU_SANS,       FTU_MONO,       FTU_SYNTH_FALLBACK,
                              FTU_SYNTH_GRID, FTU_SYNTH_GPOS, FTU_SYNTH_SYMBOL};
    for (uint32_t i = 0; i < 1500 * FONT_FUZZ_SCALE; i++) {
        const GfxFont *faces[4];
        const uint32_t nf = 1 + rnd() % 4;
        for (uint32_t k = 0; k < nf; k++) {
            faces[k] = fixture(ids[rnd() % (sizeof ids / sizeof ids[0])]);
            ASSERT_TRUE(faces[k] != NULL);
        }
        if (!oneRun(faces, nf)) {
            fprintf(stderr, "  fixture fuzz iteration %u failed\n", i);
            ASSERT_TRUE(0);
        }
    }
}

TEST(textFuzzMutatedFonts) {
    rngState = 0xBADF0457ull;
    static const int ids[] = {FTU_SYNTH_FALLBACK, FTU_SYNTH_GRID, FTU_SYNTH_GPOS, FTU_SYNTH_SYMBOL,
                              FTU_SYNTH_BAD};
    uint32_t initOk = 0;
    for (uint32_t i = 0; i < 600 * FONT_FUZZ_SCALE; i++) {
        size_t n = 0;
        const uint8_t *src = ftuFont(ids[rnd() % (sizeof ids / sizeof ids[0])], &n);
        ASSERT_TRUE(src != NULL);
        /* an exact-size heap copy, so ASan sees any read past the (possibly truncated) end */
        size_t m = rnd() % 8 == 0 ? rnd() % (n + 1) : n;
        uint8_t *copy = malloc(m != 0 ? m : 1);
        ASSERT_TRUE(copy != NULL);
        memcpy(copy, src, m);
        for (uint32_t k = 1 + rnd() % 4; m > 0 && k > 0; k--) {
            const size_t at = rnd() % 2 ? rnd() % (m < 400 ? m : 400) : rnd() % m;
            copy[at] = (uint8_t)(rnd() % 3 == 0 ? copy[at] ^ (1u << (rnd() % 8)) : rnd());
        }
        GfxFont mutated;
        if (gfxFontInit(&mutated, copy, m) == STATUS_OK) {
            initOk++;
            const GfxFont *faces[2] = {&mutated, fixture(FTU_SYNTH_GRID)};
            if (!oneRun(faces, rnd() % 2 ? 2 : 1)) {
                fprintf(stderr, "  mutated fuzz iteration %u failed\n", i);
                free(copy);
                ASSERT_TRUE(0);
            }
        }
        free(copy);
    }
    ASSERT_TRUE(initOk > 100); /* enough mutants still parse for the pipeline to be exercised */
}
