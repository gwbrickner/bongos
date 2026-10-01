/* Simplified UAX #14 line breaking (M12.3, D-156). The class tables, the invisible set and the
 * 16 pair rules are specified in docs/specs/gfx-text.md; the rule numbers below refer to it. */
#include "gfx/gfx-text.h"

#define NONE_CLASS ((uint8_t)GFX_LB_COUNT)

typedef struct {
    uint32_t lo, hi;
    uint8_t cls;
} LbRange;

/* Non-ASCII classes other than ID: sorted by `lo`, non-overlapping (checked by a host test). */
static const LbRange SPECIFIC[] = {
    {0x0080, 0x0084, GFX_LB_CM},   {0x0085, 0x0085, GFX_LB_NL},   {0x0086, 0x009F, GFX_LB_CM},
    {0x00A0, 0x00A0, GFX_LB_GL},   {0x00AD, 0x00AD, GFX_LB_BA},   {0x0300, 0x034E, GFX_LB_CM},
    {0x034F, 0x034F, GFX_LB_GL},   {0x0350, 0x036F, GFX_LB_CM},   {0x1680, 0x1680, GFX_LB_BA},
    {0x1AB0, 0x1AFF, GFX_LB_CM},   {0x1DC0, 0x1DFF, GFX_LB_CM},   {0x2000, 0x2006, GFX_LB_BA},
    {0x2007, 0x2007, GFX_LB_GL},   {0x2008, 0x200A, GFX_LB_BA},   {0x200B, 0x200B, GFX_LB_ZW},
    {0x200C, 0x200F, GFX_LB_CM},   {0x2010, 0x2010, GFX_LB_BA},   {0x2011, 0x2011, GFX_LB_GL},
    {0x2012, 0x2013, GFX_LB_BA},   {0x2014, 0x2014, GFX_LB_B2},   {0x2018, 0x2018, GFX_LB_OP},
    {0x2019, 0x2019, GFX_LB_NS},   {0x201C, 0x201C, GFX_LB_OP},   {0x201D, 0x201D, GFX_LB_NS},
    {0x2028, 0x2029, GFX_LB_BK},   {0x202A, 0x202E, GFX_LB_CM},   {0x202F, 0x202F, GFX_LB_GL},
    {0x205F, 0x205F, GFX_LB_BA},   {0x2060, 0x2060, GFX_LB_WJ},   {0x20D0, 0x20FF, GFX_LB_CM},
    {0x2E3A, 0x2E3B, GFX_LB_B2},   {0x3000, 0x3000, GFX_LB_BA},   {0x3001, 0x3002, GFX_LB_NS},
    {0x3005, 0x3005, GFX_LB_NS},   {0x3008, 0x3008, GFX_LB_OP},   {0x3009, 0x3009, GFX_LB_NS},
    {0x300A, 0x300A, GFX_LB_OP},   {0x300B, 0x300B, GFX_LB_NS},   {0x300C, 0x300C, GFX_LB_OP},
    {0x300D, 0x300D, GFX_LB_NS},   {0x300E, 0x300E, GFX_LB_OP},   {0x300F, 0x300F, GFX_LB_NS},
    {0x3010, 0x3010, GFX_LB_OP},   {0x3011, 0x3011, GFX_LB_NS},   {0x3014, 0x3014, GFX_LB_OP},
    {0x3015, 0x3015, GFX_LB_NS},   {0x3016, 0x3016, GFX_LB_OP},   {0x3017, 0x3017, GFX_LB_NS},
    {0x3018, 0x3018, GFX_LB_OP},   {0x3019, 0x3019, GFX_LB_NS},   {0x301A, 0x301A, GFX_LB_OP},
    {0x301B, 0x301C, GFX_LB_NS},   {0x301D, 0x301D, GFX_LB_OP},   {0x301E, 0x301F, GFX_LB_NS},
    {0x302A, 0x302F, GFX_LB_CM},   {0x303B, 0x303B, GFX_LB_NS},   {0x3041, 0x3041, GFX_LB_NS},
    {0x3043, 0x3043, GFX_LB_NS},   {0x3045, 0x3045, GFX_LB_NS},   {0x3047, 0x3047, GFX_LB_NS},
    {0x3049, 0x3049, GFX_LB_NS},   {0x3063, 0x3063, GFX_LB_NS},   {0x3083, 0x3083, GFX_LB_NS},
    {0x3085, 0x3085, GFX_LB_NS},   {0x3087, 0x3087, GFX_LB_NS},   {0x308E, 0x308E, GFX_LB_NS},
    {0x3095, 0x3096, GFX_LB_NS},   {0x3099, 0x309A, GFX_LB_CM},   {0x309B, 0x309E, GFX_LB_NS},
    {0x30A0, 0x30A1, GFX_LB_NS},   {0x30A3, 0x30A3, GFX_LB_NS},   {0x30A5, 0x30A5, GFX_LB_NS},
    {0x30A7, 0x30A7, GFX_LB_NS},   {0x30A9, 0x30A9, GFX_LB_NS},   {0x30C3, 0x30C3, GFX_LB_NS},
    {0x30E3, 0x30E3, GFX_LB_NS},   {0x30E5, 0x30E5, GFX_LB_NS},   {0x30E7, 0x30E7, GFX_LB_NS},
    {0x30EE, 0x30EE, GFX_LB_NS},   {0x30F5, 0x30F6, GFX_LB_NS},   {0x30FB, 0x30FE, GFX_LB_NS},
    {0x31F0, 0x31FF, GFX_LB_NS},   {0xFE00, 0xFE0F, GFX_LB_CM},   {0xFE20, 0xFE2F, GFX_LB_CM},
    {0xFEFF, 0xFEFF, GFX_LB_WJ},   {0xFF01, 0xFF01, GFX_LB_NS},   {0xFF08, 0xFF08, GFX_LB_OP},
    {0xFF09, 0xFF09, GFX_LB_NS},   {0xFF0C, 0xFF0C, GFX_LB_NS},   {0xFF0E, 0xFF0E, GFX_LB_NS},
    {0xFF1A, 0xFF1B, GFX_LB_NS},   {0xFF1F, 0xFF1F, GFX_LB_NS},   {0xFF3B, 0xFF3B, GFX_LB_OP},
    {0xFF3D, 0xFF3D, GFX_LB_NS},   {0xFF5B, 0xFF5B, GFX_LB_OP},   {0xFF5D, 0xFF5D, GFX_LB_NS},
    {0xFF5F, 0xFF5F, GFX_LB_OP},   {0xFF60, 0xFF61, GFX_LB_NS},   {0xFF62, 0xFF62, GFX_LB_OP},
    {0xFF63, 0xFF64, GFX_LB_NS},   {0xE0001, 0xE0001, GFX_LB_CM}, {0xE0020, 0xE007F, GFX_LB_CM},
    {0xE0100, 0xE01EF, GFX_LB_CM},
};

