/* glyf outlines: simple and composite glyphs into a point/contour outline, and outline -> quadratic
 * path (M12.3, D-151/D-153). This is one of the two font files that use float (D-142, D-153): the
 * glyph transform is applied in float, so `FP_CONTRACT` is off and every expression is
 * parenthesized as written (no fused multiply-add, or the goldens would differ between builds).
 * TrueType instructions are skipped, never executed. */
#pragma STDC FP_CONTRACT OFF

#include "gfx/font-internal.h"

#include <string.h>

/* Contract: no allocation, never sleeps. `a` may be NULL (default allocator). */
void gfxGlyphOutlineInit(GfxGlyphOutline *o, const GfxAllocator *a) {
    memset(o, 0, sizeof *o);
    o->alloc = a != NULL ? a : gfxAllocatorDefault();
}

/* Contract: frees the arrays; the outline is empty and reusable afterwards. */
void gfxGlyphOutlineFree(GfxGlyphOutline *o) {
    if (o == NULL) {
        return;
    }
    if (o->alloc != NULL) {
        if (o->xy != NULL) {
            o->alloc->free(o->alloc->ctx, o->xy, sizeof(float) * 2u * o->capPoints);
        }
        if (o->onCurve != NULL) {
            o->alloc->free(o->alloc->ctx, o->onCurve, o->capPoints);
        }
        if (o->contourEnd != NULL) {
            o->alloc->free(o->alloc->ctx, o->contourEnd, sizeof(uint32_t) * o->capContours);
        }
    }
    o->xy = NULL;
    o->onCurve = NULL;
    o->contourEnd = NULL;
    o->nPoints = o->nContours = o->capPoints = o->capContours = 0;
}

/* Ensures room for `points` and `contours` in total (both within the limits); doubles from
 * 64/8. NO_MEMORY on allocator failure (the outline keeps its old, valid arrays). */
static Status outlineReserve(GfxGlyphOutline *o, uint32_t points, uint32_t contours) {
    if (points > o->capPoints) {
        uint32_t cap = o->capPoints != 0 ? o->capPoints : 64u;
        while (cap < points) {
            cap *= 2u;
        }
        if (cap > GFX_FONT_MAX_POINTS) {
            cap = GFX_FONT_MAX_POINTS;
        }
        float *xy = o->alloc->alloc(o->alloc->ctx, sizeof(float) * 2u * cap);
        uint8_t *on = xy != NULL ? o->alloc->alloc(o->alloc->ctx, cap) : NULL;
        if (xy == NULL || on == NULL) {
            if (xy != NULL) {
                o->alloc->free(o->alloc->ctx, xy, sizeof(float) * 2u * cap);
            }
            return STATUS_ERR_NO_MEMORY;
        }
        if (o->nPoints != 0) {
            memcpy(xy, o->xy, sizeof(float) * 2u * o->nPoints);
            memcpy(on, o->onCurve, o->nPoints);
        }
        if (o->xy != NULL) {
            o->alloc->free(o->alloc->ctx, o->xy, sizeof(float) * 2u * o->capPoints);
            o->alloc->free(o->alloc->ctx, o->onCurve, o->capPoints);
        }
        o->xy = xy;
        o->onCurve = on;
        o->capPoints = cap;
    }
    if (contours > o->capContours) {
        uint32_t cap = o->capContours != 0 ? o->capContours : 8u;
        while (cap < contours) {
            cap *= 2u;
        }
        if (cap > GFX_FONT_MAX_CONTOURS) {
            cap = GFX_FONT_MAX_CONTOURS;
        }
        uint32_t *ce = o->alloc->alloc(o->alloc->ctx, sizeof(uint32_t) * cap);
        if (ce == NULL) {
            return STATUS_ERR_NO_MEMORY;
        }
        if (o->nContours != 0) {
            memcpy(ce, o->contourEnd, sizeof(uint32_t) * o->nContours);
        }
        if (o->contourEnd != NULL) {
            o->alloc->free(o->alloc->ctx, o->contourEnd, sizeof(uint32_t) * o->capContours);
        }
        o->contourEnd = ce;
        o->capContours = cap;
    }
    return STATUS_OK;
}

