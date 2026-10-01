/* Paragraph layout (M12.3, D-156; spec docs/specs/gfx-text.md): per-codepoint font fallback,
 * kerning, tab stops, line breaking at the opportunities of text-break.c with an emergency break
 * for a word wider than the line, all in 26.6 fixed point with int64 pen arithmetic. The text is
 * untrusted: every size is bounded before it is multiplied, and every coordinate is bounded to
 * +-2^24 px. Two identical passes over the glyph records, the first only counting lines. */
#include "gfx/gfx-text.h"

#include <string.h>

typedef struct {
    const GfxFontStack *s;
    GfxTextGlyph *g;
    uint32_t n;
    uint32_t sizeQ6, flags;
    int32_t maxWidthQ6;
    int64_t tabQ6;
    int32_t ascent, lineHeight;
    size_t len;
} Ctx;

#define FLAG_BREAK(f) (((f) & GFX_TEXT_GLYPH_BREAK_MASK) >> 2)
#define MAX_COORD     ((int64_t)GFX_TEXT_MAX_COORD_Q6)

static int64_t floorDiv(int64_t a, int64_t b) { /* b > 0 */
    int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

/* Round half away from zero to a multiple of 64. */
static int64_t wholePixels(int64_t v) {
    return v >= 0 ? (v + 32) / 64 * 64 : -((-v + 32) / 64 * 64);
}

static int64_t scaled(const Ctx *c, const GfxFont *f, int32_t units) {
    int64_t v = gfxFontScaleQ6(f, units, c->sizeQ6);
    return (c->flags & GFX_TEXT_NO_SUBPIXEL) ? wholePixels(v) : v;
}

static bool outOfRange(int64_t pen) {
    return pen > MAX_COORD || pen < -MAX_COORD;
}

/* Closes the line [first, end) as line number k: width, hanging spaces, whole-pixel positions and
 * the baseline for its records, and the line record when `lines` is not NULL. */
static Status closeLine(const Ctx *c, GfxTextLine *lines, uint32_t k, uint32_t first, uint32_t end,
                        uint8_t kind, int32_t *widest) {
    GfxTextGlyph *g = c->g;
    const int64_t baseline = c->ascent + (int64_t)k * c->lineHeight;
    if (baseline > (int64_t)1 << 24) {
        return STATUS_ERR_UNSUPPORTED;
    }
    uint32_t e = end;
    while (e > first && (g[e - 1].flags & GFX_TEXT_GLYPH_HARD)) {
        e--; /* the break characters (CR LF) are not part of the width */
    }
    while (e > first && (g[e - 1].flags & GFX_TEXT_GLYPH_SPACE)) {
        g[e - 1].flags |= GFX_TEXT_GLYPH_HANGING;
        e--;
    }
    int32_t width = e > first ? g[e - 1].y : 0; /* y holds "pen after this record" until now */
    if (width < 0) {
        width = 0;
    }
    for (uint32_t j = first; j < end; j++) {
        const int64_t q = floorDiv((int64_t)g[j].xQ6 + 8, 16);
        g[j].x = (int32_t)floorDiv(q, 4);
        g[j].bin = (uint8_t)(q & 3);
        g[j].y = (int32_t)baseline;
    }
    if (width > *widest) {
        *widest = width;
    }
    if (lines != NULL) {
        GfxTextLine *ln = &lines[k];
        ln->first = first;
        ln->count = end - first;
        ln->byteStart = end > first ? g[first].offset : (uint32_t)c->len;
        ln->byteEnd = end < c->n ? g[end].offset : (uint32_t)c->len;
        ln->baseline = (int32_t)baseline;
        ln->widthQ6 = width;
        ln->end = kind;
        memset(ln->pad, 0, sizeof ln->pad);
    }
    return STATUS_OK;
}

/* One pass: places every record and splits the lines. Returns the line count in *nLines. */
static Status layoutPass(const Ctx *c, GfxTextLine *lines, uint32_t *nLines, int32_t *widest) {
    GfxTextGlyph *g = c->g;
    uint32_t i = 0, k = 0;
    *widest = 0;
    for (;;) {
        const uint32_t first = i;
        int64_t pen = 0;
        bool havePrev = false, haveAllowed = false;
        uint8_t prevFace = 0;
        uint16_t prevGlyph = 0;
        uint32_t lastAllowed = 0, fixedEnd = 0;
        uint8_t kind = GFX_TEXT_LINE_END_TEXT;
        for (; i < c->n; i++) {
            GfxTextGlyph *r = &g[i];
            if (fixedEnd != 0 && i == fixedEnd) {
                /* past the CM run an emergency break had to step over: this record is checked
                 * like any other, so a space hangs, a hard break stays on the line, and a visible
                 * glyph still over the width breaks the line before it */
                fixedEnd = 0;
            }
            const uint32_t brk = FLAG_BREAK(r->flags);
            if (i > first && fixedEnd == 0) {
                if (brk == GFX_BREAK_MANDATORY) {
                    kind = GFX_TEXT_LINE_END_HARD;
                    break;
                }
                if (brk == GFX_BREAK_ALLOWED) {
                    lastAllowed = i;
                    haveAllowed = true;
                }
            }
            r->flags &= (uint8_t)~GFX_TEXT_GLYPH_HANGING;
            const uint8_t f = r->flags;
            if (f & GFX_TEXT_GLYPH_TAB) {
                r->xQ6 = (int32_t)pen;
                pen = (floorDiv(pen, c->tabQ6) + 1) * c->tabQ6;
                havePrev = false;
            } else if (f & GFX_TEXT_GLYPH_INVISIBLE) {
                r->xQ6 = (int32_t)pen;
                havePrev = false;
            } else {
                const GfxFont *font = c->s->faces[r->face];
                if (!(c->flags & GFX_TEXT_NO_KERNING) && havePrev && prevFace == r->face) {
                    pen += scaled(c, font, gfxFontKernUnits(font, prevGlyph, r->glyph));
                    if (outOfRange(pen)) {
                        return STATUS_ERR_UNSUPPORTED;
                    }
                }
                r->xQ6 = (int32_t)pen;
                pen += scaled(c, font, (int32_t)gfxFontAdvanceUnits(font, r->glyph));
                havePrev = true;
                prevFace = r->face;
                prevGlyph = r->glyph;
            }
            if (outOfRange(pen)) {
                return STATUS_ERR_UNSUPPORTED;
            }
            r->y = (int32_t)pen; /* "pen after this record", until closeLine overwrites it */

            const bool zeroWidth = (f & GFX_TEXT_GLYPH_INVISIBLE) && !(f & GFX_TEXT_GLYPH_TAB);
            if (c->maxWidthQ6 > 0 && pen > c->maxWidthQ6 && i > first && fixedEnd == 0 &&
                !(f & (GFX_TEXT_GLYPH_SPACE | GFX_TEXT_GLYPH_HARD)) && !zeroWidth) {
                if (haveAllowed) {
                    kind = GFX_TEXT_LINE_END_SOFT;
                    i = lastAllowed;
                    break;
                }
                /* emergency: break before i, keeping a combining mark with its base */
                uint32_t b = i;
                while (b > first && (g[b].flags & GFX_TEXT_GLYPH_CM)) {
                    b--;
                }
                if (b > first) {
                    kind = GFX_TEXT_LINE_END_EMERGENCY;
                    i = b;
                    break;
                }
                uint32_t o = i + 1;
                while (o < c->n && (g[o].flags & GFX_TEXT_GLYPH_CM)) {
                    o++;
                }
                /* keep placing the mark run without further overflow checks (each would rescan it:
                 * quadratic on a megabyte of marks); o is then checked normally */
                fixedEnd = o;
            }
        }
        uint8_t endKind = kind;
        if (i >= c->n && i > first && (g[i - 1].flags & GFX_TEXT_GLYPH_HARD)) {
            endKind = GFX_TEXT_LINE_END_HARD;
        }
        Status st = closeLine(c, lines, k, first, i, endKind, widest);
        if (st != STATUS_OK) {
            return st;
        }
        k++;
        if (i >= c->n) {
            if (c->n > 0 && (g[c->n - 1].flags & GFX_TEXT_GLYPH_HARD)) {
                st = closeLine(c, lines, k, c->n, c->n, GFX_TEXT_LINE_END_TEXT, widest);
                if (st != STATUS_OK) {
                    return st;
                }
                k++;
            }
            break;
        }
    }
    *nLines = k;
    return STATUS_OK;
}

/* Contract: zeroes *out first; see gfx-text.h for the failure modes. No locks, never sleeps. */
Status gfxTextLayout(const GfxFontStack *s, const uint8_t *text, size_t len, const GfxTextStyle *st,
                     const GfxAllocator *a, GfxTextLayout *out) {
    if (out == NULL) {
        return STATUS_ERR_INVALID;
    }
    memset(out, 0, sizeof *out);
    if (s == NULL || st == NULL || (text == NULL && len > 0) || s->nFaces == 0 ||
        s->nFaces > GFX_FONT_STACK_MAX_FACES) {
        return STATUS_ERR_INVALID;
    }
    if ((st->flags & ~(GFX_TEXT_NO_KERNING | GFX_TEXT_NO_SUBPIXEL)) != 0 || st->maxWidthQ6 < 0 ||
        st->maxWidthQ6 > GFX_TEXT_MAX_COORD_Q6 ||
        (st->tabQ6 != 0 && (st->tabQ6 < 64 || st->tabQ6 > GFX_TEXT_MAX_COORD_Q6))) {
        return STATUS_ERR_INVALID;
    }
    GfxFontMetricsPx m;
    Status status = gfxFontMetrics(s->faces[0], st->sizeQ6, &m);
    if (status != STATUS_OK) {
        return STATUS_ERR_INVALID;
    }
    if (len > GFX_TEXT_MAX_BYTES) {
        return STATUS_ERR_UNSUPPORTED;
    }
    if (a == NULL) {
        a = gfxAllocatorDefault();
    }

    Ctx c;
    memset(&c, 0, sizeof c);
    c.s = s;
    c.sizeQ6 = st->sizeQ6;
    c.flags = st->flags;
    c.maxWidthQ6 = st->maxWidthQ6;
    c.ascent = m.ascent;
    c.lineHeight = m.lineHeight;
    c.len = len;
    if (st->tabQ6 != 0) {
        c.tabQ6 = (c.flags & GFX_TEXT_NO_SUBPIXEL) ? wholePixels(st->tabQ6) : st->tabQ6;
    } else {
        const GfxFont *f0 = s->faces[0];
        const uint16_t sp = gfxFontGlyphIndex(f0, ' ');
        c.tabQ6 = 8 * scaled(&c, f0, (int32_t)gfxFontAdvanceUnits(f0, sp));
        if (c.tabQ6 == 0) {
            c.tabQ6 = (int64_t)st->sizeQ6 * 4;
            if (c.flags & GFX_TEXT_NO_SUBPIXEL) {
                c.tabQ6 = wholePixels(c.tabQ6);
            }
        }
    }
    if (c.tabQ6 < 64) {
        c.tabQ6 = 64;
    }

    const size_t n = gfxUtf8Count(text, len, NULL);
    GfxTextGlyph *g = NULL;
    if (n > 0) {
        g = a->alloc(a->ctx, n * sizeof *g);
        if (g == NULL) {
            return STATUS_ERR_NO_MEMORY;
        }
        memset(g, 0, n * sizeof *g);
    }
    c.g = g;
    c.n = (uint32_t)n;

    /* pass 1: decode, classify, pick faces */
    GfxUtf8Iter it;
    gfxUtf8IterInit(&it, text, len);
    GfxTextBreakState bs;
    gfxTextBreakInit(&bs);
    uint32_t cp;
    size_t off;
    for (uint32_t i = 0; gfxUtf8Next(&it, &cp, &off); i++) {
        GfxTextGlyph *r = &g[i];
        const GfxLineClass cls = gfxTextLineClass(cp);
        uint8_t f = (uint8_t)((uint32_t)gfxTextBreakNext(&bs, cp) << 2);
        if (cp == 0x09) {
            f |= GFX_TEXT_GLYPH_TAB;
        }
        if (cp == 0x20) {
            f |= GFX_TEXT_GLYPH_SPACE;
        }
        if (cls == GFX_LB_CM) {
            f |= GFX_TEXT_GLYPH_CM;
        }
        if (cls == GFX_LB_BK || cls == GFX_LB_CR || cls == GFX_LB_LF || cls == GFX_LB_NL) {
            f |= GFX_TEXT_GLYPH_HARD;
        }
        uint32_t face = 0;
        uint16_t glyph = 0;
        if (gfxTextIsInvisible(cp)) {
            f |= GFX_TEXT_GLYPH_INVISIBLE;
        } else {
            gfxFontStackPick(s, cp, &face, &glyph);
        }
        r->offset = (uint32_t)off;
        r->glyph = glyph;
        r->face = (uint8_t)face;
        r->flags = f;
    }

    /* pass 2a: count the lines; 2b: fill them */
    uint32_t nLines = 0;
    int32_t widest = 0;
    status = layoutPass(&c, NULL, &nLines, &widest);
    GfxTextLine *lines = NULL;
    if (status == STATUS_OK) {
        lines = a->alloc(a->ctx, (size_t)nLines * sizeof *lines);
        if (lines == NULL) {
            status = STATUS_ERR_NO_MEMORY;
        }
    }
    if (status == STATUS_OK) {
        uint32_t nLines2 = 0;
        status = layoutPass(&c, lines, &nLines2, &widest);
        if (status == STATUS_OK && nLines2 != nLines) {
            status = STATUS_ERR_INVALID; /* the two passes are identical: unreachable */
        }
    }
    if (status != STATUS_OK) {
        if (lines != NULL) {
            a->free(a->ctx, lines, (size_t)nLines * sizeof *lines);
        }
        if (g != NULL) {
            a->free(a->ctx, g, n * sizeof *g);
        }
        return status;
    }
    out->glyphs = g;
    out->lines = lines;
    out->nGlyphs = c.n;
    out->nLines = nLines;
    out->sizeQ6 = st->sizeQ6;
    out->flags = st->flags;
    out->ascent = m.ascent;
    out->descent = m.descent;
    out->lineHeight = m.lineHeight;
    out->widthQ6 = widest;
    out->height = (int32_t)(nLines * (uint32_t)m.lineHeight);
    out->stack = s;
    out->alloc = a;
    return STATUS_OK;
}

/* Contract: NULL-safe, idempotent. */
void gfxTextLayoutFree(GfxTextLayout *l) {
    if (l == NULL) {
        return;
    }
    if (l->alloc != NULL) {
        if (l->glyphs != NULL) {
            l->alloc->free(l->alloc->ctx, l->glyphs, (size_t)l->nGlyphs * sizeof *l->glyphs);
        }
        if (l->lines != NULL) {
            l->alloc->free(l->alloc->ctx, l->lines, (size_t)l->nLines * sizeof *l->lines);
        }
    }
    memset(l, 0, sizeof *l);
}
