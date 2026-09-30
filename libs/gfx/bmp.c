/* See gfx-image.h: the BMP decoder. All arithmetic on file-controlled values is done in 64 bits
 * and every read is bounds-checked against `size` before it happens. */
#include "gfx/gfx-internal.h"

#include <string.h>

static uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One color channel's mask: where it sits and how wide it is. */
typedef struct {
    uint32_t mask;
    uint32_t shift, bits;
} Chan;

/* False if `mask` is not one contiguous run of ones (computed in 64 bits so 0xFFFFFFFF works). */
static bool chanInit(Chan *c, uint32_t mask) {
    c->mask = mask;
    c->shift = c->bits = 0;
    if (mask == 0) {
        return true; /* absent */
    }
    while (((mask >> c->shift) & 1u) == 0) {
        c->shift++;
    }
    uint64_t run = (uint64_t)(mask >> c->shift);
    if (((run + 1) & run) != 0) {
        return false; /* holes in the mask */
    }
    while (run != 0) {
        c->bits++;
        run >>= 1;
    }
    return true;
}

/* The channel's value scaled to 0..255 (>= 8 bits: the top 8; fewer: scaled with rounding). */
static uint32_t chanValue(const Chan *c, uint32_t pixel) {
    if (c->bits == 0) {
        return 0; /* an absent channel */
    }
    uint32_t v = (pixel & c->mask) >> c->shift;
    if (c->bits >= 8) {
        return v >> (c->bits - 8);
    }
    uint32_t max = (1u << c->bits) - 1u;
    return (v * 255u + max / 2u) / max;
}

