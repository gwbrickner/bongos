/* See jpeg-internal.h: the exact pixel pipeline of D-161 -- "islow" IDCT (a Loeffler-Ligtenberg-
 * Moschytz factorization with 13-bit constants, written from the algorithm), replicated chroma
 * upsampling, libjpeg's 16-bit fixed-point YCbCr conversion. Integer only. */
#include "gfx/jpeg-internal.h"

#include <string.h>

const uint8_t jpegZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* round(x / 2^n), an arithmetic shift (x may be negative). */
static inline int64_t descale(int64_t x, int n) {
    return (x + ((int64_t)1 << (n - 1))) >> n;
}

/* One 8-point pass. FIX(x) = round(x * 8192); a negative value is never left-shifted (UBSan),
 * the products use `* 8192` instead. */
static void idct1d(const int64_t v[8], int64_t o[8]) {
    int64_t z1 = (v[2] + v[6]) * 4433;
    int64_t t2 = z1 - v[6] * 15137;
    int64_t t3 = z1 + v[2] * 6270;
    int64_t t0 = (v[0] + v[4]) * 8192;
    int64_t t1 = (v[0] - v[4]) * 8192;
    int64_t t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
    int64_t o0 = v[7], o1 = v[5], o2 = v[3], o3 = v[1];
    z1 = o0 + o3;
    int64_t z2 = o1 + o2, z3 = o0 + o2, z4 = o1 + o3;
    int64_t z5 = (z3 + z4) * 9633;
    o0 *= 2446;
    o1 *= 16819;
    o2 *= 25172;
    o3 *= 12299;
    z1 *= -7373;
    z2 *= -20995;
    z3 *= -16069;
    z4 *= -3196;
    z3 += z5;
    z4 += z5;
    o0 += z1 + z3;
    o1 += z2 + z4;
    o2 += z2 + z3;
    o3 += z1 + z4;
    o[0] = t10 + o3;
    o[1] = t11 + o2;
    o[2] = t12 + o1;
    o[3] = t13 + o0;
    o[4] = t13 - o0;
    o[5] = t12 - o1;
    o[6] = t11 - o2;
    o[7] = t10 - o3;
}

void jpegIdctIslow(const int16_t c[64], const uint16_t q[64], uint8_t *out, size_t stride) {
    int64_t d[64];
    for (int i = 0; i < 64; i++) {
        int64_t v = (int64_t)c[i] * q[i];
        d[i] = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
    }
    int32_t ws[64];
    for (int col = 0; col < 8; col++) {
        int64_t in[8], o[8];
        for (int i = 0; i < 8; i++) {
            in[i] = d[8 * i + col];
        }
        idct1d(in, o);
        for (int i = 0; i < 8; i++) {
            ws[8 * i + col] = (int32_t)descale(o[i], 11);
        }
    }
    for (int row = 0; row < 8; row++) {
        int64_t in[8], o[8];
        for (int i = 0; i < 8; i++) {
            in[i] = ws[8 * row + i];
        }
        idct1d(in, o);
        for (int i = 0; i < 8; i++) {
            int64_t v = descale(o[i], 18) + 128;
            out[(size_t)row * stride + (size_t)i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
}

typedef enum { CS_GRAY, CS_YCC, CS_RGB } ColorSpace;

/* libjpeg's rule (D-160): JFIF wins; else Adobe's transform flag; else the component ids. */
static ColorSpace colorSpaceOf(const JpegDec *j) {
    if (j->nComp == 1) {
        return CS_GRAY;
    }
    if (j->jfif) {
        return CS_YCC;
    }
    if (j->adobe) {
        return j->adobeTransform == 0 ? CS_RGB : CS_YCC;
    }
    if (j->comp[0].id == 'R' && j->comp[1].id == 'G' && j->comp[2].id == 'B') {
        return CS_RGB;
    }
    return CS_YCC;
}

Status jpegOutput(JpegDec *j) {
    size_t stripSize[3], total = 0;
    for (uint32_t c = 0; c < j->nComp; c++) {
        stripSize[c] = (size_t)j->comp[c].nbx * 8 * j->comp[c].v * 8;
        total += stripSize[c];
    }
    uint8_t *strips = gfxDecAlloc(j->dc, total);
    if (strips == NULL) {
        return gfxDecAllocStatus(j->dc);
    }
    uint8_t *strip[3];
    size_t off = 0;
    for (uint32_t c = 0; c < j->nComp; c++) {
        strip[c] = strips + off;
        off += stripSize[c];
    }
    const ColorSpace cs = colorSpaceOf(j);
    const uint32_t w = j->width, h = j->height;
    for (uint32_t my = 0; my < j->mcusY; my++) {
        for (uint32_t c = 0; c < j->nComp; c++) {
            const JpegComp *cp = &j->comp[c];
            size_t sw = (size_t)cp->nbx * 8;
            for (uint32_t v = 0; v < cp->v; v++) {
                uint32_t by = my * cp->v + v;
                if (by >= cp->nby) {
                    break; /* padding rows are never referenced */
                }
                for (uint32_t bx = 0; bx < cp->nbx; bx++) {
                    jpegIdctIslow(cp->coef + ((size_t)by * cp->bw + bx) * 64, cp->q,
                                  strip[c] + (size_t)v * 8 * sw + (size_t)bx * 8, sw);
                }
            }
        }
        uint32_t y0 = my * 8 * j->vmax, y1 = y0 + 8 * j->vmax;
        if (y1 > h) {
            y1 = h;
        }
        for (uint32_t y = y0; y < y1; y++) {
            uint32_t *dst = j->img.pixels + (size_t)y * w;
            const uint8_t *row[3];
            for (uint32_t c = 0; c < j->nComp; c++) {
                const JpegComp *cp = &j->comp[c];
                uint32_t sy = (uint32_t)((uint64_t)y * cp->v / j->vmax) - my * 8 * cp->v;
                row[c] = strip[c] + (size_t)sy * cp->nbx * 8;
            }
            for (uint32_t x = 0; x < w; x++) {
                uint32_t s[3];
                for (uint32_t c = 0; c < j->nComp; c++) {
                    s[c] = row[c][(uint64_t)x * j->comp[c].h / j->hmax];
                }
                uint32_t r, g, b;
                if (cs == CS_GRAY) {
                    r = g = b = s[0];
                } else if (cs == CS_RGB) {
                    r = s[0];
                    g = s[1];
                    b = s[2];
                } else {
                    jpegYccToRgb((int32_t)s[0], (int32_t)s[1], (int32_t)s[2], &r, &g, &b);
                }
                dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }
    gfxDecFree(j->dc, strips, total);
    return STATUS_OK;
}
