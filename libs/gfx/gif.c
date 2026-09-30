/* See gfx-image.h: the GIF decoder and animation compositor (M12.7, D-163, D-164).
 *
 * One streaming compositor: gfxGifOpen validates the whole container except the LZW bitstreams
 * (so the frame count is exact and every later block walk is in bounds) and allocates one W x H
 * canvas, plus a same-size save buffer only when some frame asks for disposal 3. gfxGifNextFrame
 * disposes of the previous frame, then LZW-decodes the next one straight onto the canvas. All
 * arithmetic on file-controlled values is done in 32 bits from u16 sources or in 64 bits, and
 * every read is bounds-checked against `size` before it happens. Integer-only (D-142). */
#include "gfx/gfx-internal.h"

#include <string.h>

#define GIF_MAX_CODES 4096u
#define GIF_NO_CODE   0xFFFFFFFFu
#define GIF_OPAQUE    0xFF000000u

struct GfxGif {
    GfxDecodeCtx ctx; /* accounting for the object's whole life (includes this struct) */
    const uint8_t *data;
    size_t size;
    GfxGifInfo info;
    size_t gctPos;     /* the global color table, if any */
    uint32_t gctCount; /* entries; 0 = none */
    size_t firstFrame; /* offset of the first block after the global color table */
    size_t pos;        /* offset of the next block to walk */
    uint32_t index;    /* frames returned since Open/Rewind */
    Status error;      /* sticky LZW error (STATUS_OK if none) */
    GfxImage canvas;   /* W x H, premultiplied; owns the allocation */
    uint32_t *save;    /* W x H scratch for disposal 3; NULL if no frame uses it */
    size_t saveSize;
    bool havePrev; /* a frame was drawn since Open/Rewind: its disposal is still pending */
    uint32_t prevDisposal;
    GfxRect prevRect; /* clipped; empty is {0,0,0,0} */
    /* LZW string table (GIF89a Appendix F). Codes are at most 11 bits wide as pixel indices, so
     * the suffix, first and stack entries are 16 bits (an index >= 256 draws opaque black). */
    uint16_t prefix[GIF_MAX_CODES];
    uint16_t suffix[GIF_MAX_CODES];
    uint16_t first[GIF_MAX_CODES];
    uint16_t stack[GIF_MAX_CODES];
};

static uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/* ---- block walking (shared by the open-time pre-scan and NextFrame) ----------------------- */

/* One image descriptor plus the graphic control state that applies to it. */
typedef struct {
    uint32_t x, y, w, h;
    bool interlace;
    size_t palPos;     /* the local color table, if any */
    uint32_t palCount; /* entries; 0 = none */
    uint32_t minCode;  /* LZW minimum code size, 2..11 */
    size_t dataPos;    /* the first sub-block's length byte */
    size_t endPos;     /* one past the image data's terminator */
    uint32_t disposal; /* raw 0..7 from the last Graphic Control Extension before the image */
    uint32_t delay;    /* raw, in 1/100 s */
    bool hasTransp;
    uint32_t transIdx;
} GifFrameHdr;

typedef enum { GIF_BLOCK_IMAGE, GIF_BLOCK_TRAILER } GifBlockKind;

/* Walks a sub-block chain starting at `p` (a length byte); true and *end one past the zero
 * terminator, or false if any length byte or block runs past `n`. */
static bool gifSkipSubBlocks(const uint8_t *d, size_t n, size_t p, size_t *end) {
    for (;;) {
        if (p >= n) {
            return false;
        }
        size_t len = d[p++];
        if (len == 0) {
            *end = p;
            return true;
        }
        if (n - p < len) {
            return false;
        }
        p += len;
    }
}

/* True if the application extension's first sub-block (at `p`, a length byte) names NETSCAPE2.0
 * or ANIMEXTS1.0 and the chain's second sub-block starts with 1 and holds >= 3 bytes; then
 * *loop is the little-endian u16 that follows. The chain must already be validated. */
