/* See compress.h. */
#include "compress/compress.h"

#define ADLER_MOD 65521u

uint32_t compressAdler32(uint32_t adler, const uint8_t *p, size_t n) {
    uint32_t a = adler & 0xFFFFu;
    uint32_t b = adler >> 16;
    for (size_t i = 0; i < n; i++) {
        a = (a + p[i]) % ADLER_MOD;
        b = (b + a) % ADLER_MOD;
    }
    return (b << 16) | a;
}

Status compressZlibInflate(const uint8_t *in, size_t inLen, uint8_t *out, size_t outCap,
                           size_t *outLen, size_t *inUsed) {
    if (inLen < 6) {
        return STATUS_ERR_INVALID;
    }
    uint8_t cmf = in[0];
    uint8_t flg = in[1];
    if ((cmf & 0x0Fu) != 8) {
        return STATUS_ERR_INVALID; /* CM must be 8 (deflate) */
    }
    if (((uint32_t)cmf * 256u + flg) % 31u != 0) {
        return STATUS_ERR_INVALID; /* FCHECK */
    }
    if ((flg & 0x20u) != 0) {
        return STATUS_ERR_INVALID; /* FDICT: a preset dictionary isn't supported */
    }

    size_t produced = 0, rawUsed = 0;
    Status st = compressInflateRaw(in + 2, inLen - 2, out, outCap, &produced, &rawUsed);
    if (st != STATUS_OK) {
        return st;
    }
    size_t trailer = 2 + rawUsed;
    if (inLen - trailer < 4) { /* rawUsed <= inLen - 2, so no underflow here */
        return STATUS_ERR_INVALID;
    }
    uint32_t stored = ((uint32_t)in[trailer] << 24) | ((uint32_t)in[trailer + 1] << 16) |
                      ((uint32_t)in[trailer + 2] << 8) | (uint32_t)in[trailer + 3];
    if (compressAdler32(1, out, produced) != stored) {
        return STATUS_ERR_INVALID;
    }
    if (outLen != NULL) {
        *outLen = produced;
    }
    if (inUsed != NULL) {
        *inUsed = trailer + 4;
    }
    return STATUS_OK;
}

Status compressZlibDeflate(const uint8_t *in, size_t inLen, uint32_t rowStride, uint8_t *out,
                           size_t outCap, size_t *outLen) {
    if (outCap < 6) {
        return STATUS_ERR_NO_MEMORY;
    }
    out[0] = 0x78;
    out[1] = 0x01;
    size_t payloadLen = 0;
    Status st = compressDeflateFixed(in, inLen, rowStride, out + 2, outCap - 6, &payloadLen);
    if (st != STATUS_OK) {
        return st;
    }
    uint32_t adler = compressAdler32(1, in, inLen);
    size_t trailerPos = 2 + payloadLen;
    out[trailerPos + 0] = (uint8_t)(adler >> 24);
    out[trailerPos + 1] = (uint8_t)(adler >> 16);
    out[trailerPos + 2] = (uint8_t)(adler >> 8);
    out[trailerPos + 3] = (uint8_t)adler;
    *outLen = trailerPos + 4;
    return STATUS_OK;
}
