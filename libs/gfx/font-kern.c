/* Pair kerning (M12.3, D-154): GPOS 'kern' PairPos subtables (formats 1 and 2, directly or through
 * Extension lookups) and the legacy 'kern' table (format 0, Microsoft or Apple header). Integer-
 * only. Everything here is optional data: malformed structures are skipped, never fatal. Every
 * read is bounded by the GPOS/kern table, every loop has a hard limit (GFX_FONT_MAX_*), and every
 * binary search is `lo + (hi - lo) / 2` over a validated count, so unsorted hostile data gives a
 * wrong answer but never an out-of-bounds read or an unbounded loop. */
#include "gfx/font-internal.h"

#define TAG(a, b, c, d)                                                                            \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static uint32_t popcount8(uint32_t v) {
    v &= 0xFFu;
    uint32_t n = 0;
    while (v != 0) {
        n += v & 1u;
        v >>= 1;
    }
    return n;
}

/* ---- GPOS resolution ---------------------------------------------------------------------- */

/* Adds a lookup index to the sorted, unique set of at most GFX_FONT_MAX_KERN_LOOKUPS. */
static void setInsert(uint16_t *set, uint32_t *n, uint32_t v) {
    uint32_t i = 0;
    while (i < *n && set[i] < v) {
        i++;
    }
    if (i < *n && set[i] == v) {
        return; /* already there */
    }
    if (*n >= GFX_FONT_MAX_KERN_LOOKUPS) {
        return;
    }
    for (uint32_t j = *n; j > i; j--) {
        set[j] = set[j - 1];
    }
    set[i] = (uint16_t)v;
    (*n)++;
}

/* Collects the kern lookups of one feature index into the set. */
static void collectFeature(const uint8_t *d, uint64_t fl, uint64_t end, uint32_t featureCount,
                           uint32_t lookupCount, uint32_t idx, uint16_t *set, uint32_t *n) {
    if (idx >= featureCount) {
        return;
    }
    const uint64_t rec = fl + 2ull + 6ull * idx;
    if (!fontFits(rec, 6, end) || fontRd32(d, rec, end) != TAG('k', 'e', 'r', 'n')) {
        return;
    }
    const uint64_t ft = fl + fontRd16(d, rec + 4, end);
    if (!fontFits(ft, 4, end)) {
        return;
    }
    uint32_t cnt = fontRd16(d, ft + 2, end);
    if (cnt > GFX_FONT_MAX_GPOS_FEATURES) {
        cnt = GFX_FONT_MAX_GPOS_FEATURES;
    }
    for (uint32_t i = 0; i < cnt && fontFits(ft + 4 + 2ull * i, 2, end); i++) {
        uint32_t li = fontRd16(d, ft + 4 + 2ull * i, end);
        if (li < lookupCount) {
            setInsert(set, n, li);
        }
    }
}

/* The kern lookups of one script (its default LangSys). Returns the set size. */
static uint32_t scriptLookups(const uint8_t *d, uint64_t script, uint64_t fl, uint64_t end,
                              uint32_t featureCount, uint32_t lookupCount, uint16_t *set) {
    if (!fontFits(script, 4, end)) {
        return 0;
    }
    const uint32_t dls = fontRd16(d, script, end);
    const uint32_t nls = fontRd16(d, script + 2, end);
    uint64_t ls;
    if (dls != 0) {
        ls = script + dls;
    } else if (nls != 0 && fontFits(script + 4, 6, end)) {
        ls = script + fontRd16(d, script + 8, end);
    } else {
        return 0;
    }
    if (!fontFits(ls, 6, end)) {
        return 0;
    }
    const uint32_t req = fontRd16(d, ls + 2, end);
    uint32_t cnt = fontRd16(d, ls + 4, end);
    if (cnt > GFX_FONT_MAX_GPOS_FEATURES) {
        cnt = GFX_FONT_MAX_GPOS_FEATURES;
    }
    uint32_t n = 0;
    if (req != 0xFFFFu) {
        collectFeature(d, fl, end, featureCount, lookupCount, req, set, &n);
    }
    for (uint32_t i = 0; i < cnt && fontFits(ls + 6 + 2ull * i, 2, end); i++) {
        collectFeature(d, fl, end, featureCount, lookupCount, fontRd16(d, ls + 6 + 2ull * i, end),
                       set, &n);
    }
    return n;
}

static bool pairPosValid(const uint8_t *d, uint64_t so, uint64_t end) {
    if (!fontFits(so, 10, end)) {
        return false;
    }
    const uint32_t fmt = fontRd16(d, so, end);
    if (fmt != 1 && fmt != 2) {
        return false;
    }
    if (fmt == 2 && !fontFits(so, 16, end)) {
        return false;
    }
    const uint32_t co = fontRd16(d, so + 2, end);
    const uint32_t vf1 = fontRd16(d, so + 4, end), vf2 = fontRd16(d, so + 6, end);
    return ((vf1 | vf2) & 0xFF00u) == 0 && co != 0 && fontFits(so + co, 4, end);
}

