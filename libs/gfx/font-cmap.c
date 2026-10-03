/* cmap: subtable choice (D-151) and format 4 / 12 lookups. Integer-only, every read bounded by the
 * end of the cmap table (the format-4 `length` field is ignored: it overflows in real fonts). */
#include "gfx/font-internal.h"

/* Rank of a (platform, encoding, format) triple; 0 = not usable. Higher is better. */
static int cmapRank(uint32_t plat, uint32_t enc, uint32_t fmt) {
    if (fmt == 12) {
        if (plat == 3 && enc == 10) {
            return 5;
        }
        if (plat == 0 && (enc == 4 || enc == 6)) {
            return 4;
        }
    } else if (fmt == 4) {
        if (plat == 3 && enc == 1) {
            return 3;
        }
        if (plat == 0 && enc <= 3) {
            return 2;
        }
        if (plat == 3 && enc == 0) {
            return 1;
        }
    }
    return 0;
}

/* Contract: called from gfxFontInit only; pure. INVALID for a malformed table or a malformed
 * chosen subtable, UNSUPPORTED when no usable Unicode subtable exists. */
Status fontCmapSelect(GfxFont *f) {
    const uint8_t *d = f->data;
    const uint64_t end = (uint64_t)f->cmapOff + f->cmapLen;
    if (f->cmapLen < 4) {
        return STATUS_ERR_INVALID;
    }
    uint32_t n = fontRd16(d, f->cmapOff + 2, end);
    if (!fontFits(f->cmapOff + 4ull, 8ull * n, end)) {
        return STATUS_ERR_INVALID;
    }
    int best = 0;
    uint64_t bestSub = 0;
    uint32_t bestFmt = 0;
    uint32_t bestPlat = 0, bestEnc = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t rec = f->cmapOff + 4ull + 8ull * i;
        uint32_t plat = fontRd16(d, rec, end), enc = fontRd16(d, rec + 2, end);
        uint64_t sub = (uint64_t)f->cmapOff + fontRd32(d, rec + 4, end);
        if (!fontFits(sub, 2, end)) {
            continue; /* a record pointing outside the table is not a candidate */
        }
        uint32_t fmt = fontRd16(d, sub, end);
        int r = cmapRank(plat, enc, fmt);
        if (r > best) { /* ties go to the first record */
            best = r;
            bestSub = sub;
            bestFmt = fmt;
            bestPlat = plat;
            bestEnc = enc;
        }
    }
    if (best == 0) {
        return STATUS_ERR_UNSUPPORTED;
    }
    if (bestFmt == 4) {
        uint32_t segX2 = fontRd16(d, bestSub + 6, end);
        if (segX2 < 2 || (segX2 & 1u) != 0 || !fontFits(bestSub, 16ull + 4ull * segX2, end)) {
            return STATUS_ERR_INVALID;
        }
    } else {
        uint32_t groups = fontRd32(d, bestSub + 12, end);
        if (!fontFits(bestSub, 16ull + 12ull * groups, end)) {
            return STATUS_ERR_INVALID;
        }
    }
    f->cmapSub = (uint32_t)bestSub;
    f->cmapFormat = (uint16_t)bestFmt;
    f->cmapSymbol = bestPlat == 3 && bestEnc == 0;
    return STATUS_OK;
}

static uint32_t lookupFormat4(const GfxFont *f, uint32_t cp) {
    if (cp > 0xFFFFu) {
        return 0;
    }
    const uint8_t *d = f->data;
    const uint64_t end = (uint64_t)f->cmapOff + f->cmapLen;
    const uint64_t sub = f->cmapSub;
    const uint32_t segX2 = fontRd16(d, sub + 6, end);
    const uint32_t segs = segX2 / 2;
    const uint64_t endArr = sub + 14, startArr = sub + 16 + segX2,
                   deltaArr = sub + 16 + 2ull * segX2, rangeArr = sub + 16 + 3ull * segX2;
    /* first segment with endCode >= cp */
    uint32_t lo = 0, hi = segs;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (fontRd16(d, endArr + 2ull * mid, end) >= cp) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    if (lo >= segs) {
        return 0;
    }
    const uint32_t i = lo;
    const uint32_t start = fontRd16(d, startArr + 2ull * i, end);
    if (start > cp) {
        return 0;
    }
    const uint32_t delta = fontRd16(d, deltaArr + 2ull * i, end);
    const uint32_t ro = fontRd16(d, rangeArr + 2ull * i, end);
    if (ro == 0) {
        return (cp + delta) & 0xFFFFu;
    }
    /* idRangeOffset is relative to that idRangeOffset element itself */
    const uint64_t addr = rangeArr + 2ull * i + ro + 2ull * (cp - start);
    if (!fontFits(addr, 2, end)) {
        return 0;
    }
    const uint32_t g0 = fontRd16(d, addr, end);
    return g0 == 0 ? 0 : (g0 + delta) & 0xFFFFu;
}

static uint32_t lookupFormat12(const GfxFont *f, uint32_t cp) {
    const uint8_t *d = f->data;
    const uint64_t end = (uint64_t)f->cmapOff + f->cmapLen;
    const uint64_t sub = f->cmapSub;
    const uint32_t groups = fontRd32(d, sub + 12, end);
    /* last group with startCharCode <= cp */
    uint32_t lo = 0, hi = groups;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (fontRd32(d, sub + 16 + 12ull * mid, end) <= cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return 0;
    }
    const uint64_t g = sub + 16 + 12ull * (lo - 1);
    const uint32_t start = fontRd32(d, g, end), last = fontRd32(d, g + 4, end);
    if (start > last || cp > last) {
        return 0;
    }
    uint64_t gid = (uint64_t)fontRd32(d, g + 8, end) + (cp - start);
    return gid > 0xFFFFu ? 0u : (uint32_t)gid;
}

/* Contract: pure, never sleeps, never fails. */
uint16_t gfxFontGlyphIndex(const GfxFont *f, uint32_t cp) {
    if (f == NULL || f->data == NULL || cp > 0x10FFFFu) {
        return 0;
    }
    uint32_t g = f->cmapFormat == 12 ? lookupFormat12(f, cp) : lookupFormat4(f, cp);
    if (g == 0 && f->cmapSymbol && cp <= 0xFFu) {
        g = lookupFormat4(f, 0xF000u | cp);
    }
    return g >= f->numGlyphs ? 0 : (uint16_t)g;
}
