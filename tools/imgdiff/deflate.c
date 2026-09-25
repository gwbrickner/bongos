/* See deflate.h. */
#include "deflate.h"

#include <string.h>

typedef struct {
    uint8_t *out;
    size_t cap;
    size_t bytePos;
    uint32_t bitBuf;
    int bitCount;
    bool overflow;
} BitWriter;

static void bwPutBit(BitWriter *bw, int bit) {
    bw->bitBuf |= (uint32_t)(bit & 1) << bw->bitCount;
    bw->bitCount++;
    if (bw->bitCount == 8) {
        if (bw->bytePos >= bw->cap) {
            bw->overflow = true;
        } else {
            bw->out[bw->bytePos++] = (uint8_t)bw->bitBuf;
        }
        bw->bitBuf = 0;
        bw->bitCount = 0;
    }
}

/* Non-Huffman multi-bit fields: LSB first (RFC 1951 §3.1.1). */
static void bwBits(BitWriter *bw, uint32_t value, int n) {
    for (int i = 0; i < n; i++) {
        bwPutBit(bw, (int)((value >> i) & 1));
    }
}

/* A Huffman code's own bits are packed MSB-first within the code (RFC 1951 §3.1.1), each bit fed
 * through the same LSB-first bit-packer as everything else. */
static void bwHuffman(BitWriter *bw, uint32_t code, int len) {
    for (int i = len - 1; i >= 0; i--) {
        bwPutBit(bw, (int)((code >> i) & 1));
    }
}

static void bwFlushByte(BitWriter *bw) {
    while (bw->bitCount != 0) {
        bwPutBit(bw, 0);
    }
}

/* RFC 1951 §3.2.6's fixed literal/length code assignment. */
static void fixedLitCode(int sym, uint32_t *code, int *len) {
    if (sym <= 143) {
        *len = 8;
        *code = 0x30u + (uint32_t)sym;
    } else if (sym <= 255) {
        *len = 9;
        *code = 0x190u + (uint32_t)(sym - 144);
    } else if (sym <= 279) {
        *len = 7;
        *code = (uint32_t)(sym - 256);
    } else {
        *len = 8;
        *code = 0xC0u + (uint32_t)(sym - 280);
    }
}

/* Fixed distance codes are just the plain 5-bit binary value of the symbol (§3.2.6: "Distance
 * codes 0-31 are represented by (fixed-length) 5-bit codes"). */
static void fixedDistCode(int sym, uint32_t *code, int *len) {
    *len = 5;
    *code = (uint32_t)sym;
}

static const uint16_t LENGTH_BASE[29] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                         15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                         67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LENGTH_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                         2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DIST_BASE[30] = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void emitLength(BitWriter *bw, uint32_t length) {
    int i;
    for (i = 28; i >= 0; i--) {
        if (length >= LENGTH_BASE[i]) {
            break;
        }
    }
    uint32_t code;
    int len;
    fixedLitCode(257 + i, &code, &len);
    bwHuffman(bw, code, len);
    bwBits(bw, length - LENGTH_BASE[i], LENGTH_EXTRA[i]);
}

static void emitDistance(BitWriter *bw, uint32_t distance) {
    int i;
    for (i = 29; i >= 0; i--) {
        if (distance >= DIST_BASE[i]) {
            break;
        }
    }
    uint32_t code;
    int len;
    fixedDistCode(i, &code, &len);
    bwHuffman(bw, code, len);
    bwBits(bw, distance - DIST_BASE[i], DIST_EXTRA[i]);
}

/* Byte-for-byte match length starting at `data[pos]` against `data[pos - dist]`, capped at
 * `maxLen` (DEFLATE's own 258 cap, further capped by how much data is left). Overlapping matches
 * (length > dist) are valid LZ77 and handled correctly here: comparing the *source* array against
 * itself at increasing offsets works for any run where the pattern repeats with period `dist`. */
static uint32_t matchLength(const uint8_t *data, size_t pos, uint32_t dist, uint32_t maxLen) {
    uint32_t len = 0;
    while (len < maxLen && data[pos + len] == data[pos + len - dist]) {
        len++;
    }
    return len;
}

bool deflateFixed(const uint8_t *data, size_t size, uint32_t rowStride, uint8_t *out, size_t outCap,
                  size_t *outLen) {
    BitWriter bw = {out, outCap, 0, 0, 0, false};

    bwBits(&bw, 1, 1); /* BFINAL: this is the only block */
    bwBits(&bw, 1, 2); /* BTYPE: 01 = fixed Huffman */

    size_t pos = 0;
    while (pos < size) {
        uint32_t remaining = (size - pos > 258) ? 258 : (uint32_t)(size - pos);
        uint32_t bestLen = 0, bestDist = 0;
        if (remaining >= 3) {
            if (pos >= 3) {
                uint32_t len = matchLength(data, pos, 3, remaining);
                if (len >= 3 && len > bestLen) {
                    bestLen = len;
                    bestDist = 3;
                }
            }
            if (rowStride != 0 && rowStride <= 32768 && pos >= rowStride) {
                uint32_t len = matchLength(data, pos, rowStride, remaining);
                if (len >= 3 && len > bestLen) {
                    bestLen = len;
                    bestDist = rowStride;
                }
            }
        }
        if (bestLen >= 3) {
            emitLength(&bw, bestLen);
            emitDistance(&bw, bestDist);
            pos += bestLen;
        } else {
            uint32_t code;
            int len;
            fixedLitCode(data[pos], &code, &len);
            bwHuffman(&bw, code, len);
            pos++;
        }
        if (bw.overflow) {
            return false;
        }
    }

    uint32_t code;
    int len;
    fixedLitCode(256, &code, &len); /* end-of-block */
    bwHuffman(&bw, code, len);
    bwFlushByte(&bw);

    if (bw.overflow) {
        return false;
    }
    *outLen = bw.bytePos;
    return true;
}