static void resolveGpos(GfxFont *f) {
    const uint8_t *d = f->data;
    const uint64_t g = f->gposOff, end = g + f->gposLen;
    if (f->gposLen < 10 || fontRd16(d, g, end) != 1 || fontRd16(d, g + 2, end) > 1) {
        return;
    }
    const uint32_t slo = fontRd16(d, g + 4, end), flo = fontRd16(d, g + 6, end),
                   llo = fontRd16(d, g + 8, end);
    if (slo == 0 || flo == 0 || llo == 0 || slo >= f->gposLen || flo >= f->gposLen ||
        llo >= f->gposLen) {
        return;
    }
    const uint64_t sl = g + slo, fl = g + flo, ll = g + llo;
    if (!fontFits(sl, 2, end) || !fontFits(fl, 2, end) || !fontFits(ll, 2, end)) {
        return;
    }
    const uint32_t nScripts = fontRd16(d, sl, end), featureCount = fontRd16(d, fl, end),
                   lookupCount = fontRd16(d, ll, end);
    /* candidate scripts: 'latn', then 'DFLT', then the records in order (at most 16 distinct) */
    uint32_t cand[GFX_FONT_MAX_GPOS_SCRIPTS];
    uint32_t nCand = 0;
    static const uint32_t prefer[2] = {TAG('l', 'a', 't', 'n'), TAG('D', 'F', 'L', 'T')};
    for (uint32_t p = 0; p < 2; p++) {
        for (uint32_t i = 0; i < nScripts && fontFits(sl + 2 + 6ull * i, 6, end); i++) {
            if (fontRd32(d, sl + 2 + 6ull * i, end) == prefer[p]) {
                cand[nCand++] = i;
                break;
            }
        }
    }
    for (uint32_t i = 0; i < nScripts && nCand < GFX_FONT_MAX_GPOS_SCRIPTS; i++) {
        bool seen = false;
        for (uint32_t k = 0; k < nCand; k++) {
            seen = seen || cand[k] == i;
        }
        if (!seen) {
            cand[nCand++] = i;
        }
    }
    uint16_t set[GFX_FONT_MAX_KERN_LOOKUPS];
    uint32_t nSet = 0;
    for (uint32_t k = 0; k < nCand && nSet == 0; k++) {
        const uint64_t rec = sl + 2 + 6ull * cand[k];
        if (!fontFits(rec, 6, end)) {
            continue;
        }
        nSet = scriptLookups(d, sl + fontRd16(d, rec + 4, end), fl, end, featureCount, lookupCount,
                             set);
    }
    /* resolve the PairPos subtables of each lookup, in ascending lookup order */
    for (uint32_t ord = 0; ord < nSet; ord++) {
        const uint64_t lop = ll + 2ull + 2ull * set[ord];
        if (!fontFits(lop, 2, end)) {
            continue;
        }
        const uint64_t lo = ll + fontRd16(d, lop, end);
        if (!fontFits(lo, 6, end)) {
            continue;
        }
        const uint32_t type = fontRd16(d, lo, end);
        if (type != 2 && type != 9) {
            continue;
        }
        const uint32_t subCount = fontRd16(d, lo + 4, end);
        for (uint32_t k = 0; k < subCount && fontFits(lo + 6 + 2ull * k, 2, end); k++) {
            uint64_t so = lo + fontRd16(d, lo + 6 + 2ull * k, end);
            if (type == 9) {
                if (!fontFits(so, 8, end) || fontRd16(d, so, end) != 1 ||
                    fontRd16(d, so + 2, end) != 2) {
                    continue; /* also skips a nested extension (type 9) */
                }
                so += fontRd32(d, so + 4, end);
            }
            if (!pairPosValid(d, so, end)) {
                continue;
            }
            if (f->nKernSub >= GFX_FONT_MAX_KERN_SUBTABLES) {
                return;
            }
            f->kernSubOff[f->nKernSub] = (uint32_t)so;
            f->kernSubLookup[f->nKernSub] = (uint8_t)ord;
            f->nKernSub++;
        }
    }
}

/* ---- the legacy 'kern' table -------------------------------------------------------------- */