typedef struct {
    const GfxFont *f;
    GfxGlyphOutline *o;
    uint32_t components; /* over the whole tree */
} Loader;

static float f2dot14(int32_t v) {
    return (float)v / 16384.0f;
}

/* One simple glyph, `p`..`end` (absolute offsets), appended to the outline under matrix `m`. */
static Status loadSimple(Loader *l, uint64_t p, uint64_t end, int32_t nc, const float *m) {
    const GfxFont *f = l->f;
    GfxGlyphOutline *o = l->o;
    const uint8_t *d = f->data;
    if (nc == 0) {
        return STATUS_OK;
    }
    if ((uint32_t)nc > GFX_FONT_MAX_CONTOURS - o->nContours) {
        return STATUS_ERR_UNSUPPORTED;
    }
    const uint32_t un = (uint32_t)nc;
    if (!fontFits(p + 10, 2ull * un, end)) {
        return STATUS_ERR_INVALID;
    }
    uint32_t prev = 0;
    for (uint32_t i = 0; i < un; i++) {
        uint32_t e = fontRd16(d, p + 10 + 2ull * i, end);
        if (i != 0 && e <= prev) {
            return STATUS_ERR_INVALID;
        }
        prev = e;
    }
    const uint32_t nPts = prev + 1u;
    if (nPts > GFX_FONT_MAX_POINTS - o->nPoints) {
        return STATUS_ERR_UNSUPPORTED;
    }
    uint64_t q = p + 10 + 2ull * un;
    if (!fontFits(q, 2, end)) {
        return STATUS_ERR_INVALID;
    }
    q += 2ull + fontRd16(d, q, end); /* skip the instructions */
    if (q > end) {
        return STATUS_ERR_INVALID;
    }
    Status st = outlineReserve(o, o->nPoints + nPts, o->nContours + un);
    if (st != STATUS_OK) {
        return st;
    }
    const uint32_t base = o->nPoints;
    uint8_t *flags = o->onCurve + base;
    for (uint32_t i = 0; i < nPts;) {
        if (q >= end) {
            return STATUS_ERR_INVALID;
        }
        uint8_t fl = d[q++];
        flags[i++] = fl;
        if ((fl & 0x08u) != 0) {
            if (q >= end) {
                return STATUS_ERR_INVALID;
            }
            uint32_t rep = d[q++];
            if (rep > nPts - i) {
                return STATUS_ERR_INVALID;
            }
            while (rep-- != 0) {
                flags[i++] = fl;
            }
        }
    }
    float *xy = o->xy + 2u * (size_t)base;
    int32_t acc = 0;
    for (uint32_t i = 0; i < nPts; i++) {
        uint8_t fl = flags[i];
        if ((fl & 0x02u) != 0) {
            if (q >= end) {
                return STATUS_ERR_INVALID;
            }
            int32_t v = d[q++];
            acc += (fl & 0x10u) != 0 ? v : -v;
        } else if ((fl & 0x10u) == 0) {
            if (!fontFits(q, 2, end)) {
                return STATUS_ERR_INVALID;
            }
            acc += fontRdS16(d, q, end);
            q += 2;
        }
        xy[2u * i] = (float)acc;
    }
    acc = 0;
    for (uint32_t i = 0; i < nPts; i++) {
        uint8_t fl = flags[i];
        if ((fl & 0x04u) != 0) {
            if (q >= end) {
                return STATUS_ERR_INVALID;
            }
            int32_t v = d[q++];
            acc += (fl & 0x20u) != 0 ? v : -v;
        } else if ((fl & 0x20u) == 0) {
            if (!fontFits(q, 2, end)) {
                return STATUS_ERR_INVALID;
            }
            acc += fontRdS16(d, q, end);
            q += 2;
        }
        xy[2u * i + 1u] = (float)acc;
    }
    /* x' = (a*x + c*y) + e; y' = (b*x + d*y) + f */
    for (uint32_t i = 0; i < nPts; i++) {
        float x = xy[2u * i], y = xy[2u * i + 1u];
        xy[2u * i] = ((m[0] * x) + (m[2] * y)) + m[4];
        xy[2u * i + 1u] = ((m[1] * x) + (m[3] * y)) + m[5];
        flags[i] &= 1u;
    }
    for (uint32_t i = 0; i < un; i++) {
        o->contourEnd[o->nContours + i] = base + fontRd16(d, p + 10 + 2ull * i, end);
    }
    o->nPoints += nPts;
    o->nContours += un;
    return STATUS_OK;
}