static bool gifLoopExtension(const uint8_t *d, size_t p, int32_t *loop) {
    if (d[p] != 11) {
        return false;
    }
    if (memcmp(d + p + 1, "NETSCAPE2.0", 11) != 0 && memcmp(d + p + 1, "ANIMEXTS1.0", 11) != 0) {
        return false;
    }
    size_t q = p + 12; /* the second sub-block's length byte (validated to exist) */
    if (d[q] < 3 || d[q + 1] != 1) {
        return false;
    }
    *loop = (int32_t)rd16(d + q + 2);
    return true;
}

/* Walks blocks from *pos up to and including the next image descriptor or the trailer. The last
 * Graphic Control Extension before an image applies to it; every other extension is skipped.
 * If `loop` is non-NULL and still negative, the first loop extension found sets it. INVALID for
 * anything malformed, including running out of data before the trailer. */
static Status gifScan(const uint8_t *d, size_t n, size_t *pos, GifBlockKind *kind, GifFrameHdr *fh,
                      int32_t *loop) {
    memset(fh, 0, sizeof(*fh));
    size_t p = *pos;
    for (;;) {
        if (p >= n) {
            return STATUS_ERR_INVALID;
        }
        uint8_t intro = d[p];
        if (intro == 0x3B) {
            *pos = p + 1;
            *kind = GIF_BLOCK_TRAILER;
            return STATUS_OK;
        }
        if (intro == 0x21) {
            if (n - p < 2) {
                return STATUS_ERR_INVALID;
            }
            uint8_t label = d[p + 1];
            size_t start = p + 2, end;
            if (!gifSkipSubBlocks(d, n, start, &end)) {
                return STATUS_ERR_INVALID;
            }
            if (label == 0xF9) {
                if (d[start] < 4) { /* the chain is non-empty or d[start] is the terminator, 0 */
                    return STATUS_ERR_INVALID;
                }
                uint32_t packed = d[start + 1];
                fh->disposal = (packed >> 2) & 7u;
                fh->hasTransp = (packed & 1u) != 0;
                fh->delay = rd16(d + start + 2);
                fh->transIdx = d[start + 4];
            } else if (label == 0xFF && loop != NULL && *loop < 0) {
                (void)gifLoopExtension(d, start, loop);
            }
            p = end;
            continue;
        }
        if (intro != 0x2C) {
            return STATUS_ERR_INVALID;
        }
        if (n - p < 10) {
            return STATUS_ERR_INVALID;
        }
        fh->x = rd16(d + p + 1);
        fh->y = rd16(d + p + 3);
        fh->w = rd16(d + p + 5);
        fh->h = rd16(d + p + 7);
        uint32_t packed = d[p + 9];
        fh->interlace = (packed & 0x40u) != 0;
        p += 10;
        if ((packed & 0x80u) != 0) {
            uint32_t count = 2u << (packed & 7u);
            if (n - p < (size_t)count * 3) {
                return STATUS_ERR_INVALID;
            }
            fh->palPos = p;
            fh->palCount = count;
            p += (size_t)count * 3;
        }
        if (p >= n) {
            return STATUS_ERR_INVALID;
        }
        fh->minCode = d[p++];
        if (fh->minCode < 2 || fh->minCode > 11) {
            return STATUS_ERR_INVALID;
        }
        fh->dataPos = p;
        if (!gifSkipSubBlocks(d, n, p, &fh->endPos)) {
            return STATUS_ERR_INVALID;
        }
        *pos = fh->endPos;
        *kind = GIF_BLOCK_IMAGE;
        return STATUS_OK;
    }
}

/* ---- open-time pre-scan --------------------------------------------------------------------- */

typedef struct {
    GfxGifInfo info;
    size_t gctPos, firstFrame;
    uint32_t gctCount;
    bool needsSave;
} GifScanResult;