static void resolveKernTable(GfxFont *f, uint32_t kernOff, uint32_t kernLen) {
    const uint8_t *d = f->data;
    const uint64_t k = kernOff, end = k + kernLen;
    if (kernLen < 4) {
        return;
    }
    uint64_t sub;
    uint32_t nTables, hdr, pairsAt;
    bool apple;
    if (fontRd16(d, k, end) == 0) { /* Microsoft: u16 version 0, u16 nTables */
        apple = false;
        nTables = fontRd16(d, k + 2, end);
        sub = k + 4;
        hdr = 6;
        pairsAt = 14;
    } else if (kernLen >= 8 && fontRd32(d, k, end) == 0x00010000u) { /* Apple */
        apple = true;
        nTables = fontRd32(d, k + 4, end);
        sub = k + 8;
        hdr = 8;
        pairsAt = 16;
    } else {
        return;
    }
    for (uint32_t i = 0; i < nTables && i < GFX_FONT_MAX_KERN_TABLES; i++) {
        if (!fontFits(sub, hdr + 8, end)) {
            return;
        }
        uint32_t len, cov, format;
        bool ok;
        if (apple) {
            len = fontRd32(d, sub, end);
            cov = fontRd16(d, sub + 4, end);
            format = cov & 0xFFu;
            ok = (cov & 0xE000u) == 0;
        } else {
            len = fontRd16(d, sub + 2, end);
            cov = fontRd16(d, sub + 4, end);
            format = cov >> 8;
            ok = (cov & 1u) != 0 && (cov & 2u) == 0 && (cov & 4u) == 0;
        }
        if (ok && format == 0) {
            const uint32_t claimed = fontRd16(d, sub + hdr, end);
            const uint64_t pairs = sub + pairsAt;
            /* a clamp, not a rejection: the u16 length of Microsoft subtables overflows */
            const uint64_t room = pairs <= end ? (end - pairs) / 6u : 0;
            const uint32_t n = claimed < room ? claimed : (uint32_t)room;
            if (n != 0) {
                f->kernPairsOff = (uint32_t)pairs;
                f->kernPairs = n;
                return;
            }
        }
        if (len < hdr) {
            return;
        }
        sub += len;
    }
}

/* Contract: called from gfxFontInit only; never fails. */
void fontKernResolve(GfxFont *f, uint32_t kernOff, uint32_t kernLen) {
    f->nKernSub = 0;
    f->kernPairs = 0;
    if (f->gposLen != 0) {
        resolveGpos(f);
    }
    if (f->nKernSub == 0 && kernLen != 0) {
        resolveKernTable(f, kernOff, kernLen);
    }
}

/* ---- pair lookup -------------------------------------------------------------------------- */

/* Coverage index of `g` in the table at `o`, or -1. */
static int32_t coverageIndex(const uint8_t *d, uint64_t o, uint64_t end, uint32_t g) {
    if (!fontFits(o, 4, end)) {
        return -1;
    }
    const uint32_t fmt = fontRd16(d, o, end), n = fontRd16(d, o + 2, end);
    if (fmt == 1) {
        if (!fontFits(o + 4, 2ull * n, end)) {
            return -1;
        }
        uint32_t lo = 0, hi = n;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            uint32_t v = fontRd16(d, o + 4 + 2ull * mid, end);
            if (v == g) {
                return (int32_t)mid;
            }
            if (v < g) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return -1;
    }
    if (fmt == 2) {
        if (!fontFits(o + 4, 6ull * n, end)) {
            return -1;
        }
        /* the last range with start <= g */
        uint32_t lo = 0, hi = n;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (fontRd16(d, o + 4 + 6ull * mid, end) <= g) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo == 0) {
            return -1;
        }
        const uint64_t r = o + 4 + 6ull * (lo - 1);
        const uint32_t start = fontRd16(d, r, end), last = fontRd16(d, r + 2, end);
        if (g > last || start > last) {
            return -1;
        }
        return (int32_t)((fontRd16(d, r + 4, end) + (g - start)) & 0x7FFFFFFFu);
    }
    return -1;
}

/* The class of `g` in the ClassDef at `o` (0 for an absent table or an unlisted glyph). */
static uint32_t classOf(const uint8_t *d, uint64_t o, uint64_t end, uint32_t g) {
    if (o == 0 || !fontFits(o, 4, end)) {
        return 0;
    }
    const uint32_t fmt = fontRd16(d, o, end);
    if (fmt == 1) {
        const uint32_t start = fontRd16(d, o + 2, end), n = fontRd16(d, o + 4, end);
        if (g >= start && g - start < n && fontFits(o + 6 + 2ull * (g - start), 2, end)) {
            return fontRd16(d, o + 6 + 2ull * (g - start), end);
        }
        return 0;
    }
    if (fmt == 2) {
        const uint32_t n = fontRd16(d, o + 2, end);
        if (!fontFits(o + 4, 6ull * n, end)) {
            return 0;
        }
        uint32_t lo = 0, hi = n;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (fontRd16(d, o + 4 + 6ull * mid, end) <= g) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo == 0) {
            return 0;
        }
        const uint64_t r = o + 4 + 6ull * (lo - 1);
        return g <= fontRd16(d, r + 2, end) ? fontRd16(d, r + 4, end) : 0;
    }
    return 0;
}

