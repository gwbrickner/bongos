/* See jpeg-internal.h: entropy-coded scan decoding (T.81 F.2.2 sequential, G.1.2 progressive),
 * restart handling and the strict policy of D-160. */
#include "gfx/jpeg-internal.h"

#include <string.h>

static int32_t sat16(int64_t v) {
    return v < -32768 ? -32768 : v > 32767 ? 32767 : (int32_t)v;
}

/* T.81 F.2.2.1 EXTEND: the t-bit value v as a signed number. */
static int32_t extend(uint32_t v, int t) {
    if (t == 0) {
        return 0;
    }
    return v < (1u << (t - 1)) ? (int32_t)v - (int32_t)(1u << t) + 1 : (int32_t)v;
}

/* One sequential-mode block (F.2.2): DC difference, then AC run/size pairs to EOB or k = 63. */
static Status seqBlock(JpegBits *b, const JpegHuff *dc, const JpegHuff *ac, int32_t *pred,
                       int16_t *blk) {
    int t = jpegDecodeSym(b, dc);
    if (t < 0 || t > 11) {
        return STATUS_ERR_INVALID;
    }
    *pred = sat16((int64_t)*pred + extend(jpegGetBits(b, t), t));
    blk[0] = (int16_t)*pred;
    for (int k = 1; k < 64; k++) {
        int rs = jpegDecodeSym(b, ac);
        if (rs < 0) {
            return STATUS_ERR_INVALID;
        }
        int r = rs >> 4, s = rs & 15;
        if (s != 0) {
            k += r;
            if (k > 63 || s > 10) {
                return STATUS_ERR_INVALID;
            }
            blk[jpegZigzag[k]] = (int16_t)extend(jpegGetBits(b, s), s);
        } else if (r == 15) {
            k += 15; /* ZRL: 16 zeros (the loop's k++ is the 16th) */
            if (k > 63) {
                return STATUS_ERR_INVALID;
            }
        } else {
            break; /* EOB */
        }
    }
    return b->err ? STATUS_ERR_INVALID : STATUS_OK;
}

/* At a restart point: drop the bit buffer, skip extraneous bytes to the next marker, which must
 * be exactly RST(n & 7). No resync (D-160). */
static Status restartSync(JpegBits *b, uint32_t *rst) {
    size_t m = b->marker ? b->markerPos : jpegBitsFindMarker(b);
    if (m == SIZE_MAX) {
        return STATUS_ERR_INVALID;
    }
    size_t p = m + 1;
    while (p < b->size && b->data[p] == 0xFF) {
        p++;
    }
    if (p >= b->size || b->data[p] != 0xD0 + (*rst & 7u)) {
        return STATUS_ERR_INVALID;
    }
    b->pos = p + 1;
    b->acc = 0;
    b->nbits = b->npad = 0;
    b->marker = false;
    (*rst)++;
    return STATUS_OK;
}

Status jpegDecodeScan(JpegDec *j, const JpegScan *s, size_t pos, size_t *endPos) {
    JpegBits b;
    jpegBitsInit(&b, j->data, j->size, pos);
    int32_t pred[4] = {0, 0, 0, 0};
    uint32_t rst = 0;
    const uint32_t ri = j->restartInterval;
    uint64_t total;
    if (s->ns == 1) {
        const JpegComp *c = &j->comp[s->comp[0]];
        total = (uint64_t)c->nbx * c->nby;
    } else {
        total = (uint64_t)j->mcusX * j->mcusY;
    }
    for (uint64_t i = 0; i < total; i++) {
        if (ri != 0 && i != 0 && i % ri == 0) {
            Status st = restartSync(&b, &rst);
            if (st != STATUS_OK) {
                return st;
            }
            memset(pred, 0, sizeof(pred));
        }
        if (s->ns == 1) {
            JpegComp *c = &j->comp[s->comp[0]];
            uint32_t bx = (uint32_t)(i % c->nbx), by = (uint32_t)(i / c->nbx);
            int16_t *blk = c->coef + ((size_t)by * c->bw + bx) * 64;
            Status st = seqBlock(&b, &j->dcTab[s->dcSel[0]], &j->acTab[s->acSel[0]], &pred[0], blk);
            if (st != STATUS_OK) {
                return st;
            }
        } else {
            uint32_t mx = (uint32_t)(i % j->mcusX), my = (uint32_t)(i / j->mcusX);
            for (uint32_t k = 0; k < s->ns; k++) {
                JpegComp *c = &j->comp[s->comp[k]];
                for (uint32_t v = 0; v < c->v; v++) {
                    for (uint32_t h = 0; h < c->h; h++) {
                        uint32_t bx = mx * c->h + h, by = my * c->v + v;
                        int16_t *blk = c->coef + ((size_t)by * c->bw + bx) * 64;
                        Status st = seqBlock(&b, &j->dcTab[s->dcSel[k]], &j->acTab[s->acSel[k]],
                                             &pred[k], blk);
                        if (st != STATUS_OK) {
                            return st;
                        }
                    }
                }
            }
        }
    }
    size_t m = b.marker ? b.markerPos : jpegBitsFindMarker(&b);
    if (m == SIZE_MAX) {
        return STATUS_ERR_INVALID;
    }
    *endPos = m;
    return STATUS_OK;
}
