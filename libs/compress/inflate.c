/* See compress.h. Implements RFC 1951 directly: a canonical-Huffman decoder (the counts/offsets
 * construction RFC 1951 §3.2.2 specifies), then the three block types (§3.2.3-3.2.7). */
#include "compress/compress.h"

#include <stdbool.h>
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t bytePos;
    uint32_t bitBuf;
    int bitCount;
} BitReader;

/* Returns 0 or 1, or -1 past the end of input. */
static int brBit(BitReader *br) {
    if (br->bitCount == 0) {
        if (br->bytePos >= br->size) {
            return -1;
        }
        br->bitBuf = br->data[br->bytePos++];
        br->bitCount = 8;
    }
    int bit = (int)(br->bitBuf & 1u);
    br->bitBuf >>= 1;
    br->bitCount--;
    return bit;
}

/* Non-Huffman multi-bit fields are packed LSB-first (RFC 1951 §3.1.1). Returns -1 on EOF. */
static int32_t brBits(BitReader *br, int n) {
    int32_t v = 0;
    for (int i = 0; i < n; i++) {
        int bit = brBit(br);
        if (bit < 0) {
            return -1;
        }
        v |= (int32_t)bit << i;
    }
    return v;
}

/* Canonical Huffman decode table (RFC 1951 §3.2.2): `counts[len]` is how many symbols have code
 * length `len`, and `symbols[]` holds the symbols in the canonical assignment order (grouped by
 * length, then by value within a length), for the puff.c-style bit-at-a-time decode below. */
#define HUFF_MAX_BITS 15
#define HUFF_MAX_SYMS 288
typedef struct {
    uint16_t counts[HUFF_MAX_BITS + 1];
    uint16_t symbols[HUFF_MAX_SYMS];
} HuffTree;

/* Builds the decode table and enforces zlib's code-completeness rules: an over-subscribed set is
 * always invalid; an incomplete set is invalid too, except an empty set (a block with no distance
 * codes) and a lit/len or distance set whose only code has length 1. `isCodeLengths` (the
 * code-length alphabet) must always be complete. */
static bool huffBuild(HuffTree *h, const uint8_t *lengths, int n, bool isCodeLengths) {
    if (n > HUFF_MAX_SYMS) {
        return false;
    }
    memset(h->counts, 0, sizeof(h->counts));
    for (int i = 0; i < n; i++) {
        if (lengths[i] > HUFF_MAX_BITS) {
            return false;
        }
        h->counts[lengths[i]]++;
    }
    h->counts[0] = 0; /* length 0 means "this symbol doesn't appear" */
    int maxLen = 0;
    for (int len = HUFF_MAX_BITS; len >= 1; len--) {
        if (h->counts[len] != 0) {
            maxLen = len;
            break;
        }
    }
    if (maxLen != 0) {
        int left = 1;
        for (int len = 1; len <= HUFF_MAX_BITS; len++) {
            left <<= 1;
            left -= h->counts[len];
            if (left < 0) {
                return false; /* over-subscribed */
            }
        }
        if (left > 0 && (isCodeLengths || maxLen != 1)) {
            return false; /* incomplete */
        }
    }
    uint16_t offs[HUFF_MAX_BITS + 2];
    offs[1] = 0;
    for (int len = 1; len <= HUFF_MAX_BITS; len++) {
        offs[len + 1] = (uint16_t)(offs[len] + h->counts[len]);
    }
    for (int i = 0; i < n; i++) {
        if (lengths[i] != 0) {
            h->symbols[offs[lengths[i]]++] = (uint16_t)i;
        }
    }
    return true;
}

/* Decodes one symbol bit by bit against the canonical code ranges per length (the classic
 * "puff.c"-style decode this RFC's own construction enables: no bit-reversal or lookup table
 * needed, just tracking the running code value and each length's first-code/first-index). Returns
 * -1 on EOF or an invalid code. */