/* Validates everything except the LZW bitstreams (see gfxGifDecode's scope in gfx-image.h). */
static Status gifPrescan(const GfxDecodeCtx *d, const uint8_t *data, size_t size,
                         GifScanResult *r) {
    memset(r, 0, sizeof(*r));
    if (size < 6 || (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0)) {
        return STATUS_ERR_INVALID;
    }
    if (size < 13) {
        return STATUS_ERR_INVALID;
    }
    uint32_t w = rd16(data + 6), h = rd16(data + 8), packed = data[10];
    Status st = gfxDecCheckDims(d, w, h);
    if (st != STATUS_OK) {
        return st;
    }
    size_t pos = 13;
    if ((packed & 0x80u) != 0) {
        uint32_t count = 2u << (packed & 7u);
        if (size - pos < (size_t)count * 3) {
            return STATUS_ERR_INVALID;
        }
        r->gctPos = pos;
        r->gctCount = count;
        pos += (size_t)count * 3;
    }
    r->firstFrame = pos;
    r->info.width = w;
    r->info.height = h;
    r->info.loopCount = -1;
    for (;;) {
        GifBlockKind kind;
        GifFrameHdr fh;
        st = gifScan(data, size, &pos, &kind, &fh, &r->info.loopCount);
        if (st != STATUS_OK) {
            return st;
        }
        if (kind == GIF_BLOCK_TRAILER) {
            break;
        }
        if ((uint64_t)fh.w * (uint64_t)fh.h > d->lim.maxPixels) {
            return STATUS_ERR_UNSUPPORTED;
        }
        r->info.frameCount++;
        if (fh.disposal == 3) {
            r->needsSave = true;
        }
    }
    return r->info.frameCount == 0 ? STATUS_ERR_INVALID : STATUS_OK;
}

/* ---- LZW and drawing ------------------------------------------------------------------------ */

/* Draws decoded indices into a frame, in file order, handling interlace and clipping. */
typedef struct {
    uint32_t *canvas;
    uint32_t stride;         /* canvas width */
    uint32_t fx, fy, fw, fh; /* the frame's own rectangle (unclipped) */
    uint32_t clipW, clipH;   /* how much of it lies inside the canvas */
    uint32_t col, row, pass; /* the next pixel, in frame coordinates */
    bool interlace;
    uint64_t left; /* pixels still to draw */
    uint32_t pal[256];
    uint32_t palCount;
    bool hasTransp;
    uint32_t transIdx;
} GifPainter;

static const uint8_t GIF_PASS_START[4] = {0, 4, 2, 1};
static const uint8_t GIF_PASS_STEP[4] = {8, 8, 4, 2};

/* Advances to the next frame row; interlaced passes that are empty for this height are skipped. */
static void gifNextRow(GifPainter *p) {
    if (!p->interlace) {
        p->row++;
        return;
    }
    p->row += GIF_PASS_STEP[p->pass];
    while (p->row >= p->fh && p->pass < 3) {
        p->row = GIF_PASS_START[++p->pass];
    }
}

/* Draws one pixel index. The transparent index leaves the canvas alone; an index beyond the
 * palette (or with no palette) is opaque black. Callers check `left > 0` first. */
static void gifPut(GifPainter *p, uint32_t idx) {
    if (p->row < p->clipH && p->col < p->clipW && !(p->hasTransp && idx == p->transIdx)) {
        size_t o = (size_t)(p->fy + p->row) * p->stride + p->fx + p->col;
        p->canvas[o] = idx < p->palCount ? p->pal[idx] : GIF_OPAQUE;
    }
    p->left--;
    if (++p->col == p->fw) {
        p->col = 0;
        gifNextRow(p);
    }
}

/* LSB-first code reader over the sub-blocks; codes may straddle sub-block boundaries. */
typedef struct {
    const uint8_t *d;
    size_t n, pos;
    uint32_t remain; /* bytes left in the current sub-block */
    uint32_t buf, bits;
    bool done;
} GifBits;

/* False once the data is exhausted (the zero terminator, or the end of the file). */
static bool gifReadCode(GifBits *b, uint32_t width, uint32_t *code) {
    while (b->bits < width) {
        if (b->remain == 0) {
            if (b->done || b->pos >= b->n) {
                b->done = true;
                return false;
            }
            b->remain = b->d[b->pos++];
            if (b->remain == 0) {
                b->done = true;
                return false;
            }
        }
        if (b->pos >= b->n) {
            b->done = true;
            return false;
        }
        b->buf |= (uint32_t)b->d[b->pos++] << b->bits;
        b->bits += 8;
        b->remain--;
    }
    *code = b->buf & ((1u << width) - 1u);
    b->buf >>= width;
    b->bits -= width;
    return true;
}

