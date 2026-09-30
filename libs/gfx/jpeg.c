/* See gfx-image.h: the JPEG decoder (ITU-T T.81). Marker parsing, frame setup with the exact
 * up-front budget check (D-162), and the strict policy of D-160; scans are in jpeg-scan.c and the
 * pixel pipeline in jpeg-idct.c. */
#include "gfx/jpeg-internal.h"

#include <string.h>

static uint32_t rd16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

static bool mulAdd(uint64_t *acc, uint64_t a, uint64_t b) {
    uint64_t t;
    return __builtin_mul_overflow(a, b, &t) || __builtin_add_overflow(*acc, t, acc);
}

/* DQT: one or more tables, each Pq/Tq, then 64 values (zigzag order) of 8 or 16 bits. */
static Status parseDqt(JpegDec *j, const uint8_t *p, size_t n) {
    while (n > 0) {
        uint32_t pq = p[0] >> 4, tq = p[0] & 15;
        size_t need = 1 + (pq == 0 ? 64u : 128u);
        if (pq > 1 || tq > 3 || n < need) {
            return STATUS_ERR_INVALID;
        }
        for (int k = 0; k < 64; k++) {
            uint32_t v = pq == 0 ? p[1 + k] : rd16(p + 1 + 2 * k);
            j->qt[tq][jpegZigzag[k]] = (uint16_t)v;
        }
        j->qtDef[tq] = true;
        p += need;
        n -= need;
    }
    return STATUS_OK;
}

/* DHT: one or more tables, each Tc/Th, 16 counts, then the symbols. */
static Status parseDht(JpegDec *j, const uint8_t *p, size_t n) {
    while (n > 0) {
        if (n < 17) {
            return STATUS_ERR_INVALID;
        }
        uint32_t tc = p[0] >> 4, th = p[0] & 15;
        uint32_t total = 0;
        for (int i = 0; i < 16; i++) {
            total += p[1 + i];
        }
        if (tc > 1 || th > 3 || total > 256 || n < 17 + (size_t)total) {
            return STATUS_ERR_INVALID;
        }
        Status st = jpegHuffBuild(tc == 0 ? &j->dcTab[th] : &j->acTab[th], p + 1, p + 17);
        if (st != STATUS_OK) {
            return st;
        }
        p += 17 + total;
        n -= 17 + total;
    }
    return STATUS_OK;
}

static Status parseSof(JpegDec *j, int type, const uint8_t *p, size_t n) {
    if (j->haveFrame || n < 6) {
        return STATUS_ERR_INVALID;
    }
    uint32_t prec = p[0], height = rd16(p + 1), width = rd16(p + 3), nf = p[5];
    if (n != 6 + 3 * (size_t)nf) {
        return STATUS_ERR_INVALID;
    }
    if (prec != 8 || height == 0 || nf == 2 || nf > 3) {
        return STATUS_ERR_UNSUPPORTED; /* 12-bit, DNL-defined height, CMYK/YCCK */
    }
    if (width == 0 || nf == 0) {
        return STATUS_ERR_INVALID;
    }
    for (uint32_t i = 0; i < nf; i++) {
        JpegComp *c = &j->comp[i];
        c->id = p[6 + 3 * i];
        c->h = p[7 + 3 * i] >> 4;
        c->v = p[7 + 3 * i] & 15;
        c->tq = p[8 + 3 * i];
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3) {
            return STATUS_ERR_INVALID;
        }
        for (uint32_t k = 0; k < i; k++) {
            if (j->comp[k].id == c->id) {
                return STATUS_ERR_INVALID;
            }
        }
    }
    Status st = gfxDecCheckDims(j->dc, width, height);
    if (st != STATUS_OK) {
        return st;
    }
    j->nComp = nf;
    j->width = width;
    j->height = height;
    j->sofType = type;
    for (uint32_t i = 0; i < nf; i++) {
        j->hmax = j->comp[i].h > j->hmax ? j->comp[i].h : j->hmax;
        j->vmax = j->comp[i].v > j->vmax ? j->comp[i].v : j->vmax;
    }
    j->mcusX = (width + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcusY = (height + 8 * j->vmax - 1) / (8 * j->vmax);
    /* Everything this decode will ever hold at once, checked BEFORE allocating: the state (already
     * live), the coefficients, the output and the output pass's strips (D-162). */
    uint64_t coefBytes = 0, stripBytes = 0;
    bool ovf = false;
    for (uint32_t i = 0; i < nf; i++) {
        JpegComp *c = &j->comp[i];
        c->compW = (uint32_t)(((uint64_t)width * c->h + j->hmax - 1) / j->hmax);
        c->compH = (uint32_t)(((uint64_t)height * c->v + j->vmax - 1) / j->vmax);
        c->nbx = (c->compW + 7) / 8;
        c->nby = (c->compH + 7) / 8;
        c->bw = j->mcusX * c->h;
        c->bh = j->mcusY * c->v;
        ovf |= mulAdd(&coefBytes, (uint64_t)c->bw * c->bh, 128);
        ovf |= mulAdd(&stripBytes, (uint64_t)c->nbx * 8, (uint64_t)c->v * 8);
        memset(c->coefBits, -1, sizeof(c->coefBits));
    }
    uint64_t need = j->dc->live;
    ovf |= __builtin_add_overflow(need, coefBytes, &need);
    ovf |= __builtin_add_overflow(need, (uint64_t)width * height * 4, &need);
    ovf |= __builtin_add_overflow(need, stripBytes, &need);
    if (ovf || need > j->dc->lim.maxTotalBytes || coefBytes > (uint64_t)SIZE_MAX) {
        return STATUS_ERR_UNSUPPORTED;
    }
    j->coefBuf = gfxDecAlloc(j->dc, (size_t)coefBytes);
    if (j->coefBuf == NULL) {
        return gfxDecAllocStatus(j->dc);
    }
    j->coefBytes = (size_t)coefBytes;
    memset(j->coefBuf, 0, j->coefBytes);
    size_t off = 0;
    for (uint32_t i = 0; i < nf; i++) {
        j->comp[i].coef = j->coefBuf + off;
        off += (size_t)j->comp[i].bw * j->comp[i].bh * 64;
    }
    j->haveFrame = true;
    return gfxDecAllocImage(j->dc, width, height, &j->img);
}

