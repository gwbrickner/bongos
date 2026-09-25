/* See zlib_wrap.h. */
#include "zlib_wrap.h"

#include "deflate.h"
#include "inflate.h"

#define ADLER_MOD 65521u

uint32_t adler32Compute(const uint8_t *data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % ADLER_MOD;
        b = (b + a) % ADLER_MOD;
    }
    return (b << 16) | a;
}

bool zlibInflate(const uint8_t *data, size_t size, uint8_t *out, size_t outCap, size_t *outLen) {
    if (size < 6) {
        return false;
    }
    uint8_t cmf = data[0];
    uint8_t flg = data[1];
    if ((cmf & 0x0Fu) != 8) {
        return false; /* CM must be 8 (deflate) */
    }
    if (((uint32_t)cmf * 256u + flg) % 31u != 0) {
        return false; /* FCHECK */
    }
    if ((flg & 0x20u) != 0) {
        return false; /* FDICT: a preset dictionary isn't supported */
    }

    if (!inflateRaw(data + 2, size - 6, out, outCap, outLen)) {
        return false;
    }

    uint32_t stored = ((uint32_t)data[size - 4] << 24) | ((uint32_t)data[size - 3] << 16) |
                      ((uint32_t)data[size - 2] << 8) | (uint32_t)data[size - 1];
    return adler32Compute(out, *outLen) == stored;
}

bool zlibDeflate(const uint8_t *data, size_t size, uint32_t rowStride, uint8_t *out, size_t outCap,
                 size_t *outLen) {
    if (outCap < 6) {
        return false;
    }
    out[0] = 0x78;
    out[1] = 0x01;
    size_t payloadLen = 0;
    if (!deflateFixed(data, size, rowStride, out + 2, outCap - 6, &payloadLen)) {
        return false;
    }
    uint32_t adler = adler32Compute(data, size);
    size_t trailerPos = 2 + payloadLen;
    out[trailerPos + 0] = (uint8_t)(adler >> 24);
    out[trailerPos + 1] = (uint8_t)(adler >> 16);
    out[trailerPos + 2] = (uint8_t)(adler >> 8);
    out[trailerPos + 3] = (uint8_t)adler;
    *outLen = trailerPos + 4;
    return true;
}
