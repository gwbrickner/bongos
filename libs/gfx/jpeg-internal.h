/* Private to libs/gfx (and its host tests): the JPEG decoder's state and building blocks (M12.7,
 * D-160..D-162). Everything here is pure, reentrant, never sleeps, takes no locks and allocates
 * only through GfxDecodeCtx; the decoder's contract is in gfx-image.h. */
#ifndef LIBS_GFX_JPEG_INTERNAL_H
#define LIBS_GFX_JPEG_INTERNAL_H

#include "gfx/gfx-internal.h"

#define JPEG_MAX_SCANS 128u

/* T.81 Figure A.6: zigzag position -> natural (row-major) index. */
extern const uint8_t jpegZigzag[64];

/* A Huffman table (T.81 Annex C) with a 9-bit lookahead. `maxcode[l]` (l = 1..16) is the largest
 * code of length l or -1; `valoff[l]` maps a length-l code to its index in `vals`. */
typedef struct {
    uint8_t vals[256];
    int32_t maxcode[17];
    int32_t valoff[17];
    uint16_t lut[512]; /* len << 8 | symbol for codes of length <= 9; 0 = longer or invalid */
    bool defined;
} JpegHuff;

/* Builds `t` from the DHT counts (bits[i] = number of codes of length i+1) and `vals`. INVALID if
 * the counts sum to more than 256 or the code space is over-subscribed or uses the all-ones code
 * (T.81 C.2 reserves it). An all-zero count table is valid (no symbols). */
Status jpegHuffBuild(JpegHuff *t, const uint8_t bits[16], const uint8_t *vals);

/* MSB-first entropy bit reader. Reads past the end of the entropy segment (a marker, or the end of
 * the data) yield zero PAD bits: a Huffman lookahead may peek into them, but consuming one sets
 * `err`. FF 00 is a data 0xFF; FF FF 00 too (fill). */
typedef struct {
    const uint8_t *data;
    size_t size, pos;
    uint64_t acc;
    int nbits, npad; /* valid bits in acc (low bits); how many of them (at the bottom) are pad */
    bool marker;     /* a marker was reached: no more bytes are read */
    size_t markerPos;
    bool err;
} JpegBits;

/* Starts reading at data[pos]; pure, never fails. */
void jpegBitsInit(JpegBits *b, const uint8_t *data, size_t size, size_t pos);
/* n in 0..16. On a pad-bit read sets b->err and returns 0. */
uint32_t jpegGetBits(JpegBits *b, int n);
/* Decodes one symbol; -1 (and b->err) if the code is not in the table or runs into pad bits. */
int jpegDecodeSym(JpegBits *b, const JpegHuff *t);
/* Position of the FF of the next marker (skipping extraneous bytes, FF00 pairs and fill FFs) at or
 * after b->pos, or SIZE_MAX if there is none. */
size_t jpegBitsFindMarker(const JpegBits *b);

typedef struct {
    uint8_t id, h, v, tq;
    bool latched;   /* q holds the table current at this component's first scan (D-162) */
    bool coded;     /* sequential: some scan carried this component */
    uint16_t q[64]; /* natural order */
    int8_t
        coefBits[64]; /* progressive: successive-approximation state per coefficient, -1 = none */
    uint32_t compW, compH, nbx, nby, bw, bh;
    int16_t *coef; /* bw * bh blocks of 64, natural order, quantized */
} JpegComp;

typedef struct {
    GfxDecodeCtx *dc;
    const uint8_t *data;
    size_t size;
    uint16_t qt[4][64]; /* natural order */
    bool qtDef[4];
    JpegHuff dcTab[4], acTab[4];
    JpegComp comp[4];
    uint32_t nComp, width, height, hmax, vmax, mcusX, mcusY;
    uint32_t restartInterval;
    uint32_t nScans;
    int sofType; /* 0 baseline, 1 extended sequential, 2 progressive */
    bool haveFrame, jfif, adobe;
    uint8_t adobeTransform;
    int16_t *coefBuf;
    size_t coefBytes;
    GfxImage img;
} JpegDec;

typedef struct {
    uint32_t ns;
    uint32_t comp[4]; /* indexes into JpegDec.comp, in scan order */
    uint32_t dcSel[4], acSel[4];
    uint32_t ss, se, ah, al;
} JpegScan;

/* Decodes the entropy-coded data of one scan starting at `pos` into the coefficient buffer.
 * On success *endPos is the position of the FF of the next marker. Every malformed condition is
 * INVALID (D-160). */
Status jpegDecodeScan(JpegDec *j, const JpegScan *s, size_t pos, size_t *endPos);

/* The exact "islow" integer IDCT of a dequantized block (D-161): `c` quantized coefficients and
 * `q` the quantization table, both natural order; writes 8x8 samples (0..255) with row stride
 * `stride`. */
void jpegIdctIslow(const int16_t c[64], const uint16_t q[64], uint8_t *out, size_t stride);

/* libjpeg's 16-bit fixed-point YCbCr -> RGB (D-161); y, cb, cr are 0..255. */
static inline void jpegYccToRgb(int32_t y, int32_t cb, int32_t cr, uint32_t *r, uint32_t *g,
                                uint32_t *b) {
    cb -= 128;
    cr -= 128;
    int32_t rr = y + ((91881 * cr + 32768) >> 16);
    int32_t gg = y + ((-22554 * cb - 46802 * cr + 32768) >> 16);
    int32_t bb = y + ((116130 * cb + 32768) >> 16);
    *r = (uint32_t)(rr < 0 ? 0 : rr > 255 ? 255 : rr);
    *g = (uint32_t)(gg < 0 ? 0 : gg > 255 ? 255 : gg);
    *b = (uint32_t)(bb < 0 ? 0 : bb > 255 ? 255 : bb);
}

/* IDCT + upsample + color conversion of the whole frame into j->img, one MCU row at a time
 * through a strip buffer. NO_MEMORY/UNSUPPORTED from the accounting. */
Status jpegOutput(JpegDec *j);

#endif