static Status loadGlyph(Loader *l, uint16_t gid, const float *m, uint32_t depth) {
    const GfxFont *f = l->f;
    uint32_t start, endOff;
    Status st = fontGlyphRange(f, gid, &start, &endOff);
    if (st != STATUS_OK) {
        return st;
    }
    if (start == endOff) {
        return STATUS_OK; /* empty glyph */
    }
    if (endOff - start < 10) {
        return STATUS_ERR_INVALID;
    }
    const uint64_t p = (uint64_t)f->glyfOff + start, end = (uint64_t)f->glyfOff + endOff;
    const uint8_t *d = f->data;
    const int32_t nc = fontRdS16(d, p, end);
    if (nc >= 0) {
        return loadSimple(l, p, end, nc, m);
    }
    if (depth >= GFX_FONT_MAX_COMPOSITE_DEPTH) {
        return STATUS_ERR_UNSUPPORTED; /* the depth limit also catches cycles */
    }
    uint64_t q = p + 10;
    for (;;) {
        if (!fontFits(q, 4, end)) {
            return STATUS_ERR_INVALID;
        }
        const uint32_t flags = fontRd16(d, q, end);
        const uint16_t cgid = (uint16_t)fontRd16(d, q + 2, end);
        q += 4;
        if (++l->components > GFX_FONT_MAX_COMPONENTS) {
            return STATUS_ERR_UNSUPPORTED;
        }
        int32_t dx, dy;
        if ((flags & 0x0001u) != 0) {
            if (!fontFits(q, 4, end)) {
                return STATUS_ERR_INVALID;
            }
            dx = fontRdS16(d, q, end);
            dy = fontRdS16(d, q + 2, end);
            q += 4;
        } else {
            if (!fontFits(q, 2, end)) {
                return STATUS_ERR_INVALID;
            }
            dx = (int8_t)d[q];
            dy = (int8_t)d[q + 1];
            q += 2;
        }
        if ((flags & 0x0002u) == 0) {
            return STATUS_ERR_UNSUPPORTED; /* point-matching arguments */
        }
        float a = 1.0f, b = 0.0f, c = 0.0f, dd = 1.0f;
        if ((flags & 0x0008u) != 0) {
            if (!fontFits(q, 2, end)) {
                return STATUS_ERR_INVALID;
            }
            a = dd = f2dot14(fontRdS16(d, q, end));
            q += 2;
        } else if ((flags & 0x0040u) != 0) {
            if (!fontFits(q, 4, end)) {
                return STATUS_ERR_INVALID;
            }
            a = f2dot14(fontRdS16(d, q, end));
            dd = f2dot14(fontRdS16(d, q + 2, end));
            q += 4;
        } else if ((flags & 0x0080u) != 0) {
            if (!fontFits(q, 8, end)) {
                return STATUS_ERR_INVALID;
            }
            a = f2dot14(fontRdS16(d, q, end));
            b = f2dot14(fontRdS16(d, q + 2, end));
            c = f2dot14(fontRdS16(d, q + 4, end));
            dd = f2dot14(fontRdS16(d, q + 6, end));
            q += 8;
        }
        float e = (float)dx, ef = (float)dy;
        if ((flags & 0x0800u) != 0 && (flags & 0x1000u) == 0) {
            e = (a * (float)dx) + (c * (float)dy);
            ef = (b * (float)dx) + (dd * (float)dy);
        }
        /* child = M o comp */
        float cm[6];
        cm[0] = (m[0] * a) + (m[2] * b);
        cm[1] = (m[1] * a) + (m[3] * b);
        cm[2] = (m[0] * c) + (m[2] * dd);
        cm[3] = (m[1] * c) + (m[3] * dd);
        cm[4] = ((m[0] * e) + (m[2] * ef)) + m[4];
        cm[5] = ((m[1] * e) + (m[3] * ef)) + m[5];
        st = loadGlyph(l, cgid, cm, depth + 1u);
        if (st != STATUS_OK) {
            return st;
        }
        if ((flags & 0x0020u) == 0) {
            break;
        }
    }
    return STATUS_OK;
}

