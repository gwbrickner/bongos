/* Drawing a laid-out paragraph through the glyph cache (M12.3, D-156; spec
 * docs/specs/gfx-text.md). Each cached image is used immediately: it is only valid until the next
 * gfxFontStackGlyph call. */
#include "gfx/gfx-text.h"

/* Contract: clipping is the canvas'; no locks, never sleeps. */
Status gfxTextDraw(GfxCanvas *c, GfxFontStack *s, const GfxTextLayout *l, int32_t ox, int32_t oy,
                   GfxColor col) {
    if (l == NULL) {
        return STATUS_ERR_INVALID;
    }
    return gfxTextDrawLines(c, s, l, 0, l->nLines, ox, oy, col);
}

/* Contract: see gfx-text.h; `nLines` is clamped to the layout. NO_MEMORY from the cache skips that
 * glyph and is reported at the end; any other error stops the drawing. */
Status gfxTextDrawLines(GfxCanvas *c, GfxFontStack *s, const GfxTextLayout *l, uint32_t firstLine,
                        uint32_t nLines, int32_t ox, int32_t oy, GfxColor col) {
    if (c == NULL || s == NULL || l == NULL || l->stack != s) {
        return STATUS_ERR_INVALID;
    }
    if (firstLine >= l->nLines) {
        return STATUS_OK;
    }
    uint32_t end = l->nLines;
    if (nLines < end - firstLine) {
        end = firstLine + nLines;
    }
    Status result = STATUS_OK;
    for (uint32_t li = firstLine; li < end; li++) {
        const GfxTextLine *ln = &l->lines[li];
        for (uint32_t j = ln->first; j < ln->first + ln->count; j++) {
            const GfxTextGlyph *r = &l->glyphs[j];
            if (r->flags & GFX_TEXT_GLYPH_INVISIBLE) {
                continue;
            }
            const GfxGlyphImage *img;
            const Status st = gfxFontStackGlyph(s, r->face, r->glyph, l->sizeQ6, r->bin, &img);
            if (st == STATUS_ERR_NO_MEMORY) {
                result = st;
                continue;
            }
            if (st != STATUS_OK) {
                return st;
            }
            if (img->mask.data == NULL) {
                continue;
            }
            const int64_t x = (int64_t)ox + r->x + img->left;
            const int64_t y = (int64_t)oy + r->y + img->top;
            if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) {
                continue;
            }
            gfxFillMask(c, (int32_t)x, (int32_t)y, &img->mask, col);
        }
    }
    return result;
}
