/* sfnt directory, head/hhea/maxp/OS-2/hmtx/loca, metrics and advances (M12.3, D-151/D-152).
 * Integer-only; every read is bounded by the table it belongs to. */
#include "gfx/font-internal.h"

#include <string.h>

#define TAG(a, b, c, d)                                                                            \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

enum {
    TAG_HEAD = TAG('h', 'e', 'a', 'd'),
    TAG_HHEA = TAG('h', 'h', 'e', 'a'),
    TAG_MAXP = TAG('m', 'a', 'x', 'p'),
    TAG_HMTX = TAG('h', 'm', 't', 'x'),
    TAG_CMAP = TAG('c', 'm', 'a', 'p'),
    TAG_LOCA = TAG('l', 'o', 'c', 'a'),
    TAG_GLYF = TAG('g', 'l', 'y', 'f'),
    TAG_KERN = TAG('k', 'e', 'r', 'n'),
    TAG_GPOS = TAG('G', 'P', 'O', 'S'),
    TAG_OS2 = TAG('O', 'S', '/', '2'),
    TAG_CFF = TAG('C', 'F', 'F', ' '),
    TAG_CFF2 = TAG('C', 'F', 'F', '2'),
};

typedef struct {
    uint32_t off, len;
    bool present;
} TableRef;

/* A sorted-ish set of the tables we know; the first record for a tag wins. */
enum {
    T_HEAD,
    T_HHEA,
    T_MAXP,
    T_HMTX,
    T_CMAP,
    T_LOCA,
    T_GLYF,
    T_KERN,
    T_GPOS,
    T_OS2,
    T_CFF,
    T_COUNT
};

static int tableSlot(uint32_t tag) {
    switch (tag) {
        case TAG_HEAD:
            return T_HEAD;
        case TAG_HHEA:
            return T_HHEA;
        case TAG_MAXP:
            return T_MAXP;
        case TAG_HMTX:
            return T_HMTX;
        case TAG_CMAP:
            return T_CMAP;
        case TAG_LOCA:
            return T_LOCA;
        case TAG_GLYF:
            return T_GLYF;
        case TAG_KERN:
            return T_KERN;
        case TAG_GPOS:
            return T_GPOS;
        case TAG_OS2:
            return T_OS2;
        case TAG_CFF:
        case TAG_CFF2:
            return T_CFF;
        default:
            return -1;
    }
}