/* Contract: allocates through o->alloc; never sleeps; see gfx-font.h for the failure modes. */
Status gfxFontGlyphOutline(const GfxFont *f, uint16_t glyph, const GfxFontXform *xf,
                           GfxGlyphOutline *o) {
    if (f == NULL || f->data == NULL || o == NULL) {
        return STATUS_ERR_INVALID;
    }
    if (o->alloc == NULL) {
        o->alloc = gfxAllocatorDefault();
    }
    o->nPoints = 0;
    o->nContours = 0;
    float m[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    if (xf != NULL) {
        m[0] = xf->a;
        m[1] = xf->b;
        m[2] = xf->c;
        m[3] = xf->d;
        m[4] = xf->e;
        m[5] = xf->f;
    }
    Loader l = {f, o, 0};
    return loadGlyph(&l, glyph, m, 0);
}

/* Contract: appends to `p`; returns the path's sticky error. */
Status gfxGlyphOutlineToPath(const GfxGlyphOutline *o, float dx, float dy, GfxPath *p) {
    if (o == NULL || p == NULL) {
        return STATUS_ERR_INVALID;
    }
    uint32_t s = 0;
    for (uint32_t ci = 0; ci < o->nContours; ci++) {
        const uint32_t e = o->contourEnd[ci];
        if (e >= o->nPoints || e < s) {
            return STATUS_ERR_INVALID;
        }
        const uint32_t first = s;
        s = e + 1u;
        if (e - first + 1u < 2u) {
            continue;
        }
        const float *xy = o->xy;
        const uint8_t *on = o->onCurve;
        float sx, sy; /* the contour's start point */
        uint32_t from, to;
        if (on[first] != 0) {
            sx = xy[2u * first];
            sy = xy[2u * first + 1u];
            from = first + 1u;
            to = e;
        } else if (on[e] != 0) {
            sx = xy[2u * e];
            sy = xy[2u * e + 1u];
            from = first;
            to = e - 1u;
        } else {
            sx = (xy[2u * e] + xy[2u * first]) * 0.5f;
            sy = (xy[2u * e + 1u] + xy[2u * first + 1u]) * 0.5f;
            from = first;
            to = e;
        }
        gfxPathMoveTo(p, sx + dx, sy + dy);
        bool pending = false;
        float px = 0.0f, py = 0.0f;
        for (uint32_t i = from; i <= to; i++) {
            const float x = xy[2u * i], y = xy[2u * i + 1u];
            if (on[i] != 0) {
                if (pending) {
                    gfxPathQuadTo(p, px + dx, py + dy, x + dx, y + dy);
                    pending = false;
                } else {
                    gfxPathLineTo(p, x + dx, y + dy);
                }
            } else {
                if (pending) {
                    gfxPathQuadTo(p, px + dx, py + dy, ((px + x) * 0.5f) + dx,
                                  ((py + y) * 0.5f) + dy);
                }
                px = x;
                py = y;
                pending = true;
            }
        }
        if (pending) {
            gfxPathQuadTo(p, px + dx, py + dy, sx + dx, sy + dy);
        }
        gfxPathClose(p);
        if (p->error != STATUS_OK) {
            return p->error;
        }
    }
    return p->error;
}