/* value1 XAdvance of the ValueRecord at `o` given its format (0 if absent). */
static int32_t valueXAdvance(const uint8_t *d, uint64_t o, uint64_t end, uint32_t vf) {
    if ((vf & 4u) == 0) {
        return 0;
    }
    return fontRdS16(d, o + 2ull * popcount8(vf & 3u), end);
}

/* Does the PairPos subtable match (l, r)? On a match *adv gets the first glyph's XAdvance. */
static bool pairMatch(const uint8_t *d, uint64_t so, uint64_t end, uint32_t l, uint32_t r,
                      int32_t *adv) {
    const uint32_t fmt = fontRd16(d, so, end);
    const uint32_t vf1 = fontRd16(d, so + 4, end), vf2 = fontRd16(d, so + 6, end);
    const int32_t ci = coverageIndex(d, so + fontRd16(d, so + 2, end), end, l);
    if (ci < 0) {
        return false;
    }
    const uint32_t s1 = 2u * popcount8(vf1), s2 = 2u * popcount8(vf2);
    if (fmt == 1) {
        const uint32_t nSets = fontRd16(d, so + 8, end);
        if ((uint32_t)ci >= nSets || !fontFits(so + 10, 2ull * nSets, end)) {
            return false;
        }
        const uint64_t ps = so + fontRd16(d, so + 10 + 2ull * (uint32_t)ci, end);
        if (!fontFits(ps, 2, end)) {
            return false;
        }
        const uint32_t n = fontRd16(d, ps, end), rec = 2u + s1 + s2;
        if (!fontFits(ps + 2, (uint64_t)n * rec, end)) {
            return false;
        }
        uint32_t lo = 0, hi = n;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            const uint64_t at = ps + 2 + (uint64_t)mid * rec;
            uint32_t v = fontRd16(d, at, end);
            if (v == r) {
                *adv = valueXAdvance(d, at + 2, end, vf1);
                return true;
            }
            if (v < r) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return false;
    }
    /* format 2: class based; a covered first glyph with in-range classes ends the lookup even
     * when the value is 0 */
    const uint32_t cd1 = fontRd16(d, so + 8, end), cd2 = fontRd16(d, so + 10, end);
    const uint32_t n1 = fontRd16(d, so + 12, end), n2 = fontRd16(d, so + 14, end);
    const uint32_t c1 = classOf(d, cd1 != 0 ? so + cd1 : 0, end, l);
    const uint32_t c2 = classOf(d, cd2 != 0 ? so + cd2 : 0, end, r);
    if (c1 >= n1 || c2 >= n2) {
        return false;
    }
    const uint64_t rec = so + 16 + ((uint64_t)c1 * n2 + c2) * (uint64_t)(s1 + s2);
    if (!fontFits(rec, s1 + s2, end)) {
        return false;
    }
    *adv = valueXAdvance(d, rec, end, vf1);
    return true;
}

/* Contract: pure, never sleeps, never fails. */
int32_t gfxFontKernUnits(const GfxFont *f, uint16_t left, uint16_t right) {
    if (f == NULL || f->data == NULL) {
        return 0;
    }
    if (f->nKernSub != 0) {
        const uint64_t end = (uint64_t)f->gposOff + f->gposLen;
        int32_t total = 0;
        int32_t lastOrd = -1;
        for (uint32_t i = 0; i < f->nKernSub; i++) {
            if ((int32_t)f->kernSubLookup[i] == lastOrd) {
                continue; /* this lookup already matched: the first matching subtable wins */
            }
            int32_t adv;
            if (pairMatch(f->data, f->kernSubOff[i], end, left, right, &adv)) {
                total += adv;
                lastOrd = (int32_t)f->kernSubLookup[i];
            }
        }
        return total;
    }
    if (f->kernPairs != 0) {
        const uint64_t end = (uint64_t)f->size;
        const uint32_t key = ((uint32_t)left << 16) | right;
        uint32_t lo = 0, hi = f->kernPairs;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            const uint64_t at = f->kernPairsOff + 6ull * mid;
            const uint32_t v = (fontRd16(f->data, at, end) << 16) | fontRd16(f->data, at + 2, end);
            if (v == key) {
                return fontRdS16(f->data, at + 4, end);
            }
            if (v < key) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
    }
    return 0;
}

GfxFontKernSource gfxFontKernSource(const GfxFont *f) {
    if (f == NULL || f->data == NULL) {
        return GFX_FONT_KERN_NONE;
    }
    if (f->nKernSub != 0) {
        return GFX_FONT_KERN_GPOS;
    }
    return f->kernPairs != 0 ? GFX_FONT_KERN_TABLE : GFX_FONT_KERN_NONE;
}