/* First pixel index of the string for `c` (a literal or a table entry). */
static uint32_t gifFirst(const GfxGif *g, uint32_t c, uint32_t clear) {
    return c < clear ? c : g->first[c];
}

/* Expands the string for `c` onto g->stack, last pixel first; returns its length, or 0 if the
 * chain does not end in a literal within GIF_MAX_CODES steps (cannot happen for a table built
 * by gifLzw; a defense against a broken invariant). */
static uint32_t gifExpand(GfxGif *g, uint32_t c, uint32_t clear) {
    uint32_t n = 0;
    while (c >= clear + 2) {
        if (n >= GIF_MAX_CODES) {
            return 0;
        }
        g->stack[n++] = g->suffix[c];
        c = g->prefix[c];
    }
    if (n >= GIF_MAX_CODES || c >= clear) {
        return 0;
    }
    g->stack[n++] = (uint16_t)c;
    return n;
}

/* Draws the stacked string (built by gifExpand) in order, stopping when the frame is full. */
static void gifDrawStack(const GfxGif *g, GifPainter *p, uint32_t n) {
    while (n > 0 && p->left > 0) {
        gifPut(p, g->stack[--n]);
    }
}

/* Decodes the frame's LZW data and draws it. STATUS_ERR_INVALID on an LZW error; data or an
 * end-of-information code arriving early is tolerated (the rest of the frame is left alone),
 * and codes after the frame is full are ignored. */
static Status gifLzw(GfxGif *g, const GifFrameHdr *fh, GifPainter *p) {
    uint32_t m = fh->minCode, clear = 1u << m, eoi = clear + 1;
    uint32_t width = m + 1, next = clear + 2, prev = GIF_NO_CODE;
    GifBits b = {g->data, g->size, fh->dataPos, 0, 0, 0, false};
    while (p->left > 0) {
        uint32_t code;
        if (!gifReadCode(&b, width, &code)) {
            break;
        }
        if (code == clear) {
            width = m + 1;
            next = clear + 2;
            prev = GIF_NO_CODE;
            continue;
        }
        if (code == eoi) {
            break;
        }
        if (prev == GIF_NO_CODE) {
            if (code >= clear) {
                return STATUS_ERR_INVALID; /* the first code after a clear must be a literal */
            }
            gifPut(p, code);
            prev = code;
            continue;
        }
        uint32_t f, len;
        if (code < next) {
            len = gifExpand(g, code, clear);
            if (len == 0) {
                return STATUS_ERR_INVALID;
            }
            f = g->stack[len - 1];
            gifDrawStack(g, p, len);
        } else if (code == next) { /* KwKwK: the string for `prev` plus its own first pixel */
            len = gifExpand(g, prev, clear);
            if (len == 0) {
                return STATUS_ERR_INVALID;
            }
            f = g->stack[len - 1];
            gifDrawStack(g, p, len);
            if (p->left > 0) {
                gifPut(p, f);
            }
        } else {
            return STATUS_ERR_INVALID;
        }
        if (next < GIF_MAX_CODES) {
            g->prefix[next] = (uint16_t)prev;
            g->suffix[next] = (uint16_t)f;
            g->first[next] = (uint16_t)gifFirst(g, prev, clear);
            next++;
            if (next >= (1u << width) && width < 12) {
                width++;
            }
        }
        prev = code;
    }
    return STATUS_OK;
}

/* ---- the streaming API ---------------------------------------------------------------------- */

/* Frees the object and whatever it owns. The accounting lives inside the object, so it is copied
 * out first: gfxDecFree updates its context after the block is gone. */
static void gifDestroy(GfxGif *g) {
    GfxDecodeCtx d = g->ctx;
    gfxDecFree(&d, g->save, g->saveSize);
    gfxDecFree(&d, g->canvas.pixels, g->canvas.allocSize);
    gfxDecFree(&d, g, sizeof(*g));
}

/* Never sleeps, takes no locks, calls only the allocator's alloc/free. NO_MEMORY if the
 * allocator fails; UNSUPPORTED if the canvas (plus save buffer) or the frame area is over a
 * limit, or the whole object would exceed maxTotalBytes; INVALID for a malformed container.
 * On failure *out is NULL and nothing is left allocated. */