/* SOS header -> `s`; latches quantization tables at each component's first scan (D-162). */
static Status parseSos(JpegDec *j, const uint8_t *p, size_t n, JpegScan *s) {
    if (!j->haveFrame || n < 1) {
        return STATUS_ERR_INVALID;
    }
    uint32_t ns = p[0];
    if (ns == 0 || ns > 4 || ns > j->nComp || n != 4 + 2 * (size_t)ns) {
        return STATUS_ERR_INVALID;
    }
    s->ns = ns;
    uint32_t blocks = 0;
    for (uint32_t i = 0; i < ns; i++) {
        uint32_t idx = j->nComp;
        for (uint32_t k = 0; k < j->nComp; k++) {
            if (j->comp[k].id == p[1 + 2 * i]) {
                idx = k;
            }
        }
        if (idx == j->nComp) {
            return STATUS_ERR_INVALID;
        }
        for (uint32_t k = 0; k < i; k++) {
            if (s->comp[k] == idx) {
                return STATUS_ERR_INVALID;
            }
        }
        s->comp[i] = idx;
        s->dcSel[i] = p[2 + 2 * i] >> 4;
        s->acSel[i] = p[2 + 2 * i] & 15;
        if (s->dcSel[i] > 3 || s->acSel[i] > 3) {
            return STATUS_ERR_INVALID;
        }
        blocks += (uint32_t)j->comp[idx].h * j->comp[idx].v;
    }
    if (ns > 1 && blocks > 10) {
        return STATUS_ERR_INVALID;
    }
    s->ss = p[1 + 2 * ns];
    s->se = p[2 + 2 * ns];
    s->ah = p[3 + 2 * ns] >> 4;
    s->al = p[3 + 2 * ns] & 15;
    bool needDc = true, needAc = true;
    if (j->sofType == 2) {
        /* T.81 G.1.1.1.1: DC scans (Ss = 0) may be interleaved, AC scans are single-component */
        if (s->ss == 0 ? s->se != 0 : (s->se < s->ss || s->se > 63 || ns != 1)) {
            return STATUS_ERR_INVALID;
        }
        if (s->ah > 13 || s->al > 13 || (s->ah != 0 && s->al != s->ah - 1)) {
            return STATUS_ERR_INVALID;
        }
        needDc = s->ss == 0 && s->ah == 0;
        needAc = s->ss != 0;
        for (uint32_t i = 0; i < ns; i++) {
            JpegComp *c = &j->comp[s->comp[i]];
            if (s->ss != 0 && c->coefBits[0] < 0) {
                return STATUS_ERR_INVALID; /* an AC scan before the component's DC first scan */
            }
            for (uint32_t k = s->ss; k <= s->se; k++) {
                if (c->coefBits[k] != (s->ah == 0 ? -1 : (int8_t)s->ah)) {
                    return STATUS_ERR_INVALID; /* not the next refinement of this coefficient */
                }
            }
        }
    }
    for (uint32_t i = 0; i < ns; i++) {
        JpegComp *c = &j->comp[s->comp[i]];
        if (j->sofType != 2 && c->coded) {
            return STATUS_ERR_INVALID; /* a component in two sequential scans */
        }
        if (!c->latched) {
            if (!j->qtDef[c->tq]) {
                return STATUS_ERR_INVALID;
            }
            memcpy(c->q, j->qt[c->tq], sizeof(c->q));
            c->latched = true;
        }
        if ((needDc && !j->dcTab[s->dcSel[i]].defined) ||
            (needAc && !j->acTab[s->acSel[i]].defined)) {
            return STATUS_ERR_INVALID;
        }
    }
    if (j->sofType == 2) {
        for (uint32_t i = 0; i < ns; i++) {
            for (uint32_t k = s->ss; k <= s->se; k++) {
                j->comp[s->comp[i]].coefBits[k] = (int8_t)s->al;
            }
        }
    }
    return STATUS_OK;
}

