/* Host tests for libs/gfx's UTF-8 decoder and line breaker (M12.3, D-156). The oracles are written
 * by tests/data/font/gen.py from docs/specs/gfx-text.md and Python's own UTF-8 decoder, not from
 * the C tables: utf8.cases (hex -> codepoints), utf8.digest (a 1 MiB seeded stream),
 * linebreak.ranges (the class of every codepoint 0..10FFFF plus the invisible set) and break.cases.
 */
#include "font_testutil.h"
#include "framework/test.h"
#include "gfx/gfx-text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexVal(int c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

/* Parses "hh hh ..." or "hhhh..." (no spaces) into bytes; "-" is empty. Returns the count. */
static size_t parseHexBytes(const char *s, size_t n, uint8_t *out) {
    size_t k = 0;
    for (size_t i = 0; i + 1 < n + 1 && s[i] != 0 && i < n; i++) {
        if (s[i] == '-') {
            return 0;
        }
        if (hexVal(s[i]) >= 0 && i + 1 < n && hexVal(s[i + 1]) >= 0) {
            out[k++] = (uint8_t)(hexVal(s[i]) * 16 + hexVal(s[i + 1]));
            i++;
        }
    }
    return k;
}

TEST(utf8CasesMatchPython) {
    const char *text = ftuOracle("utf8.cases");
    ASSERT_TRUE(text != NULL);
    size_t pos = 0, lines = 0;
    char line[1024];
    while (ftuNextLine(text, &pos, line, sizeof line)) {
        char *colon = strstr(line, " : ");
        ASSERT_TRUE(colon != NULL);
        uint8_t bytes[64];
        const size_t nb = parseHexBytes(line, (size_t)(colon - line), bytes);
        uint32_t want[64];
        size_t nw = 0, nBadWant = 0;
        for (const char *p = colon + 3; *p != 0;) {
            char *end;
            want[nw++] = (uint32_t)strtoul(p, &end, 10);
            if (want[nw - 1] == 0xFFFD) {
                nBadWant++;
            }
            p = *end == ' ' ? end + 1 : end;
        }
        /* an exact-size heap copy, so ASan sees any read past the end */
        uint8_t *buf = malloc(nb != 0 ? nb : 1);
        ASSERT_TRUE(buf != NULL);
        memcpy(buf, bytes, nb);
        size_t pos2 = 0, k = 0;
        GfxUtf8Iter it;
        gfxUtf8IterInit(&it, buf, nb);
        uint32_t cp;
        size_t off;
        while (gfxUtf8Next(&it, &cp, &off)) {
            size_t used;
            const uint32_t d = gfxUtf8Decode(buf + pos2, nb - pos2, &used);
            ASSERT_TRUE(k < nw);
            ASSERT_EQ(d, want[k]);
            ASSERT_EQ(cp, want[k]);
            ASSERT_EQ(off, pos2);
            ASSERT_TRUE(used >= 1 && used <= nb - pos2);
            pos2 += used;
            k++;
        }
        ASSERT_EQ(k, nw);
        ASSERT_EQ(pos2, nb);
        size_t nBad = 99;
        ASSERT_EQ(gfxUtf8Count(buf, nb, &nBad), nw);
        ASSERT_EQ(nBad, nBadWant); /* the cases contain no genuine U+FFFD */
        ASSERT_EQ(gfxUtf8Count(buf, nb, NULL), nw);
        free(buf);
        lines++;
    }
    ASSERT_TRUE(lines > 300);
}

TEST(utf8EveryPrefixIsBounded) {
    const char *text = ftuOracle("utf8.cases");
    ASSERT_TRUE(text != NULL);
    size_t pos = 0;
    char line[1024];
    while (ftuNextLine(text, &pos, line, sizeof line)) {
        char *colon = strstr(line, " : ");
        ASSERT_TRUE(colon != NULL);
        uint8_t bytes[64];
        const size_t nb = parseHexBytes(line, (size_t)(colon - line), bytes);
        for (size_t cut = 0; cut <= nb; cut++) {
            uint8_t *buf = malloc(cut != 0 ? cut : 1);
            ASSERT_TRUE(buf != NULL);
            memcpy(buf, bytes, cut);
            size_t p = 0;
            while (p < cut) {
                size_t used = 0;
                (void)gfxUtf8Decode(buf + p, cut - p, &used);
                ASSERT_TRUE(used >= 1 && used <= cut - p);
                p += used;
            }
            free(buf);
        }
    }
}