/* Ideographic ranges (class ID): sorted, non-overlapping. SPECIFIC wins where they overlap. */
static const LbRange IDEO[] = {
    {0x2E80, 0x2FFF, GFX_LB_ID},   {0x3000, 0x31FF, GFX_LB_ID},   {0x3200, 0x4DBF, GFX_LB_ID},
    {0x4E00, 0x9FFF, GFX_LB_ID},   {0xA000, 0xA4CF, GFX_LB_ID},   {0xAC00, 0xD7A3, GFX_LB_ID},
    {0xF900, 0xFAFF, GFX_LB_ID},   {0xFE30, 0xFE4F, GFX_LB_ID},   {0xFF00, 0xFF60, GFX_LB_ID},
    {0xFFE0, 0xFFE6, GFX_LB_ID},   {0x1F000, 0x1FAFF, GFX_LB_ID}, {0x20000, 0x2FFFD, GFX_LB_ID},
    {0x30000, 0x3FFFD, GFX_LB_ID},
};

/* Never drawn, no advance: sorted, non-overlapping. */
static const LbRange INVISIBLE[] = {
    {0x0000, 0x001F, 0},   {0x007F, 0x009F, 0},   {0x00AD, 0x00AD, 0},   {0x034F, 0x034F, 0},
    {0x061C, 0x061C, 0},   {0x115F, 0x1160, 0},   {0x17B4, 0x17B5, 0},   {0x180B, 0x180F, 0},
    {0x200B, 0x200F, 0},   {0x2028, 0x202E, 0},   {0x2060, 0x206F, 0},   {0x3164, 0x3164, 0},
    {0xFE00, 0xFE0F, 0},   {0xFEFF, 0xFEFF, 0},   {0xFFA0, 0xFFA0, 0},   {0xFFF9, 0xFFFB, 0},
    {0x1BCA0, 0x1BCA3, 0}, {0x1D173, 0x1D17A, 0}, {0xE0000, 0xE0FFF, 0},
};

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

/* Index of the range containing cp, or -1. */
static int findRange(const LbRange *t, size_t n, uint32_t cp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (cp < t[mid].lo) {
            hi = mid;
        } else if (cp > t[mid].hi) {
            lo = mid + 1;
        } else {
            return (int)mid;
        }
    }
    return -1;
}

static GfxLineClass asciiClass(uint32_t cp) {
    switch (cp) {
        case 0x09:
        case 0x7C:
            return GFX_LB_BA;
        case 0x0A:
            return GFX_LB_LF;
        case 0x0B:
        case 0x0C:
            return GFX_LB_BK;
        case 0x0D:
            return GFX_LB_CR;
        case 0x20:
            return GFX_LB_SP;
        case 0x28:
        case 0x5B:
        case 0x7B:
            return GFX_LB_OP;
        case 0x21:
        case 0x29:
        case 0x2C:
        case 0x2E:
        case 0x3A:
        case 0x3B:
        case 0x3F:
        case 0x5D:
        case 0x7D:
            return GFX_LB_NS;
        case 0x2D:
            return GFX_LB_HY;
        default:
            break;
    }
    if (cp >= 0x30 && cp <= 0x39) {
        return GFX_LB_NU;
    }
    return cp < 0x20 || cp == 0x7F ? GFX_LB_CM : GFX_LB_AL;
}

