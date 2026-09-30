/* See gfx-image.h: the PNG decoder (W3C PNG spec, second edition). Chunk framing and CRCs are
 * verified, sizes are derived from IHDR in 64 bits and checked against the decode limits before
 * any allocation, and the inflate output buffer is exactly the raw size IHDR implies, so a
 * decompression bomb is rejected as soon as it exceeds it. */
#include "compress/compress.h"
#include "gfx/gfx-internal.h"

#include <string.h>

static const uint8_t PNG_SIGNATURE[8] = {137, 80, 78, 71, 13, 10, 26, 10};

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Adam7 passes: origin and step of pass p's pixels. A non-interlaced image is one pass. */
static const uint8_t ADAM_XS[7] = {0, 4, 0, 2, 0, 1, 0};
static const uint8_t ADAM_YS[7] = {0, 0, 4, 0, 2, 0, 1};
static const uint8_t ADAM_DX[7] = {8, 8, 4, 4, 2, 2, 1};
static const uint8_t ADAM_DY[7] = {8, 8, 8, 4, 4, 2, 2};

typedef struct {
    uint32_t w, h;
    uint32_t depth, ctype, interlaced;
    uint32_t channels, bitsPerPixel;
    uint32_t nPal;
    uint32_t pal[256]; /* premultiplied, with tRNS alpha applied */
    bool hasTrns;      /* types 0 and 2: a single transparent color */
    uint32_t trnsR, trnsG, trnsB;
} PngInfo;

typedef struct {
    uint32_t pw, ph; /* pass dimensions in pixels; 0 for an empty pass */
    uint32_t xs, ys, dx, dy;
    uint64_t rowBytes;
} Pass;

static uint32_t passCount(const PngInfo *pi) {
    return pi->interlaced ? 7u : 1u;
}

static Pass passOf(const PngInfo *pi, uint32_t p) {
    Pass ps;
    if (!pi->interlaced) {
        ps.pw = pi->w;
        ps.ph = pi->h;
        ps.xs = ps.ys = 0;
        ps.dx = ps.dy = 1;
    } else {
        ps.xs = ADAM_XS[p];
        ps.ys = ADAM_YS[p];
        ps.dx = ADAM_DX[p];
        ps.dy = ADAM_DY[p];
        ps.pw = pi->w > ps.xs ? (pi->w - ps.xs + ps.dx - 1) / ps.dx : 0;
        ps.ph = pi->h > ps.ys ? (pi->h - ps.ys + ps.dy - 1) / ps.dy : 0;
    }
    ps.rowBytes = ((uint64_t)ps.pw * pi->bitsPerPixel + 7) / 8;
    return ps;
}

/* Bytes of one pass in the inflated stream: an empty pass has none, not even filter bytes. */
static uint64_t passBytes(const Pass *ps) {
    return (ps->pw == 0 || ps->ph == 0) ? 0 : (uint64_t)ps->ph * (1 + ps->rowBytes);
}

static bool validCombo(uint32_t ctype, uint32_t depth) {
    switch (ctype) {
        case 0:
            return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
        case 2:
        case 4:
        case 6:
            return depth == 8 || depth == 16;
        case 3:
            return depth == 1 || depth == 2 || depth == 4 || depth == 8;
        default:
            return false;
    }
}