TEST(utf8EncodeDecodeRoundTrip) {
    size_t used = 7;
    ASSERT_EQ(gfxUtf8Decode((const uint8_t *)"", 0, &used), GFX_UTF8_END);
    ASSERT_EQ(used, (size_t)0);
    for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
        uint8_t b[4];
        const size_t n = gfxUtf8Encode(cp, b);
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            ASSERT_EQ(n, (size_t)3);
            ASSERT_TRUE(b[0] == 0xEF && b[1] == 0xBF && b[2] == 0xBD);
            continue;
        }
        ASSERT_EQ(n, cp < 0x80 ? 1u : cp < 0x800 ? 2u : cp < 0x10000 ? 3u : 4u);
        ASSERT_EQ(gfxUtf8Decode(b, n, &used), cp);
        ASSERT_EQ(used, n);
    }
    uint8_t b[4];
    ASSERT_EQ(gfxUtf8Encode(0x110000, b), (size_t)3);
    ASSERT_EQ(gfxUtf8Encode(0xFFFFFFFFu, b), (size_t)3);
    ASSERT_TRUE(b[0] == 0xEF && b[1] == 0xBF && b[2] == 0xBD);
    /* a genuine U+FFFD is not a replacement; a malformed byte is */
    size_t bad = 9;
    ASSERT_EQ(gfxUtf8Count((const uint8_t *)"\xEF\xBF\xBD", 3, &bad), (size_t)1);
    ASSERT_EQ(bad, (size_t)0);
    ASSERT_EQ(gfxUtf8Count((const uint8_t *)"\xEF\xBF\xBDz\xFF", 5, &bad), (size_t)3);
    ASSERT_EQ(bad, (size_t)1);
}

TEST(utf8MegabyteStreamMatchesPython) {
    const char *text = ftuOracle("utf8.digest");
    ASSERT_TRUE(text != NULL);
    unsigned long long wantCount = 0, wantFnv = 0;
    ASSERT_EQ(sscanf(text, "count %llu\nfnv %llx", &wantCount, &wantFnv), 2);
    static const uint8_t alphabet[] = {0x41, 0x7F, 0x80, 0x9F, 0xA0, 0xBF, 0xC0, 0xC1, 0xC2, 0xDF,
                                       0xE0, 0xE1, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF4, 0xF5, 0xFF,
                                       0x8F, 0x90, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80};
    const size_t n = 1u << 20;
    uint8_t *buf = malloc(n);
    ASSERT_TRUE(buf != NULL);
    uint64_t state = 0x853C49E6748FEA9Bull;
    for (size_t i = 0; i < n; i++) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        buf[i] = ((state >> 20) & 7) != 0 ? alphabet[(state >> 33) % sizeof alphabet]
                                          : (uint8_t)((state >> 40) & 0xFF);
    }
    uint64_t h = 0xCBF29CE484222325ull;
    size_t count = 0, p = 0;
    while (p < n) {
        size_t used;
        const uint32_t cp = gfxUtf8Decode(buf + p, n - p, &used);
        for (int k = 0; k < 4; k++) {
            h = (h ^ ((cp >> (8 * k)) & 0xFF)) * 0x100000001B3ull;
        }
        p += used;
        count++;
    }
    ASSERT_EQ(count, (size_t)wantCount);
    ASSERT_TRUE(h == wantFnv);
    ASSERT_EQ(gfxUtf8Count(buf, n, NULL), count);
    free(buf);
}

/* ---- line breaking ------------------------------------------------------------------------ */

static const char *const LB_NAMES[GFX_LB_COUNT] = {"AL", "BK", "CR", "LF", "NL", "SP",
                                                   "ZW", "WJ", "GL", "BA", "HY", "B2",
                                                   "OP", "NS", "CM", "NU", "ID"};