/* Contract: pure; AL for anything outside the tables. */
GfxLineClass gfxTextLineClass(uint32_t cp) {
    if (cp < 0x80) {
        return asciiClass(cp);
    }
    int i = findRange(SPECIFIC, COUNTOF(SPECIFIC), cp);
    if (i >= 0) {
        return (GfxLineClass)SPECIFIC[i].cls;
    }
    return findRange(IDEO, COUNTOF(IDEO), cp) >= 0 ? GFX_LB_ID : GFX_LB_AL;
}

/* Contract: pure. */
bool gfxTextIsInvisible(uint32_t cp) {
    return findRange(INVISIBLE, COUNTOF(INVISIBLE), cp) >= 0;
}

void gfxTextBreakInit(GfxTextBreakState *st) {
    st->prev2 = NONE_CLASS;
    st->prev = NONE_CLASS;
    st->lastNonSp = NONE_CLASS;
    st->started = false;
}

static bool isOneOf(uint32_t c, const GfxLineClass *set, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (c == (uint32_t)set[i]) {
            return true;
        }
    }
    return false;
}
#define IN(c, ...)                                                                                 \
    isOneOf((c), (const GfxLineClass[]){__VA_ARGS__},                                              \
            sizeof((const GfxLineClass[]){__VA_ARGS__}) / sizeof(GfxLineClass))

static GfxBreak pairRule(const GfxTextBreakState *st, uint32_t cur) {
    const uint32_t p2 = st->prev2, p = st->prev, last = st->lastNonSp;
    if (IN(p, GFX_LB_BK, GFX_LB_LF, GFX_LB_NL) || (p == GFX_LB_CR && cur != GFX_LB_LF)) {
        return GFX_BREAK_MANDATORY; /* 2 */
    }
    if (p == GFX_LB_CR && cur == GFX_LB_LF) {
        return GFX_BREAK_NONE; /* 3 */
    }
    if (IN(cur, GFX_LB_BK, GFX_LB_CR, GFX_LB_LF, GFX_LB_NL, GFX_LB_SP, GFX_LB_ZW)) {
        return GFX_BREAK_NONE; /* 4 */
    }
    if (last == GFX_LB_ZW) {
        return GFX_BREAK_ALLOWED; /* 5 */
    }
    if (p == GFX_LB_WJ || cur == GFX_LB_WJ) {
        return GFX_BREAK_NONE; /* 6 */
    }
    if (p == GFX_LB_GL || (cur == GFX_LB_GL && !IN(p, GFX_LB_SP, GFX_LB_BA, GFX_LB_HY))) {
        return GFX_BREAK_NONE; /* 7 */
    }
    if (cur == GFX_LB_NS || cur == GFX_LB_CM) {
        return GFX_BREAK_NONE; /* 8 */
    }
    if (last == GFX_LB_OP) {
        return GFX_BREAK_NONE; /* 9 */
    }
    if (last == GFX_LB_B2 && cur == GFX_LB_B2) {
        return GFX_BREAK_NONE; /* 10 */
    }
    if (p == GFX_LB_SP) {
        return GFX_BREAK_ALLOWED; /* 11 */
    }
    if (cur == GFX_LB_BA || cur == GFX_LB_HY) {
        return GFX_BREAK_NONE; /* 12 */
    }
    if (p == GFX_LB_HY && (cur == GFX_LB_NU ||
                           (cur == GFX_LB_AL &&
                            (p2 == NONE_CLASS || IN(p2, GFX_LB_BK, GFX_LB_CR, GFX_LB_LF, GFX_LB_NL,
                                                    GFX_LB_SP, GFX_LB_ZW, GFX_LB_GL))))) {
        return GFX_BREAK_NONE; /* 13 */
    }
    if (IN(p, GFX_LB_BA, GFX_LB_HY, GFX_LB_B2) || cur == GFX_LB_B2) {
        return GFX_BREAK_ALLOWED; /* 14 */
    }
    if (p == GFX_LB_ID || cur == GFX_LB_ID) {
        return GFX_BREAK_ALLOWED; /* 15 */
    }
    return GFX_BREAK_NONE; /* 16 */
}

/* Contract: pure apart from *st, which must have been through gfxTextBreakInit. */
GfxBreak gfxTextBreakNext(GfxTextBreakState *st, uint32_t cp) {
    const GfxLineClass cur = gfxTextLineClass(cp);
    GfxBreak r = GFX_BREAK_NONE; /* 1: nothing before the first codepoint */
    if (st->started) {
        r = pairRule(st, cur);
    }
    st->started = true;
    st->prev2 = st->prev;
    st->prev = (uint8_t)cur;
    if (cur != GFX_LB_SP) {
        st->lastNonSp = (uint8_t)cur;
    }
    return r;
}
