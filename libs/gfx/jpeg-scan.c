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

/* Progressive DC scan, first pass (G.1.2.1): the point-transformed DC difference. */
static Status dcFirst(JpegBits *b, const JpegHuff *dc, int32_t *pred, int16_t *blk, uint32_t al) {
    int t = jpegDecodeSym(b, dc);
    if (t < 0 || t > 11) {
        return STATUS_ERR_INVALID;
    }
    *pred = sat16((int64_t)*pred + extend(jpegGetBits(b, t), t));
    blk[0] = (int16_t)sat16((int64_t)*pred * ((int64_t)1 << al));
    return b->err ? STATUS_ERR_INVALID : STATUS_OK;
}

/* DC refinement: one bit per block, OR-ed in as two's complement (the DC point transform is an
 * arithmetic shift). */
static Status dcRefine(JpegBits *b, int16_t *blk, uint32_t al) {
    if (jpegGetBits(b, 1) != 0) {
        blk[0] = (int16_t)(blk[0] | (1 << al));
    }
    return b->err ? STATUS_ERR_INVALID : STATUS_OK;
}

/* AC first pass over the band ss..se (G.1.2.2), with EOB runs. */
static Status acFirst(JpegBits *b, const JpegHuff *ac, int16_t *blk, uint32_t ss, uint32_t se,
                      uint32_t al, uint32_t *eobrun) {
    if (*eobrun > 0) {
        (*eobrun)--;
        return STATUS_OK;
    }
    for (uint32_t k = ss; k <= se; k++) {
        int rs = jpegDecodeSym(b, ac);
        if (rs < 0) {
            return STATUS_ERR_INVALID;
        }
        uint32_t r = (uint32_t)rs >> 4, s = (uint32_t)rs & 15;
        if (s != 0) {
            k += r;
            if (k > se || s > 10) {
                return STATUS_ERR_INVALID;
            }
            blk[jpegZigzag[k]] = (int16_t)sat16((int64_t)extend(jpegGetBits(b, (int)s), (int)s) *
                                                ((int64_t)1 << al));
        } else if (r == 15) {
            k += 15; /* ZRL */
            if (k > se) {
                return STATUS_ERR_INVALID;
            }
        } else {
            *eobrun = (1u << r) + (r != 0 ? jpegGetBits(b, (int)r) : 0) - 1;
            break;
        }
    }
    return b->err ? STATUS_ERR_INVALID : STATUS_OK;
}

static void refineCoef(JpegBits *b, int16_t *cf, int32_t p1) {
    if (jpegGetBits(b, 1) != 0 && (*cf & p1) == 0) {
        *cf = (int16_t)sat16((int64_t)*cf + (*cf >= 0 ? p1 : -p1));
    }
}

/* AC refinement (G.1.2.3): correction bits for coefficients that are already nonzero, and new
 * +-1 coefficients placed after `r` zero-history coefficients. */
static Status acRefine(JpegBits *b, const JpegHuff *ac, int16_t *blk, uint32_t ss, uint32_t se,
                       uint32_t al, uint32_t *eobrun) {
    const int32_t p1 = 1 << al;
    uint32_t k = ss;
    if (*eobrun == 0) {
        for (; k <= se; k++) {
            int rs = jpegDecodeSym(b, ac);
            if (rs < 0) {
                return STATUS_ERR_INVALID;
            }
            int32_t r = rs >> 4, s = rs & 15, val = 0;
            if (s != 0) {
                if (s != 1) {
                    return STATUS_ERR_INVALID;
                }
                val = jpegGetBits(b, 1) != 0 ? p1 : -p1;
            } else if (r != 15) {
                *eobrun = (1u << r) + (r != 0 ? jpegGetBits(b, r) : 0);
                break; /* the end-of-run tail below handles this block */
            }
            do { /* r == 15 with s == 0 is a ZRL: skip 16 zero-history coefficients */
                int16_t *cf = &blk[jpegZigzag[k]];
                if (*cf != 0) {
                    refineCoef(b, cf, p1);
                } else if (--r < 0) {
                    break;
                }
                k++;
            } while (k <= se);
            if (val != 0) {
                if (k > se) {
                    return STATUS_ERR_INVALID;
                }
                blk[jpegZigzag[k]] = (int16_t)val;
            }
        }
    }
    if (*eobrun > 0) {
        for (; k <= se; k++) {
            int16_t *cf = &blk[jpegZigzag[k]];
            if (*cf != 0) {
                refineCoef(b, cf, p1);
            }
        }
        (*eobrun)--;
    }
    return b->err ? STATUS_ERR_INVALID : STATUS_OK;
}

typedef struct {
    JpegBits b;
    int32_t pred[4];
    uint32_t eobrun;
} ScanState;

/* Decodes one block of scan component `k` in the scan's mode. */
static Status scanBlock(const JpegDec *j, const JpegScan *s, ScanState *st, uint32_t k,
                        int16_t *blk) {
    if (j->sofType != 2) {
        return seqBlock(&st->b, &j->dcTab[s->dcSel[k]], &j->acTab[s->acSel[k]], &st->pred[k], blk);
    }
    if (s->ss == 0) {
        return s->ah == 0 ? dcFirst(&st->b, &j->dcTab[s->dcSel[k]], &st->pred[k], blk, s->al)
                          : dcRefine(&st->b, blk, s->al);
    }
    const JpegHuff *ac = &j->acTab[s->acSel[k]];
    return s->ah == 0 ? acFirst(&st->b, ac, blk, s->ss, s->se, s->al, &st->eobrun)
                      : acRefine(&st->b, ac, blk, s->ss, s->se, s->al, &st->eobrun);
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
    ScanState st;
    memset(&st, 0, sizeof(st));
    jpegBitsInit(&st.b, j->data, j->size, pos);
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
            Status rs = restartSync(&st.b, &rst);
            if (rs != STATUS_OK) {
                return rs;
            }
            memset(st.pred, 0, sizeof(st.pred));
            st.eobrun = 0;
        }
        if (s->ns == 1) {
            JpegComp *c = &j->comp[s->comp[0]];
            uint32_t bx = (uint32_t)(i % c->nbx), by = (uint32_t)(i / c->nbx);
            Status bs = scanBlock(j, s, &st, 0, c->coef + ((size_t)by * c->bw + bx) * 64);
            if (bs != STATUS_OK) {
                return bs;
            }
        } else {
            uint32_t mx = (uint32_t)(i % j->mcusX), my = (uint32_t)(i / j->mcusX);
            for (uint32_t k = 0; k < s->ns; k++) {
                JpegComp *c = &j->comp[s->comp[k]];
                for (uint32_t v = 0; v < c->v; v++) {
                    for (uint32_t h = 0; h < c->h; h++) {
                        uint32_t bx = mx * c->h + h, by = my * c->v + v;
                        Status bs =
                            scanBlock(j, s, &st, k, c->coef + ((size_t)by * c->bw + bx) * 64);
                        if (bs != STATUS_OK) {
                            return bs;
                        }
                    }
                }
            }
        }
    }
    size_t m = st.b.marker ? st.b.markerPos : jpegBitsFindMarker(&st.b);
    if (m == SIZE_MAX) {
        return STATUS_ERR_INVALID;
    }
    *endPos = m;
    return STATUS_OK;
}