static uint32_t paeth(uint32_t a, uint32_t b, uint32_t c) {
    int32_t p = (int32_t)a + (int32_t)b - (int32_t)c;
    int32_t pa = p > (int32_t)a ? p - (int32_t)a : (int32_t)a - p;
    int32_t pb = p > (int32_t)b ? p - (int32_t)b : (int32_t)b - p;
    int32_t pc = p > (int32_t)c ? p - (int32_t)c : (int32_t)c - p;
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

/* Reverses the row filters of one pass in place. `bpp` is bytes per complete pixel (at least 1).
 * The row above the first row is all zeros. False for a filter type above 4. */
static bool unfilterPass(uint8_t *p, const Pass *ps, uint32_t bpp) {
    size_t rb = (size_t)ps->rowBytes;
    for (uint32_t r = 0; r < ps->ph; r++) {
        uint8_t *row = p + (size_t)r * (rb + 1);
        uint8_t ft = row[0];
        uint8_t *cur = row + 1;
        const uint8_t *prev = r != 0 ? cur - (rb + 1) : NULL;
        if (ft > 4) {
            return false;
        }
        for (size_t i = 0; i < rb; i++) {
            uint32_t a = i >= bpp ? cur[i - bpp] : 0u;
            uint32_t b = prev != NULL ? prev[i] : 0u;
            uint32_t c = (prev != NULL && i >= bpp) ? prev[i - bpp] : 0u;
            uint32_t v = cur[i];
            switch (ft) {
                case 1:
                    v += a;
                    break;
                case 2:
                    v += b;
                    break;
                case 3:
                    v += (a + b) / 2u;
                    break;
                case 4:
                    v += paeth(a, b, c);
                    break;
                default:
                    break;
            }
            cur[i] = (uint8_t)v;
        }
    }
    return true;
}

/* Sample `i` (a channel index across the row) of a sub-byte or 8-bit row, native depth. */
static uint32_t sampleAt(const uint8_t *row, uint64_t i, uint32_t depth) {
    if (depth == 8) {
        return row[i];
    }
    if (depth == 16) {
        return ((uint32_t)row[2 * i] << 8) | row[2 * i + 1];
    }
    uint64_t bit = i * depth;
    uint32_t byte = row[bit / 8];
    uint32_t shift = 8 - depth - (uint32_t)(bit % 8);
    return (byte >> shift) & ((1u << depth) - 1u);
}

/* Native sample -> 0..255. */
static uint32_t to8(uint32_t v, uint32_t depth) {
    switch (depth) {
        case 16:
            return (v * 255u + 32767u) / 65535u;
        case 8:
            return v;
        default:
            return v * (255u / ((1u << depth) - 1u)); /* 1, 2, 4 bits: x255, x85, x17 */
    }
}

static uint32_t premulArgb(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return (a << 24) | (gfxMulDiv255(r, a) << 16) | (gfxMulDiv255(g, a) << 8) | gfxMulDiv255(b, a);
}

/* Pixel `i` of a row. Sets *bad for a palette index past PLTE. */
static uint32_t pixelAt(const PngInfo *pi, const uint8_t *row, uint32_t i, bool *bad) {
    uint32_t d = pi->depth;
    switch (pi->ctype) {
        case 0: {
            uint32_t v = sampleAt(row, i, d);
            uint32_t g = to8(v, d);
            return premulArgb(g, g, g, (pi->hasTrns && v == pi->trnsG) ? 0u : 255u);
        }
        case 2: {
            uint32_t r = sampleAt(row, 3 * (uint64_t)i, d),
                     g = sampleAt(row, 3 * (uint64_t)i + 1, d),
                     b = sampleAt(row, 3 * (uint64_t)i + 2, d);
            bool clear = pi->hasTrns && r == pi->trnsR && g == pi->trnsG && b == pi->trnsB;
            return premulArgb(to8(r, d), to8(g, d), to8(b, d), clear ? 0u : 255u);
        }
        case 3: {
            uint32_t idx = sampleAt(row, i, d);
            if (idx >= pi->nPal) {
                *bad = true;
                return 0;
            }
            return pi->pal[idx];
        }
        case 4: {
            uint32_t g = to8(sampleAt(row, 2 * (uint64_t)i, d), d),
                     a = to8(sampleAt(row, 2 * (uint64_t)i + 1, d), d);
            return premulArgb(g, g, g, a);
        }
        default: {
            uint32_t r = to8(sampleAt(row, 4 * (uint64_t)i, d), d),
                     g = to8(sampleAt(row, 4 * (uint64_t)i + 1, d), d),
                     b = to8(sampleAt(row, 4 * (uint64_t)i + 2, d), d),
                     a = to8(sampleAt(row, 4 * (uint64_t)i + 3, d), d);
            return premulArgb(r, g, b, a);
        }
    }
}

Status gfxPngDecode(const uint8_t *data, size_t size, const GfxDecodeLimits *lim,
                    const GfxAllocator *a, GfxImage *out) {
    memset(out, 0, sizeof(*out));
    if (size < 8 || memcmp(data, PNG_SIGNATURE, 8) != 0) {
        return STATUS_ERR_INVALID;
    }
    GfxDecodeCtx d;
    gfxDecodeCtxInit(&d, lim, a);

    PngInfo *pi = gfxDecAlloc(&d, sizeof(PngInfo));
    if (pi == NULL) {
        return gfxDecAllocStatus(&d);
    }
    memset(pi, 0, sizeof(*pi));
    Status st = STATUS_OK;
    uint8_t *idat = NULL, *raw = NULL;
    size_t idatLen = 0, rawLen = 0;

    /* Pass 1: walk the chunks, validating framing, CRCs and ordering. */
    const uint8_t *plte = NULL, *trns = NULL;
    size_t plteLen = 0, trnsLen = 0;
    size_t idatStart = 0;
    uint64_t idatTotal = 0;
    bool haveIhdr = false, sawIdat = false, idatEnded = false, sawIend = false;
    size_t off = 8;
    for (uint32_t index = 0; off < size && !sawIend; index++) {
        if (size - off < 12) {
            st = STATUS_ERR_INVALID; /* truncated chunk header */
            goto done;
        }
        uint32_t len = rd32be(data + off);
        if (len > 0x7FFFFFFFu || (uint64_t)len > size - off - 12) {
            st = STATUS_ERR_INVALID;
            goto done;
        }
        const uint8_t *type = data + off + 4, *body = data + off + 8;
        uint32_t crc = compressCrc32(0, type, 4);
        crc = compressCrc32(crc, body, len);
        if (crc != rd32be(body + len)) {
            st = STATUS_ERR_INVALID;
            goto done;
        }
        bool isIhdr = memcmp(type, "IHDR", 4) == 0, isIdat = memcmp(type, "IDAT", 4) == 0;
        if ((index == 0) != isIhdr) {
            st = STATUS_ERR_INVALID; /* IHDR must come first, and only once */
            goto done;
        }
        if (sawIdat && !isIdat) {
            idatEnded = true;
        }
        if (isIhdr) {
            if (len != 13) {
                st = STATUS_ERR_INVALID;
                goto done;
            }
            pi->w = rd32be(body);
            pi->h = rd32be(body + 4);
            pi->depth = body[8];
            pi->ctype = body[9];
            pi->interlaced = body[12];
            if (pi->w == 0 || pi->h == 0 || pi->w > 0x7FFFFFFFu || pi->h > 0x7FFFFFFFu ||
                !validCombo(pi->ctype, pi->depth) || body[10] != 0 || body[11] != 0 ||
                pi->interlaced > 1) {
                st = STATUS_ERR_INVALID;
                goto done;
            }
            st = gfxDecCheckDims(&d, pi->w, pi->h);
            if (st != STATUS_OK) {
                goto done;
            }
            static const uint8_t CHANNELS[7] = {1, 0, 3, 1, 2, 0, 4};
            pi->channels = CHANNELS[pi->ctype];
            pi->bitsPerPixel = pi->channels * pi->depth;
            haveIhdr = true;
        } else if (memcmp(type, "PLTE", 4) == 0) {
            if (plte != NULL || sawIdat || pi->ctype == 0 || pi->ctype == 4 || len % 3 != 0 ||
                len == 0 || len > 768 || (pi->ctype == 3 && len / 3 > (1u << pi->depth))) {
                st = STATUS_ERR_INVALID;
                goto done;
            }
            plte = body;
            plteLen = len;
        } else if (memcmp(type, "tRNS", 4) == 0) {
            if (trns != NULL || sawIdat) {
                st = STATUS_ERR_INVALID;
                goto done;
            }
            if (pi->ctype == 0 && len != 2) {
                st = STATUS_ERR_INVALID;
                goto done;
            }
            if (pi->ctype == 2 && len != 6) {
                st = STATUS_ERR_INVALID;
                goto done;
            }
            if (pi->ctype == 3 && (plte == NULL || len > plteLen / 3)) {
                st = STATUS_ERR_INVALID; /* tRNS needs its PLTE first, and no more entries */
                goto done;
            }
            trns = body;
            trnsLen = len;
        } else if (isIdat) {
            if (idatEnded) {
                st = STATUS_ERR_INVALID; /* IDATs must be consecutive */
                goto done;
            }
            if (!sawIdat) {
                idatStart = off;
                sawIdat = true;
            }
            idatTotal += len;
        } else if (memcmp(type, "IEND", 4) == 0) {
            if (len != 0) {
                st = STATUS_ERR_INVALID;
                goto done;
            }
            sawIend = true;
        } else if ((type[0] & 0x20u) == 0) {
            st = STATUS_ERR_UNSUPPORTED; /* an unknown critical chunk */
            goto done;
        }
        off += 12 + (size_t)len;
    }
    if (!haveIhdr || !sawIdat || !sawIend || (pi->ctype == 3 && plte == NULL)) {
        st = STATUS_ERR_INVALID;
        goto done;
    }

    /* Palette and transparency tables. */
    if (pi->ctype == 3) {
        pi->nPal = (uint32_t)(plteLen / 3);
        for (uint32_t i = 0; i < pi->nPal; i++) {
            uint32_t alpha = (trns != NULL && i < trnsLen) ? trns[i] : 255u;
            pi->pal[i] = premulArgb(plte[3 * i], plte[3 * i + 1], plte[3 * i + 2], alpha);
        }
    } else if (trns != NULL && pi->ctype == 0) {
        pi->hasTrns = true;
        pi->trnsG = ((uint32_t)trns[0] << 8) | trns[1];
        if (pi->depth < 16) {
            pi->trnsG &= (1u << pi->depth) - 1u; /* only the low bits are meaningful */
        }
    } else if (trns != NULL && pi->ctype == 2) {
        pi->hasTrns = true;
        pi->trnsR = ((uint32_t)trns[0] << 8) | trns[1];
        pi->trnsG = ((uint32_t)trns[2] << 8) | trns[3];
        pi->trnsB = ((uint32_t)trns[4] << 8) | trns[5];
        if (pi->depth < 16) {
            uint32_t m = (1u << pi->depth) - 1u;
            pi->trnsR &= m;
            pi->trnsG &= m;
            pi->trnsB &= m;
        }
    }
    (void)trnsLen;

    /* The inflated size IHDR implies, before anything big is allocated. */
    uint64_t rawTotal = 0;
    for (uint32_t p = 0; p < passCount(pi); p++) {
        Pass ps = passOf(pi, p);
        rawTotal += passBytes(&ps);
    }
    if (rawTotal > (uint64_t)SIZE_MAX || idatTotal > (uint64_t)SIZE_MAX) {
        st = STATUS_ERR_UNSUPPORTED;
        goto done;
    }
    rawLen = (size_t)rawTotal;
    idatLen = (size_t)idatTotal;

    /* Pass 2: concatenate the IDAT payloads. */
    idat = gfxDecAlloc(&d, idatLen);
    if (idat == NULL) {
        st = gfxDecAllocStatus(&d);
        goto done;
    }
    {
        size_t o = idatStart, w = 0;
        while (o < size && memcmp(data + o + 4, "IDAT", 4) == 0) {
            uint32_t len = rd32be(data + o);
            memcpy(idat + w, data + o + 8, len);
            w += len;
            o += 12 + (size_t)len;
        }
    }
    raw = gfxDecAlloc(&d, rawLen);
    if (raw == NULL) {
        st = gfxDecAllocStatus(&d);
        goto done;
    }
    size_t got = 0;
    st = compressZlibInflate(idat, idatLen, raw, rawLen, &got, NULL);
    if (st == STATUS_ERR_NO_MEMORY) {
        st = STATUS_ERR_INVALID; /* the stream inflates to more than IHDR allows: a bomb */
    }
    if (st != STATUS_OK) {
        goto done;
    }
    if (got != rawLen) {
        st = STATUS_ERR_INVALID; /* too little data */
        goto done;
    }
    gfxDecFree(&d, idat, idatLen);
    idat = NULL;

    /* Unfilter each pass, then convert into the image. */
    uint32_t bpp = pi->bitsPerPixel >= 8 ? pi->bitsPerPixel / 8 : 1;
    st = gfxDecAllocImage(&d, pi->w, pi->h, out);
    if (st != STATUS_OK) {
        goto done;
    }
    uint8_t *p = raw;
    for (uint32_t pass = 0; pass < passCount(pi); pass++) {
        Pass ps = passOf(pi, pass);
        if (passBytes(&ps) == 0) {
            continue;
        }
        if (!unfilterPass(p, &ps, bpp)) {
            st = STATUS_ERR_INVALID;
            goto done;
        }
        bool bad = false;
        for (uint32_t j = 0; j < ps.ph; j++) {
            const uint8_t *row = p + (size_t)j * ((size_t)ps.rowBytes + 1) + 1;
            uint32_t *dst = out->pixels + (size_t)(ps.ys + (uint64_t)j * ps.dy) * pi->w + ps.xs;
            for (uint32_t i = 0; i < ps.pw; i++) {
                dst[(size_t)i * ps.dx] = pixelAt(pi, row, i, &bad);
            }
        }
        if (bad) {
            st = STATUS_ERR_INVALID; /* a palette index past PLTE */
            goto done;
        }
        p += passBytes(&ps);
    }

done:
    gfxDecFree(&d, idat, idatLen);
    gfxDecFree(&d, raw, rawLen);
    gfxDecFree(&d, pi, sizeof(PngInfo));
    if (st != STATUS_OK) {
        gfxImageFree(out);
    }
    return st;
}