/* Contract: pure, allocation-free, never sleeps; see gfx-font.h. */
Status gfxFontInit(GfxFont *f, const uint8_t *data, size_t size) {
    if (f == NULL) {
        return STATUS_ERR_INVALID;
    }
    memset(f, 0, sizeof *f);
    if (data == NULL || size < 12) {
        return STATUS_ERR_INVALID;
    }
    if (size > GFX_FONT_MAX_FILE_BYTES) {
        return STATUS_ERR_UNSUPPORTED;
    }
    const uint64_t n = size;
    uint32_t ver = fontRd32(data, 0, n);
    if (ver != 0x00010000u && ver != TAG('t', 'r', 'u', 'e')) {
        return STATUS_ERR_UNSUPPORTED; /* OTTO (CFF), ttcf, wOFF, wOF2, anything else */
    }
    uint32_t numTables = fontRd16(data, 4, n);
    if (!fontFits(12, 16ull * numTables, n)) {
        return STATUS_ERR_INVALID;
    }

    TableRef t[T_COUNT];
    memset(t, 0, sizeof t);
    for (uint32_t i = 0; i < numTables; i++) {
        uint64_t rec = 12 + 16ull * i;
        int slot = tableSlot(fontRd32(data, rec, n));
        if (slot < 0 || t[slot].present) {
            continue; /* unknown tag, or a duplicate: the first record wins */
        }
        uint32_t off = fontRd32(data, rec + 8, n), len = fontRd32(data, rec + 12, n);
        if (!fontFits(off, len, n)) {
            return STATUS_ERR_INVALID;
        }
        t[slot].off = off;
        t[slot].len = len;
        t[slot].present = true;
    }

    if (!t[T_GLYF].present || !t[T_LOCA].present) {
        return t[T_CFF].present ? STATUS_ERR_UNSUPPORTED : STATUS_ERR_INVALID;
    }
    if (!t[T_HEAD].present || !t[T_HHEA].present || !t[T_MAXP].present || !t[T_HMTX].present ||
        !t[T_CMAP].present) {
        return STATUS_ERR_INVALID;
    }

    /* head */
    const uint64_t headEnd = (uint64_t)t[T_HEAD].off + t[T_HEAD].len;
    const uint32_t head = t[T_HEAD].off;
    if (t[T_HEAD].len < 54 || fontRd32(data, head + 12, headEnd) != 0x5F0F3CF5u) {
        return STATUS_ERR_INVALID;
    }
    uint32_t upem = fontRd16(data, head + 18, headEnd);
    if (upem < 16 || upem > 16384) {
        return STATUS_ERR_INVALID;
    }
    uint32_t locFmt = fontRd16(data, head + 50, headEnd);
    if (locFmt > 1) {
        return STATUS_ERR_INVALID;
    }
    if (fontRd16(data, head + 52, headEnd) != 0) {
        return STATUS_ERR_UNSUPPORTED;
    }

    /* maxp */
    const uint64_t maxpEnd = (uint64_t)t[T_MAXP].off + t[T_MAXP].len;
    if (t[T_MAXP].len < 6) {
        return STATUS_ERR_INVALID;
    }
    uint32_t numGlyphs = fontRd16(data, (uint64_t)t[T_MAXP].off + 4, maxpEnd);
    if (numGlyphs < 1) {
        return STATUS_ERR_INVALID;
    }

    /* hhea + hmtx */
    const uint64_t hheaEnd = (uint64_t)t[T_HHEA].off + t[T_HHEA].len;
    if (t[T_HHEA].len < 36) {
        return STATUS_ERR_INVALID;
    }
    uint32_t nHM = fontRd16(data, (uint64_t)t[T_HHEA].off + 34, hheaEnd);
    if (nHM == 0) {
        return STATUS_ERR_INVALID;
    }
    if (nHM > numGlyphs) {
        nHM = numGlyphs;
    }
    if (t[T_HMTX].len < 4ull * nHM) {
        return STATUS_ERR_INVALID;
    }

    /* loca */
    if (t[T_LOCA].len < (uint64_t)(numGlyphs + 1) * (locFmt != 0 ? 4u : 2u)) {
        return STATUS_ERR_INVALID;
    }

    /* Metrics (D-152). */
    int32_t asc, desc, gap;
    const uint32_t os2 = t[T_OS2].off;
    const uint64_t os2End = (uint64_t)t[T_OS2].off + t[T_OS2].len;
    const bool haveOs2 = t[T_OS2].present && t[T_OS2].len >= 78;
    const uint32_t hhea = t[T_HHEA].off;
    const int32_t hheaAsc = fontRdS16(data, hhea + 4, hheaEnd);
    const int32_t hheaDesc = fontRdS16(data, hhea + 6, hheaEnd);
    if (haveOs2 && (fontRd16(data, os2 + 62, os2End) & 0x80u) != 0) {
        asc = fontRdS16(data, os2 + 68, os2End);
        desc = fontRdS16(data, os2 + 70, os2End);
        gap = fontRdS16(data, os2 + 72, os2End);
    } else if (hheaAsc != 0 || hheaDesc != 0) {
        asc = hheaAsc;
        desc = hheaDesc;
        gap = fontRdS16(data, hhea + 8, hheaEnd);
    } else if (haveOs2 &&
               (fontRdS16(data, os2 + 68, os2End) != 0 || fontRdS16(data, os2 + 70, os2End) != 0)) {
        asc = fontRdS16(data, os2 + 68, os2End);
        desc = fontRdS16(data, os2 + 70, os2End);
        gap = fontRdS16(data, os2 + 72, os2End);
    } else {
        asc = fontRdS16(data, head + 42, headEnd);
        desc = fontRdS16(data, head + 38, headEnd);
        if (desc > 0) {
            desc = 0; /* a box that starts above the baseline has no descender */
        }
        gap = 0;
    }
    if (asc < 0) {
        asc = 0;
    }
    if (desc > 0) {
        desc = -desc;
    }
    if (gap < 0) {
        gap = 0;
    }

    f->data = data;
    f->size = (uint32_t)size;
    f->unitsPerEm = (uint16_t)upem;
    f->numGlyphs = (uint16_t)numGlyphs;
    f->numHMetrics = (uint16_t)nHM;
    f->locaLong = locFmt != 0;
    f->ascender = (int16_t)asc;
    f->descender = (int16_t)desc;
    f->lineGap = (int16_t)gap;
    f->glyfOff = t[T_GLYF].off;
    f->glyfLen = t[T_GLYF].len;
    f->locaOff = t[T_LOCA].off;
    f->locaLen = t[T_LOCA].len;
    f->hmtxOff = t[T_HMTX].off;
    f->hmtxLen = t[T_HMTX].len;
    f->cmapOff = t[T_CMAP].off;
    f->cmapLen = t[T_CMAP].len;
    if (t[T_GPOS].present) {
        f->gposOff = t[T_GPOS].off;
        f->gposLen = t[T_GPOS].len;
    }

    Status st = fontCmapSelect(f);
    if (st != STATUS_OK) {
        memset(f, 0, sizeof *f);
        return st;
    }
    fontKernResolve(f, t[T_KERN].present ? t[T_KERN].off : 0,
                    t[T_KERN].present ? t[T_KERN].len : 0);
    return STATUS_OK;
}

