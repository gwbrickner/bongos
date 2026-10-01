/* See jpeg-internal.h: the JPEG entropy bit reader and Huffman tables (T.81 Annex C, F.2.2.3). */
#include "gfx/jpeg-internal.h"

#include <string.h>

Status jpegHuffBuild(JpegHuff *t, const uint8_t bits[16], const uint8_t *vals) {
    uint32_t total = 0;
    for (int i = 0; i < 16; i++) {
        total += bits[i];
    }
    if (total > 256) {
        return STATUS_ERR_INVALID;
    }
    memset(t, 0, sizeof(*t));
    memcpy(t->vals, vals, total);
    uint32_t code = 0, k = 0;
    for (uint32_t l = 1; l <= 16; l++) {
        uint32_t n = bits[l - 1];
        t->maxcode[l] = -1;
        if (n != 0) {
            /* over-subscription, and the reserved all-ones code, both end up here */
            if (code + n >= (1u << l)) {
                memset(t, 0, sizeof(*t));
                return STATUS_ERR_INVALID;
            }
            t->valoff[l] = (int32_t)k - (int32_t)code;
            if (l <= 9) {
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t base = (code + i) << (9 - l);
                    uint32_t span = 1u << (9 - l);
                    for (uint32_t s = 0; s < span; s++) {
                        t->lut[base + s] = (uint16_t)((l << 8) | t->vals[k + i]);
                    }
                }
            }
            code += n;
            k += n;
            t->maxcode[l] = (int32_t)code - 1;
        }
        code <<= 1;
    }
    t->defined = true;
    return STATUS_OK;
}

void jpegBitsInit(JpegBits *b, const uint8_t *data, size_t size, size_t pos) {
    memset(b, 0, sizeof(*b));
    b->data = data;
    b->size = size;
    b->pos = pos;
}

/* Tops the accumulator up to at least 57 bits, appending zero pad bits once a marker or the end
 * of the data is reached. */
static void bitsFill(JpegBits *b) {
    while (b->nbits <= 56) {
        uint32_t byte = 0;
        bool pad = b->marker || b->pos >= b->size;
        if (!pad) {
            uint8_t c = b->data[b->pos];
            if (c != 0xFF) {
                byte = c;
                b->pos++;
            } else {
                size_t p = b->pos + 1;
                while (p < b->size && b->data[p] == 0xFF) {
                    p++;
                }
                if (p < b->size && b->data[p] == 0x00) {
                    byte = 0xFF;
                    b->pos = p + 1;
                } else {
                    /* a marker (or a dangling FF at the end: the caller's marker read fails) */
                    b->marker = true;
                    b->markerPos = p - 1;
                    pad = true;
                }
            }
        }
        if (pad) {
            b->npad += 8;
        }
        b->acc = (b->acc << 8) | byte;
        b->nbits += 8;
    }
}

uint32_t jpegGetBits(JpegBits *b, int n) {
    if (n == 0) {
        return 0;
    }
    if (b->nbits < 32) {
        bitsFill(b);
    }
    if (n > b->nbits - b->npad) {
        b->err = true;
        return 0;
    }
    uint32_t v = (uint32_t)(b->acc >> (b->nbits - n)) & ((1u << n) - 1u);
    b->nbits -= n;
    return v;
}

int jpegDecodeSym(JpegBits *b, const JpegHuff *t) {
    if (b->nbits < 32) {
        bitsFill(b);
    }
    uint32_t look = (uint32_t)(b->acc >> (b->nbits - 9)) & 0x1FFu;
    uint16_t e = t->lut[look];
    int len = 0, sym = 0;
    if (e != 0) {
        len = e >> 8;
        sym = e & 0xFF;
    } else {
        for (int l = 10; l <= 16; l++) {
            int32_t code = (int32_t)((b->acc >> (b->nbits - l)) & (((uint64_t)1 << l) - 1u));
            if (code <= t->maxcode[l]) {
                len = l;
                sym = t->vals[t->valoff[l] + code];
                break;
            }
        }
    }
    if (len == 0 || len > b->nbits - b->npad) {
        b->err = true;
        return -1;
    }
    b->nbits -= len;
    return sym;
}

size_t jpegBitsFindMarker(const JpegBits *b) {
    size_t p = b->pos;
    while (p + 1 < b->size) {
        if (b->data[p] != 0xFF) {
            p++;
            continue;
        }
        size_t q = p + 1;
        while (q < b->size && b->data[q] == 0xFF) {
            q++;
        }
        if (q >= b->size) {
            return SIZE_MAX;
        }
        if (b->data[q] != 0x00) {
            return q - 1;
        }
        p = q + 1;
    }
    return SIZE_MAX;
}