Status gfxGifOpen(const uint8_t *data, size_t size, const GfxDecodeLimits *lim,
                  const GfxAllocator *a, GfxGif **out) {
    *out = NULL;
    GfxDecodeCtx d;
    gfxDecodeCtxInit(&d, lim, a);
    GifScanResult r;
    Status st = gifPrescan(&d, data, size, &r);
    if (st != STATUS_OK) {
        return st;
    }
    GfxGif *g = gfxDecAlloc(&d, sizeof(*g));
    if (g == NULL) {
        return gfxDecAllocStatus(&d);
    }
    memset(g, 0, sizeof(*g));
    g->ctx = d;
    g->data = data;
    g->size = size;
    g->info = r.info;
    g->gctPos = r.gctPos;
    g->gctCount = r.gctCount;
    g->firstFrame = g->pos = r.firstFrame;
    g->error = STATUS_OK;
    /* The exact budget is checked before either buffer is allocated. */
    uint64_t need = (uint64_t)r.info.width * r.info.height * 4u * (r.needsSave ? 2u : 1u);
    if (need > g->ctx.lim.maxTotalBytes || g->ctx.live > g->ctx.lim.maxTotalBytes - need) {
        gifDestroy(g);
        return STATUS_ERR_UNSUPPORTED;
    }
    st = gfxDecAllocImage(&g->ctx, r.info.width, r.info.height, &g->canvas);
    if (st != STATUS_OK) {
        gifDestroy(g);
        return st;
    }
    memset(g->canvas.pixels, 0, g->canvas.allocSize);
    if (r.needsSave) {
        g->saveSize = g->canvas.allocSize;
        g->save = gfxDecAlloc(&g->ctx, g->saveSize);
        if (g->save == NULL) {
            st = gfxDecAllocStatus(&g->ctx);
            gifDestroy(g);
            return st;
        }
    }
    *out = g;
    return STATUS_OK;
}

/* Pure. A NULL `g` yields {0, 0, 0, -1}. */
GfxGifInfo gfxGifGetInfo(const GfxGif *g) {
    if (g == NULL) {
        GfxGifInfo none = {0, 0, 0, -1};
        return none;
    }
    return g->info;
}

/* Applies the previous frame's disposal to its clipped rectangle: 2 clears it to transparent,
 * 3 restores what the save buffer held before that frame was drawn, anything else keeps it. */
static void gifDispose(GfxGif *g) {
    GfxRect r = g->prevRect;
    if (gfxRectIsEmpty(r)) {
        return;
    }
    uint32_t w = (uint32_t)r.x1 - (uint32_t)r.x0;
    for (uint32_t y = (uint32_t)r.y0; y < (uint32_t)r.y1; y++) {
        uint32_t *row = g->canvas.pixels + (size_t)y * g->canvas.width + (uint32_t)r.x0;
        if (g->prevDisposal == 2) {
            memset(row, 0, (size_t)w * 4u);
        } else if (g->prevDisposal == 3 && g->save != NULL) {
            memcpy(row, g->save + (size_t)(y - (uint32_t)r.y0) * w, (size_t)w * 4u);
        }
    }
}

/* Never sleeps, takes no locks, does not allocate. Per-call work is bounded by the frame area
 * (<= maxPixels) plus the data walked. Not thread-safe per object. */