/* Contract: pure, never sleeps. */
uint16_t gfxFontAdvanceUnits(const GfxFont *f, uint16_t glyph) {
    if (f == NULL || f->data == NULL || glyph >= f->numGlyphs) {
        return 0;
    }
    uint32_t idx = glyph < f->numHMetrics ? glyph : (uint32_t)f->numHMetrics - 1u;
    return (uint16_t)fontRd16(f->data, f->hmtxOff + 4ull * idx, (uint64_t)f->hmtxOff + f->hmtxLen);
}

/* Contract: pure, never sleeps. Round half away from zero, saturated to int32. */
int32_t gfxFontScaleQ6(const GfxFont *f, int32_t units, uint32_t sizeQ6) {
    if (f == NULL || f->unitsPerEm == 0) {
        return 0;
    }
    const int64_t upem = f->unitsPerEm;
    const int64_t n = (int64_t)units * (int64_t)sizeQ6;
    int64_t r = n >= 0 ? (n + upem / 2) / upem : -((-n + upem / 2) / upem);
    if (r > INT32_MAX) {
        r = INT32_MAX;
    } else if (r < INT32_MIN) {
        r = INT32_MIN;
    }
    return (int32_t)r;
}

static int64_t ceilDiv64(int64_t a, int64_t b) { /* b > 0, a >= 0 */
    return (a + b - 1) / b;
}

/* Contract: pure, never sleeps. */
Status gfxFontMetrics(const GfxFont *f, uint32_t sizeQ6, GfxFontMetricsPx *out) {
    if (f == NULL || f->data == NULL || f->unitsPerEm == 0 || out == NULL ||
        sizeQ6 < GFX_FONT_MIN_SIZE_Q6 || sizeQ6 > GFX_FONT_MAX_SIZE_Q6) {
        return STATUS_ERR_INVALID;
    }
    const int64_t den = (int64_t)f->unitsPerEm * 64;
    out->ascent = (int32_t)ceilDiv64((int64_t)f->ascender * sizeQ6, den);
    out->descent = (int32_t)ceilDiv64(-(int64_t)f->descender * sizeQ6, den);
    out->lineGap = (int32_t)(((int64_t)f->lineGap * sizeQ6 + den / 2) / den);
    int32_t lh = out->ascent + out->descent + out->lineGap;
    out->lineHeight = lh < 1 ? 1 : lh;
    return STATUS_OK;
}

/* Contract: pure, never sleeps. */
Status fontGlyphRange(const GfxFont *f, uint16_t glyph, uint32_t *start, uint32_t *end) {
    if (glyph >= f->numGlyphs) {
        return STATUS_ERR_INVALID;
    }
    const uint64_t locaEnd = (uint64_t)f->locaOff + f->locaLen;
    uint64_t a, b;
    if (f->locaLong) {
        a = fontRd32(f->data, f->locaOff + 4ull * glyph, locaEnd);
        b = fontRd32(f->data, f->locaOff + 4ull * glyph + 4, locaEnd);
    } else {
        a = 2ull * fontRd16(f->data, f->locaOff + 2ull * glyph, locaEnd);
        b = 2ull * fontRd16(f->data, f->locaOff + 2ull * glyph + 2, locaEnd);
    }
    if (a > b || b > f->glyfLen) {
        return STATUS_ERR_INVALID;
    }
    *start = (uint32_t)a;
    *end = (uint32_t)b;
    return STATUS_OK;
}