static int lbIndex(const char *name) {
    for (int i = 0; i < GFX_LB_COUNT; i++) {
        if (strcmp(LB_NAMES[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

TEST(lineClassOfEveryCodepointMatchesOracle) {
    const char *text = ftuOracle("linebreak.ranges");
    ASSERT_TRUE(text != NULL);
    uint8_t *inv = calloc(0x110000, 1);
    ASSERT_TRUE(inv != NULL);
    size_t pos = 0, covered = 0;
    uint32_t next = 0;
    char line[128];
    while (ftuNextLine(text, &pos, line, sizeof line)) {
        unsigned lo, hi;
        char name[8];
        if (sscanf(line, "class %x %x %7s", &lo, &hi, name) == 3) {
            ASSERT_EQ(lo, next); /* the runs tile 0..10FFFF in order */
            const int want = lbIndex(name);
            ASSERT_TRUE(want >= 0);
            for (uint32_t cp = lo; cp <= hi; cp++) {
                if ((int)gfxTextLineClass(cp) != want) {
                    fprintf(stderr, "  class of U+%04X: got %d want %s\n", cp,
                            (int)gfxTextLineClass(cp), name);
                    ASSERT_TRUE(0);
                }
            }
            covered += hi - lo + 1;
            next = hi + 1;
        } else if (sscanf(line, "invisible %x %x", &lo, &hi) == 2) {
            memset(inv + lo, 1, hi - lo + 1);
        }
    }
    ASSERT_EQ(covered, (size_t)0x110000);
    for (uint32_t cp = 0; cp < 0x110000; cp++) {
        if (gfxTextIsInvisible(cp) != (inv[cp] != 0)) {
            fprintf(stderr, "  invisible U+%04X: got %d\n", cp, gfxTextIsInvisible(cp));
            ASSERT_TRUE(0);
        }
    }
    /* outside Unicode: AL and visible */
    ASSERT_EQ((int)gfxTextLineClass(0x110000), (int)GFX_LB_AL);
    ASSERT_EQ((int)gfxTextLineClass(0xFFFFFFFFu), (int)GFX_LB_AL);
    ASSERT_TRUE(!gfxTextIsInvisible(0x110000) && !gfxTextIsInvisible(0xFFFFFFFFu));
    free(inv);
}

static void breaksOf(const uint32_t *cps, size_t n, uint8_t *out) {
    GfxTextBreakState st;
    gfxTextBreakInit(&st);
    for (size_t i = 0; i < n; i++) {
        out[i] = (uint8_t)gfxTextBreakNext(&st, cps[i]);
    }
}

TEST(breakCasesMatchOracle) {
    const char *text = ftuOracle("break.cases");
    ASSERT_TRUE(text != NULL);
    size_t pos = 0, lines = 0;
    char line[256];
    while (ftuNextLine(text, &pos, line, sizeof line)) {
        char *colon = strstr(line, " : ");
        ASSERT_TRUE(colon != NULL);
        uint32_t cps[32];
        size_t n = 0;
        for (const char *p = line; p < colon;) {
            char *end;
            cps[n++] = (uint32_t)strtoul(p, &end, 16);
            p = *end == ' ' ? end + 1 : end;
        }
        ASSERT_EQ(strlen(colon + 3), n);
        uint8_t got[32];
        breaksOf(cps, n, got);
        for (size_t i = 0; i < n; i++) {
            if (got[i] != colon[3 + i] - '0') {
                fprintf(stderr, "  break case %zu (%s) boundary %zu: got %d\n", lines, line, i,
                        got[i]);
                ASSERT_TRUE(0);
            }
        }
        lines++;
    }
    ASSERT_TRUE(lines > 2000);
}

/* Hand-written cases in the spec's notation ('|' allowed, '!' mandatory before the next char),
 * checked straight against the pair rules, independently of break.cases. */
static int quietMarked;
static int checkMarked(const char *marked) {
    uint32_t cps[32];
    uint8_t want[32];
    size_t n = 0, pos = 0, len = strlen(marked);
    uint8_t pend = 0;
    while (pos < len) {
        size_t used;
        const uint32_t cp = gfxUtf8Decode((const uint8_t *)marked + pos, len - pos, &used);
        pos += used;
        if (cp == '|') {
            pend = GFX_BREAK_ALLOWED;
        } else if (cp == '!') {
            pend = GFX_BREAK_MANDATORY;
        } else {
            cps[n] = cp;
            want[n++] = pend;
            pend = 0;
        }
    }
    uint8_t got[32];
    breaksOf(cps, n, got);
    if (memcmp(got, want, n) != 0) {
        if (!quietMarked) {
            fprintf(stderr, "  FAIL break case \"%s\"\n", marked);
        }
        return 0;
    }
    return 1;
}

TEST(breakHandCases) {
    static const char *const cases[] = {
        "a |b",
        "a  |b",
        "well-|known",
        "10-20",
        "a |-b",
        "x|\xE2\x80\x94|y",
        "x|\xE2\x80\x94\xE2\x80\x94|y",
        "f(x) |g",
        "( a",
        "\xE4\xB8\x80|\xE4\xBA\x8C\xE3\x80\x82|\xE4\xB8\x89",
        "\xEF\xBC\x88\xE4\xB8\x80\xEF\xBC\x89",
        "a\xC2\xA0"
        "b |c",
        "a\xE2\x80\x8B|b",
        "a\r\n!b",
        "a\r!b",
        "a\n!\n!b",
        "a\xCC\x81 |b",
        "e.g. |x",
        "a\t|b",
        "a\xE2\x81\xA0"
        "b",
        "a-1",
        "-a",
        "ab\x0B!cd",
        "a |b |c",
        "a|\xE4\xB8\x80",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ASSERT_TRUE(checkMarked(cases[i]));
    }
    /* the checker itself must reject a wrong mark (a break where none is allowed, and a missing
     * one) so the cases above cannot pass vacuously */
    quietMarked = 1;
    const int r1 = checkMarked("ab|c"), r2 = checkMarked("a b"), r3 = checkMarked("a\nb");
    quietMarked = 0;
    ASSERT_TRUE(!r1 && !r2 && !r3);
}

TEST(breakStateStartsFresh) {
    GfxTextBreakState st;
    gfxTextBreakInit(&st);
    ASSERT_EQ((int)gfxTextBreakNext(&st, ' '), (int)GFX_BREAK_NONE);
    ASSERT_EQ((int)gfxTextBreakNext(&st, 'a'), (int)GFX_BREAK_ALLOWED);
    gfxTextBreakInit(&st); /* re-init forgets the past */
    ASSERT_EQ((int)gfxTextBreakNext(&st, 'a'), (int)GFX_BREAK_NONE);
    ASSERT_EQ((int)gfxTextBreakNext(&st, 'b'), (int)GFX_BREAK_NONE);
    gfxTextBreakInit(&st);
    ASSERT_EQ((int)gfxTextBreakNext(&st, '\n'), (int)GFX_BREAK_NONE);
    ASSERT_EQ((int)gfxTextBreakNext(&st, 'b'), (int)GFX_BREAK_MANDATORY);
}