static int32_t huffDecode(BitReader *br, const HuffTree *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= HUFF_MAX_BITS; len++) {
        int bit = brBit(br);
        if (bit < 0) {
            return -1;
        }
        code |= bit;
        int count = h->counts[len];
        if (code - first < count) {
            return h->symbols[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
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

static void buildFixedTrees(HuffTree *lit, HuffTree *dist) {
    uint8_t litLengths[288];
    int i = 0;
    for (; i < 144; i++) {
        litLengths[i] = 8;
    }
    for (; i < 256; i++) {
        litLengths[i] = 9;
    }
    for (; i < 280; i++) {
        litLengths[i] = 7;
    }
    for (; i < 288; i++) {
        litLengths[i] = 8;
    }
    huffBuild(lit, litLengths, 288, false);

    /* 32 five-bit codes, not 30: a 30-code set is incomplete and would fail huffBuild's check;
     * symbols 30 and 31 are rejected where distances are decoded. */
    uint8_t distLengths[32];
    for (i = 0; i < 32; i++) {
        distLengths[i] = 5;
    }
    huffBuild(dist, distLengths, 32, false);
}

static const uint8_t CLC_ORDER[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                      11, 4,  12, 3, 13, 2, 14, 1, 15};

static bool buildDynamicTrees(BitReader *br, HuffTree *lit, HuffTree *dist) {
    int32_t hlitField = brBits(br, 5);
    int32_t hdistField = brBits(br, 5);
    int32_t hclenField = brBits(br, 4);
    if (hlitField < 0 || hdistField < 0 || hclenField < 0) {
        return false;
    }
    int hlit = hlitField + 257;
    int hdist = hdistField + 1;
    int hclen = hclenField + 4;
    if (hlit > 286 || hdist > 30) {
        return false;
    }

    uint8_t clLengths[19];
    memset(clLengths, 0, sizeof(clLengths));
    for (int i = 0; i < hclen; i++) {
        int32_t v = brBits(br, 3);
        if (v < 0) {
            return false;
        }
        clLengths[CLC_ORDER[i]] = (uint8_t)v;
    }
    HuffTree clTree;
    if (!huffBuild(&clTree, clLengths, 19, true)) {
        return false;
    }

    uint8_t lengths[288 + 32];
    memset(lengths, 0, sizeof(lengths));
    int total = hlit + hdist;
    if (total > (int)sizeof(lengths)) {
        return false;
    }
    int n = 0;
    while (n < total) {
        int32_t sym = huffDecode(br, &clTree);
        if (sym < 0) {
            return false;
        }
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (n == 0) {
                return false;
            }
            int32_t rep = brBits(br, 2);
            if (rep < 0) {
                return false;
            }
            rep += 3;
            uint8_t prev = lengths[n - 1];
            for (int i = 0; i < rep; i++) {
                if (n >= total) {
                    return false;
                }
                lengths[n++] = prev;
            }
        } else if (sym == 17 || sym == 18) {
            int32_t rep = brBits(br, sym == 17 ? 3 : 7);
            if (rep < 0) {
                return false;
            }
            rep += (sym == 17) ? 3 : 11;
            for (int i = 0; i < rep; i++) {
                if (n >= total) {
                    return false;
                }
                lengths[n++] = 0;
            }
        } else {
            return false;
        }
    }
    if (lengths[256] == 0) {
        return false; /* no end-of-block code: the block could never terminate */
    }
    return huffBuild(lit, lengths, hlit, false) && huffBuild(dist, lengths + hlit, hdist, false);
}

static Status inflateBlockData(BitReader *br, const HuffTree *lit, const HuffTree *dist,
                               uint8_t *out, size_t outCap, size_t *outLen) {
    for (;;) {
        int32_t sym = huffDecode(br, lit);
        if (sym < 0) {
            return STATUS_ERR_INVALID;
        }
        if (sym < 256) {
            if (*outLen >= outCap) {
                return STATUS_ERR_NO_MEMORY;
            }
            out[(*outLen)++] = (uint8_t)sym;
        } else if (sym == 256) {
            return STATUS_OK;
        } else {
            uint32_t lsym = (uint32_t)sym - 257;
            if (lsym >= 29) {
                return STATUS_ERR_INVALID;
            }
            int32_t extra = brBits(br, LENGTH_EXTRA[lsym]);
            if (extra < 0) {
                return STATUS_ERR_INVALID;
            }
            uint32_t length = LENGTH_BASE[lsym] + (uint32_t)extra;

            int32_t dsym = huffDecode(br, dist);
            if (dsym < 0 || dsym >= 30) {
                return STATUS_ERR_INVALID;
            }
            int32_t dextra = brBits(br, DIST_EXTRA[dsym]);
            if (dextra < 0) {
                return STATUS_ERR_INVALID;
            }
            uint32_t distance = DIST_BASE[dsym] + (uint32_t)dextra;
            if (distance == 0 || distance > *outLen) {
                return STATUS_ERR_INVALID;
            }
            if (length > outCap - *outLen) {
                return STATUS_ERR_NO_MEMORY;
            }
            size_t srcPos = *outLen - distance;
            for (uint32_t i = 0; i < length; i++) {
                out[*outLen] = out[srcPos];
                (*outLen)++;
                srcPos++;
            }
        }
    }
}

Status compressInflateRaw(const uint8_t *in, size_t inLen, uint8_t *out, size_t outCap,
                          size_t *outLen, size_t *inUsed) {
    BitReader br = {in, inLen, 0, 0, 0};
    size_t produced = 0;
    Status st = STATUS_OK;
    for (;;) {
        int bfinal = brBit(&br);
        int32_t btype = brBits(&br, 2);
        if (bfinal < 0 || btype < 0) {
            st = STATUS_ERR_INVALID;
            break;
        }
        if (btype == 0) {
            br.bitCount = 0; /* discard the rest of the current byte -- stored blocks are
                              * byte-aligned */
            if (br.size - br.bytePos < 4) {
                st = STATUS_ERR_INVALID;
                break;
            }
            uint16_t len = (uint16_t)(br.data[br.bytePos] | (br.data[br.bytePos + 1] << 8));
            uint16_t nlen = (uint16_t)(br.data[br.bytePos + 2] | (br.data[br.bytePos + 3] << 8));
            if ((uint16_t)(~len) != nlen) {
                st = STATUS_ERR_INVALID;
                break;
            }
            br.bytePos += 4;
            if (len > br.size - br.bytePos) {
                st = STATUS_ERR_INVALID;
                break;
            }
            if (len > outCap - produced) {
                st = STATUS_ERR_NO_MEMORY;
                break;
            }
            memcpy(out + produced, br.data + br.bytePos, len);
            produced += len;
            br.bytePos += len;
        } else if (btype == 1 || btype == 2) {
            HuffTree lit, dist;
            if (btype == 1) {
                buildFixedTrees(&lit, &dist);
            } else if (!buildDynamicTrees(&br, &lit, &dist)) {
                st = STATUS_ERR_INVALID;
                break;
            }
            st = inflateBlockData(&br, &lit, &dist, out, outCap, &produced);
            if (st != STATUS_OK) {
                break;
            }
        } else {
            st = STATUS_ERR_INVALID; /* btype 3 is reserved */
            break;
        }
        if (bfinal) {
            break;
        }
    }
    if (outLen != NULL) {
        *outLen = produced;
    }
    if (inUsed != NULL) {
        *inUsed = (st == STATUS_OK) ? br.bytePos : 0;
    }
    return st;
}