static Status jpegRun(JpegDec *j, GfxImage *out) {
    const uint8_t *d = j->data;
    size_t size = j->size, pos = 2; /* past SOI (the caller sniffed FF D8) */
    for (;;) {
        if (pos >= size || d[pos] != 0xFF) {
            return STATUS_ERR_INVALID;
        }
        while (pos < size && d[pos] == 0xFF) {
            pos++;
        }
        if (pos >= size || d[pos] == 0x00) {
            return STATUS_ERR_INVALID;
        }
        uint32_t m = d[pos++];
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            continue; /* TEM and stray RSTn carry no length */
        }
        if (m == 0xD9) {
            if (!j->haveFrame || j->nScans == 0) {
                return STATUS_ERR_INVALID;
            }
            break;
        }
        if (m == 0xD8) {
            return STATUS_ERR_INVALID;
        }
        if ((m >= 0xC3 && m <= 0xCF && m != 0xC4) || m == 0xDE || m == 0xDF ||
            (m >= 0xF0 && m <= 0xFD) || (m >= 0x02 && m <= 0xBF)) {
            return STATUS_ERR_UNSUPPORTED;
        }
        if (pos + 2 > size) {
            return STATUS_ERR_INVALID;
        }
        size_t len = rd16(d + pos);
        if (len < 2 || pos + len > size) {
            return STATUS_ERR_INVALID;
        }
        const uint8_t *p = d + pos + 2;
        size_t n = len - 2;
        pos += len;
        Status st = STATUS_OK;
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            st = parseSof(j, (int)(m - 0xC0), p, n);
        } else if (m == 0xC4) {
            st = parseDht(j, p, n);
        } else if (m == 0xDB) {
            st = parseDqt(j, p, n);
        } else if (m == 0xDD) {
            if (n != 2) {
                return STATUS_ERR_INVALID;
            }
            j->restartInterval = rd16(p);
        } else if (m == 0xE0 && n >= 5 && memcmp(p, "JFIF\0", 5) == 0) {
            j->jfif = true;
        } else if (m == 0xEE && n >= 12 && memcmp(p, "Adobe", 5) == 0) {
            j->adobe = true;
            j->adobeTransform = p[11];
        } else if (m == 0xDA) {
            JpegScan s;
            st = parseSos(j, p, n, &s);
            if (st == STATUS_OK && j->nScans >= JPEG_MAX_SCANS) {
                st = STATUS_ERR_UNSUPPORTED;
            }
            if (st == STATUS_OK) {
                j->nScans++;
                st = jpegDecodeScan(j, &s, pos, &pos);
            }
            if (st == STATUS_OK) {
                for (uint32_t i = 0; i < s.ns; i++) {
                    j->comp[s.comp[i]].coded = true;
                }
            }
        } /* everything else (APPn, COM, DNL) is length-checked and skipped */
        if (st != STATUS_OK) {
            return st;
        }
    }
    for (uint32_t i = 0; i < j->nComp; i++) {
        /* every component needs its (first) DC data: a sequential scan or a progressive DC scan */
        if (j->sofType == 2 ? j->comp[i].coefBits[0] < 0 : !j->comp[i].coded) {
            return STATUS_ERR_INVALID;
        }
    }
    Status st = jpegOutput(j);
    if (st == STATUS_OK) {
        *out = j->img;
        memset(&j->img, 0, sizeof(j->img));
    }
    return st;
}

Status gfxJpegDecode(const uint8_t *data, size_t size, const GfxDecodeLimits *lim,
                     const GfxAllocator *a, GfxImage *out) {
    memset(out, 0, sizeof(*out));
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return STATUS_ERR_INVALID;
    }
    GfxDecodeCtx d;
    gfxDecodeCtxInit(&d, lim, a);
    JpegDec *j = gfxDecAlloc(&d, sizeof(*j));
    if (j == NULL) {
        return gfxDecAllocStatus(&d);
    }
    memset(j, 0, sizeof(*j));
    j->dc = &d;
    j->data = data;
    j->size = size;
    Status st = jpegRun(j, out);
    if (st != STATUS_OK) {
        memset(out, 0, sizeof(*out));
    }
    gfxImageFree(&j->img); /* only set if the decode failed after the output was allocated */
    if (j->coefBuf != NULL) {
        gfxDecFree(&d, j->coefBuf, j->coefBytes);
    }
    gfxDecFree(&d, j, sizeof(*j));
    return st;
}