Status gfxBmpDecode(const uint8_t *data, size_t size, const GfxDecodeLimits *lim,
                    const GfxAllocator *a, GfxImage *out) {
    memset(out, 0, sizeof(*out));
    if (size < 14 + 12 || data[0] != 'B' || data[1] != 'M') {
        return STATUS_ERR_INVALID;
    }
    uint64_t offBits = rd32(data + 10);
    uint32_t dib = rd32(data + 14);
    if (dib == 12 || dib == 64) {
        return STATUS_ERR_UNSUPPORTED; /* OS/2 headers */
    }
    if (dib != 40 && dib != 52 && dib != 56 && dib != 108 && dib != 124) {
        return STATUS_ERR_INVALID;
    }
    if ((uint64_t)14 + dib > size) {
        return STATUS_ERR_INVALID;
    }
    const uint8_t *h = data + 14;
    int32_t sw = (int32_t)rd32(h + 4), sh = (int32_t)rd32(h + 8);
    uint32_t planes = rd16(h + 12), bpp = rd16(h + 14), comp = rd32(h + 16), clrUsed = rd32(h + 32);
    if (sw <= 0 || sh == 0 || sh == INT32_MIN) {
        return STATUS_ERR_INVALID;
    }
    bool topDown = sh < 0;
    uint32_t width = (uint32_t)sw, height = topDown ? (uint32_t)(-(int64_t)sh) : (uint32_t)sh;
    if (planes != 1) {
        return STATUS_ERR_UNSUPPORTED;
    }
    if (comp == 1 || comp == 2 || comp == 4 || comp == 5) {
        return STATUS_ERR_UNSUPPORTED; /* RLE8, RLE4, embedded JPEG, embedded PNG */
    }
    if (comp != 0 && comp != 3 && comp != 6) {
        return STATUS_ERR_INVALID;
    }
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
        return STATUS_ERR_INVALID;
    }
    bool bitfields = comp == 3 || comp == 6;
    if (bitfields && bpp != 16 && bpp != 32) {
        return STATUS_ERR_INVALID;
    }

    GfxDecodeCtx d;
    gfxDecodeCtxInit(&d, lim, a);
    Status st = gfxDecCheckDims(&d, width, height);
    if (st != STATUS_OK) {
        return st;
    }

    /* Channel masks: in the header for V2+ (offset 54), else after a 40-byte header. */
    Chan cr, cg, cb, ca;
    uint32_t mr = 0, mg = 0, mb = 0, ma = 0;
    size_t maskEnd = (size_t)14 + dib; /* where the palette would start */
    if (bitfields) {
        if (dib == 40) { /* the masks follow the header: 3 for BITFIELDS, 4 for ALPHABITFIELDS */
            size_t nmask = comp == 6 ? 4 : 3;
            if ((uint64_t)14 + dib + nmask * 4 > size) {
                return STATUS_ERR_INVALID;
            }
            const uint8_t *m = data + 14 + dib;
            maskEnd += nmask * 4;
            mr = rd32(m);
            mg = rd32(m + 4);
            mb = rd32(m + 8);
            ma = comp == 6 ? rd32(m + 12) : 0;
        } else { /* V2+ headers hold R,G,B masks at 40; V3+ also the alpha mask at 52 */
            mr = rd32(h + 40);
            mg = rd32(h + 44);
            mb = rd32(h + 48);
            if (comp == 6 && dib < 56) {
                return STATUS_ERR_INVALID; /* a 52-byte header has no alpha mask to use */
            }
            ma = dib >= 56 ? rd32(h + 52) : 0;
        }
    } else if (bpp == 16) {
        mr = 0x7C00u, mg = 0x03E0u, mb = 0x001Fu; /* 5-5-5 */
    } else if (bpp == 32 || bpp == 24) {
        mr = 0x00FF0000u, mg = 0x0000FF00u, mb = 0x000000FFu; /* the fourth byte is ignored */
    }
    if (bpp == 16 && ((mr | mg | mb | ma) >> 16) != 0) {
        return STATUS_ERR_INVALID;
    }
    if (!chanInit(&cr, mr) || !chanInit(&cg, mg) || !chanInit(&cb, mb) || !chanInit(&ca, ma)) {
        return STATUS_ERR_INVALID;
    }
    if ((bpp == 16 || bpp == 32) && (mr == 0 || mg == 0 || mb == 0)) {
        return STATUS_ERR_INVALID;
    }
    if ((mr & mg) || (mr & mb) || (mg & mb) || (ma & (mr | mg | mb))) {
        return STATUS_ERR_INVALID; /* overlapping masks */
    }

    /* Palette. */
    uint32_t palette[256];
    uint32_t nPal = 0;
    if (bpp <= 8) {
        uint32_t maxPal = 1u << bpp;
        nPal = clrUsed != 0 ? clrUsed : maxPal;
        if (nPal > maxPal) {
            return STATUS_ERR_INVALID;
        }
        uint64_t palEnd = (uint64_t)maskEnd + (uint64_t)nPal * 4;
        if (palEnd > size || palEnd > offBits) {
            return STATUS_ERR_INVALID;
        }
        for (uint32_t i = 0; i < nPal; i++) {
            const uint8_t *e = data + maskEnd + (size_t)i * 4;
            palette[i] = 0xFF000000u | ((uint32_t)e[2] << 16) | ((uint32_t)e[1] << 8) | e[0];
        }
    }

    /* Pixel array: stride is rows padded to 4 bytes; it must lie wholly inside the file. */
    uint64_t stride = (((uint64_t)width * bpp + 31) / 32) * 4;
    if (offBits < (uint64_t)maskEnd || offBits > size || stride * height > size - offBits) {
        return STATUS_ERR_INVALID;
    }

    st = gfxDecAllocImage(&d, width, height, out);
    if (st != STATUS_OK) {
        return st;
    }
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *row = data + offBits + (uint64_t)y * stride;
        uint32_t *dst = out->pixels + (size_t)(topDown ? y : height - 1 - y) * width;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t px;
            switch (bpp) {
                case 1:
                case 4:
                case 8: {
                    uint32_t per = 8 / bpp;
                    uint32_t byte = row[x / per];
                    uint32_t idx = (byte >> (8 - bpp - (x % per) * bpp)) & ((1u << bpp) - 1u);
                    if (idx >= nPal) {
                        gfxImageFree(out);
                        return STATUS_ERR_INVALID; /* index past the palette */
                    }
                    dst[x] = palette[idx];
                    continue;
                }
                case 16:
                    px = rd16(row + (size_t)x * 2);
                    break;
                case 24:
                    px = ((uint32_t)row[(size_t)x * 3 + 2] << 16) |
                         ((uint32_t)row[(size_t)x * 3 + 1] << 8) | row[(size_t)x * 3];
                    break;
                default:
                    px = rd32(row + (size_t)x * 4);
                    break;
            }
            uint32_t alpha = ca.mask != 0 ? chanValue(&ca, px) : 255u;
            dst[x] =
                gfxPremulArgb(chanValue(&cr, px), chanValue(&cg, px), chanValue(&cb, px), alpha);
        }
    }
    return STATUS_OK;
}