Status gfxGifNextFrame(GfxGif *g, GfxGifFrame *f) {
    memset(f, 0, sizeof(*f));
    if (g->error != STATUS_OK) {
        return g->error;
    }
    if (g->index >= g->info.frameCount) {
        return STATUS_ERR_NOT_FOUND;
    }
    if (g->havePrev) {
        gifDispose(g);
    }
    GifBlockKind kind;
    GifFrameHdr fh;
    size_t pos = g->pos;
    Status st = gifScan(g->data, g->size, &pos, &kind, &fh, NULL);
    if (st == STATUS_OK && kind != GIF_BLOCK_IMAGE) {
        st = STATUS_ERR_INVALID; /* the pre-scan counted this frame, so this cannot happen */
    }
    if (st != STATUS_OK) {
        g->error = st;
        return st;
    }
    uint32_t W = g->canvas.width, H = g->canvas.height;
    uint32_t x0 = fh.x < W ? fh.x : W, y0 = fh.y < H ? fh.y : H;
    uint32_t x1 = fh.x + fh.w < W ? fh.x + fh.w : W, y1 = fh.y + fh.h < H ? fh.y + fh.h : H;
    bool empty = x1 <= x0 || y1 <= y0;
    GfxRect rect = {0, 0, 0, 0};
    if (!empty) {
        rect = (GfxRect){(int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1};
    }
    if (fh.disposal == 3 && !empty && g->save != NULL) { /* save BEFORE drawing */
        for (uint32_t y = y0; y < y1; y++) {
            memcpy(g->save + (size_t)(y - y0) * (x1 - x0), g->canvas.pixels + (size_t)y * W + x0,
                   (size_t)(x1 - x0) * 4u);
        }
    }
    if (fh.w != 0 && fh.h != 0) {
        GifPainter pt; /* ~1 KB with the palette; on the stack */
        GifPainter *p = &pt;
        memset(p, 0, sizeof(*p));
        p->canvas = g->canvas.pixels;
        p->stride = W;
        p->fx = fh.x;
        p->fy = fh.y;
        p->fw = fh.w;
        p->fh = fh.h;
        p->clipW = empty ? 0 : x1 - x0;
        p->clipH = empty ? 0 : y1 - y0;
        p->interlace = fh.interlace;
        p->left = (uint64_t)fh.w * fh.h;
        p->hasTransp = fh.hasTransp;
        p->transIdx = fh.transIdx;
        size_t palPos = fh.palCount != 0 ? fh.palPos : g->gctPos;
        p->palCount = fh.palCount != 0 ? fh.palCount : g->gctCount;
        for (uint32_t i = 0; i < p->palCount; i++) {
            const uint8_t *c = g->data + palPos + (size_t)i * 3;
            p->pal[i] = GIF_OPAQUE | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
        }
        st = gifLzw(g, &fh, p);
        if (st != STATUS_OK) {
            g->error = st;
            return st;
        }
    }
    g->pos = pos;
    g->havePrev = true;
    g->prevDisposal = fh.disposal;
    g->prevRect = rect;
    f->canvas = gfxImageSurface(&g->canvas);
    f->index = g->index++;
    f->delayMs = fh.delay * 10u;
    f->disposal = fh.disposal;
    f->rect = rect;
    return STATUS_OK;
}

/* Never sleeps, takes no locks, does not allocate. Not thread-safe per object. */
void gfxGifRewind(GfxGif *g) {
    if (g == NULL) {
        return;
    }
    g->pos = g->firstFrame;
    g->index = 0;
    g->error = STATUS_OK;
    g->havePrev = false;
    g->prevDisposal = 0;
    g->prevRect = (GfxRect){0, 0, 0, 0};
    memset(g->canvas.pixels, 0, g->canvas.allocSize);
}

/* NULL-safe. Never sleeps, takes no locks; only the allocator's free is called. */
void gfxGifClose(GfxGif *g) {
    if (g == NULL) {
        return;
    }
    gifDestroy(g);
}

/* Frame 0 composited onto a transparent canvas. Never sleeps, takes no locks. On failure *out is
 * zeroed and everything is freed (statuses as gfxGifOpen; INVALID also for an LZW error). On
 * success `out` owns the canvas (gfxImageFree it). */
Status gfxGifDecode(const uint8_t *data, size_t size, const GfxDecodeLimits *lim,
                    const GfxAllocator *a, GfxImage *out) {
    memset(out, 0, sizeof(*out));
    GfxGif *g;
    Status st = gfxGifOpen(data, size, lim, a, &g);
    if (st != STATUS_OK) {
        return st;
    }
    GfxGifFrame fr;
    st = gfxGifNextFrame(g, &fr);
    if (st == STATUS_OK) {
        *out = g->canvas; /* steal: alloc and allocSize stay intact for gfxImageFree */
        memset(&g->canvas, 0, sizeof(g->canvas));
    }
    gfxGifClose(g);
    return st;
}
